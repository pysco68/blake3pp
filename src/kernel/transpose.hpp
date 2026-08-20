#pragma once

// The message transpose, done as a hardware-isomorphic radix-2 shuffle tree.
//
// hash_batch needs the 16 message words of a block gathered ACROSS lanes
// (word j of every input in one vector). No simd provider exposes a portable
// permute for that (the std::simd MVP has no shuffle API at all), so the
// naive route stages through a scalar array, and it costs ~40% of the whole
// hash (upstream's SSE4.1 assembly matches our AVX2 because of it).
//
// The bypass: GCC and Clang both provide __builtin_shufflevector over vector
// -extension types with compile-time indices, and both instruction-select a
// shuffle whose indices exactly match a hardware macro into that single
// instruction. A W x W word transpose decomposes into log2(W) radix-2
// stages whose index patterns are precisely the in-lane 32-bit unpacks
// (vpunpckl/hdq), the in-lane 64-bit unpacks (vpunpckl/hqdq), and the
// 128-bit lane merges (vperm2i128 / vinserti128): 24 single-uop shuffles
// for the AVX2 8x8 case, verified on GCC 16 (integer domain) and Clang 22
// (same network, float domain: vunpcklps/vunpcklpd/vperm2f128).
//
// The shuffled register then enters the provider's vector type via
// std::bit_cast: all three providers' types are register-sized and
// trivially copyable, so the cast is free. When the provider is xsimd the
// same networks are instead expressed through xsimd::shuffle
// (provider-native, works on MSVC where vector extensions don't exist; on
// GCC/Clang xsimd lowers it through __builtin_shufflevector, so the
// codegen is identical, verified by object-histogram diff). Exotic
// widths fall back to the scalar staging gather automatically.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR)
#include <arm_neon.h>
#endif

#include "kernel/simd_facade.hpp"

