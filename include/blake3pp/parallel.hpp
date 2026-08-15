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
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <blake3pp/core.hpp>

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

    // One result slot per worker, each padded to its own cache line:
    // adjacent workers complete at unrelated times, and a bare 32-byte CV
    // array would put two workers' completion writes in the same line:
    // textbook false sharing on the only memory the workers share.
    // (std::hardware_destructive_interference_size is the standard name
    // for this boundary; we pin its value, 64 on every target we build,
    // because GCC warns on ABI-sensitive uses of the constant in headers.)
    struct alignas(64) padded_cv {
      std::uint32_t words[8];
    };
    // Starting from counter 0 in part-sized steps, every part is
    // automatically subtree-aligned.
    padded_cv cvs[2 * max_parts];
    const std::byte* const base = input.data();

    auto work = ex::schedule(sched) |
                ex::bulk(ex::par, n_parts, [&](std::size_t i) noexcept {
                  detail::compress_subtree_cv(
                      ops, base + i * part * chunk_size, part,
                      static_cast<std::uint64_t>(i) * part, cvs[i].words);
                });
    ex::sync_wait(std::move(work));

    hasher h{ops};
    for (std::size_t i = 0; i < n_parts; ++i) {
      h.push_subtree_cv(cvs[i].words, part);
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

namespace detail {

// Fans one full window (num_chunks: power of two, counter-aligned) out
// over the scheduler and absorbs the part CVs in order.
template <class Scheduler>
void hash_window_parallel(const kern::kernel_ops* ops, Scheduler& sched,
                          hasher& h, const std::byte* data,
                          std::size_t num_chunks,
                          std::uint64_t chunk_counter) {
  constexpr std::size_t max_parts = 256;
  const std::size_t part = std::bit_floor(
      std::max<std::size_t>(num_chunks / max_parts + 1, 16));
  if (part >= num_chunks) {
    // Window too small to fan out; hash it inline.
    h.update(std::span<const std::byte>{data, num_chunks * chunk_size});
    return;
  }
  const std::size_t n_parts = num_chunks / part;

  struct alignas(64) padded_cv {
    std::uint32_t words[8];
  };
  padded_cv cvs[2 * max_parts];

  auto work = ex::schedule(sched) |
              ex::bulk(ex::par, n_parts, [&](std::size_t i) noexcept {
                compress_subtree_cv(ops, data + i * part * chunk_size, part,
                                    chunk_counter + i * part, cvs[i].words);
              });
  ex::sync_wait(std::move(work));
  for (std::size_t i = 0; i < n_parts; ++i) {
    h.push_subtree_cv(cvs[i].words, part);
  }
}

}  // namespace detail

struct parallel_hasher_options {
  arch a = arch::auto_detect;
  // Rounded down to a power-of-2 multiple of the chunk size, min 64 KiB.
  // Buffered input below one window hashes sequentially at finalize().
  std::size_t window_bytes = 8 * 1024 * 1024;
};

// The incremental counterpart of the parallel hash(): the same
// update()/finalize()/reset() interface as hasher, with multi-core
// subtree hashing happening internally. Input accumulates into an aligned
// window; a full window is fanned out over the scheduler as soon as one
// more byte arrives: the "one byte in reserve" that keeps BLAKE3's final
// chunk with the hasher for ROOT finalization. All alignment and
// final-chunk discipline lives here, not with the caller.
//
// The window buffer is the type's one allocation, made at construction.
// finalize() is non-destructive, exactly like hasher's. Not thread-safe;
// the scheduler's workers are used only inside update().
template <class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
class parallel_hasher {
 public:
  explicit parallel_hasher(Scheduler sched,
                           const parallel_hasher_options& opts = {})
      : sched_(std::move(sched)),
        ops_(detail::resolve(opts.a)),
        h_(ops_),
        window_(std::bit_floor(
            std::max<std::size_t>(opts.window_bytes, 64 * 1024))) {}

  void update(std::span<const std::byte> input) {
    const std::byte* p = input.data();
    std::size_t len = input.size();
    while (len > 0) {
      if (filled_ == window_.size()) {
        // More input exists, so the buffered window is provably not the
        // message's end: safe to offload.
        flush_window();
      }
      const std::size_t take = std::min(window_.size() - filled_, len);
      std::memcpy(window_.data() + filled_, p, take);
      filled_ += take;
      p += take;
      len -= take;
    }
  }

  void update(std::string_view input) {
    update(std::as_bytes(std::span{input.data(), input.size()}));
  }

  [[nodiscard]] digest finalize() const {
    hasher h = h_;  // flat value type; copying keeps finalize() const
    h.update(std::span<const std::byte>{window_.data(), filled_});
    return h.finalize();
  }

  void reset() noexcept {
    h_.reset();
    filled_ = 0;
    chunk_counter_ = 0;
  }

 private:
  void flush_window() {
    const std::size_t chunks = window_.size() / chunk_size;
    detail::hash_window_parallel(ops_, sched_, h_, window_.data(), chunks,
                                 chunk_counter_);
    chunk_counter_ += chunks;
    filled_ = 0;
  }

  Scheduler sched_;
  const kern::kernel_ops* ops_;
  hasher h_;
  std::vector<std::byte> window_;
  std::size_t filled_ = 0;
  std::uint64_t chunk_counter_ = 0;
};

}  // namespace blake3pp
