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
#include <blake3pp/detail/tree_reducer.hpp>
#include <blake3pp/trace.hpp>

#if defined(__linux__)
#include <sched.h>  // sched_getcpu() for agent_record::cpu
#endif

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
/// The input is split into at most parts() parts whatever its size, and
/// each part's 32-byte chaining value sits in that table. The default is
/// 32 KiB, which is 1024 parts. A budget that is not a multiple of 32
/// bytes, or that holds fewer than two parts, does not compile.
///
/// There is no upper limit, because the stack a calling thread has is not
/// known where this header is compiled. Code that knows its own thread
/// checks the budget against it:
///
/// @code
/// constexpr blake3pp::stack_budget budget{1024};
/// static_assert(budget.bytes <= CONFIG_MAIN_STACK_SIZE / 4);   // e.g. on Zephyr
/// @endcode
///
/// @warning The budget covers this table alone. Lowering it makes each part
/// larger, which makes the per-agent reduction deeper, so it moves stack
/// from the caller to the agents rather than saving any. Nothing checks an
/// agent thread's stack at run time.
///
/// @see docs/api.md for what the knob trades away, and
/// docs/freestanding.md for the measured frames and a worked case.
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

// The same floor for a window of the file pipeline, where the trade is a
// different one: a window is absorbed as one subtree, so fewer and larger
// parts shrink the fold and the bulk's shape, while lengthening the
// straggler tail that the window's join waits for. The engine cannot ask
// a generic scheduler how many agents it has, so the floor is the only
// lever on that balance.
inline constexpr std::size_t window_min_part_chunks = min_part_chunks;

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

// What one for_each_part() call records when tracing: the agents' summed
// compress time and their count, each one atomic add per bulk invocation,
// and through buf one agent_record per invocation when the buffer wants
// them. window is the id the caller gives the records; for_each_part()
// does not know what it is running over.
struct part_trace {
  trace_buffer* buf;
  std::uint64_t window;
  std::uint32_t part_chunks;
  std::atomic<std::uint64_t> busy_ns{0};
  std::atomic<std::uint32_t> active{0};
};

// A scheduler may complete the work stopped instead of running it, on a
// stop request or with a pool already shutting down. sync_wait then
// returns an empty optional and nothing was computed, so the caller must
// not read its results. That is reported as operation_canceled rather
// than finished sequentially, because the caller asked for the work to
// stop.
template <class Work>
void wait_for_parts(Work&& work) {
  if (!ex::sync_wait(std::forward<Work>(work))) {
    throw std::system_error(std::make_error_code(std::errc::operation_canceled),
                            "the scheduler stopped the parallel hash");
  }
}

// The schedule-and-bulk sender that runs body(i) for every i in [0, n).
// for_each_part() waits on it; the file pipeline starts one per window
// and lets the driver loop carry on.
//
// The bulk shape only provides the agents: each call pulls indices from
// `next` until none are left, so the split follows each agent's actual
// speed rather than the fixed shares the provider's bulk may hand out.
// Every index runs exactly once however the implementation distributes
// the calls.
//
// `body` and `next` are the caller's and must outlive the sender --
// for_each_part keeps them in its own frame, the pipeline in the window
// object.
//
// Traced is a template parameter and not a null check inside the body
// for two reasons: the untraced body is the hot one and stays exactly as
// it was, and a pipeline that connects one sender per window needs the
// type settled before the run starts.
//
// The traced body reads the clock once before its first part and, when
// agent records are wanted, once after every part (the end of one is the
// start of the next). Per bulk invocation, not per part: two atomic adds
// on pt and at most one record claim. An invocation that finds the
// counter exhausted records nothing and is not an active agent.
template <bool Traced, class Scheduler, class Body>
[[nodiscard]] auto part_bulk_sender(Scheduler& sched, std::size_t n,
                                    Body& body, std::atomic<std::size_t>& next,
                                    part_trace* pt) {
  if constexpr (Traced) {
    return ex::schedule(sched) |
           ex::bulk(ex::par, n, [n, &body, &next, pt](std::size_t) noexcept {
             std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
             if (i >= n) {
               return;
             }
             trace_buffer& buf = *pt->buf;
             const bool per_agent = buf.wants_agents();
             const std::int64_t t_begin = buf.now();
             std::int64_t t = t_begin;
             std::uint32_t parts = 0;
             std::uint64_t min_ns = ~std::uint64_t{0};
             std::uint64_t max_ns = 0;
             std::uint32_t cpu = ~std::uint32_t{0};
             if (per_agent) {
#if defined(__linux__)
               const int c = ::sched_getcpu();
               cpu = c < 0 ? ~std::uint32_t{0} : static_cast<std::uint32_t>(c);
#endif
               do {
                 body(i);
                 const std::int64_t t_end = buf.now();
                 const auto d = static_cast<std::uint64_t>(t_end - t);
                 min_ns = std::min(min_ns, d);
                 max_ns = std::max(max_ns, d);
                 t = t_end;
                 ++parts;
                 i = next.fetch_add(1, std::memory_order_relaxed);
               } while (i < n);
             } else {
               do {
                 body(i);
                 ++parts;
                 i = next.fetch_add(1, std::memory_order_relaxed);
               } while (i < n);
               t = buf.now();
             }
             const auto busy = static_cast<std::uint64_t>(t - t_begin);
             pt->busy_ns.fetch_add(busy, std::memory_order_relaxed);
             pt->active.fetch_add(1, std::memory_order_relaxed);
             if (per_agent) {
               if (agent_record* const r = buf.claim_agent()) {
                 r->window = pt->window;
                 r->t_begin = t_begin;
                 r->busy_ns = busy;
                 r->min_part_ns = min_ns;
                 r->max_part_ns = max_ns;
                 r->parts = parts;
                 r->part_chunks = pt->part_chunks;
                 r->cpu = cpu;
               }
             }
           });
  } else {
    return ex::schedule(sched) |
           ex::bulk(ex::par, n, [n, &body, &next](std::size_t) noexcept {
             for (std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                  i < n; i = next.fetch_add(1, std::memory_order_relaxed)) {
               body(i);
             }
           });
  }
}

