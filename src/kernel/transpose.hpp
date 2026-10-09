#pragma once

// The message transpose. (The wide word's rotate and its per-ISA escape
// hatches live in rotate.hpp.)
//
// hash_batch needs the 16 message words of a block gathered ACROSS lanes,
// meaning word j of every input in one vector. No simd provider exposes a
// portable permute for that, and the std::simd MVP has no shuffle API at
// all.
//
// So the naive route stages through a scalar array, and that costs about
// 40% of the whole hash: with it, upstream's SSE4.1 assembly matches an
// AVX2 kernel.
//
// The bypass is a radix-2 shuffle tree whose index patterns ARE hardware
// macro-ops; it lives in shuffle/networks.hpp, written once over whichever
// backend shuffle.hpp selected for this TU. What remains here is the part
// that is genuinely about transposing a message: which registers to load
// from where, and where their transposed results go. Widths other than
// 4/8/16, and TUs with no shuffle backend at all, fall back to the scalar
// staging gather at the bottom of each function.
//
// At width 16 the register tree is not the fastest form. The quartered one
// is: 128-bit pieces land block-transposed by ADDRESSING, through one
// insert-load each, and the registers run only the two in-lane stages. It
// wins on both full-width and double-pumped AVX-512 datapaths, from an
// input that fits in cache to one streaming from DRAM, which is why the
// width-16 path has no alternative to choose between.

#include <cstddef>
#include <cstdint>

#include "kernel/endian.hpp"
#include "kernel/force_inline.hpp"
#include "kernel/shuffle.hpp"
#include "kernel/simd_facade.hpp"

#ifndef BLAKE3PP_ARCH_NS
#error "transpose.hpp is kernel-TU-internal; compile with -DBLAKE3PP_ARCH_NS=<variant>"
#endif

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
namespace {
namespace transpose_detail {

#if defined(BLAKE3PP_HAVE_SHUFFLE)

// Gathers the block's 16 message words across W lanes through the shuffle
// networks.
template <class B, std::size_t W>
BLAKE3PP_FORCE_INLINE void shuffle_load(const std::uint8_t* const* inputs,
                                        std::size_t offset,
                                        u32v m[16]) noexcept {
  using V = typename B::template reg<W>;
  if constexpr (W == 16) {
    // Quartered: 128-bit pieces land block-transposed by ADDRESSING;
    // registers only run the two in-lane stages.
    for (std::size_t q = 0; q < 4; ++q) {
      V r[4];
      for (std::size_t k = 0; k < 4; ++k) {
        const std::uint8_t* const piece[4] = {
            inputs[k] + offset + 16 * q, inputs[4 + k] + offset + 16 * q,
            inputs[8 + k] + offset + 16 * q,
            inputs[12 + k] + offset + 16 * q};
        r[k] = B::template load_quad<W>(piece);
      }
      V t[4];
      shuffle_detail::inlane_4x4<B>(r, t);
      for (std::size_t j = 0; j < 4; ++j) {
        m[4 * q + j] = B::template to_word<W>(t[j]);
      }
    }
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
  }
}

// The mirror image: 16 wide words out to W lane-major 64-byte blocks.
template <class B, std::size_t W>
BLAKE3PP_FORCE_INLINE void shuffle_store(const u32v (&w)[16],
                                         std::uint8_t* out) noexcept {
  using V = typename B::template reg<W>;
  if constexpr (W == 16) {
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
        std::uint8_t* const piece[4] = {
            out + k * 64 + 16 * q, out + (4 + k) * 64 + 16 * q,
            out + (8 + k) * 64 + 16 * q, out + (12 + k) * 64 + 16 * q};
        B::template store_quad<W>(piece, t[k]);
      }
    }
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
  }
}

#endif  // BLAKE3PP_HAVE_SHUFFLE

}  // namespace transpose_detail

// Fills m[0..15] with the block's message words transposed across W lanes:
// m[j][lane] = word j of inputs[lane] at byte offset `offset`.
template <std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE void load_transposed(const std::uint8_t* const* inputs,
                                           std::size_t offset,
                                           u32v m[16]) noexcept {
#if defined(BLAKE3PP_HAVE_SHUFFLE)
  if constexpr (shuffle_backend::supports<W>) {
    transpose_detail::shuffle_load<shuffle_backend, W>(inputs, offset, m);
    return;
  }
#endif
  std::uint32_t lanes[W];
  for (std::size_t j = 0; j < 16; ++j) {
    for (std::size_t lane = 0; lane < W; ++lane) {
      lanes[lane] = endian::load32(inputs[lane] + offset + 4 * j);
    }
    m[j] = u32v::load(lanes);
  }
}

// Writes 16 wide words lane-major: lane l receives words w[0..15][l] as
// 64 little-endian bytes at out + l*64, the mirror of load_transposed.
template <std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE void store_transposed(const u32v (&w)[16],
                                            std::uint8_t* out) noexcept {
#if defined(BLAKE3PP_HAVE_SHUFFLE)
  if constexpr (shuffle_backend::supports<W>) {
    transpose_detail::shuffle_store<shuffle_backend, W>(w, out);
    return;
  }
#endif
  std::uint32_t lanes[W];
  for (std::size_t j = 0; j < 16; ++j) {
    w[j].store(lanes);
    for (std::size_t lane = 0; lane < W; ++lane) {
      endian::store32(out + lane * 64 + 4 * j, lanes[lane]);
    }
  }
}

}  // namespace
}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
