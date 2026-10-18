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
/// the join. Their chaining values are folded lanes-wide in groups inside
/// the parallel phase, one CV per window reaches the hasher's CV-stack
/// discipline, and the tail finishes sequentially.
///
/// The provider is a build-time choice, BLAKE3PP_EXECUTION_PROVIDER:
/// std::execution where the standard library ships it, beman.execution as
/// the conformance-first polyfill, NVIDIA stdexec as the performance
/// workhorse. The same source compiles against all three, as with the simd providers.
///
/// What this allocates: the window pool, each window carrying its part
/// table (`stack_budget` bytes, 32 KiB by default) and the chaining values
/// its groups fold into, the reducer's nodes and the operation cells, all
/// taken once when a run is planned and freed with it. One table per window
/// grows later, on the first window of a shape that needs it: the lane
/// slots a window's parts leave their kernel batches in, `n_parts * width`
/// chaining values, up to 512 KiB at the default budget on a 16-lane
/// kernel. The part table is a member of a window rather than a frame of the
/// caller, which is what `stack_budget` bounds -- the agents' own frames
/// are a constant of the build (`BLAKE3PP_MAX_SIMD_DEGREE` and
/// `BLAKE3PP_SUBTREE_FOLD`; see docs/freestanding.md).

#include <algorithm>
#include <atomic>
#include <mutex>
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
// The size process_pool() will be built with, 0 meaning every core.
// <blake3pp/parallel_backend.hpp>'s size_parallel_scheduler() writes it, and
// so does the test suite's main() in builds without that backend, which
// needs a bound so a parallel ctest run does not fan every case out over
// every core. Either way only before the pool exists; the flag below is how
// it knows.
inline std::atomic<unsigned>& process_pool_threads() noexcept {
  static std::atomic<unsigned> threads{0};
  return threads;
}
inline std::atomic<bool>& process_pool_started() noexcept {
  static std::atomic<bool> started{false};
  return started;
}

// Reading the size and declaring the pool started is one decision, and so
// is size_parallel_scheduler()'s check-and-set, so both take this. Without
// it a caller could check the flag, see this initializer has not set it
// yet, store a size the initializer has already read past, and be told it
// sized a pool that in fact runs at hardware_concurrency().
inline std::mutex& process_pool_sizing() noexcept {
  static std::mutex m;
  return m;
}

