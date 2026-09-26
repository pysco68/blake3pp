#pragma once

/// @file
/// Parallel BLAKE3 over the sender/receiver model.
///
/// BLAKE3's binary Merkle tree makes the parallel decomposition exact
/// rather than heuristic. Any power-of-2, position-aligned run of chunks
/// reduces to one chaining value independently of everything else, so the
/// digest does not depend on how the work was divided.
///
/// The engine partitions the input into equal such subtrees. The
/// scheduler's execution agents pull them from a shared counter until none
/// are left, which means a slow agent takes fewer instead of holding up
/// the join. The chaining values are then absorbed in order through the
/// hasher's CV-stack discipline, and the tail finishes sequentially. The
/// merge work after the parallel phase is O(parts) scalar compressions,
/// which is noise.
///
/// The provider is a build-time choice, BLAKE3PP_EXECUTION_PROVIDER:
/// std::execution where the standard library ships it, beman.execution as
/// the conformance-first polyfill, NVIDIA stdexec as the performance
/// workhorse. Same source and same story as the simd providers.
///
/// Nothing here touches the heap. The CV table lives on the caller's stack
/// and sender operation states live inside sync_wait's frame.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/compress.hpp>
#include <blake3pp/detail/ex.hpp>
#include <blake3pp/detail/file_pipeline.hpp>
#include <blake3pp/detail/memory_driver.hpp>
#include <blake3pp/detail/tree_reducer.hpp>
#include <blake3pp/trace.hpp>

#if defined(__linux__)
#include <sched.h>  // sched_getcpu() for agent_record::cpu
#endif

#if defined(BLAKE3PP_EXECUTION_STDEXEC) || \
    (!defined(BLAKE3PP_EXECUTION_STD) && !defined(BLAKE3PP_EXECUTION_BEMAN))
#if BLAKE3PP_HAS_STD_THREAD
#include <exec/static_thread_pool.hpp>
#endif
#endif

