#pragma once

// Wide subtree compression: reduces a chunk-aligned power-of-2 subtree to a
// single chaining value with BOTH levels of work batched through hash_many:
// chunks across SIMD lanes, and parent nodes across SIMD lanes too. This is
// what closes the gap to hand-tuned implementations: with parents compressed
// one at a time (scalar), a binary tree spends ~1 scalar block per chunk on
// interior nodes, a measured ~1.5x drag at AVX2 chunk speeds.
//
// Everything here is allocation-free. The recursion holds one 2*max_degree
// CV buffer per level (1 KiB); depth is log2(subtree chunks), so even a
// 2^54-chunk maximum-size subtree stays under ~64 KiB of stack. M3's
// parallel engine reuses these pieces per subtree.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kernel/kernel.hpp"

namespace blake3pp::core {

// One generation of parents: pairs of child CVs (contiguous, LE bytes)
// become one-block parent nodes, hashed lanes-wide. An odd trailing child
// passes through unchanged (spec 2.4). Returns the new generation's count.
inline std::size_t compress_parents_wide(const kern::kernel_ops& k,
                                         const std::uint8_t* child_cvs,
                                         std::size_t num_children,
                                         std::span<const std::uint32_t, 8> key,
                                         std::uint32_t base_flags,
                                         std::uint8_t* out) noexcept {
  const std::size_t num_parents = num_children / 2;
  const std::uint8_t* parent_blocks[kern::max_batch_inputs];
  for (std::size_t i = 0; i < num_parents; ++i) {
    parent_blocks[i] = child_cvs + 2 * i * kern::out_len;
  }
  k.hash_many(parent_blocks, num_parents, 1, key.data(), 0,
              /*increment_counter=*/false, kern::flag_parent | base_flags, 0,
              0, out);
  if (num_children % 2 != 0) {
    std::copy_n(child_cvs + (num_children - 1) * kern::out_len, kern::out_len,
                out + num_parents * kern::out_len);
    return num_parents + 1;
  }
  return num_parents;
}

// num_chunks is a power of two; input holds num_chunks complete chunks.
// Returns min(num_chunks, 2 * simd_degree) CVs in out_cvs. The leaf spans
// TWO SIMD batches so hash_many can run its dual-batch interleaved path.
inline std::size_t compress_subtree_wide(const kern::kernel_ops& k,
                                         const std::uint8_t* input,
                                         std::size_t num_chunks,
                                         std::uint64_t chunk_counter,
                                         std::span<const std::uint32_t, 8> key,
                                         std::uint32_t base_flags,
                                         std::uint8_t* out_cvs) noexcept {
  if (num_chunks <= 2 * k.simd_degree) {
    const std::uint8_t* chunks[kern::max_batch_inputs];
    for (std::size_t i = 0; i < num_chunks; ++i) {
      chunks[i] = input + i * kern::chunk_len;
    }
    k.hash_many(chunks, num_chunks, kern::chunk_len / kern::block_len,
                key.data(),
                chunk_counter, /*increment_counter=*/true, base_flags,
                kern::flag_chunk_start, kern::flag_chunk_end, out_cvs);
    return num_chunks;
  }

  const std::size_t half = num_chunks / 2;
  std::uint8_t child_cvs[2 * kern::max_batch_inputs * kern::out_len];
  const std::size_t nl = compress_subtree_wide(k, input, half, chunk_counter,
                                               key, base_flags, child_cvs);
  const std::size_t nr = compress_subtree_wide(
      k, input + half * kern::chunk_len, half, chunk_counter + half, key,
      base_flags, child_cvs + nl * kern::out_len);
  return compress_parents_wide(k, child_cvs, nl + nr, key, base_flags,
                               out_cvs);
}

// Full reduction of a power-of-2 subtree (>= 2 chunks) to one CV. The final
// log2(degree) generations run below full lane occupancy, but that tail is
// O(log degree) blocks per subtree and amortizes to noise.
inline void compress_subtree_to_cv(const kern::kernel_ops& k,
                                   const std::uint8_t* input,
                                   std::size_t num_chunks,
                                   std::uint64_t chunk_counter,
                                   std::span<const std::uint32_t, 8> key,
                                   std::uint32_t base_flags,
                                   std::span<std::uint32_t, 8> out_cv) noexcept {
  std::uint8_t cvs[kern::max_batch_inputs * kern::out_len];
  std::uint8_t next[kern::max_batch_inputs * kern::out_len];
  std::size_t n = compress_subtree_wide(k, input, num_chunks, chunk_counter,
                                        key, base_flags, cvs);
  while (n > 1) {
    n = compress_parents_wide(k, cvs, n, key, base_flags, next);
    std::copy_n(next, n * kern::out_len, cvs);
  }
  for (std::size_t w = 0; w < 8; ++w) {
    const std::uint8_t* b = cvs + 4 * w;
    out_cv[w] = static_cast<std::uint32_t>(b[0]) |
                (static_cast<std::uint32_t>(b[1]) << 8) |
                (static_cast<std::uint32_t>(b[2]) << 16) |
                (static_cast<std::uint32_t>(b[3]) << 24);
  }
}

}  // namespace blake3pp::core