// Function-local static: constructed on first use, threads joined during
// static destruction; the same lifetime the standard's parallel scheduler
// has. The pool is built outside the lock, since it starts threads.
inline exec::static_thread_pool& process_pool() {
  static exec::static_thread_pool pool{[] {
    const unsigned n = [] {
      const std::lock_guard lock{process_pool_sizing()};
      process_pool_started().store(true, std::memory_order_relaxed);
      return process_pool_threads().load(std::memory_order_relaxed);
    }();
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
// Measured on a Ryzen 7 PRO 6860Z (16 threads), best of 81, the
// sequential path against the pipeline forced on:
//
//     320 KiB   3.9-4.1  against  2.6-2.9 GiB/s
//     384 KiB   3.5-4.0  against  2.6-3.3
//     448 KiB   3.3-3.5  against  2.7-3.1
//     512 KiB   3.4-3.5  against  3.3-3.4      <- even
//     640 KiB   3.5-3.6  against  4.0-4.7
//
// So the crossover is 512 KiB, and from 640 KiB the pipeline is ahead by
// a fifth and climbing. The figure is one machine's; the curve either
// side of it is flat enough that a factor of two costs little.
//
inline constexpr std::size_t min_pipeline_bytes = 512 * 1024;

// The window a span already in memory is cut into.
//
// A window is what a file pipeline reads while it hashes the previous
// one. A span has nothing to overlap: the bytes are all there already, so
// a second window buys no concurrency and costs a join, a reducer insert
// and a part table. The answer is the FEWEST windows that still bound a
// part table, which is one window up to max_window_bytes.
//
// One window per span against fixed 8 MiB windows, 16 threads, ABBA,
// eight samples a side, medians: +8% at 8 MiB, +15% at 64 MiB, +53% at
// 1 GiB, where the fixed windows are 128 narrow fan-outs against one
// wide one.
//
// default_window_bytes is only the floor: a span at or below it is one
// window either way.
[[nodiscard]] constexpr std::size_t memory_window_bytes(
    std::uint64_t span_bytes) noexcept {
  return std::clamp(static_cast<std::size_t>(span_bytes), default_window_bytes,
                    max_window_bytes);
}

// The one-shot engine: the same pipeline update_file drives, over a
// source whose windows are views of the caller's span.
//
// Every full window folds to one chaining value and the ends go through
// the canonical decomposition, so the reducer takes them in any order
// and the hasher sees them in message order, so the part count need not
// be a power of two. Plain, keyed and derive_key hashers all work.
template <stack_budget Budget, class Scheduler>
[[nodiscard]] digest hash_into(hasher& h, std::span<const std::byte> input,
                               Scheduler&& sched,
                               const kern::kernel_ops* ops) {
  if (input.size() < min_pipeline_bytes) {
    // The call finalizes, so these bytes end the message.
    h.update_last(input);
    return h.finalize();
  }
  const std::size_t window =
      memory_window_bytes(static_cast<std::uint64_t>(input.size()));
  memory_driver drv;
  memory_driver::file f(drv, input);
  const pipeline_plan plan = plan_memory_pipeline(
      h.count(), window, static_cast<std::uint64_t>(input.size()));
  run_window_pipeline<Budget>(h, drv, f, std::forward<Scheduler>(sched), plan,
                              {.window_bytes = window, .ops = ops});
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
/// @tparam Budget     What its part table may take; see stack_budget.
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
/// @tparam Budget     What its part table may take; see stack_budget.
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
/// @tparam Budget     What its part table may take; see stack_budget.
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
/// @tparam Budget     What its part table may take; see stack_budget.
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
/// @tparam Budget     What its part table may take; see stack_budget.
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
/// @tparam Budget     What its part table may take; see stack_budget.
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
/// @tparam Budget     What its part table may take; see stack_budget.
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
  /// Bytes accumulated before a window is fanned out; rounded by
  /// detail::rounded_window_bytes to a power of two in
  /// [64 KiB, 1 GiB]. Buffered input below one window hashes
  /// sequentially at `finalize()`.
  std::size_t window_bytes = detail::default_window_bytes;
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
/// @tparam Budget     What a window's part table may take; see
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
        remainder_(detail::rounded_window_bytes(opts.window_bytes)) {}

  /// Keyed (MAC/PRF) mode.
  /// @param sched  Where the subtree reductions run.
  /// @param key    Exactly key_size bytes, enforced by the span extent.
  /// @param opts   The variant and the window size.
  parallel_hasher(Scheduler sched, std::span<const std::byte, key_size> key,
                  const parallel_hasher_options& opts = {})
      : sched_(std::move(sched)),
        ops_(detail::resolve(opts.a)),
        h_(hasher::keyed(key, ops_)),
        remainder_(detail::rounded_window_bytes(opts.window_bytes)) {}

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
        remainder_(detail::rounded_window_bytes(opts.window_bytes)) {}

  /// Absorbs the next bytes of the message; complete windows are fanned
  /// out over the scheduler from here.
  ///
  /// Whole windows of `input` are hashed where they lie, so the cost in
  /// copying is bounded: at most ONE window is copied per call, and only
  /// to finish a window a previous call left part-built. The buffer that
  /// holds it is one window, never more.
  /// @param input  Any length, including zero.
  void update(std::span<const std::byte> input) {
    if (input.empty()) {
      return;
    }
    // Whole windows of the caller's own bytes are hashed where they lie.
    // The only copying is what it takes to finish a window the last call
    // left part-built, which is under one window and never more.
    if (filled_ > 0) {
      const std::size_t take =
          std::min(remainder_.size() - filled_, input.size());
      std::copy_n(input.data(), take, remainder_.data() + filled_);
      filled_ += take;
      input = input.subspan(take);
      if (input.empty()) {
        return;
      }
      // More input exists, so what is buffered is provably not the
      // message's end: it may go to the pool as a subtree. The buffer is
      // emptied before the call, not after: absorb() may throw -- the
      // scheduler stopping the run, a provider's allocation -- and bytes
      // the hasher has not taken must not be absorbed twice by a later
      // call. A throw loses them, which the exception says.
      filled_ = 0;
      absorb(std::span<const std::byte>{remainder_});
    }

    // What is left starts on a window boundary, because the hasher does:
    // everything absorbed below is a whole number of windows.
    //
    // The final window is always held back, even when the input ends
    // exactly on a boundary. A subtree pushed into the hasher must not
    // contain the message's last chunk -- that chunk is finalized as the
    // root -- and from here there is no way to know whether more is
    // coming. So the remainder is never empty once anything has arrived,
    // and finalize absorbs it through the hasher itself.
    const std::size_t rest = input.size() % remainder_.size();
    const std::size_t keep = rest == 0 ? remainder_.size() : rest;
    if (const std::size_t whole = input.size() - keep; whole > 0) {
      absorb(input.first(whole));
      input = input.subspan(whole);
    }
    std::copy_n(input.data(), input.size(), remainder_.data());
    filled_ = input.size();
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
  }

  /// Total bytes absorbed since construction or reset.
  [[nodiscard]] std::uint64_t count() const noexcept {
    return h_.count() + filled_;
  }

 private:
  // The finalize seam: a copy of the flat internal hasher with the
  // buffered tail absorbed. Copying is what keeps every finalize form
  // const and non-destructive, exactly like hasher's.
  //
  // The tail is up to a whole window, so it is worth fanning out too.
  // This is the one absorb that DOES end the message, which is what the
  // pipeline's last-window chain is for: the parts before the final
  // chunk go to the pool, and only the part carrying that chunk stays
  // with the hasher.
  [[nodiscard]] hasher drained() const {
    hasher h = h_;
    const std::span<const std::byte> tail{remainder_.data(), filled_};
    if (filled_ < detail::min_pipeline_bytes) {
      h.update_last(tail);
      return h;
    }
    detail::memory_driver drv;
    detail::memory_driver::file f(drv, tail);
    const detail::pipeline_plan plan = detail::plan_memory_pipeline(
        h.count(), remainder_.size(), static_cast<std::uint64_t>(filled_));
    detail::run_window_pipeline<Budget>(
        h, drv, f, sched_, plan,
        {.window_bytes = remainder_.size(), .ops = ops_});
    return h;
  }

  // A whole number of windows, straight into the hasher. The pipeline
  // reduces them in any order and drains the reducer before it returns,
  // which is what leaves the hasher on a window boundary for the next
  // call. Below the threshold the scope costs more than it saves.
  void absorb(std::span<const std::byte> bytes) {
    if (bytes.size() < detail::min_pipeline_bytes) {
      h_.update(bytes);
      return;
    }
    detail::memory_driver drv;
    detail::memory_driver::file f(drv, bytes);
    detail::pipeline_plan plan = detail::plan_memory_pipeline(
        h_.count(), remainder_.size(),
        static_cast<std::uint64_t>(bytes.size()));
    // Never: update() holds the final window back, so nothing absorbed
    // here can contain the message's end.
    plan.ends_message = false;
    detail::run_window_pipeline<Budget>(
        h_, drv, f, sched_, plan,
        {.window_bytes = remainder_.size(), .ops = ops_});
  }

  Scheduler sched_;
  const kern::kernel_ops* ops_;
  hasher h_;
  // One window's worth, holding whatever has not yet reached a window
  // boundary. Never more: the rest of the message is hashed where the
  // caller put it.
  std::vector<std::byte> remainder_;
  std::size_t filled_ = 0;
};

/// The segment a parallel XOF fill splits its request into by default, and
/// the granularity a generator writing that stream out naturally works in.
inline constexpr std::size_t default_fill_segment_bytes = 4 * 1024 * 1024;

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
///                       Defaults to default_fill_segment_bytes.
/// @throws std::system_error with errc::operation_canceled if sched
///         completes the work stopped; r is then left where it was.
template <class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
void fill(output_reader& r, std::span<std::byte> out, Scheduler&& sched,
          std::size_t segment_bytes = default_fill_segment_bytes) {
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
