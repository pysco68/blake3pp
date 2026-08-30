#pragma once

// The message transpose. (The wide word's rotate and its per-ISA escape
// hatches live in rotate.hpp.)
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

#include <cstddef>
#include <cstdint>
#include <cstring>

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

}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