#ifndef BLAKE3PP_ARCH_NS
#error "transpose.hpp is kernel-TU-internal; compile with -DBLAKE3PP_ARCH_NS=<variant>"
#endif

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
namespace transpose_detail {

// The W==16 strategy is a RUNTIME dial (kern::transpose16_active, set via
// blake3pp::set_transpose16 / tune_transpose16): on double-pumped AVX-512
// (Strix Point) the register tree measured 20% slower than the scalar
// staging gather while the quartered form measured 17% faster, and no
// CPUID bit distinguishes those microarchitectures, so the winner is
// raced, not detected. All three paths compile into the W==16 kernel; the
// relaxed load deciding between them amortizes over a >=16 KiB batch.

BLAKE3PP_FORCE_INLINE transpose16_mode t16_mode() noexcept {
  return transpose16_active.load(std::memory_order_relaxed);
}

#if (defined(__GNUC__) || defined(__clang__)) && !defined(BLAKE3PP_FORCE_SCALAR)
#define BLAKE3PP_HAVE_SHUFFLE_TREE 1

// GCC raises -Wpsabi for vector types wider than the TU's -m flags allow
// natively. These types never appear in any cross-TU signature (that is the
// entire point of this file's design), so the ABI concern is moot; GCC's
// own <simd> internals suppress the warning on the same reasoning.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"

template <std::size_t W>
struct vext;
template <>
struct vext<4> {
  typedef std::uint32_t type __attribute__((vector_size(16)));
};
template <>
struct vext<8> {
  typedef std::uint32_t type __attribute__((vector_size(32)));
};
template <>
struct vext<16> {
  typedef std::uint32_t type __attribute__((vector_size(64)));
};

template <int... I, class V>
BLAKE3PP_FORCE_INLINE V shuf(V a, V b) noexcept {
  return __builtin_shufflevector(a, b, I...);
}

template <class V>
BLAKE3PP_FORCE_INLINE V load_row(const std::uint8_t* p) noexcept {
  V r;
  std::memcpy(&r, p, sizeof(r));
  return r;
}

// 4x4: two radix-2 stages (32-bit unpacks, then 64-bit unpacks).
BLAKE3PP_FORCE_INLINE void transpose(const vext<4>::type r[4], vext<4>::type out[4]) noexcept {
  using V = vext<4>::type;
  const V a0 = shuf<0, 4, 1, 5>(r[0], r[1]);
  const V a1 = shuf<2, 6, 3, 7>(r[0], r[1]);
  const V a2 = shuf<0, 4, 1, 5>(r[2], r[3]);
  const V a3 = shuf<2, 6, 3, 7>(r[2], r[3]);
  out[0] = shuf<0, 1, 4, 5>(a0, a2);
  out[1] = shuf<2, 3, 6, 7>(a0, a2);
  out[2] = shuf<0, 1, 4, 5>(a1, a3);
  out[3] = shuf<2, 3, 6, 7>(a1, a3);
}

// 8x8: three radix-2 stages. The index sets are lane-local on purpose:
// they are exactly vpunpckl/hdq, vpunpckl/hqdq, and the final cross-lane
// merge vperm2i128/vinserti128.
BLAKE3PP_FORCE_INLINE void transpose(const vext<8>::type r[8], vext<8>::type out[8]) noexcept {
  using V = vext<8>::type;
  const V a0 = shuf<0, 8, 1, 9, 4, 12, 5, 13>(r[0], r[1]);
  const V a1 = shuf<2, 10, 3, 11, 6, 14, 7, 15>(r[0], r[1]);
  const V a2 = shuf<0, 8, 1, 9, 4, 12, 5, 13>(r[2], r[3]);
  const V a3 = shuf<2, 10, 3, 11, 6, 14, 7, 15>(r[2], r[3]);
  const V a4 = shuf<0, 8, 1, 9, 4, 12, 5, 13>(r[4], r[5]);
  const V a5 = shuf<2, 10, 3, 11, 6, 14, 7, 15>(r[4], r[5]);
  const V a6 = shuf<0, 8, 1, 9, 4, 12, 5, 13>(r[6], r[7]);
  const V a7 = shuf<2, 10, 3, 11, 6, 14, 7, 15>(r[6], r[7]);
  const V b0 = shuf<0, 1, 8, 9, 4, 5, 12, 13>(a0, a2);
  const V b1 = shuf<2, 3, 10, 11, 6, 7, 14, 15>(a0, a2);
  const V b2 = shuf<0, 1, 8, 9, 4, 5, 12, 13>(a1, a3);
  const V b3 = shuf<2, 3, 10, 11, 6, 7, 14, 15>(a1, a3);
  const V b4 = shuf<0, 1, 8, 9, 4, 5, 12, 13>(a4, a6);
  const V b5 = shuf<2, 3, 10, 11, 6, 7, 14, 15>(a4, a6);
  const V b6 = shuf<0, 1, 8, 9, 4, 5, 12, 13>(a5, a7);
  const V b7 = shuf<2, 3, 10, 11, 6, 7, 14, 15>(a5, a7);
  out[0] = shuf<0, 1, 2, 3, 8, 9, 10, 11>(b0, b4);
  out[1] = shuf<0, 1, 2, 3, 8, 9, 10, 11>(b1, b5);
  out[2] = shuf<0, 1, 2, 3, 8, 9, 10, 11>(b2, b6);
  out[3] = shuf<0, 1, 2, 3, 8, 9, 10, 11>(b3, b7);
  out[4] = shuf<4, 5, 6, 7, 12, 13, 14, 15>(b0, b4);
  out[5] = shuf<4, 5, 6, 7, 12, 13, 14, 15>(b1, b5);
  out[6] = shuf<4, 5, 6, 7, 12, 13, 14, 15>(b2, b6);
  out[7] = shuf<4, 5, 6, 7, 12, 13, 14, 15>(b3, b7);
}

// 16x16: four radix-2 stages. Index lists derived from the same recursive
// construction (verified by simulation): in-lane dword unpacks
// (vpunpckl/hdq), in-lane qword unpacks (vpunpckl/hqdq), then two levels
// of 128-bit-block merges, AVX-512's vshufi32x4 territory; even a
// generic lowering lands on vpermt2d (any two-source dword permute, one
// uop). 64 two-register shuffles replace the 256 scalar load/stores of
// the staging gather.
BLAKE3PP_FORCE_INLINE void transpose(const vext<16>::type r[16],
                      vext<16>::type out[16]) noexcept {
  using V = vext<16>::type;
  V a[16];
  for (std::size_t g = 0; g < 8; ++g) {
    a[2 * g] = shuf<0, 16, 1, 17, 4, 20, 5, 21, 8, 24, 9, 25, 12, 28, 13,
                    29>(r[2 * g], r[2 * g + 1]);
    a[2 * g + 1] = shuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30,
                        15, 31>(r[2 * g], r[2 * g + 1]);
  }
  V b[16];
  for (std::size_t q = 0; q < 4; ++q) {
    const std::size_t k = 4 * q;
    b[k + 0] = shuf<0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28,
                    29>(a[k + 0], a[k + 2]);
    b[k + 1] = shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30,
                    31>(a[k + 0], a[k + 2]);
    b[k + 2] = shuf<0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28,
                    29>(a[k + 1], a[k + 3]);
    b[k + 3] = shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30,
                    31>(a[k + 1], a[k + 3]);
  }
  V c[16];
  for (std::size_t h = 0; h < 2; ++h) {
    const std::size_t k = 8 * h;
    for (std::size_t j = 0; j < 4; ++j) {
      c[k + j] = shuf<0, 1, 2, 3, 16, 17, 18, 19, 8, 9, 10, 11, 24, 25, 26,
                      27>(b[k + j], b[k + j + 4]);
      c[k + j + 4] = shuf<4, 5, 6, 7, 20, 21, 22, 23, 12, 13, 14, 15, 28,
                          29, 30, 31>(b[k + j], b[k + j + 4]);
    }
  }
  for (std::size_t j = 0; j < 8; ++j) {
    out[j] = shuf<0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23>(
        c[j], c[j + 8]);
    out[j + 8] = shuf<8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29,
                      30, 31>(c[j], c[j + 8]);
  }
}

// The two in-lane stages alone, factored for the quartered form: with
// block-level transposition already done by 128-bit addressing, a 4x4
// transpose per 128-bit lane finishes the job (same s1/s2 index lists as
// the full tree; all vpunpck, no cross-lane traffic).
template <class V>
BLAKE3PP_FORCE_INLINE void inlane_4x4(const V (&r)[4], V (&t)[4]) noexcept {
  const V a0 = shuf<0, 16, 1, 17, 4, 20, 5, 21, 8, 24, 9, 25, 12, 28, 13,
                    29>(r[0], r[1]);
  const V a1 = shuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15,
                    31>(r[0], r[1]);
  const V a2 = shuf<0, 16, 1, 17, 4, 20, 5, 21, 8, 24, 9, 25, 12, 28, 13,
                    29>(r[2], r[3]);
  const V a3 = shuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15,
                    31>(r[2], r[3]);
  t[0] = shuf<0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28, 29>(a0,
                                                                        a2);
  t[1] = shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(
      a0, a2);
  t[2] = shuf<0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28, 29>(a1,
                                                                        a3);
  t[3] = shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(
      a1, a3);
}

// Byte-granular rotate: rotr by a multiple of 8 bits is a byte permutation
// within each 32-bit element, and the lane-local constexpr pattern is
// exactly vpshufb: one uop instead of the three (shift, shift, or) that a
// generic rotate costs on ISAs without a native rotate instruction.
template <std::size_t W>
struct bext;
template <>
struct bext<4> {
  typedef std::uint8_t type __attribute__((vector_size(16)));
};
template <>
struct bext<8> {
  typedef std::uint8_t type __attribute__((vector_size(32)));
};

template <int RB, std::size_t W>
BLAKE3PP_FORCE_INLINE typename vext<W>::type rot_bytes(typename vext<W>::type x) noexcept {
  using B = typename bext<W>::type;
  const B b = std::bit_cast<B>(x);
  const B r = [&]<std::size_t... I>(std::index_sequence<I...>) {
    // Little-endian: rotr by 8*RB bits moves source byte (j+RB)%4 into
    // destination byte j of each element.
    return __builtin_shufflevector(b, b,
                                   ((I / 4) * 4 + ((I % 4) + RB) % 4)...);
  }(std::make_index_sequence<4 * W>{});
  return std::bit_cast<typename vext<W>::type>(r);
}

#pragma GCC diagnostic pop
#endif

#if defined(BLAKE3PP_HAS_XSIMD) && !defined(BLAKE3PP_FORCE_SCALAR) && \
    !(defined(_M_ARM64) && !defined(__clang__))
// The pure-MSVC-arm64 exclusion: xsimd 14.3's neon64 swizzle (which the
// shuffle decomposition instantiates on non-builtin frontends) returns
// through a vreinterpretq_* chain that MSVC's arm64_neon.h defines as
// no-ops over one shared __n128 type, so a batch<uint8_t> lands in a
// batch<uint32_t> return seat and C2440 follows. Until that is fixed
// upstream, cl-on-arm64 keeps NEON rounds but stages its transposes.
#define BLAKE3PP_HAVE_XSIMD_SHUFFLE 1

// The same radix-2 networks, expressed through xsimd's portable two-input
// constant shuffle instead of raw vector extensions. On GCC>=13 and Clang
// (incl. clang-cl) xsimd lowers this through __builtin_shufflevector, so
// the codegen is instruction-identical to the vext tree (verified: 24
// shuffles for the AVX2 8x8). On MSVC, which has no such builtin, xsimd
// decomposes each shuffle into swizzle(x)+swizzle(y)+select (~3 uops),
// still far ahead of the scalar staging gather. This is what makes the
// xsimd provider self-contained: no compiler-specific machinery required.
template <std::uint32_t... I, class B>
BLAKE3PP_FORCE_INLINE B xshuf(B a, B b) noexcept {
  return xsimd::shuffle(
      a, b,
      xsimd::batch_constant<std::uint32_t, typename B::arch_type, I...>{});
}

template <class B>
BLAKE3PP_FORCE_INLINE void xtranspose(const B (&r)[4], B (&out)[4]) noexcept {
  const B a0 = xshuf<0, 4, 1, 5>(r[0], r[1]);
  const B a1 = xshuf<2, 6, 3, 7>(r[0], r[1]);
  const B a2 = xshuf<0, 4, 1, 5>(r[2], r[3]);
  const B a3 = xshuf<2, 6, 3, 7>(r[2], r[3]);
  out[0] = xshuf<0, 1, 4, 5>(a0, a2);
  out[1] = xshuf<2, 3, 6, 7>(a0, a2);
  out[2] = xshuf<0, 1, 4, 5>(a1, a3);
  out[3] = xshuf<2, 3, 6, 7>(a1, a3);
}

template <class B>
BLAKE3PP_FORCE_INLINE void xtranspose(const B (&r)[8], B (&out)[8]) noexcept {
  const B a0 = xshuf<0, 8, 1, 9, 4, 12, 5, 13>(r[0], r[1]);
  const B a1 = xshuf<2, 10, 3, 11, 6, 14, 7, 15>(r[0], r[1]);
  const B a2 = xshuf<0, 8, 1, 9, 4, 12, 5, 13>(r[2], r[3]);
  const B a3 = xshuf<2, 10, 3, 11, 6, 14, 7, 15>(r[2], r[3]);
  const B a4 = xshuf<0, 8, 1, 9, 4, 12, 5, 13>(r[4], r[5]);
  const B a5 = xshuf<2, 10, 3, 11, 6, 14, 7, 15>(r[4], r[5]);
  const B a6 = xshuf<0, 8, 1, 9, 4, 12, 5, 13>(r[6], r[7]);
  const B a7 = xshuf<2, 10, 3, 11, 6, 14, 7, 15>(r[6], r[7]);
  const B b0 = xshuf<0, 1, 8, 9, 4, 5, 12, 13>(a0, a2);
  const B b1 = xshuf<2, 3, 10, 11, 6, 7, 14, 15>(a0, a2);
  const B b2 = xshuf<0, 1, 8, 9, 4, 5, 12, 13>(a1, a3);
  const B b3 = xshuf<2, 3, 10, 11, 6, 7, 14, 15>(a1, a3);
  const B b4 = xshuf<0, 1, 8, 9, 4, 5, 12, 13>(a4, a6);
  const B b5 = xshuf<2, 3, 10, 11, 6, 7, 14, 15>(a4, a6);
  const B b6 = xshuf<0, 1, 8, 9, 4, 5, 12, 13>(a5, a7);
  const B b7 = xshuf<2, 3, 10, 11, 6, 7, 14, 15>(a5, a7);
  out[0] = xshuf<0, 1, 2, 3, 8, 9, 10, 11>(b0, b4);
  out[1] = xshuf<0, 1, 2, 3, 8, 9, 10, 11>(b1, b5);
  out[2] = xshuf<0, 1, 2, 3, 8, 9, 10, 11>(b2, b6);
  out[3] = xshuf<0, 1, 2, 3, 8, 9, 10, 11>(b3, b7);
  out[4] = xshuf<4, 5, 6, 7, 12, 13, 14, 15>(b0, b4);
  out[5] = xshuf<4, 5, 6, 7, 12, 13, 14, 15>(b1, b5);
  out[6] = xshuf<4, 5, 6, 7, 12, 13, 14, 15>(b2, b6);
  out[7] = xshuf<4, 5, 6, 7, 12, 13, 14, 15>(b3, b7);
}

template <class B>
BLAKE3PP_FORCE_INLINE void xtranspose(const B (&r)[16], B (&out)[16]) noexcept {
  B a[16];
  for (std::size_t g = 0; g < 8; ++g) {
    a[2 * g] = xshuf<0, 16, 1, 17, 4, 20, 5, 21, 8, 24, 9, 25, 12, 28, 13,
                     29>(r[2 * g], r[2 * g + 1]);
    a[2 * g + 1] = xshuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30,
                         15, 31>(r[2 * g], r[2 * g + 1]);
  }
  B b[16];
  for (std::size_t q = 0; q < 4; ++q) {
    const std::size_t k = 4 * q;
    b[k + 0] = xshuf<0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28,
                     29>(a[k + 0], a[k + 2]);
    b[k + 1] = xshuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30,
                     31>(a[k + 0], a[k + 2]);
    b[k + 2] = xshuf<0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28,
                     29>(a[k + 1], a[k + 3]);
    b[k + 3] = xshuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30,
                     31>(a[k + 1], a[k + 3]);
  }
  B c[16];
  for (std::size_t h = 0; h < 2; ++h) {
    const std::size_t k = 8 * h;
    for (std::size_t j = 0; j < 4; ++j) {
      c[k + j] = xshuf<0, 1, 2, 3, 16, 17, 18, 19, 8, 9, 10, 11, 24, 25, 26,
                       27>(b[k + j], b[k + j + 4]);
      c[k + j + 4] = xshuf<4, 5, 6, 7, 20, 21, 22, 23, 12, 13, 14, 15, 28,
                           29, 30, 31>(b[k + j], b[k + j + 4]);
    }
  }
  for (std::size_t j = 0; j < 8; ++j) {
    out[j] = xshuf<0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23>(
        c[j], c[j + 8]);
    out[j + 8] = xshuf<8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29,
                       30, 31>(c[j], c[j + 8]);
  }
}

// Byte-rotate mask: dest byte i of each 32-bit element takes source byte
// ((i%4)+RB)%4, a little-endian rotr by 8*RB bits, the same pattern the
// vext rot_bytes encodes. Lane-local by construction, which is what makes
// xsimd 14.3's constant u8 swizzle emit a single vpshufb (its
// is_cross_lane check) instead of a cross-lane fixup.
template <int RB>
struct rot_bytes_gen {
  static constexpr std::uint8_t get(std::size_t i, std::size_t) noexcept {
    return static_cast<std::uint8_t>((i / 4) * 4 + ((i % 4) + RB) % 4);
  }
};

template <class B>
BLAKE3PP_FORCE_INLINE void xinlane_4x4(const B (&r)[4], B (&t)[4]) noexcept {
  const B a0 = xshuf<0, 16, 1, 17, 4, 20, 5, 21, 8, 24, 9, 25, 12, 28, 13,
                     29>(r[0], r[1]);
  const B a1 = xshuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15,
                     31>(r[0], r[1]);
  const B a2 = xshuf<0, 16, 1, 17, 4, 20, 5, 21, 8, 24, 9, 25, 12, 28, 13,
                     29>(r[2], r[3]);
  const B a3 = xshuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15,
                     31>(r[2], r[3]);
  t[0] = xshuf<0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28, 29>(
      a0, a2);
  t[1] = xshuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(
      a0, a2);
  t[2] = xshuf<0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28, 29>(
      a1, a3);
  t[3] = xshuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(
      a1, a3);
}
#endif  // BLAKE3PP_HAS_XSIMD

BLAKE3PP_FORCE_INLINE std::uint32_t ld32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace transpose_detail

// Fills m[0..15] with the block's message words transposed across W lanes:
// m[j][lane] = word j of inputs[lane] at byte offset `offset`. Radix-2
// shuffle tree where expressible (W of 4, 8 or 16), scalar staging gather
// everywhere else.
template <std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE void load_transposed(const std::uint8_t* const* inputs,
                            std::size_t offset, u32v m[16]) noexcept {
  namespace td = transpose_detail;
  // Note the preprocessor gates doubling the if-constexpr ones: a discarded
  // constexpr branch still name-looks-up its non-dependent identifiers, so
  // the shuffle machinery must not even be *named* in TUs that lack it.
#if defined(BLAKE3PP_HAVE_XSIMD_SHUFFLE)
  // Provider-native path: when u32v wraps an xsimd batch, transpose the
  // batches directly: self-contained (works on MSVC), and on GCC/Clang
  // instruction-identical to the vext tree below.
  if constexpr ((W == 4 || W == 8 || W == 16) &&
                std::endian::native == std::endian::little) {
    using B = typename u32v::impl;
    if constexpr (W == 16) {
      const kern::transpose16_mode mode = td::t16_mode();
      if (mode == kern::transpose16_mode::quartered) {
        // Quartered: 128-bit pieces land block-transposed by ADDRESSING;
        // registers only run the two in-lane stages.
        for (std::size_t q = 0; q < 4; ++q) {
          B r[4];
          for (std::size_t k = 0; k < 4; ++k) {
            std::uint8_t quad[64];
            for (std::size_t l = 0; l < 4; ++l) {
              std::memcpy(quad + 16 * l,
                          inputs[4 * l + k] + offset + 16 * q, 16);
            }
            std::memcpy(&r[k], quad, 64);
          }
          B t[4];
          td::xinlane_4x4(r, t);
          for (std::size_t j = 0; j < 4; ++j) {
            m[4 * q + j] = u32v{t[j]};
          }
        }
        return;
      }
      if (mode == kern::transpose16_mode::tree) {
        B r[16];
        for (std::size_t lane = 0; lane < 16; ++lane) {
          std::memcpy(&r[lane], inputs[lane] + offset, sizeof(B));
        }
        B t[16];
        td::xtranspose(r, t);
        for (std::size_t j = 0; j < 16; ++j) {
          m[j] = u32v{t[j]};
        }
        return;
      }
      // staging: fall through to the scalar gather below.
    } else {
      constexpr std::size_t groups = 16 / W;
      for (std::size_t g = 0; g < groups; ++g) {
        B r[W];
        for (std::size_t lane = 0; lane < W; ++lane) {
          std::memcpy(&r[lane], inputs[lane] + offset + g * W * 4,
                      sizeof(B));
        }
        B t[W];
        td::xtranspose(r, t);
        for (std::size_t j = 0; j < W; ++j) {
          m[g * W + j] = u32v{t[j]};
        }
      }
      return;
    }
  }
#endif
#if defined(BLAKE3PP_HAVE_SHUFFLE_TREE)
  if constexpr ((W == 4 || W == 8 || W == 16) &&
                std::endian::native == std::endian::little &&
                sizeof(typename u32v::impl) == 4 * W &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    using V = typename td::vext<W>::type;
    if constexpr (W == 16) {
      const kern::transpose16_mode mode = td::t16_mode();
      if (mode == kern::transpose16_mode::quartered) {
        for (std::size_t q = 0; q < 4; ++q) {
          V r[4];
          for (std::size_t k = 0; k < 4; ++k) {
            std::uint8_t quad[64];
            for (std::size_t l = 0; l < 4; ++l) {
              std::memcpy(quad + 16 * l,
                          inputs[4 * l + k] + offset + 16 * q, 16);
            }
            r[k] = td::load_row<V>(quad);
          }
          V t[4];
          td::inlane_4x4(r, t);
          for (std::size_t j = 0; j < 4; ++j) {
            m[4 * q + j] = u32v{std::bit_cast<typename u32v::impl>(t[j])};
          }
        }
        return;
      }
      if (mode == kern::transpose16_mode::tree) {
        V r[16];
        for (std::size_t lane = 0; lane < 16; ++lane) {
          r[lane] = td::load_row<V>(inputs[lane] + offset);
        }
        V t[16];
        td::transpose(r, t);
        for (std::size_t j = 0; j < 16; ++j) {
          m[j] = u32v{std::bit_cast<typename u32v::impl>(t[j])};
        }
        return;
      }
      // staging: fall through.
    } else {
      constexpr std::size_t groups = 16 / W;
      for (std::size_t g = 0; g < groups; ++g) {
        V r[W];
        for (std::size_t lane = 0; lane < W; ++lane) {
          r[lane] = td::load_row<V>(inputs[lane] + offset + g * W * 4);
        }
        V t[W];
        td::transpose(r, t);
        for (std::size_t j = 0; j < W; ++j) {
          m[g * W + j] = u32v{std::bit_cast<typename u32v::impl>(t[j])};
        }
      }
      return;
    }
  }
#endif
  std::uint32_t lanes[W];
  for (std::size_t j = 0; j < 16; ++j) {
    for (std::size_t lane = 0; lane < W; ++lane) {
      lanes[lane] = td::ld32(inputs[lane] + offset + 4 * j);
    }
    m[j] = u32v::load(lanes);
  }
}

// Writes 16 wide words lane-major: lane l receives words w[0..15][l] as
// 64 little-endian bytes at out + l*64, the mirror of load_transposed,
// using the same radix-2 shuffle trees where expressible.
template <std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE void store_transposed(const u32v (&w)[16], std::uint8_t* out) noexcept {
  namespace td = transpose_detail;
#if defined(BLAKE3PP_HAVE_XSIMD_SHUFFLE)
  if constexpr ((W == 4 || W == 8 || W == 16) &&
                std::endian::native == std::endian::little) {
    using B = typename u32v::impl;
    if constexpr (W == 16) {
      const kern::transpose16_mode mode = td::t16_mode();
      if (mode == kern::transpose16_mode::quartered) {
        // Quartered mirror: two in-lane stages, then 128-bit pieces go
        // to their destinations by addressing (extract-stores).
        for (std::size_t q = 0; q < 4; ++q) {
          B r[4];
          for (std::size_t j = 0; j < 4; ++j) {
            r[j] = w[4 * q + j].v;
          }
          B t[4];
          td::xinlane_4x4(r, t);
          for (std::size_t k = 0; k < 4; ++k) {
            std::uint8_t quad[64];
            std::memcpy(quad, &t[k], 64);
            for (std::size_t l = 0; l < 4; ++l) {
              std::memcpy(out + (4 * l + k) * 64 + 16 * q, quad + 16 * l,
                          16);
            }
          }
        }
        return;
      }
      if (mode == kern::transpose16_mode::tree) {
        B r[16];
        for (std::size_t j = 0; j < 16; ++j) {
          r[j] = w[j].v;
        }
        B t[16];
        td::xtranspose(r, t);
        for (std::size_t lane = 0; lane < 16; ++lane) {
          std::memcpy(out + lane * 64, &t[lane], sizeof(B));
        }
        return;
      }
      // staging: fall through.
    } else {
      constexpr std::size_t groups = 16 / W;
      for (std::size_t g = 0; g < groups; ++g) {
        B r[W];
        for (std::size_t j = 0; j < W; ++j) {
          r[j] = w[g * W + j].v;
        }
        B t[W];
        td::xtranspose(r, t);
        for (std::size_t lane = 0; lane < W; ++lane) {
          std::memcpy(out + lane * 64 + g * W * 4, &t[lane], sizeof(B));
        }
      }
      return;
    }
  }
#endif
#if defined(BLAKE3PP_HAVE_SHUFFLE_TREE)
  if constexpr ((W == 4 || W == 8 || W == 16) &&
                std::endian::native == std::endian::little &&
                sizeof(typename u32v::impl) == 4 * W &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    using V = typename td::vext<W>::type;
    if constexpr (W == 16) {
      const kern::transpose16_mode mode = td::t16_mode();
      if (mode == kern::transpose16_mode::quartered) {
        for (std::size_t q = 0; q < 4; ++q) {
          V r[4];
          for (std::size_t j = 0; j < 4; ++j) {
            r[j] = std::bit_cast<V>(w[4 * q + j].v);
          }
          V t[4];
          td::inlane_4x4(r, t);
          for (std::size_t k = 0; k < 4; ++k) {
            std::uint8_t quad[64];
            std::memcpy(quad, &t[k], 64);
            for (std::size_t l = 0; l < 4; ++l) {
              std::memcpy(out + (4 * l + k) * 64 + 16 * q, quad + 16 * l,
                          16);
            }
          }
        }
        return;
      }
      if (mode == kern::transpose16_mode::tree) {
        V r[16];
        for (std::size_t j = 0; j < 16; ++j) {
          r[j] = std::bit_cast<V>(w[j].v);
        }
        V t[16];
        td::transpose(r, t);
        for (std::size_t lane = 0; lane < 16; ++lane) {
          std::memcpy(out + lane * 64, &t[lane], sizeof(V));
        }
        return;
      }
      // staging: fall through.
    } else {
      constexpr std::size_t groups = 16 / W;
      for (std::size_t g = 0; g < groups; ++g) {
        V r[W];
        for (std::size_t j = 0; j < W; ++j) {
          r[j] = std::bit_cast<V>(w[g * W + j].v);
        }
        V t[W];
        td::transpose(r, t);
        for (std::size_t lane = 0; lane < W; ++lane) {
          std::memcpy(out + lane * 64 + g * W * 4, &t[lane], sizeof(V));
        }
      }
      return;
    }
  }
#endif
  std::uint32_t lanes[W];
  for (std::size_t j = 0; j < 16; ++j) {
    w[j].store(lanes);
    for (std::size_t lane = 0; lane < W; ++lane) {
      const std::uint32_t v = lanes[lane];
      std::uint8_t* p = out + lane * 64 + 4 * j;
      p[0] = static_cast<std::uint8_t>(v);
      p[1] = static_cast<std::uint8_t>(v >> 8);
      p[2] = static_cast<std::uint8_t>(v >> 16);
      p[3] = static_cast<std::uint8_t>(v >> 24);
    }
  }
}

// Compile-time-amount rotate for the wide word: byte-shuffle single-uop
// path for the 16- and 8-bit rotates where expressible, generic shift-or
// otherwise. The scalar word overload lives in kernel.cpp.
// W is a defaulted template parameter (not read directly off u32v) so the
// discarded constexpr branch stays dependent, the same trap as in
// load_transposed: non-dependent constructs in a discarded branch are still
// instantiated (vext<16> exists nowadays, but the discipline stays).
template <int N, std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE u32v rot(u32v a) noexcept {
#if defined(BLAKE3PP_HAVE_SHUFFLE_TREE)
  if constexpr ((N == 16 || N == 8) && (W == 4 || W == 8) &&
                std::endian::native == std::endian::little &&
                sizeof(typename u32v::impl) == 4 * W &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    using V = typename transpose_detail::vext<W>::type;
    return u32v{std::bit_cast<typename u32v::impl>(
        transpose_detail::rot_bytes<N / 8, W>(std::bit_cast<V>(a.v)))};
  }
#endif
#if defined(BLAKE3PP_HAVE_XSIMD_SHUFFLE)
  // Provider-native byte-rotate for frontends without vector extensions
  // (pure MSVC): one vpshufb via xsimd's constant u8 swizzle instead of
  // the three-op shift-or. W==16 stays on shift-or: avx512bw has no
  // constant u8 swizzle in xsimd 14.3 (and GNU compilers fuse shift-or
  // into vprold there anyway). CRITICAL gate: the hardware must actually
  // HAVE a byte shuffle. MSVC has no /arch:SSE4.2, so the "sse42" kernel
  // is xsimd-sse2 there, and xsimd's sse2 u8 swizzle is a per-byte
  // scalar loop that measured 4x WORSE than shift-or. x86 needs ssse3+;
  // NEON and wasm carry native byte shuffles.
  constexpr bool is_x86 =
      std::is_base_of_v<xsimd::sse2, typename u32v::impl::arch_type>;
  constexpr bool has_byte_shuffle =
      !is_x86 ||
      std::is_base_of_v<xsimd::ssse3, typename u32v::impl::arch_type>;
  if constexpr ((N == 16 || N == 8) && (W == 4 || W == 8) &&
                has_byte_shuffle &&
                std::endian::native == std::endian::little) {
    using B = typename u32v::impl;
    const auto bytes = xsimd::bitwise_cast<std::uint8_t>(a.v);
    constexpr auto mask = xsimd::make_batch_constant<
        std::uint8_t, transpose_detail::rot_bytes_gen<N / 8>,
        typename B::arch_type>();
    return u32v{
        xsimd::bitwise_cast<std::uint32_t>(xsimd::swizzle(bytes, mask))};
  }
#endif
#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR)
  // The rotate amounts with no byte-granular shuffle (12 and 7): shl+sri
  // instead of the shl+usra clang selects for the generic shift-or. SRI and
  // USRA cost the same two instructions, but SRI is a cycle faster on Apple
  // cores, and these rotates sit on g's serial critical path. This was the
  // entire residual against upstream's blake3_neon.c, whose explicit
  // intrinsics reach sri directly (their PR #319 measured the same):
  // 1.61 -> 1.71 GiB/s on Apple M2 / clang 22, exactly upstream's number;
  // the two hash loops are otherwise instruction-for-instruction identical.
  if constexpr (W == 4 && sizeof(typename u32v::impl) == 16 &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    const uint32x4_t x = std::bit_cast<uint32x4_t>(a.v);
    return u32v{std::bit_cast<typename u32v::impl>(
        vsriq_n_u32(vshlq_n_u32(x, 32 - N), x, N))};
  }
#endif
  return rotr(a, N);
}

}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
