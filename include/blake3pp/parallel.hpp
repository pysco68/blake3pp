#pragma once

/// @file
/// Parallel BLAKE3 over the sender/receiver model.
///
/// BLAKE3's binary Merkle tree makes the parallel decomposition exact, not
/// heuristic: any power-of-2, position-aligned run of chunks reduces to
/// one chaining value independently of everything else. So the engine
/// partitions the input into equal such subtrees, which the scheduler's
/// execution agents pull from a shared counter until none are left (a
/// slow agent takes fewer, rather than holding up the join), then absorbs
/// the CVs in order through the hasher's CV-stack discipline and finishes
/// the tail sequentially. The merge work after the parallel phase is
/// O(parts) scalar compressions, which is noise.
///
/// The provider is a build-time choice (BLAKE3PP_EXECUTION_PROVIDER):
/// std::execution where the standard library ships it, beman.execution as
/// the conformance-first polyfill, NVIDIA stdexec as the performance
/// workhorse (same source, same story as the simd providers). No heap
/// allocations in this header: the CV table lives on the caller's stack
/// and sender operation states live inside sync_wait's frame.

#include <algorithm>
#include <atomic>
#include <bit>
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

#if defined(BLAKE3PP_EXECUTION_STD)
#include <execution>
#elif defined(BLAKE3PP_EXECUTION_BEMAN)
#include <beman/execution/execution.hpp>
#else  // BLAKE3PP_EXECUTION_STDEXEC
#include <stdexec/execution.hpp>
#if BLAKE3PP_HAS_STD_THREAD
#include <exec/static_thread_pool.hpp>
#endif
#endif

namespace blake3pp {

/// The sender/receiver vocabulary this build uses (std::execution,
/// beman::execution or stdexec), so the library and its callers spell
/// schedule, bulk and sync_wait the same way whichever provider is built.
namespace ex {
#if defined(BLAKE3PP_EXECUTION_STD)
using namespace std::execution;
using std::this_thread::sync_wait;
#elif defined(BLAKE3PP_EXECUTION_BEMAN)
using namespace beman::execution;
#else
using namespace stdexec;
#endif
}  // namespace ex

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
// Deliberately not constexpr and never defined: a stack_budget constructor
// that reaches one fails its constant evaluation, and the diagnostic names
// the violated rule.
void stack_budget_not_a_multiple_of_32_bytes();
void stack_budget_below_two_parts();
}  // namespace detail

/// The stack the multi-core functions may spend on their table of part
/// chaining values, passed as their first template argument:
///
/// @code
/// blake3pp::hash<blake3pp::stack_budget{1024}>(input, sched);   // 32 parts
/// @endcode
///
/// The input is split into at most parts() parts, each with a 32-byte
/// chaining value in a table on the calling thread's stack, whatever the
/// input's size. Agents pull parts from a shared counter, so a slow agent
/// takes fewer instead of holding up the join, and more parts balance more
/// finely; a smaller budget splits the input into fewer, larger parts,
/// which suits a machine with few cores.
///
/// A budget that is not a multiple of 32 bytes, or that holds fewer than
/// two parts, does not compile. There is no upper limit, because the stack
/// the calling thread has is not known where this header is compiled; the
/// platform, the linker, the thread's creator or the application's
/// configuration sets it. Code that knows its thread checks the budget
/// against that at compile time, leaving room for its own frames:
///
/// @code
/// constexpr blake3pp::stack_budget budget{1024};
/// static_assert(budget.bytes <= CONFIG_MAIN_STACK_SIZE / 4);   // e.g. on Zephyr
/// @endcode
///
/// The budget covers this table alone. Each part is reduced on the agent
/// that took it, by a recursion holding one chaining-value buffer per level
/// (2 KiB while a 16-wide kernel is compiled in: the buffer is sized for
/// the widest compiled variant, not the running one), as deep as halving
/// the part takes to reach twice the running variant's degree in chunks. A
/// smaller budget makes parts larger and that recursion deeper, so agent
/// threads need stack of their own.
struct stack_budget {
  /// The stack one part's chaining value takes.
  static constexpr std::size_t bytes_per_part = 32;

  /// The budget in bytes.
  std::size_t bytes;

  /// @param n  A multiple of bytes_per_part, at least two parts' worth.
  consteval explicit stack_budget(std::size_t n) : bytes{n} {
    if (n % bytes_per_part != 0) {
      detail::stack_budget_not_a_multiple_of_32_bytes();
    }
    if (n / bytes_per_part < 2) {
      detail::stack_budget_below_two_parts();
    }
  }

  /// The most parts the input is split into.
  [[nodiscard]] constexpr std::size_t parts() const noexcept {
    return bytes / bytes_per_part;
  }
};

/// The default budget: 32 KiB of stack, 1024 parts.
inline constexpr stack_budget default_stack_budget{32 * 1024};

