#pragma once

// The message transpose, and the wide word's compile-time-amount rotate.
//
// hash_batch needs the 16 message words of a block gathered ACROSS lanes
// (word j of every input in one vector). No simd provider exposes a portable
// permute for that (the std::simd MVP has no shuffle API at all), so the
// naive route stages through a scalar array, and it costs ~40% of the whole
// hash (upstream's SSE4.1 assembly matches our AVX2 because of it).
//
// The bypass is a radix-2 shuffle tree whose index patterns ARE hardware
// macro-ops; it lives in shuffle/networks.hpp, written once over whichever
// backend shuffle.hpp selected for this TU. What remains here is the part
// that is genuinely about transposing a message: which registers to load
// from where, and where their transposed results go. Widths other than
// 4/8/16, and TUs with no shuffle backend at all, fall back to the scalar
// staging gather at the bottom of each function.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR)
#include <arm_neon.h>
#endif

#include "kernel/force_inline.hpp"
#include "kernel/shuffle.hpp"
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

BLAKE3PP_FORCE_INLINE std::uint32_t ld32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

#if defined(BLAKE3PP_HAVE_SHUFFLE)

// Gathers the block's 16 message words across W lanes through the shuffle
// networks. Returns false only for the W==16 `staging` dial setting, whose
// whole point is to use the scalar gather instead.
template <class B, std::size_t W>
BLAKE3PP_FORCE_INLINE bool shuffle_load(const std::uint8_t* const* inputs,
                                        std::size_t offset, u32v m[16],
                                        [[maybe_unused]]
                                        transpose16_mode mode) noexcept {
  using V = typename B::template reg<W>;
  if constexpr (W == 16) {
    if (mode == transpose16_mode::quartered) {
      // Quartered: 128-bit pieces land block-transposed by ADDRESSING;
      // registers only run the two in-lane stages.
      for (std::size_t q = 0; q < 4; ++q) {
        V r[4];
        for (std::size_t k = 0; k < 4; ++k) {
          std::uint8_t quad[64];
          for (std::size_t l = 0; l < 4; ++l) {
            std::memcpy(quad + 16 * l, inputs[4 * l + k] + offset + 16 * q,
                        16);
          }
          r[k] = B::template load<W>(quad);
        }
        V t[4];
        shuffle_detail::inlane_4x4<B>(r, t);
        for (std::size_t j = 0; j < 4; ++j) {
          m[4 * q + j] = B::template to_word<W>(t[j]);
        }
      }
      return true;
    }
    if (mode == transpose16_mode::tree) {
      V r[16];
      for (std::size_t lane = 0; lane < 16; ++lane) {
        r[lane] = B::template load<W>(inputs[lane] + offset);
      }
      V t[16];
      shuffle_detail::transpose_net<B>(r, t);
      for (std::size_t j = 0; j < 16; ++j) {
        m[j] = B::template to_word<W>(t[j]);
      }
      return true;
    }
    return false;  // staging
  } else {
    constexpr std::size_t groups = 16 / W;
    for (std::size_t g = 0; g < groups; ++g) {
      V r[W];
      for (std::size_t lane = 0; lane < W; ++lane) {
        r[lane] = B::template load<W>(inputs[lane] + offset + g * W * 4);
      }
      V t[W];
      shuffle_detail::transpose_net<B>(r, t);
      for (std::size_t j = 0; j < W; ++j) {
        m[g * W + j] = B::template to_word<W>(t[j]);
      }
    }
    return true;
  }
}

// The mirror image: 16 wide words out to W lane-major 64-byte blocks.
template <class B, std::size_t W>
BLAKE3PP_FORCE_INLINE bool shuffle_store(const u32v (&w)[16],
                                         std::uint8_t* out,
                                         [[maybe_unused]]
                                         transpose16_mode mode) noexcept {
  using V = typename B::template reg<W>;
  if constexpr (W == 16) {
    if (mode == transpose16_mode::quartered) {
      // Quartered mirror: two in-lane stages, then 128-bit pieces go to
      // their destinations by addressing (extract-stores).
      for (std::size_t q = 0; q < 4; ++q) {
        V r[4];
        for (std::size_t j = 0; j < 4; ++j) {
          r[j] = B::template from_word<W>(w[4 * q + j]);
        }
        V t[4];
        shuffle_detail::inlane_4x4<B>(r, t);
        for (std::size_t k = 0; k < 4; ++k) {
          std::uint8_t quad[64];
          B::template store<W>(quad, t[k]);
          for (std::size_t l = 0; l < 4; ++l) {
            std::memcpy(out + (4 * l + k) * 64 + 16 * q, quad + 16 * l, 16);
          }
        }
      }
      return true;
    }
    if (mode == transpose16_mode::tree) {
      V r[16];
      for (std::size_t j = 0; j < 16; ++j) {
        r[j] = B::template from_word<W>(w[j]);
      }
      V t[16];
      shuffle_detail::transpose_net<B>(r, t);
      for (std::size_t lane = 0; lane < 16; ++lane) {
        B::template store<W>(out + lane * 64, t[lane]);
      }
      return true;
    }
    return false;  // staging
  } else {
    constexpr std::size_t groups = 16 / W;
    for (std::size_t g = 0; g < groups; ++g) {
      V r[W];
      for (std::size_t j = 0; j < W; ++j) {
        r[j] = B::template from_word<W>(w[g * W + j]);
      }
      V t[W];
      shuffle_detail::transpose_net<B>(r, t);
      for (std::size_t lane = 0; lane < W; ++lane) {
        B::template store<W>(out + lane * 64 + g * W * 4, t[lane]);
      }
    }
    return true;
  }
}

