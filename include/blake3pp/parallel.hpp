#pragma once

// Parallel BLAKE3 over the sender/receiver model.
//
// BLAKE3's binary Merkle tree makes the parallel decomposition exact, not
// heuristic: any power-of-2, position-aligned run of chunks reduces to one
// chaining value independently of everything else. So the engine partitions
// the input into equal such subtrees, bulk-schedules the (allocation-free)
// subtree reductions across the scheduler's execution agents, then absorbs
// the CVs in order through the hasher's CV-stack discipline and finishes
// the tail sequentially. The merge work after the parallel phase is
// O(parts) scalar compressions, which is noise.
//
// std::execution where the standard library ships it, NVIDIA stdexec
// otherwise (same source, same story as the simd providers). No heap
// allocations in this header: the CV table lives on the caller's stack and
// sender operation states live inside sync_wait's frame.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include <blake3pp/blake3pp.hpp>

#if defined(BLAKE3PP_HAS_STD_SENDERS)
#include <execution>
#else
#include <stdexec/execution.hpp>
#endif

namespace blake3pp {

namespace ex {
#if defined(BLAKE3PP_HAS_STD_SENDERS)
using namespace std::execution;
using std::this_thread::sync_wait;
#else
using namespace stdexec;
#endif
}  // namespace ex

// Expert overload: as below, but on a caller-supplied kernel table (the
// same seam hasher's expert constructor exposes).
template <class Scheduler>
[[nodiscard]] digest hash(std::span<const std::byte> input, Scheduler&& sched,
                          const kern::kernel_ops* ops) {
  // Only chunks with at least one byte after them may be offloaded: the
  // message's final chunk must stay with the hasher for ROOT finalization.
  const std::size_t safe_chunks =
      input.size() > chunk_size ? (input.size() - 1) / chunk_size : 0;

  constexpr std::size_t max_parts = 256;
  constexpr std::size_t min_part_chunks = 16;  // 16 KiB per task minimum

  if (safe_chunks >= 2 * min_part_chunks) {
    const std::size_t want = (safe_chunks + max_parts - 1) / max_parts;
    const std::size_t part = std::bit_floor(std::max(want, min_part_chunks));
    const std::size_t n_parts = safe_chunks / part;  // < 2 * max_parts

    // Starting from counter 0 in part-sized steps, every part is
    // automatically subtree-aligned.
    std::uint32_t cvs[2 * max_parts][8];
    const std::byte* const base = input.data();

    auto work = ex::schedule(sched) |
                ex::bulk(ex::par, n_parts, [&](std::size_t i) noexcept {
                  detail::compress_subtree_cv(
                      ops, base + i * part * chunk_size, part,
                      static_cast<std::uint64_t>(i) * part, cvs[i]);
                });
    ex::sync_wait(std::move(work));

    hasher h{ops};
    for (std::size_t i = 0; i < n_parts; ++i) {
      h.push_subtree_cv(cvs[i], part);
    }
    h.update(input.subspan(n_parts * part * chunk_size));
    return h.finalize();
  }

  hasher h{ops};
  h.update(input);
  return h.finalize();
}

// Hashes input, scheduling subtree reductions onto sched. Any
// std::execution-style scheduler works; small inputs (where parallelism
// cannot pay for itself) fall back to the sequential path.
template <class Scheduler>
[[nodiscard]] digest hash(std::span<const std::byte> input, Scheduler&& sched,
                          arch a = arch::auto_detect) {
  return hash(input, std::forward<Scheduler>(sched), detail::resolve(a));
}

}  // namespace blake3pp
