#pragma once

// Wide subtree compression: reduces a chunk-aligned power-of-2 subtree to a
// single chaining value. Both levels of work go through hash_many, chunks
// across SIMD lanes and parent nodes across SIMD lanes too.
//
// Batching the parents is what closes the gap to hand-tuned
// implementations. Compressed one at a time, a binary tree spends about one
// scalar block per chunk on interior nodes, a measured 1.5x drag at AVX2
// chunk speeds.
//
// Everything here is allocation-free. The recursion holds one
// 4*max_simd_degree CV buffer per level (2 KiB while a 16-wide kernel is
// compiled in), sized for the widest variant compiled rather than the one
// that runs, and it stops at twice the RUNNING variant's degree, so a
// scalar kernel recurses deepest: a 2^54-chunk maximum-size subtree would
// take over 100 KiB of stack. The parallel engine's stack_budget bounds its
// own part table, not this; the engine reuses these pieces per subtree.

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include "core/core.hpp"
#include "kernel/kernel.hpp"
#include "kernel/uninitialized.hpp"

namespace blake3pp::core {

// The staging buffers below are sized by MaxDegree, the widest kernel the
// build may contain, and the fold level is a build choice too. Both come
// from the build system as macros, and both are template parameters here
// rather than constants read inside the function bodies. A template
// argument is part of the mangled name, so two translation units that
// disagree on either value instantiate two different functions instead of
// two definitions of one, which the linker would merge without a word.
#if defined(BLAKE3PP_SUBTREE_FOLD)
constexpr std::size_t subtree_fold_levels = BLAKE3PP_SUBTREE_FOLD;
#else
constexpr std::size_t subtree_fold_levels = 0;  // the grouped sweep
#endif

// How many chunk batches the grouped sweep hashes before it reduces their
// parents, in leaves of 2*simd_degree chunks. A kernel pays for alternating
// between the 16-block chunk shape and the 1-block parent shape -- measured
// on Zen 5, one parent call per leaf holds AVX-512 to 6.7 GiB/s where the
// chunk batch alone runs at 8.5, and bursting the parents over 8 leaves
// reaches 7.6, over 32 leaves 7.8. The staging buffers are 1.5 *
// Leaves * 2 * MaxDegree chaining values, 12 KiB at the default and a
// 16-lane build, which is why the count is a parameter: a small-device
// target lowers it, the recursion it replaces spent 2 KiB per level.
#if defined(BLAKE3PP_SUBTREE_GROUP)
constexpr std::size_t subtree_group_leaves = BLAKE3PP_SUBTREE_GROUP;
#else
constexpr std::size_t subtree_group_leaves = 8;
#endif
static_assert(std::has_single_bit(subtree_group_leaves),
              "BLAKE3PP_SUBTREE_GROUP must be a power of two: the binary "
              "counter merges groups of equal size");

// One generation of parents: pairs of child CVs (contiguous, LE bytes)
// become one-block parent nodes, hashed lanes-wide. An odd trailing child
// passes through unchanged (spec 2.4). Returns the new generation's count.
template <std::size_t MaxDegree = kern::max_simd_degree>
inline std::size_t compress_parents_wide(const kern::kernel_ops& k,
                                         const std::uint8_t* child_cvs,
                                         std::size_t num_children,
                                         std::span<const std::uint32_t, 8> key,
                                         std::uint32_t base_flags,
                                         std::uint8_t* out) noexcept {
  const std::size_t num_parents = num_children / 2;
  const std::uint8_t* parent_blocks BLAKE3PP_CXXATTR_UNINITIALIZED [2 * MaxDegree];
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
template <std::size_t MaxDegree = kern::max_simd_degree>
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
    const std::uint8_t* chunks BLAKE3PP_CXXATTR_UNINITIALIZED [2 * MaxDegree];
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
  std::uint8_t child_cvs BLAKE3PP_CXXATTR_UNINITIALIZED [4 * MaxDegree * kern::out_len];
  const std::size_t nl = compress_subtree_wide<MaxDegree>(
      k, input, half, chunk_counter, key, base_flags, child_cvs);
  const std::size_t nr = compress_subtree_wide<MaxDegree>(
      k, input + half * kern::chunk_len, half, chunk_counter + half, key,
      base_flags, child_cvs + nl * kern::out_len);
  return compress_parents_wide<MaxDegree>(k, child_cvs, nl + nr, key,
                                          base_flags, out_cvs);
}

// Parent generations over n CVs in cvs (LE bytes), lanes-wide, until at most
// stop_at remain; next is scratch for n/2 CVs. Returns the count.
//
// A generation wider than one call's pointer staging (4*MaxDegree children,
// which is 2*MaxDegree parents) is split across calls, so n is not bounded
// by the batch.
template <std::size_t MaxDegree = kern::max_simd_degree>
inline std::size_t reduce_generations(const kern::kernel_ops& k,
                                      std::uint8_t* cvs, std::uint8_t* next,
                                      std::size_t n, std::size_t stop_at,
                                      std::span<const std::uint32_t, 8> key,
                                      std::uint32_t base_flags) noexcept {
  constexpr std::size_t batch_children = 4 * MaxDegree;
  while (n > stop_at) {
    std::size_t written = 0;
    for (std::size_t at = 0; at < n; at += batch_children) {
      const std::size_t take = std::min(batch_children, n - at);
      written += compress_parents_wide<MaxDegree>(
          k, cvs + at * kern::out_len, take, key, base_flags,
          next + written * kern::out_len);
    }
    std::copy_n(next, written * kern::out_len, cvs);
    n = written;
  }
  return n;
}

// The reduction of a power-of-2 subtree stopped at its last wide level:
// the chaining values are reduced only while a generation still fills the
// lanes, or to `stop_at` CVs when that is fewer, and what is left,
// min(num_chunks, stop_at) CVs of equal-sized adjacent subtrees, goes to
// the caller. With stop_at 1 this is the full reduction; the generations
// it skips at the default run below full occupancy, and folded across
// several parts they run full. Returns how many CVs were written.
template <std::size_t MaxDegree = kern::max_simd_degree>
inline std::size_t compress_subtree_to_cvs(
    const kern::kernel_ops& k, const std::uint8_t* input,
    std::size_t num_chunks, std::uint64_t chunk_counter,
    std::span<const std::uint32_t, 8> key, std::uint32_t base_flags,
    std::span<std::array<std::uint32_t, 8>> out_cvs,
    std::size_t stop_at) noexcept {
  assert(num_chunks > 0 && std::has_single_bit(num_chunks));
  std::uint8_t cvs BLAKE3PP_CXXATTR_UNINITIALIZED [2 * MaxDegree * kern::out_len];
  std::uint8_t next BLAKE3PP_CXXATTR_UNINITIALIZED [2 * MaxDegree * kern::out_len];
  std::size_t n = compress_subtree_wide<MaxDegree>(
      k, input, num_chunks, chunk_counter, key, base_flags, cvs);
  n = reduce_generations<MaxDegree>(k, cvs, next, n, stop_at, key, base_flags);
  assert(n <= out_cvs.size());
  for (std::size_t i = 0; i < n; ++i) {
    load_cv_le(cvs + i * kern::out_len, out_cvs[i]);
  }
  return n;
}

template <std::size_t MaxDegree = kern::max_simd_degree>
inline std::size_t compress_subtree_to_cvs(
    const kern::kernel_ops& k, const std::uint8_t* input,
    std::size_t num_chunks, std::uint64_t chunk_counter,
    std::span<const std::uint32_t, 8> key, std::uint32_t base_flags,
    std::span<std::array<std::uint32_t, 8>> out_cvs) noexcept {
  return compress_subtree_to_cvs<MaxDegree>(k, input, num_chunks,
                                            chunk_counter, key, base_flags,
                                            out_cvs, k.simd_degree);
}

// Full reduction of a power-of-2 subtree (>= 2 chunks) to one CV. The final
// log2(degree) generations run below full lane occupancy, but that tail is
// O(log degree) blocks per subtree and amortizes to noise.
template <std::size_t MaxDegree = kern::max_simd_degree>
inline void compress_subtree_to_cv_recursive(
    const kern::kernel_ops& k, const std::uint8_t* input,
    std::size_t num_chunks, std::uint64_t chunk_counter,
    std::span<const std::uint32_t, 8> key, std::uint32_t base_flags,
    std::span<std::uint32_t, 8> out_cv) noexcept {
  std::array<std::uint32_t, 8> cv[1]{};
  compress_subtree_to_cvs<MaxDegree>(k, input, num_chunks, chunk_counter, key,
                                     base_flags, cv, /*stop_at=*/1);
  copy_cv(cv[0], out_cv);
}

// The same reduction, folding as it goes instead of holding the tree. One
// group of 2*simd_degree chunks goes through hash_many at a time, is reduced
// to one CV by wide parent passes, then merges into a binary-counter stack
// (the shape hasher::push_cv uses, src/blake3pp.cpp).
//
// Parents stay batched across lanes inside a group. Only the one parent that
// joins a group to the stack is compressed alone, once per 2*simd_degree
// chunks. The working set is two group buffers plus the stack, so it does
// not grow with the subtree's depth, where the recursion above costs one
// buffer per level.
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
// opt-in for narrow targets through BLAKE3PP_SUBTREE_FOLD. There it
// replaces one buffer per level with a fixed working set, 720 bytes at 12
// levels against 480 + 272 per level.
template <std::size_t MaxStack = 54, std::size_t MaxDegree = kern::max_simd_degree>
inline void compress_subtree_to_cv_folded(
    const kern::kernel_ops& k, const std::uint8_t* input,
    std::size_t num_chunks, std::uint64_t chunk_counter,
    std::span<const std::uint32_t, 8> key, std::uint32_t base_flags,
    std::span<std::uint32_t, 8> out_cv) noexcept {
  std::uint8_t cvs BLAKE3PP_CXXATTR_UNINITIALIZED [2 * MaxDegree * kern::out_len];
  std::uint8_t next BLAKE3PP_CXXATTR_UNINITIALIZED [2 * MaxDegree * kern::out_len];
  std::uint8_t stack BLAKE3PP_CXXATTR_UNINITIALIZED [MaxStack * kern::out_len];
  std::size_t depth = 0;

  // Both are powers of two, so every group is a subtree-aligned unit and the
  // last group is full whenever the subtree spans more than one.
  const std::size_t group = 2 * k.simd_degree;
  std::uint64_t groups = 0;
  for (std::size_t done = 0; done < num_chunks; done += group) {
    const std::size_t n = std::min(group, num_chunks - done);
    const std::uint8_t* chunks BLAKE3PP_CXXATTR_UNINITIALIZED [2 * MaxDegree];
    for (std::size_t i = 0; i < n; ++i) {
      chunks[i] = input + (done + i) * kern::chunk_len;
    }
    k.hash_many(chunks, n, kern::chunk_len / kern::block_len, key.data(),
                chunk_counter + done, /*increment_counter=*/true, base_flags,
                kern::flag_chunk_start, kern::flag_chunk_end, cvs);
    // The group's own parents, lanes-wide, down to its single root CV.
    reduce_generations<MaxDegree>(k, cvs, next, n, 1, key, base_flags);
    assert(depth < MaxStack);
    std::copy_n(cvs, kern::out_len, stack + depth * kern::out_len);
    depth++;
    // Two subtrees of equal size on top merge; as many times as the group
    // count has trailing zeros, which is the binary counter's carry.
    ++groups;
    for (int carries = std::countr_zero(groups); carries > 0; --carries) {
      assert(depth >= 2);
      depth -= 2;
      compress_parents_wide<MaxDegree>(k, stack + depth * kern::out_len, 2,
                                       key, base_flags, next);
      std::copy_n(next, kern::out_len, stack + depth * kern::out_len);
      depth++;
    }
  }
  assert(depth == 1);
  load_cv_le(stack, out_cv);
}

// The sweep: Leaves chunk batches go through hash_many back to back, then
// that whole group's parent generations run in one burst, and the group's
// single CV merges into a binary-counter stack (the shape hasher::push_cv
// uses). This is the library's default path.
//
// It exists because of what the burst is worth, not because of the tree
// shape, which is the recursion's. A kernel body at full width is large
// enough that the 16-block chunk loop and the 1-block parent call do not
// stay hot together, so compressing a node's parents as soon as its children
// exist -- what the recursion does, once per two leaves -- pays for the
// switch every time. Measured on Zen 5 with the parent work unchanged and
// only its grouping varied: avx512 6.7 GiB/s at one leaf per burst, 7.6 at
// eight, 7.8 at thirty-two, against 8.5 for the chunk batches alone; avx2
// 3.1, 3.5, 3.8 against 4.0.
//
// The working set is two staging buffers plus the stack and does not grow
// with the subtree's depth, where the recursion holds one buffer per level.
// MaxStack bounds the stack in CVs and so the subtree this can reduce:
// log2(num_chunks / group) + 1 entries, 54 covering the largest subtree
// BLAKE3 defines.
template <std::size_t Leaves = subtree_group_leaves, std::size_t MaxStack = 54,
          std::size_t MaxDegree = kern::max_simd_degree>
inline void compress_subtree_to_cv_grouped(
    const kern::kernel_ops& k, const std::uint8_t* input,
    std::size_t num_chunks, std::uint64_t chunk_counter,
    std::span<const std::uint32_t, 8> key, std::uint32_t base_flags,
    std::span<std::uint32_t, 8> out_cv) noexcept {
  assert(num_chunks > 0 && std::has_single_bit(num_chunks));
  constexpr std::size_t max_group = Leaves * 2 * MaxDegree;
  std::uint8_t cvs BLAKE3PP_CXXATTR_UNINITIALIZED [max_group * kern::out_len];
  std::uint8_t next BLAKE3PP_CXXATTR_UNINITIALIZED [max_group / 2 * kern::out_len];
  std::uint8_t stack BLAKE3PP_CXXATTR_UNINITIALIZED [MaxStack * kern::out_len];
  std::size_t depth = 0;

  // Both are powers of two, so every group but a lone short one is full and
  // the binary counter below only ever merges equal sizes.
  const std::size_t leaf = 2 * k.simd_degree;
  const std::size_t group = Leaves * leaf;
  std::uint64_t groups = 0;
  for (std::size_t done = 0; done < num_chunks; done += group) {
    const std::size_t n = std::min(group, num_chunks - done);
    for (std::size_t at = 0; at < n; at += leaf) {
      const std::size_t take = std::min(leaf, n - at);
      const std::uint8_t* chunks BLAKE3PP_CXXATTR_UNINITIALIZED [2 * MaxDegree];
      for (std::size_t i = 0; i < take; ++i) {
        chunks[i] = input + (done + at + i) * kern::chunk_len;
      }
      k.hash_many(chunks, take, kern::chunk_len / kern::block_len, key.data(),
                  chunk_counter + done + at, /*increment_counter=*/true,
                  base_flags, kern::flag_chunk_start, kern::flag_chunk_end,
                  cvs + at * kern::out_len);
    }
    // The group's own parents, lanes-wide, down to its single root CV: the
    // burst this shape exists for.
    reduce_generations<MaxDegree>(k, cvs, next, n, 1, key, base_flags);
    assert(depth < MaxStack);
    std::copy_n(cvs, kern::out_len, stack + depth * kern::out_len);
    depth++;
    // Two subtrees of equal size on top merge; as many times as the group
    // count has trailing zeros, which is the binary counter's carry.
    ++groups;
    for (int carries = std::countr_zero(groups); carries > 0; --carries) {
      assert(depth >= 2);
      depth -= 2;
      compress_parents_wide<MaxDegree>(k, stack + depth * kern::out_len, 2, key,
                                       base_flags, next);
      std::copy_n(next, kern::out_len, stack + depth * kern::out_len);
      depth++;
    }
  }
  assert(depth == 1);
  load_cv_le(stack, out_cv);
}

// Which of the three the library uses: the grouped sweep, unless a build
// opts into the single-leaf fold, which only a target that cannot spare the
// sweep's staging buffers should (Fold is then the stack bound in levels;
// see ArchKernels.cmake). All three stay compiled so the tests can compare
// them on every machine.
template <std::size_t Fold = subtree_fold_levels,
          std::size_t MaxDegree = kern::max_simd_degree>
inline void compress_subtree_to_cv(const kern::kernel_ops& k,
                                   const std::uint8_t* input,
                                   std::size_t num_chunks,
                                   std::uint64_t chunk_counter,
                                   std::span<const std::uint32_t, 8> key,
                                   std::uint32_t base_flags,
                                   std::span<std::uint32_t, 8> out_cv) noexcept {
  assert(num_chunks > 0 && std::has_single_bit(num_chunks));
  if constexpr (Fold > 0) {
    compress_subtree_to_cv_folded<Fold, MaxDegree>(
        k, input, num_chunks, chunk_counter, key, base_flags, out_cv);
  } else {
    compress_subtree_to_cv_grouped<subtree_group_leaves, 54, MaxDegree>(
        k, input, num_chunks, chunk_counter, key, base_flags, out_cv);
  }
}

// An arbitrary run of complete chunks, reduced to the subtree CVs the spec's
// tree has at that position and handed to sink left to right as (cv,
// subtree_chunks) pairs, ready for a binary-counter stack. The caller decides
// only how many chunks it may give away, which at a message's end means
// holding back the chunk that could carry ROOT; which batches that run splits
// into is decided here.
//
// Two shapes, chosen per step. While a position-aligned power-of-2 subtree of
// at least 2*simd_degree chunks is available, that subtree is reduced whole,
// so its parent generations stay batched across lanes. Otherwise up to
// 2*simd_degree chunks go through one hash_many and are emitted as one CV
// each, which costs the caller one parent block per chunk (the drag noted at
// the top of this file) but replaces a chunk-at-a-time serial compression
// with a full-width batch, and is bounded to that many chunks. Such a run is
// trimmed to the next 2*simd_degree boundary while chunks follow it, so a
// misaligned start costs one batch instead of a ladder of narrow subtrees.
//
// max_subtree caps a single emission in chunks, rounded down to a power of
// two, which a caller that needs more than one emission uses: a complete
// message's root is the parent of its last subtree and that subtree's left
// sibling, so the last emission must not be the whole run.
template <class Sink, std::size_t MaxDegree = kern::max_simd_degree>
inline void compress_chunks(const kern::kernel_ops& k,
                            const std::uint8_t* input, std::size_t num_chunks,
                            std::uint64_t chunk_counter,
                            std::span<const std::uint32_t, 8> key,
                            std::uint32_t base_flags, Sink&& sink,
                            std::size_t max_subtree =
                                std::numeric_limits<std::size_t>::max()) noexcept {
  assert(max_subtree > 0);
  const std::size_t wide = 2 * k.simd_degree;
  const std::size_t cap = std::bit_floor(max_subtree);
  while (num_chunks > 0) {
    if (num_chunks >= wide) {
      std::size_t subtree = std::min(std::bit_floor(num_chunks), cap);
      // A subtree merged as one CV must sit on a subtree-aligned position
      // in the overall tree.
      while (((subtree - 1) & chunk_counter) != 0) {
        subtree /= 2;
      }
      if (subtree >= wide) {
        std::array<std::uint32_t, 8> cv;
        compress_subtree_to_cv<subtree_fold_levels, MaxDegree>(
            k, input, subtree, chunk_counter, key, base_flags, cv);
        sink(std::span<const std::uint32_t, 8>{cv},
             static_cast<std::uint64_t>(subtree));
        input += subtree * kern::chunk_len;
        chunk_counter += subtree;
        num_chunks -= subtree;
        continue;
      }
    }

    std::size_t take = std::min(num_chunks, wide);
    if (num_chunks > take) {
      take = std::min(
          take, wide - static_cast<std::size_t>(chunk_counter % wide));
    }
    const std::uint8_t* chunks BLAKE3PP_CXXATTR_UNINITIALIZED [2 * MaxDegree];
    for (std::size_t i = 0; i < take; ++i) {
      chunks[i] = input + i * kern::chunk_len;
    }
    std::uint8_t cvs BLAKE3PP_CXXATTR_UNINITIALIZED [2 * MaxDegree * kern::out_len];
    k.hash_many(chunks, take, kern::chunk_len / kern::block_len, key.data(),
                chunk_counter, /*increment_counter=*/true, base_flags,
                kern::flag_chunk_start, kern::flag_chunk_end, cvs);
    // One chaining value per chunk, which leaves the caller one scalar
    // parent block per chunk: six for an 8 KiB message, around a sixth
    // of its time. Compressing those parents here as generations, one
    // hash_many call each, is slower as soon as the input is larger than
    // the cache. An 8 KiB message's generations are 4 and 2 lanes wide,
    // which at 16 lanes is one padded batch and two scalar blocks, and
    // 8 KiB messages over 512 MiB go 4.66 -> 3.68 GiB/s on Zen 5 at
    // avx512 while the same binary measures unchanged over 2 MiB. The
    // bookkeeping is not what costs: compressing those generations
    // scalar instead measures as this does. What a parent batch could
    // win is bounded anyway, since one 16-lane one-block batch is 532
    // cycles against a 285-cycle port bound (llvm-mca, znver5), a
    // dependency chain one batch cannot fill.
    for (std::size_t i = 0; i < take; ++i) {
      std::array<std::uint32_t, 8> cv;
      load_cv_le(cvs + i * kern::out_len, cv);
      sink(std::span<const std::uint32_t, 8>{cv}, std::uint64_t{1});
    }
    input += take * kern::chunk_len;
    chunk_counter += take;
    num_chunks -= take;
  }
}

// Reduces sibling CVs of equal-sized adjacent subtrees, left to right, to
// the CV of their common ancestor: one generation at a time, lanes-wide.
// cvs is the working array and is clobbered, each generation writing its
// parents to the front of it.
//
// The batch is 2 * MaxDegree parents per hash_many call, which is
// kern::max_batch_inputs for a build's widest kernel and exactly what
// compress_parents_wide stages pointers for. hash_many itself takes any
// number of inputs, hashing full lanes and then the remainder one at a
// time, so the pointer array is the only ceiling. The two staging buffers
// are that batch's children and parents, 3 KiB where 16 lanes are
// compiled in, and neither grows with cvs.size().
//
// The number of CVs is a power of two, so no generation has an odd child
// to pass through, and the caller's tree stays the one the hasher would
// have built out of the same subtrees.
template <std::size_t MaxDegree = kern::max_simd_degree>
inline void fold_sibling_cvs_wide(const kern::kernel_ops& k,
                                  std::span<std::array<std::uint32_t, 8>> cvs,
                                  std::span<const std::uint32_t, 8> key,
                                  std::uint32_t base_flags,
                                  std::span<std::uint32_t, 8> out_cv) noexcept {
  assert(cvs.size() >= 2 && std::has_single_bit(cvs.size()));
  constexpr std::size_t batch_parents = 2 * MaxDegree;
  std::uint8_t children BLAKE3PP_CXXATTR_UNINITIALIZED [2 * batch_parents * kern::out_len];
  std::uint8_t parents BLAKE3PP_CXXATTR_UNINITIALIZED [batch_parents * kern::out_len];

  for (std::size_t n = cvs.size(); n > 1;) {
    std::size_t written = 0;
    for (std::size_t i = 0; i < n; i += 2 * batch_parents) {
      const std::size_t take = std::min(2 * batch_parents, n - i);
      // The whole batch is serialised before any of its parents is
      // written back, and a parent's index is never past the children it
      // came from, so the generation reduces cvs in place.
      for (std::size_t j = 0; j < take; ++j) {
        store_cv_le(cvs[i + j], children + j * kern::out_len);
      }
      compress_parents_wide<MaxDegree>(k, children, take, key, base_flags,
                                       parents);
      for (std::size_t j = 0; j < take / 2; ++j) {
        load_cv_le(parents + j * kern::out_len, cvs[written + j]);
      }
      written += take / 2;
    }
    n = written;
  }
  for (std::size_t w = 0; w < 8; ++w) {
    out_cv[w] = cvs[0][w];
  }
}

}  // namespace blake3pp::core