namespace blake3pp {

// The process-wide parallel scheduler, in P2079's shape: the same call
// C++26 application code makes. Calling it is the explicit opt-in that may
// create the process pool (provider-dependent). The scheduler-taking hash
// overloads below stay the primary, caller-controlled API, and anyone who
// needs a sized or bounded pool constructs their provider's pool directly.
//
// parallel_scheduler_t is a per-build concrete type, not type-erased: the
// provider is fixed at configure time, so dispatch stays fully inlinable.
#if defined(BLAKE3PP_EXECUTION_STD)

/// The type of the process-wide scheduler: a per-build concrete type, not
/// type-erased, so dispatch stays fully inlinable.
using parallel_scheduler_t = decltype(std::execution::get_parallel_scheduler());
/// The process-wide parallel scheduler, in P2079's shape.
///
/// The same call C++26 application code makes. Calling it is the explicit
/// opt-in that may create the process pool. The scheduler-taking overloads
/// stay the primary, caller-controlled API; a sized or bounded pool is the
/// provider's own, passed in directly.
[[nodiscard]] inline parallel_scheduler_t get_parallel_scheduler() {
  return std::execution::get_parallel_scheduler();
}

#elif defined(BLAKE3PP_EXECUTION_BEMAN)

/// The type of the process-wide scheduler: a per-build concrete type, not
/// type-erased, so dispatch stays fully inlinable.
using parallel_scheduler_t = beman::execution::parallel_scheduler;
/// The process-wide parallel scheduler, in P2079's shape.
///
/// Backed by beman.execution's own parallel_scheduler backend. A program
/// may replace it by defining query_parallel_scheduler_backend() itself:
/// P2079 replaceability, one definition per program, like a global
/// allocator.
[[nodiscard]] inline parallel_scheduler_t get_parallel_scheduler() {
  return beman::execution::get_parallel_scheduler();
}

#elif BLAKE3PP_HAS_STD_THREAD  // BLAKE3PP_EXECUTION_STDEXEC

namespace detail {
// The size process_pool() will be built with, 0 meaning every core. Only
// <blake3pp/parallel_backend.hpp>'s size_parallel_scheduler() writes it,
// and only before the pool exists; the flag below is how it knows.
inline std::atomic<unsigned>& process_pool_threads() noexcept {
  static std::atomic<unsigned> threads{0};
  return threads;
}
inline std::atomic<bool>& process_pool_started() noexcept {
  static std::atomic<bool> started{false};
  return started;
}

// Function-local static: constructed on first use, threads joined during
// static destruction; the same lifetime the standard's parallel scheduler
// has.
inline exec::static_thread_pool& process_pool() {
  static exec::static_thread_pool pool{[] {
    process_pool_started().store(true, std::memory_order_relaxed);
    const unsigned n = process_pool_threads().load(std::memory_order_relaxed);
    return n != 0 ? n : std::thread::hardware_concurrency();
  }()};
  return pool;
}
}  // namespace detail

/// The type of the process-wide scheduler: a per-build concrete type, not
/// type-erased, so dispatch stays fully inlinable.
using parallel_scheduler_t =
    decltype(detail::process_pool().get_scheduler());
/// The process-wide parallel scheduler, in P2079's shape.
///
/// The same call C++26 application code makes. The first call creates the
/// process pool (one thread per hardware thread), joined during static
/// destruction. The scheduler-taking overloads stay the primary,
/// caller-controlled API; a sized or bounded pool is the provider's own
/// (exec::static_thread_pool pool(8); pool.get_scheduler()).
[[nodiscard]] inline parallel_scheduler_t get_parallel_scheduler() {
  return detail::process_pool().get_scheduler();
}

#endif  // no process pool without std::thread: see BLAKE3PP_HAS_STD_THREAD.
        // The scheduler-taking hash overloads below are unaffected, and are
        // the primary API in any case -- a freestanding caller brings its
        // own scheduler because only it knows what its agents should be.

namespace detail {

// Below this, the pipeline is not worth starting: a scope builds its
// window objects and its reducer before the first byte is compressed,
// and the sequential hasher beats all of it.
//
// Measured on the Z13 (Ryzen 7 PRO 6860Z, 16 threads), best of 81, this
// build's sequential path against the same build's pipeline forced on:
//
//     320 KiB   3.9-4.1  against  2.6-2.9 GiB/s
//     384 KiB   3.5-4.0  against  2.6-3.3
//     448 KiB   3.3-3.5  against  2.7-3.1
//     512 KiB   3.4-3.5  against  3.3-3.4      <- even
//     640 KiB   3.5-3.6  against  4.0-4.7
//
// So the crossover is 512 KiB, and from 640 KiB the pipeline is ahead by
// a fifth and climbing. The figure is one machine's; it is a threshold
// rather than a tuned constant because the shape either side of it is
// flat enough that being wrong by a factor of two costs little.
inline constexpr std::size_t min_pipeline_bytes = 512 * 1024;

// The window a source already in memory is cut into. A window is a view,
// so this buys no buffers: what it sets is the subtree granularity and
// how many windows the reducer sees.
inline constexpr std::size_t memory_window_bytes = 8 * 1024 * 1024;

// The one-shot engine: the same pipeline update_file drives, over a
// source whose windows are views of the caller's span.
//
// Every full window folds to one chaining value and the ends go through
// the canonical decomposition, so the reducer takes them in any order
// and the hasher sees them in message order. That is what lets the part
// count stop being a power of two, which one bulk over the whole span
// had to be. Plain, keyed and derive_key hashers all work.
template <stack_budget Budget, class Scheduler>
[[nodiscard]] digest hash_into(hasher& h, std::span<const std::byte> input,
                               Scheduler&& sched,
                               const kern::kernel_ops* ops) {
  if (input.size() < min_pipeline_bytes) {
    h.update(input);
    return h.finalize();
  }
  memory_driver drv;
  memory_driver::file f(drv, input);
  const pipeline_plan plan = plan_memory_pipeline(
      h.count(), memory_window_bytes,
      static_cast<std::uint64_t>(input.size()));
  run_window_pipeline<Budget>(
      h, drv, f, std::forward<Scheduler>(sched), plan,
      {.window_bytes = memory_window_bytes, .ops = ops});
  return h.finalize();
}

}  // namespace detail

// The multi-core one-shots: core.hpp's hash / keyed_hash / derive_key
// family with a scheduler added, same spellings, same string_view
// conveniences. Every template is constrained on the scheduler concept
// so none of them can hijack a core overload (an arch enum or a
// string_view in the scheduler's position fails to match).

/// Expert: multi-core hash on a caller-supplied kernel table, the same
/// seam hasher's expert constructor exposes.
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param input  Any length.
/// @param sched  Where the subtree reductions run.
/// @param ops    The kernel table; must outlive the call.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash(std::span<const std::byte> input, Scheduler&& sched,
                          const kern::kernel_ops* ops) {
  hasher h{ops};
  return detail::hash_into<Budget>(h, input, std::forward<Scheduler>(sched),
                                   ops);
}