// Runs body(i) for every i in [0, n) on sched and waits for all of them.
// Untraced: the one-shot engine has no window records to fill, and the
// file pipeline, which does, connects part_bulk_sender itself.
template <class Scheduler, class Body>
void for_each_part(Scheduler& sched, std::size_t n, Body body) {
  std::atomic<std::size_t> next{0};
  wait_for_parts(part_bulk_sender<false>(sched, n, body, next, nullptr));
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

namespace detail {

// The part size of a full window: the same arithmetic wherever a window
// is compressed, so a window folds identically whichever path ran it.
// A result at or above num_chunks means the window is too small to fan
// out at all.
template <stack_budget Budget>
[[nodiscard]] constexpr std::size_t window_part_chunks(
    std::size_t num_chunks) noexcept {
  return std::max(part_chunks<Budget>(num_chunks), window_min_part_chunks);
}

// One window's compress stage, and everything it touches.
//
// hash_window_parallel keeps the part table and the pull counter in its
// own frame because it waits for them there. The pipeline cannot: its
// compress stage is a sender that outlives the call which built it, and
// several are in flight at once. So the state moves into an object the
// window owns, allocated once with the window and reused for every
// window that passes through it.
//
// Also the bulk body: an agent calls operator()(i) for one part, and
// concurrent calls write disjoint slots of cvs.
template <stack_budget Budget>
struct window_compress {
  const kern::kernel_ops* ops = nullptr;
  const std::byte* data = nullptr;
  std::uint64_t chunk_counter = 0;
  std::size_t num_chunks = 0;
  std::size_t part = 0;
  std::size_t n_parts = 0;
  std::array<std::uint32_t, 8> key{};
  std::uint32_t flags = 0;

  part_cvs<Budget> cvs;               // one CV per part
  std::atomic<std::size_t> next{0};   // the agents pull their parts from here
  std::array<std::uint32_t, 8> cv{};  // where the fold leaves the window's CV
  part_trace pt{nullptr, 0, 0};

  window_compress() = default;
  window_compress(const window_compress&) = delete;
  window_compress& operator=(const window_compress&) = delete;

  // Takes a full, counter-aligned window. False means it is too small to
  // fan out and belongs on the sequential path, the same verdict
  // hash_window_parallel reaches.
  [[nodiscard]] bool prepare(const kern::kernel_ops* o, const std::byte* d,
                             std::size_t chunks, std::uint64_t counter,
                             std::span<const std::uint32_t, 8> k,
                             std::uint32_t f, trace_buffer* buf,
                             std::uint64_t window_id) noexcept {
    const std::size_t p = window_part_chunks<Budget>(chunks);
    if (p >= chunks) {
      return false;
    }
    ops = o;
    data = d;
    chunk_counter = counter;
    num_chunks = chunks;
    part = p;
    n_parts = chunks / p;
    std::ranges::copy(k, key.begin());
    flags = f;
    next.store(0, std::memory_order_relaxed);
    pt.buf = buf;
    pt.window = window_id;
    pt.part_chunks = static_cast<std::uint32_t>(p);
    pt.busy_ns.store(0, std::memory_order_relaxed);
    pt.active.store(0, std::memory_order_relaxed);
    return true;
  }

  // An edge window: the caller says how big a part is and how many of
  // them belong to the tree, because the rest of the window is the
  // hasher's own. counter is the absolute chunk of part 0.
  void prepare_edge(const kern::kernel_ops* o, const std::byte* d,
                    std::size_t part_chunks, std::size_t parts,
                    std::uint64_t counter,
                    std::span<const std::uint32_t, 8> k, std::uint32_t f,
                    trace_buffer* buf, std::uint64_t window_id) noexcept {
    assert(parts <= Budget.parts());
    ops = o;
    data = d;
    chunk_counter = counter;
    num_chunks = parts * part_chunks;
    part = part_chunks;
    n_parts = parts;
    std::ranges::copy(k, key.begin());
    flags = f;
    next.store(0, std::memory_order_relaxed);
    pt.buf = buf;
    pt.window = window_id;
    pt.part_chunks = static_cast<std::uint32_t>(part_chunks);
    pt.busy_ns.store(0, std::memory_order_relaxed);
    pt.active.store(0, std::memory_order_relaxed);
  }

  void operator()(std::size_t i) noexcept {
    compress_subtree_cv(ops, data + i * part * chunk_size, part,
                        chunk_counter + i * part, key, flags, cvs[i]);
  }
};

// One window's compress stage as a sender: the provider's bulk over the
// parts, then the fold that turns their CVs into the window's own. Value:
// the window CV.
//
// Where this runs, which is the whole point of it being a sender:
//
// The file pipeline starts this sender from inside the read's
// completion, which is on its driver thread, inside poll(). Starting it
// starts `schedule(sched)`, and that only enqueues onto sched -- every
// part of the bulk, and the fold behind it, then run on an agent of
// sched. The driver's share of a window's compute is that enqueue and
// nothing else, which is what lets one thread drive the reads for a
// whole file while sitting parked over 99% of the time.
//
// A provider whose bulk ran its function on the thread that started the
// sender would move the entire compression onto the driver instead, and
// the driver's busy share in trace_buffer::driver() is where that would
// show. Measured on both providers a preset can select: stdexec and
// beman.execution both leave the driver at or below 1%.
//
// The fold runs on whichever agent finished the bulk, so the driver
// thread never touches it either. The window chain around this sender
// does not know what is inside it, which makes this the one place a
// different compress stage would go.
template <bool Traced, stack_budget Budget, class Scheduler>
[[nodiscard]] auto compress_on(Scheduler& sched, window_compress<Budget>& w) {
  return part_bulk_sender<Traced>(sched, w.n_parts, w, w.next, &w.pt) |
         ex::then([&w]() noexcept {
           // Every part pairs with a sibling all the way up: the window
           // is a power-of-two number of chunks and part is a power of
           // two, so n_parts is one too.
           assert(std::has_single_bit(w.n_parts));
           fold_sibling_cvs(w.ops, std::span{w.cvs}.first(w.n_parts), w.key,
                            w.flags, w.cv);
           return w.cv;
         });
}

// An edge window's compress stage: the same bulk over a run of parts the
// caller chose, and then, instead of one fold to one CV, the canonical
// decomposition of that run into reducer nodes.
//
// An edge window is one whose bytes cannot all become a subtree -- the
// last window carries the message's final chunk, the first may start
// part-way through a part -- so only the middle of it goes to the pool,
// and what comes back is a handful of nodes rather than a single
// chaining value. Value: how many nodes were written to `out`.
//
// The fold runs on the agent that finished the bulk, like the full
// window's; `out` is the caller's and must outlive the sender.
template <bool Traced, stack_budget Budget, class Scheduler>
[[nodiscard]] auto compress_edge_on(Scheduler& sched,
                                    window_compress<Budget>& w,
                                    std::span<tree_reducer::node> out) {
  return part_bulk_sender<Traced>(sched, w.n_parts, w, w.next, &w.pt) |
         ex::then([&w, out]() noexcept {
           return fold_aligned_runs(
               w.ops, w.key, w.flags,
               cv_run{std::span(w.cvs).first(w.n_parts), 0, w.n_parts,
                      w.chunk_counter, w.part},
               out);
         });
}

// Fans one full window (num_chunks: power of two, counter-aligned) out
// over the scheduler and absorbs it as one subtree. parallel_hasher's
// window; update_file's go through the file pipeline instead.
template <stack_budget Budget, class Scheduler>
void hash_window_parallel(const kern::kernel_ops* ops, Scheduler& sched,
                          hasher& h, const std::byte* data,
                          std::size_t num_chunks,
                          std::uint64_t chunk_counter) {
  const std::size_t part = window_part_chunks<Budget>(num_chunks);
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
  // Every part pairs with a sibling all the way up: parallel_hasher
  // rounds its buffer down to a power-of-two number of chunks and part
  // is a power of two, so n_parts is one too.
  assert(std::has_single_bit(n_parts));
  std::array<std::uint32_t, 8> window_cv;
  fold_sibling_cvs(ops, std::span{cvs}.first(n_parts), h.key_words(),
                   h.mode_flags(), window_cv);
  h.push_subtree_cv(window_cv, num_chunks);
}

}  // namespace detail

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