#endif  // BLAKE3PP_HAVE_SHUFFLE

}  // namespace transpose_detail

// Fills m[0..15] with the block's message words transposed across W lanes:
// m[j][lane] = word j of inputs[lane] at byte offset `offset`.
template <std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE void load_transposed(const std::uint8_t* const* inputs,
                                           std::size_t offset, u32v m[16],
                                           [[maybe_unused]]
                                           transpose16_mode mode) noexcept {
  namespace td = transpose_detail;
#if defined(BLAKE3PP_HAVE_SHUFFLE)
  if constexpr (shuffle_backend::supports<W>) {
    if (td::shuffle_load<shuffle_backend, W>(inputs, offset, m, mode)) {
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
// 64 little-endian bytes at out + l*64, the mirror of load_transposed.
template <std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE void store_transposed(const u32v (&w)[16],
                                            std::uint8_t* out,
                                            [[maybe_unused]]
                                            transpose16_mode mode) noexcept {
  namespace td = transpose_detail;
#if defined(BLAKE3PP_HAVE_SHUFFLE)
  if constexpr (shuffle_backend::supports<W>) {
    if (td::shuffle_store<shuffle_backend, W>(w, out, mode)) {
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

// Which SOURCE spelling reaches the one-instruction rotate is a property of
// the compiler, not of the hardware. Measured for rot(d ^ a, N), the shape
// g actually uses, at both 128- and 256-bit width:
//
//                       clang 22        GCC 14/16
//   generic shift-or    1x pshufb       pslld+psrld+por
//   byte shuffle        N=16: pshuflw+pshufhw    1x pshufb
//                       N=8:  1x pshufb          1x pshufb
//
// Mirror images: LLVM canonicalizes the shift-or rotate idiom straight to
// pshufb, but lowers OUR byte-shuffle to the 16-bit-lane pair for N==16
// (both are legal; its cost model dislikes materializing the mask, even
// though upstream's asm hoists exactly that mask out of the loop). GCC
// never forms a shuffle from shift-or and needs the byte spelling. Neither
// form is portable-optimal, so the choice is made here per compiler.
// Forcing it with _mm256_shuffle_epi8 does NOT work: InstCombine folds a
// constant-mask pshufb intrinsic back into a generic shuffle and lowers it
// the same way. The split is stable across the clang range this project
// builds with (18.1, 20.1 and 22 all lower both spellings identically),
// so the predicate is a compiler-family choice, not a version workaround.
//
// Worth +4.0% avx2 and +4.2% sse42 on clang 22 / Zen 3+ (256 MiB, best of
// 3, interleaved; the reference-kernel rows moved 0.8% over the same runs).
// Not because it shrinks the loop; it does not: 1489 -> 1486 instructions,
// because the freed pshuflw/pshufhw pair comes back as spills once the mask
// occupies a register all loop long. What shortens is g's SERIAL chain, two
// dependent shuffles down to one. Same lesson as the aarch64 sri escape:
// on this kernel the metric is critical-path length, not instruction count.
template <int N>
constexpr bool prefer_byte_rot() noexcept {
#if defined(__clang__) && \
    (defined(__x86_64__) || defined(__i386__) || defined(_M_X64))
  return N == 8;  // N == 16 is one instruction cheaper as generic shift-or
#else
  return N == 16 || N == 8;
#endif
}

// Compile-time-amount rotate for the wide word: a single byte shuffle for
// the 16- and 8-bit amounts where that is the better spelling (above), then
// the aarch64 shl+sri pair, then the generic shift-or. The scalar word
// overload lives in kernel.cpp.
// W is a defaulted template parameter (not read directly off u32v) so the
// discarded constexpr branches stay dependent; non-dependent constructs in
// a discarded branch are still instantiated.
template <int N, std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE u32v rot(u32v a) noexcept {
#if defined(BLAKE3PP_HAVE_SHUFFLE)
  if constexpr (prefer_byte_rot<N>() && shuffle_backend::supports_byte_rot<W>) {
    return shuffle_backend::rot_bytes<N / 8, W>(a);
  }
#endif
// Set by cmake/ArchKernels.cmake from -DBLAKE3PP_KERNEL_SRI_ROTATE=
// auto|on|off; the fallback repeats that default. Off restores the generic
// shift-or, which is how this escape gets re-measured on a core it has not
// been measured on. GCC 15 selects usra without it exactly as clang does,
// and whether sri wins outside Apple cores is still unverified.
#ifndef BLAKE3PP_KERNEL_SRI_ROTATE
#define BLAKE3PP_KERNEL_SRI_ROTATE 1
#endif

#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR) && \
    BLAKE3PP_KERNEL_SRI_ROTATE
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