/// Multi-core one-shot hash: the subtree reductions of input run on sched,
/// and the digest is identical to the sequential hash(input).
///
/// Any std::execution-style scheduler works; inputs too small for
/// parallelism to pay for itself take the sequential path.
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param input  Any length.
/// @param sched  Where the subtree reductions run.
/// @param a      The variant to run on.
/// @throws std::system_error with errc::operation_canceled if sched
///         completes the work stopped instead of running it.
///
/// @code
/// auto sched = blake3pp::get_parallel_scheduler();
/// blake3pp::digest d = blake3pp::hash(big_buffer, sched);
/// @endcode
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash(std::span<const std::byte> input, Scheduler&& sched,
                          arch a = arch::auto_detect) {
  return hash<Budget>(input, std::forward<Scheduler>(sched),
                      detail::resolve(a));
}

/// Multi-core one-shot hash of a string's bytes.
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param input  The bytes of the string.
/// @param sched  Where the subtree reductions run.
/// @param a      The variant to run on.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash(std::string_view input, Scheduler&& sched,
                          arch a = arch::auto_detect) {
  return hash<Budget>(std::as_bytes(std::span{input.data(), input.size()}),
                      std::forward<Scheduler>(sched), a);
}

/// Multi-core keyed one-shot: the MAC/PRF of input under a 32-byte key,
/// same decomposition as hash().
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param key    Exactly key_size bytes, enforced by the span extent.
/// @param input  Any length.
/// @param sched  Where the subtree reductions run.
/// @param a      The variant to run on.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest keyed_hash(std::span<const std::byte, key_size> key,
                                std::span<const std::byte> input,
                                Scheduler&& sched,
                                arch a = arch::auto_detect) {
  // Reuse the ops overload's partitioning by seeding it with a keyed
  // hasher: the engine takes key material from the hasher itself.
  const kern::kernel_ops* const ops = detail::resolve(a);
  hasher h = hasher::keyed(key, ops);
  return detail::hash_into<Budget>(h, input, std::forward<Scheduler>(sched),
                                   ops);
}

/// Multi-core keyed one-shot of a string's bytes.
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param key    Exactly key_size bytes.
/// @param input  The bytes of the string.
/// @param sched  Where the subtree reductions run.
/// @param a      The variant to run on.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest keyed_hash(std::span<const std::byte, key_size> key,
                                std::string_view input, Scheduler&& sched,
                                arch a = arch::auto_detect) {
  return keyed_hash<Budget>(
      key, std::as_bytes(std::span{input.data(), input.size()}),
      std::forward<Scheduler>(sched), a);
}

/// Multi-core key derivation, for key material large enough to matter (a
/// file's worth of entropy, a whole seed image); see core.hpp's
/// derive_key() for the context contract.
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param context       The domain-separation string; not a secret.
/// @param key_material  The secret to derive from.
/// @param sched         Where the subtree reductions run.
/// @param a             The variant to run on.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest derive_key(std::string_view context,
                                std::span<const std::byte> key_material,
                                Scheduler&& sched,
                                arch a = arch::auto_detect) {
  const kern::kernel_ops* const ops = detail::resolve(a);
  hasher h = hasher::derive_key(context, ops);
  return detail::hash_into<Budget>(
      h, key_material, std::forward<Scheduler>(sched), ops);
}

