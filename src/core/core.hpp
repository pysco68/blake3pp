#pragma once

// Arch-agnostic BLAKE3 structure: chunk states, the chaining-value stack
// discipline, parent nodes, and root finalization. All compression is routed
// through a kernel_ops table so the same logic drives every architecture
// variant. Header-only and allocation-free; the parallel engine (M3) reuses
// these pieces for subtree hashing.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <blake3pp/core.hpp>

#include "kernel/kernel.hpp"

namespace blake3pp::core {

// A node whose chaining value has not been computed yet. Keeping the inputs
// around (rather than eagerly compressing) is what makes the ROOT flag
// possible: the last node's compression waits until it is known to be last.
struct output {
  std::array<std::uint32_t, 8> input_cv;
  std::array<std::uint8_t, kern::block_len> block;
  std::uint32_t block_len;
  std::uint64_t counter;
  std::uint32_t flags;
};

inline void chaining_value(const kern::kernel_ops& k, const output& o,
                           std::array<std::uint32_t, 8>& out_cv) noexcept {
  out_cv = o.input_cv;
  k.compress_in_place(out_cv.data(), o.block.data(), o.block_len, o.counter,
                      o.flags);
}

inline void chunk_init(detail::chunk_state& cs,
                       std::span<const std::uint32_t, 8> key,
                       std::uint64_t chunk_counter) noexcept {
  std::ranges::copy(key, cs.cv.begin());
  cs.chunk_counter = chunk_counter;
  cs.block.fill(0);
  cs.block_len = 0;
  cs.blocks_compressed = 0;
}

inline std::size_t chunk_len(const detail::chunk_state& cs) noexcept {
  return kern::block_len * cs.blocks_compressed + cs.block_len;
}

inline std::uint32_t chunk_start_flag(const detail::chunk_state& cs) noexcept {
  return cs.blocks_compressed == 0 ? kern::flag_chunk_start : 0;
}

// Feeds up to (chunk_len - len) bytes; caller ensures the chunk has room.
// base_flags carries the hashing mode (0, KEYED_HASH, DERIVE_KEY_*).
inline void chunk_update(const kern::kernel_ops& k, detail::chunk_state& cs,
                         const std::uint8_t* input, std::size_t len,
                         std::uint32_t base_flags) noexcept {
  while (len > 0) {
    // A full buffered block is only compressed once more input shows up: if
    // it turned out to be the chunk's last block it needs CHUNK_END later.
    if (cs.block_len == kern::block_len) {
      k.compress_in_place(cs.cv.data(), cs.block.data(), kern::block_len,
                          cs.chunk_counter, chunk_start_flag(cs) | base_flags);
      cs.blocks_compressed++;
      cs.block_len = 0;
      cs.block.fill(0);
    }
    const std::size_t take =
        len < kern::block_len - cs.block_len ? len
                                             : kern::block_len - cs.block_len;
    std::copy_n(input, take, cs.block.begin() + cs.block_len);
    cs.block_len = static_cast<std::uint8_t>(cs.block_len + take);
    input += take;
    len -= take;
  }
}

inline output chunk_output(const detail::chunk_state& cs,
                           std::uint32_t base_flags) noexcept {
  output o;
  o.input_cv = cs.cv;
  o.block = cs.block;
  o.block_len = cs.block_len;
  o.counter = cs.chunk_counter;
  o.flags = chunk_start_flag(cs) | kern::flag_chunk_end | base_flags;
  return o;
}

// Parent nodes always compress one full block (left CV || right CV) with
// counter 0 (spec section 2.4).
inline output parent_output(std::span<const std::uint32_t, 8> left_cv,
                            std::span<const std::uint32_t, 8> right_cv,
                            std::span<const std::uint32_t, 8> key,
                            std::uint32_t base_flags) noexcept {
  output o;
  std::ranges::copy(key, o.input_cv.begin());
  for (std::size_t w = 0; w < 8; ++w) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      o.block[4 * w + byte] =
          static_cast<std::uint8_t>(left_cv[w] >> (8 * byte));
      o.block[32 + 4 * w + byte] =
          static_cast<std::uint8_t>(right_cv[w] >> (8 * byte));
    }
  }
  o.block_len = kern::block_len;
  o.counter = 0;
  o.flags = kern::flag_parent | base_flags;
  return o;
}

}  // namespace blake3pp::core
