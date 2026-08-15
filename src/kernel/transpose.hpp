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
// trivially copyable, so the cast is free. MSVC (no vector extensions) and
// exotic widths fall back to the scalar staging gather automatically.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "kernel/simd_facade.hpp"

#ifndef BLAKE3PP_ARCH_NS
#error "transpose.hpp is kernel-TU-internal; compile with -DBLAKE3PP_ARCH_NS=<variant>"
#endif

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
namespace transpose_detail {

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

template <int... I, class V>
inline V shuf(V a, V b) noexcept {
  return __builtin_shufflevector(a, b, I...);
}

template <class V>
inline V load_row(const std::uint8_t* p) noexcept {
  V r;
  std::memcpy(&r, p, sizeof(r));
  return r;
}

// 4x4: two radix-2 stages (32-bit unpacks, then 64-bit unpacks).
inline void transpose(const vext<4>::type r[4], vext<4>::type out[4]) noexcept {
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
inline void transpose(const vext<8>::type r[8], vext<8>::type out[8]) noexcept {
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
inline typename vext<W>::type rot_bytes(typename vext<W>::type x) noexcept {
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

inline std::uint32_t ld32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace transpose_detail

// Fills m[0..15] with the block's message words transposed across W lanes:
// m[j][lane] = word j of inputs[lane] at byte offset `offset`. Radix-2
// shuffle tree where expressible (W of 4 or 8 on GCC/Clang; AVX-512's 16
// still stages; TODO: a 16x16 network over vshufi32x4), scalar staging
// gather everywhere else.
template <std::size_t W = u32v::width>
inline void load_transposed(const std::uint8_t* const* inputs,
                            std::size_t offset, u32v m[16]) noexcept {
  namespace td = transpose_detail;
  // Note the preprocessor gate doubling the if-constexpr one: a discarded
  // constexpr branch still name-looks-up its non-dependent identifiers, so
  // the shuffle machinery must not even be *named* in TUs that lack it.
#if defined(BLAKE3PP_HAVE_SHUFFLE_TREE)
  if constexpr ((W == 4 || W == 8) &&
                std::endian::native == std::endian::little &&
                sizeof(typename u32v::impl) == 4 * W &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    using V = typename td::vext<W>::type;
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
#endif
  std::uint32_t lanes[W];
  for (std::size_t j = 0; j < 16; ++j) {
    for (std::size_t lane = 0; lane < W; ++lane) {
      lanes[lane] = td::ld32(inputs[lane] + offset + 4 * j);
    }
    m[j] = u32v::load(lanes);
  }
}

// Compile-time-amount rotate for the wide word: byte-shuffle single-uop
// path for the 16- and 8-bit rotates where expressible, generic shift-or
// otherwise. The scalar word overload lives in kernel.cpp.
// W is a defaulted template parameter (not read directly off u32v) so the
// discarded constexpr branch stays dependent, the same trap as in
// load_transposed: non-dependent constructs in a discarded branch are still
// instantiated, and vext<16> has no definition.
template <int N, std::size_t W = u32v::width>
inline u32v rot(u32v a) noexcept {
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
  return rotr(a, N);
}

}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