/// Multi-core key derivation from a string's bytes.
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param context       The domain-separation string; not a secret.
/// @param key_material  The secret to derive from.
/// @param sched         Where the subtree reductions run.
/// @param a             The variant to run on.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest derive_key(std::string_view context,
                                std::string_view key_material,
                                Scheduler&& sched,
                                arch a = arch::auto_detect) {
  return derive_key<Budget>(
      context,
      std::as_bytes(std::span{key_material.data(), key_material.size()}),
      std::forward<Scheduler>(sched), a);
}

/// parallel_hasher's knobs.
struct parallel_hasher_options {
  /// The SIMD variant of the internal hasher.
  arch a = arch::auto_detect;
  /// Bytes accumulated before a window is fanned out; rounded down to a
  /// power-of-2 multiple of chunk_size, minimum 64 KiB. Buffered input
  /// below one window hashes sequentially at `finalize()`.
  std::size_t window_bytes = 8 * 1024 * 1024;
};

/// The incremental counterpart of the multi-core hash(). It offers the
/// same `update()`, `finalize()` and `reset()` interface as hasher, and
/// fans the subtree hashing out over a scheduler internally.
///
/// What a caller can rely on:
///   - The digest equals the sequential one.
///   - All alignment and final-chunk discipline lives here, not with the
///     caller.
///   - The window buffer is the type's one allocation, made at
///     construction.
///   - finalize() is non-destructive, like hasher's.
///   - Not thread-safe. The scheduler's workers are used only inside
///     update().
///
/// Input accumulates into an aligned window, and a full window is fanned
/// out as soon as one more byte arrives. That is the "one byte in reserve"
/// which keeps BLAKE3's final chunk with the hasher for ROOT
/// finalization.
/// @tparam Scheduler  Any std::execution-style scheduler, held by value.
/// @tparam Budget     The stack a window's part table may take; see
///                    stack_budget.
///
/// @code
/// blake3pp::parallel_hasher ph{blake3pp::get_parallel_scheduler()};
/// while (auto block = source.next_block()) {
///   ph.update(*block);
/// }
/// blake3pp::digest d = ph.finalize();   // == the sequential digest
/// @endcode
template <class Scheduler, stack_budget Budget = default_stack_budget>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
class parallel_hasher {
 public:
  /// Plain mode.
  /// @param sched  Where the subtree reductions run.
  /// @param opts   The variant and the window size.
  explicit parallel_hasher(Scheduler sched,
                           const parallel_hasher_options& opts = {})
      : sched_(std::move(sched)),
        ops_(detail::resolve(opts.a)),
        h_(ops_),
        window_(std::bit_floor(
            std::max<std::size_t>(opts.window_bytes, 64 * 1024))) {}

  /// Keyed (MAC/PRF) mode.
  /// @param sched  Where the subtree reductions run.
  /// @param key    Exactly key_size bytes, enforced by the span extent.
  /// @param opts   The variant and the window size.
  parallel_hasher(Scheduler sched, std::span<const std::byte, key_size> key,
                  const parallel_hasher_options& opts = {})
      : sched_(std::move(sched)),
        ops_(detail::resolve(opts.a)),
        h_(hasher::keyed(key, ops_)),
        window_(std::bit_floor(
            std::max<std::size_t>(opts.window_bytes, 64 * 1024))) {}

  /// Key-derivation mode: the input is the key material, domain-separated
  /// by context (see hasher::derive_key).
  /// @param sched    Where the subtree reductions run.
  /// @param context  The domain-separation string; not a secret.
  /// @param opts     The variant and the window size.
  parallel_hasher(Scheduler sched, std::string_view context,
                  const parallel_hasher_options& opts = {})
      : sched_(std::move(sched)),
        ops_(detail::resolve(opts.a)),
        h_(hasher::derive_key(context, ops_)),
        window_(std::bit_floor(
            std::max<std::size_t>(opts.window_bytes, 64 * 1024))) {}

