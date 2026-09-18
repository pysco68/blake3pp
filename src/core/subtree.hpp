#pragma once

// Wide subtree compression: reduces a chunk-aligned power-of-2 subtree to a
// single chaining value with BOTH levels of work batched through hash_many:
// chunks across SIMD lanes, and parent nodes across SIMD lanes too. This is
// what closes the gap to hand-tuned implementations: with parents compressed
// one at a time (scalar), a binary tree spends ~1 scalar block per chunk on
// interior nodes, a measured ~1.5x drag at AVX2 chunk speeds.
//
// Everything here is allocation-free. The recursion holds one
// 4*max_simd_degree CV buffer per level (2 KiB while a 16-wide kernel is
// compiled in), sized for the widest variant compiled rather than the one
// that runs, and it stops at twice the RUNNING variant's degree, so a
// scalar kernel recurses deepest: a 2^54-chunk maximum-size subtree would
// take over 100 KiB of stack. The parallel engine's stack_budget bounds its
// own part table, not this. M3's parallel engine reuses these pieces per
// subtree.

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
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
    // kern::out_len is a compile-time constant; a counted loop keeps this
    // inline on MSVC, which turns the algorithm call into memcpy.
    const std::uint8_t* odd = child_cvs + (num_children - 1) * kern::out_len;
    std::uint8_t* dst = out + num_parents * kern::out_len;
    for (std::size_t i = 0; i < kern::out_len; ++i) {
      dst[i] = odd[i];
    }
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
  // A power of two splits into equal halves all the way down, so
  // compress_parents_wide never sees an odd child count and its output
  // fits the max_batch_inputs CVs the callers provide.
  assert(std::has_single_bit(num_chunks));
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
inline void compress_subtree_to_cv_recursive(
    const kern::kernel_ops& k, const std::uint8_t* input,
    std::size_t num_chunks, std::uint64_t chunk_counter,
    std::span<const std::uint32_t, 8> key, std::uint32_t base_flags,
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

// The same reduction, folding as it goes instead of holding the tree: one
// group of 2*simd_degree chunks at a time through hash_many, reduced to one
// CV by wide parent passes, then merged into a binary-counter stack (the
// shape hasher::push_cv uses, src/blake3pp.cpp). Parents stay batched across
// lanes inside a group; only the one parent that joins a group to the stack
// is compressed alone, once per 2*simd_degree chunks. The working set is two
// group buffers plus the stack, so it does not grow with the subtree's
// depth, where the recursion above costs one buffer per level.
//
// MaxStack bounds the stack in CVs and so the subtree this can reduce:
// log2(num_chunks / group) + 1 entries are needed, 54 covering the largest
// subtree BLAKE3 defines. It is also the working set, so a target picks the
// smallest bound its parts need.
//
// Measured against the recursion (tests/subtree_fold.cpp pins the outputs
// equal): the same 1023 parent blocks for a 1024-chunk subtree, but spread
// over 319 hash_many calls instead of 67 on avx2, none of them at full lane
// occupancy, which costs 11% on sse42 and avx2. On a scalar kernel there is
// no occupancy to lose and throughput is unchanged, which is why this is
// opt-in for narrow targets (BLAKE3PP_SUBTREE_FOLD) rather than a default:
// there it replaces one buffer per level with a fixed working set, 720 bytes
// at 12 levels against 480 + 272 per level.
template <std::size_t MaxStack = 54>
inline void compress_subtree_to_cv_folded(
    const kern::kernel_ops& k, const std::uint8_t* input,
    std::size_t num_chunks, std::uint64_t chunk_counter,
    std::span<const std::uint32_t, 8> key, std::uint32_t base_flags,
    std::span<std::uint32_t, 8> out_cv) noexcept {
  std::uint8_t cvs[kern::max_batch_inputs * kern::out_len];
  std::uint8_t next[kern::max_batch_inputs * kern::out_len];
  std::uint8_t stack[MaxStack * kern::out_len];
  std::size_t depth = 0;

  // Both are powers of two, so every group is a subtree-aligned unit and the
  // last group is full whenever the subtree spans more than one.
  const std::size_t group = 2 * k.simd_degree;
  std::uint64_t groups = 0;
  for (std::size_t done = 0; done < num_chunks; done += group) {
    const std::size_t n = std::min(group, num_chunks - done);
    const std::uint8_t* chunks[kern::max_batch_inputs];
    for (std::size_t i = 0; i < n; ++i) {
      chunks[i] = input + (done + i) * kern::chunk_len;
    }
    k.hash_many(chunks, n, kern::chunk_len / kern::block_len, key.data(),
                chunk_counter + done, /*increment_counter=*/true, base_flags,
                kern::flag_chunk_start, kern::flag_chunk_end, cvs);
    // The group's own parents, lanes-wide, down to its single root CV.
    for (std::size_t m = n; m > 1;) {
      m = compress_parents_wide(k, cvs, m, key, base_flags, next);
      std::copy_n(next, m * kern::out_len, cvs);
    }
    assert(depth < MaxStack);
    std::copy_n(cvs, kern::out_len, stack + depth * kern::out_len);
    depth++;
    // Two subtrees of equal size on top merge; as many times as the group
    // count has trailing zeros, which is the binary counter's carry.
    ++groups;
    for (int carries = std::countr_zero(groups); carries > 0; --carries) {
      assert(depth >= 2);
      depth -= 2;
      compress_parents_wide(k, stack + depth * kern::out_len, 2, key,
                            base_flags, next);
      std::copy_n(next, kern::out_len, stack + depth * kern::out_len);
      depth++;
    }
  }
  assert(depth == 1);
  for (std::size_t w = 0; w < 8; ++w) {
    const std::uint8_t* b = stack + 4 * w;
    out_cv[w] = static_cast<std::uint32_t>(b[0]) |
                (static_cast<std::uint32_t>(b[1]) << 8) |
                (static_cast<std::uint32_t>(b[2]) << 16) |
                (static_cast<std::uint32_t>(b[3]) << 24);
  }
}

// Which of the two the library uses: the recursion unless a build opts into
// the fold, which only narrow targets should. Both stay compiled so the
// tests can compare them on every machine.
inline void compress_subtree_to_cv(const kern::kernel_ops& k,
                                   const std::uint8_t* input,
                                   std::size_t num_chunks,
                                   std::uint64_t chunk_counter,
                                   std::span<const std::uint32_t, 8> key,
                                   std::uint32_t base_flags,
                                   std::span<std::uint32_t, 8> out_cv) noexcept {
  assert(num_chunks > 0 && std::has_single_bit(num_chunks));
#if defined(BLAKE3PP_SUBTREE_FOLD)
  // The macro's value is the stack bound in levels; see ArchKernels.cmake.
  compress_subtree_to_cv_folded<BLAKE3PP_SUBTREE_FOLD>(
      k, input, num_chunks, chunk_counter, key, base_flags, out_cv);
#else
  compress_subtree_to_cv_recursive(k, input, num_chunks, chunk_counter, key,
                                   base_flags, out_cv);
#endif
}

}  // namespace blake3pp::core