namespace detail {

// A part is at least 16 chunks (16 KiB), which keeps small inputs in few
// tasks.
inline constexpr std::size_t min_part_chunks = 16;

// The part size in chunks for num_chunks: a power of two, so every part
// starting at a multiple of it is subtree-aligned, and large enough that
// num_chunks / part <= Budget.parts().
template <stack_budget Budget>
[[nodiscard]] constexpr std::size_t part_chunks(std::size_t num_chunks) noexcept {
  return std::bit_ceil(std::max(
      (num_chunks + Budget.parts() - 1) / Budget.parts(), min_part_chunks));
}

// One CV slot per part, on the caller's stack. The slots are unpadded:
// each is written once per part-sized task, so neighbours sharing a cache
// line cost nothing measurable.
template <stack_budget Budget>
using part_cvs = std::array<std::array<std::uint32_t, 8>, Budget.parts()>;
static_assert(sizeof(std::array<std::uint32_t, 8>) == stack_budget::bytes_per_part);

// Runs body(i) for every i in [0, n) on sched. The bulk shape only
// provides the agents: each call pulls indices from a shared counter until
// none are left, so the split follows each agent's actual speed rather
// than the fixed shares the provider's bulk may hand out. Every index runs
// exactly once however the implementation distributes the calls.
//
// A scheduler may complete the work stopped instead of running it (a
// stop request on the pool, a pool already shutting down); sync_wait then
// returns an empty optional and nothing was computed, so the caller must
// not read its results. That is reported as operation_canceled rather
// than finished sequentially: the caller asked for the work to stop.
template <class Scheduler, class Body>
void for_each_part(Scheduler& sched, std::size_t n, Body body) {
  std::atomic<std::size_t> next{0};
  auto work = ex::schedule(sched) |
              ex::bulk(ex::par, n, [&](std::size_t) noexcept {
                for (std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                     i < n; i = next.fetch_add(1, std::memory_order_relaxed)) {
                  body(i);
                }
              });
  if (!ex::sync_wait(std::move(work))) {
    throw std::system_error(std::make_error_code(std::errc::operation_canceled),
                            "the scheduler stopped the parallel hash");
  }
}

// The one-shot engine: partitions input into aligned subtrees, fans them
// out over sched, and finishes inside h, whose key_words() and
// mode_flags() drive the workers. Plain, keyed and derive_key hashers all
// work.
template <stack_budget Budget, class Scheduler>
[[nodiscard]] digest hash_into(hasher& h, std::span<const std::byte> input,
                               Scheduler&& sched,
                               const kern::kernel_ops* ops) {
  // Only chunks with at least one byte after them may be offloaded: the
  // message's final chunk must stay with the hasher for ROOT finalization.
  const std::size_t safe_chunks =
      input.size() > chunk_size ? (input.size() - 1) / chunk_size : 0;

  if (safe_chunks >= 2 * min_part_chunks) {
    const std::size_t part = part_chunks<Budget>(safe_chunks);
    const std::size_t n_parts = safe_chunks / part;  // <= Budget.parts()
    part_cvs<Budget> cvs;
    const std::byte* const base = input.data();

    // Starting from counter 0 in part-sized steps, every part is
    // automatically subtree-aligned.
    for_each_part(sched, n_parts, [&](std::size_t i) noexcept {
      detail::compress_subtree_cv(ops, base + i * part * chunk_size, part,
                                  static_cast<std::uint64_t>(i) * part,
                                  h.key_words(), h.mode_flags(), cvs[i]);
    });

    for (std::size_t i = 0; i < n_parts; ++i) {
      h.push_subtree_cv(cvs[i], part);
    }
    h.update(input.subspan(n_parts * part * chunk_size));
    return h.finalize();
  }

  h.update(input);
  return h.finalize();
}

}  // namespace detail

// The multi-core one-shots: core.hpp's hash / keyed_hash / derive_key
// family with a scheduler added, same spellings, same string_view
// conveniences. Every template is constrained on the scheduler concept
// so none of them can hijack a core overload (an arch enum or a
// string_view in the scheduler's position simply fails to match).

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

namespace detail {

// Fans one full window (num_chunks: power of two, counter-aligned) out
// over the scheduler and absorbs the part CVs in order.
template <stack_budget Budget, class Scheduler>
void hash_window_parallel(const kern::kernel_ops* ops, Scheduler& sched,
                          hasher& h, const std::byte* data,
                          std::size_t num_chunks,
                          std::uint64_t chunk_counter) {
  const std::size_t part = part_chunks<Budget>(num_chunks);
  if (part >= num_chunks) {
    // Window too small to fan out; hash it inline.
    h.update(std::span<const std::byte>{data, num_chunks * chunk_size});
    return;
  }
  const std::size_t n_parts = num_chunks / part;
  part_cvs<Budget> cvs;

  for_each_part(sched, n_parts, [&](std::size_t i) noexcept {
    compress_subtree_cv(ops, data + i * part * chunk_size, part,
                        chunk_counter + i * part, h.key_words(),
                        h.mode_flags(), cvs[i]);
  });
  for (std::size_t i = 0; i < n_parts; ++i) {
    h.push_subtree_cv(cvs[i], part);
  }
}

}  // namespace detail

/// parallel_hasher's knobs.
struct parallel_hasher_options {
  /// The SIMD variant of the internal hasher.
  arch a = arch::auto_detect;
  /// Bytes accumulated before a window is fanned out; rounded down to a
  /// power-of-2 multiple of chunk_size, minimum 64 KiB. Buffered input
  /// below one window hashes sequentially at finalize().
  std::size_t window_bytes = 8 * 1024 * 1024;
};

/// The incremental counterpart of the multi-core hash(). It offers the
/// same `update()`, `finalize()` and `reset()` interface as hasher, and
/// fans the subtree hashing out over a scheduler internally.
///
/// Input accumulates into an aligned window; a full window is fanned out
/// as soon as one more byte arrives, the "one byte in reserve" that keeps
/// BLAKE3's final chunk with the hasher for ROOT finalization. All
/// alignment and final-chunk discipline lives here, not with the caller,
/// and the digest equals the sequential one. The window buffer is the
/// type's one allocation, made at construction. finalize() is
/// non-destructive, like hasher's. Not thread-safe; the scheduler's
/// workers are used only inside update().
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

  /// Extended output: fills out with the first out.size() bytes of the
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
/// @param out            Receives the next out.size() bytes of r's stream.
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
