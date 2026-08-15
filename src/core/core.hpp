#pragma once

// Arch-agnostic BLAKE3 structure: chunk states, the chaining-value stack
// discipline, parent nodes, and root finalization. All compression is routed
// through a kernel_ops table so the same logic drives every architecture
// variant. Header-only and allocation-free; the parallel engine (M3) reuses
// these pieces for subtree hashing.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <blake3pp/core.hpp>

#include "kernel/kernel.hpp"

namespace blake3pp::core {

// A node whose chaining value has not been computed yet. Keeping the inputs
// around (rather than eagerly compressing) is what makes the ROOT flag
// possible: the last node's compression must wait until we know it is last.
struct output {
  std::uint32_t input_cv[8];
  std::uint8_t block[kern::block_len];
  std::uint32_t block_len;
  std::uint64_t counter;
  std::uint32_t flags;
};

inline void chaining_value(const kern::kernel_ops& k, const output& o,
                           std::uint32_t out_cv[8]) noexcept {
  std::memcpy(out_cv, o.input_cv, 8 * sizeof(std::uint32_t));
  k.compress_in_place(out_cv, o.block, o.block_len, o.counter, o.flags);
}

inline void chunk_init(detail::chunk_state& cs, const std::uint32_t key[8],
                       std::uint64_t chunk_counter) noexcept {
  std::memcpy(cs.cv, key, sizeof(cs.cv));
  cs.chunk_counter = chunk_counter;
  std::memset(cs.block, 0, sizeof(cs.block));
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
inline void chunk_update(const kern::kernel_ops& k, detail::chunk_state& cs,
                         const std::uint8_t* input, std::size_t len) noexcept {
  while (len > 0) {
    // A full buffered block is only compressed once more input shows up: if
    // it turned out to be the chunk's last block it needs CHUNK_END later.
    if (cs.block_len == kern::block_len) {
      k.compress_in_place(cs.cv, cs.block,
                          static_cast<std::uint32_t>(kern::block_len),
                          cs.chunk_counter, chunk_start_flag(cs));
      cs.blocks_compressed++;
      cs.block_len = 0;
      std::memset(cs.block, 0, sizeof(cs.block));
    }
    const std::size_t take =
        len < kern::block_len - cs.block_len ? len
                                             : kern::block_len - cs.block_len;
    std::memcpy(cs.block + cs.block_len, input, take);
    cs.block_len = static_cast<std::uint8_t>(cs.block_len + take);
    input += take;
    len -= take;
  }
}

inline output chunk_output(const detail::chunk_state& cs) noexcept {
  output o;
  std::memcpy(o.input_cv, cs.cv, sizeof(o.input_cv));
  std::memcpy(o.block, cs.block, sizeof(o.block));
  o.block_len = cs.block_len;
  o.counter = cs.chunk_counter;
  o.flags = chunk_start_flag(cs) | kern::flag_chunk_end;
  return o;
}

// Parent nodes always compress one full block (left CV || right CV) with
// counter 0 (spec section 2.4).
inline output parent_output(const std::uint32_t left_cv[8],
                            const std::uint32_t right_cv[8],
                            const std::uint32_t key[8]) noexcept {
  output o;
  std::memcpy(o.input_cv, key, 8 * sizeof(std::uint32_t));
  for (std::size_t w = 0; w < 8; ++w) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      o.block[4 * w + byte] =
          static_cast<std::uint8_t>(left_cv[w] >> (8 * byte));
      o.block[32 + 4 * w + byte] =
          static_cast<std::uint8_t>(right_cv[w] >> (8 * byte));
    }
  }
  o.block_len = static_cast<std::uint32_t>(kern::block_len);
  o.counter = 0;
  o.flags = kern::flag_parent;
  return o;
}

}  // namespace blake3pp::core