  /// Absorbs the next bytes of the message; complete windows are fanned
  /// out over the scheduler from here.
  /// @param input  Any length, including zero.
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
      std::copy_n(p, take, window_.data() + filled_);
      filled_ += take;
      p += take;
      len -= take;
    }
  }

  /// Absorbs the next bytes of the message, given as text.
  /// @param input  The bytes of the string, not including any terminator.
  void update(std::string_view input) {
    update(std::as_bytes(std::span{input.data(), input.size()}));
  }

  /// The digest of everything absorbed so far; the hasher stays usable.
  [[nodiscard]] digest finalize() const { return drained().finalize(); }

  /// Extended output: fills out with the first `out.size()` bytes of the
  /// output stream.
  /// @param out  Any length.
  void finalize(std::span<std::byte> out) const { drained().finalize(out); }

  /// Extended output by value: the first N bytes of the output stream.
  /// @tparam N  The number of bytes to return.
  template <std::size_t N>
  [[nodiscard]] std::array<std::byte, N> finalize() const {
    return drained().template finalize<N>();
  }

  /// Extended output as a seekable stream, independent of this
  /// parallel_hasher afterwards.
  [[nodiscard]] output_reader finalize_xof() const {
    return drained().finalize_xof();
  }

  /// Returns the hasher to its just-constructed state, keeping its mode,
  /// key, variant and window.
  void reset() noexcept {
    h_.reset();
    filled_ = 0;
    chunk_counter_ = 0;
  }

  /// Total bytes absorbed since construction or reset.
  [[nodiscard]] std::uint64_t count() const noexcept {
    return h_.count() + filled_;
  }

 private:
  // The finalize seam: a copy of the flat internal hasher with the
  // buffered tail absorbed. Copying is what keeps every finalize form
  // const and non-destructive, exactly like hasher's.
  [[nodiscard]] hasher drained() const {
    hasher h = h_;
    h.update(std::span<const std::byte>{window_.data(), filled_});
    return h;
  }

  void flush_window() {
    const std::size_t chunks = window_.size() / chunk_size;
    detail::hash_window_parallel<Budget>(ops_, sched_, h_, window_.data(),
                                         chunks, chunk_counter_);
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

/// Fills a buffer from an output reader on every core: the request is
/// split into segments, and each task copies the reader, seeks its own
/// segment and fills it straight into the caller's buffer.
///
/// Extended output is seekable in O(1), which makes this exact. r advances
/// past out afterwards exactly as r.fill(out) would have. Requests of one
/// segment or less take the sequential path.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param r              The reader to advance.
/// @param out            Receives the next `out.size()` bytes of r's stream.
/// @param sched          Where the segments are filled.
/// @param segment_bytes  Bytes per task; rounded down to a multiple of
///                       block_size so every task starts on the wide path.
///                       The default matches a generator's natural write
///                       granularity.
/// @throws std::system_error with errc::operation_canceled if sched
///         completes the work stopped; r is then left where it was.
template <class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
void fill(output_reader& r, std::span<std::byte> out, Scheduler&& sched,
          std::size_t segment_bytes = 4 * 1024 * 1024) {
  const std::size_t segment = std::max(
      segment_bytes - segment_bytes % block_size, block_size);
  if (out.size() <= segment) {
    r.fill(out);
    return;
  }
  const std::uint64_t base = r.position();
  const std::size_t n_segs = (out.size() + segment - 1) / segment;
  auto work = ex::schedule(sched) |
              ex::bulk(ex::par, n_segs, [&](std::size_t i) noexcept {
                output_reader part = r;
                const std::size_t off = i * segment;
                part.seek(base + off);
                part.fill(out.subspan(off, std::min(segment, out.size() - off)));
              });
  if (!ex::sync_wait(std::move(work))) {
    throw std::system_error(std::make_error_code(std::errc::operation_canceled),
                            "the scheduler stopped the parallel fill");
  }
  r.seek(base + out.size());
}

}  // namespace blake3pp
