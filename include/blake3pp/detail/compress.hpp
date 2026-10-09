#pragma once

// The compress primitives the parallel engines share: the stack budget
// callers spend on a part table, how an input is cut into parts, and one
// window's compress stage as a sender.
//
// They live below <blake3pp/parallel.hpp> because the file pipeline
// needs them and the parallel API needs the file pipeline; the public
// names defined here (stack_budget and its default) are supplied to
// users by <blake3pp/parallel.hpp>.
//
// Internal to the library. Names an execution provider.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sched.h>  // sched_getcpu() for agent_record::cpu
#endif

#include <blake3pp/core.hpp>
#include <blake3pp/detail/ex.hpp>
#include <blake3pp/detail/tree_reducer.hpp>
#include <blake3pp/trace.hpp>

namespace blake3pp {

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
/// @note A full window's grouped reduction keeps a second table of
/// parts() x simd_degree chaining values, 512 KiB at most for the default
/// budget on a 16-lane kernel; it lives on the heap with the window that
/// owns it, allocated when the window first needs it and kept.
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

// One CV slot per part. The slots are unpadded: each is written once per
// part-sized task, so neighbours sharing a cache line cost nothing
// measurable.
template <stack_budget Budget>
using part_cvs = std::array<std::array<std::uint32_t, 8>, Budget.parts()>;

// A full window's parts are reduced in groups of this many, by the agent
// that finishes a group's last part, before the fold after the bulk sees
// anything. A part reduced on its own ends in log2(part) parent
// generations below full lane occupancy, a measured 15% of a 16-chunk
// part's time on AVX2 and 22% on AVX-512; folded 16 parts at a time the
// generations stay lane-filling down to the last log2(degree), 4% and 8%.
// The reduction has to happen inside the bulk: moved whole into the fold
// behind it, which one agent runs, it is a sixteenth of the window's
// blocks on one thread, and from eight agents up that costs more wall
// time than the parts saved.
inline constexpr std::size_t fold_group_parts = 16;
static_assert(sizeof(std::array<std::uint32_t, 8>) == stack_budget::bytes_per_part);

// What one bulk over a window's parts records when tracing: the agents' summed
// compress time and their count, each one atomic add per bulk invocation,
// and through buf one agent_record per invocation when the buffer wants
// them. window is the id the caller gives the records; the sender
// does not know what it is running over.
struct part_trace {
  trace_buffer* buf;
  std::uint64_t window;
  std::uint32_t part_chunks;
  std::atomic<std::uint64_t> busy_ns{0};
  std::atomic<std::uint32_t> active{0};
};

// The schedule-and-bulk sender that runs body(i) for every i in [0, n).
// The file pipeline starts one per window and lets the driver loop
// carry on.
//
// The bulk shape only provides the agents: each call pulls indices from
// `next` until none are left, so the split follows each agent's actual
// speed rather than the fixed shares the provider's bulk may hand out.
// Every index runs exactly once however the implementation distributes
// the calls.
//
// `body` and `next` are the caller's and must outlive the sender; the
// pipeline keeps them in the window object.
//
// Traced is a template parameter and not a null check inside the body:
// the untraced body is the hot one and carries no branch, and a pipeline
// that connects one sender per window needs the type settled before the
// run starts.
//
// The traced body reads the clock once before its first part and, when
// agent records are wanted, once after every part (the end of one is the
// start of the next). Per bulk invocation, not per part: two atomic adds
// on pt and at most one record claim. An invocation that finds the
// counter exhausted records nothing and is not an active agent.
// Whether the agents should stop claiming parts. An unstoppable token
// -- what every caller but the file pipeline passes -- makes this a
// compile-time false, so the loops below keep their exact shape and the
// hot path pays nothing.
template <class Token>
[[nodiscard]] constexpr bool parts_stopped(const Token& tok) noexcept {
  if constexpr (ex::unstoppable_token<Token>) {
    return false;
  } else {
    return tok.stop_requested();
  }
}

template <bool Traced, class Scheduler, class Body,
          class Token = ex::never_stop_token>
[[nodiscard]] auto part_bulk_sender(Scheduler& sched, std::size_t n,
                                    Body& body, std::atomic<std::size_t>& next,
                                    part_trace* pt, Token tok = {}) {
  if constexpr (Traced) {
    return ex::schedule(sched) |
           ex::bulk(ex::par, n, [n, &body, &next, pt,
                                tok](std::size_t) noexcept {
             if (parts_stopped(tok)) {
               return;
             }
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
                 if (parts_stopped(tok)) {
                   break;
                 }
                 i = next.fetch_add(1, std::memory_order_relaxed);
               } while (i < n);
             } else {
               do {
                 body(i);
                 ++parts;
                 if (parts_stopped(tok)) {
                   break;
                 }
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
           ex::bulk(ex::par, n, [n, &body, &next, tok](std::size_t) noexcept {
             while (!parts_stopped(tok)) {
               const std::size_t i =
                   next.fetch_add(1, std::memory_order_relaxed);
               if (i >= n) {
                 return;
               }
               body(i);
             }
           });
  }
}


}  // namespace detail

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
// A window's compress stage keeps the part table and the pull counter in its
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

  // One CV per part on the edge path, one per group on the grouped one;
  // what the fold after the bulk reads.
  part_cvs<Budget> cvs;
  std::atomic<std::size_t> next{0};   // the agents pull their parts from here
  std::array<std::uint32_t, 8> cv{};  // where the fold leaves the window's CV
  part_trace pt{nullptr, 0, 0};

  // The grouped reduction of a full window (fold_group_parts). Each part
  // leaves its `width` chaining values in lanes, at i * width; the agent
  // whose part completes a group folds that group's slots into cvs[g].
  // lanes holds n_parts * width CVs, grown on the first window that
  // needs the size and kept, 512 KiB at most for the default budget on
  // a 16-lane kernel; the rest of the window's state is fixed-size.
  bool grouped = false;
  std::size_t group = 0;  // parts per group
  std::size_t width = 0;  // CVs per part: the kernel's simd_degree
  std::vector<std::array<std::uint32_t, 8>> lanes;
  std::array<std::atomic<std::uint32_t>,
             std::max<std::size_t>(1, Budget.parts() / fold_group_parts)>
      done;

  window_compress() = default;
  window_compress(const window_compress&) = delete;
  window_compress& operator=(const window_compress&) = delete;

  // Takes a full, counter-aligned window. False means it is too small to
  // fan out and belongs on the sequential path, the same verdict
  // a window's compress stage reaches.
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
    grouped = true;
    group = std::min(fold_group_parts, n_parts);
    width = simd_degree(o);
    if (lanes.size() < n_parts * width) {
      lanes.resize(n_parts * width);
    }
    for (auto& count : done) {
      count.store(0, std::memory_order_relaxed);
    }
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
    grouped = false;
  }

  // How many CVs the fold after the bulk reduces.
  [[nodiscard]] std::size_t fold_inputs() const noexcept {
    return grouped ? n_parts / group : n_parts;
  }

  void operator()(std::size_t i) noexcept {
    const std::byte* d = data + i * part * chunk_size;
    const std::uint64_t counter = chunk_counter + i * part;
    if (!grouped) {
      compress_subtree_cv(ops, d, part, counter, key, flags, cvs[i]);
      return;
    }
    // A part is at least min_part_chunks chunks, never fewer than the
    // kernel's lanes, so every part leaves exactly `width` CVs and the
    // group's slots are contiguous.
    [[maybe_unused]] const std::size_t n = compress_subtree_cvs(
        ops, d, part, counter, key, flags,
        std::span{lanes}.subspan(i * width, width));
    assert(n == width);
    // The count's release pairs with the completing agent's acquire, so
    // the group's slots are visible to whoever folds them. The fold is
    // charged to that agent's part in the trace.
    const std::size_t g = i / group;
    if (done[g].fetch_add(1, std::memory_order_acq_rel) + 1 == group) {
      fold_sibling_cvs(ops, std::span{lanes}.subspan(g * group * width, group * width),
                       key, flags, cvs[g]);
    }
  }
};

// One window's compress stage as a sender: the provider's bulk over the
// parts, then the fold that turns their CVs into the window's own. Value:
// the window CV.
//
// Where this runs, which is why it is a sender:
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
template <bool Traced, stack_budget Budget, class Scheduler,
          class Token = ex::never_stop_token>
[[nodiscard]] auto compress_on(Scheduler& sched, window_compress<Budget>& w,
                               Token tok = {}) {
  return part_bulk_sender<Traced>(sched, w.n_parts, w, w.next, &w.pt, tok) |
         ex::then([&w]() noexcept {
           // The fold runs even when the agents stopped early: the part
           // table is zeroed with the window it lives in, so this reads
           // nothing uninitialised, and what it produces is dropped by
           // the stop check behind this stage rather than branched
           // around here, on the path every window takes.
           //
           // Every group pairs with a sibling all the way up: the window
           // is a power-of-two number of chunks and part and group are
           // powers of two, so the group count is one too.
           const std::size_t n = w.fold_inputs();
           assert(std::has_single_bit(n));
           if (n == 1) {
             w.cv = w.cvs[0];
           } else {
             fold_sibling_cvs(w.ops, std::span{w.cvs}.first(n), w.key,
                              w.flags, w.cv);
           }
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
template <bool Traced, stack_budget Budget, class Scheduler,
          class Token = ex::never_stop_token>
[[nodiscard]] auto compress_edge_on(Scheduler& sched,
                                    window_compress<Budget>& w,
                                    std::span<tree_reducer::node> out,
                                    Token tok = {}) {
  return part_bulk_sender<Traced>(sched, w.n_parts, w, w.next, &w.pt, tok) |
         ex::then([&w, out]() noexcept {
           return fold_aligned_runs(
               w.ops, w.key, w.flags,
               cv_run{std::span(w.cvs).first(w.n_parts), 0, w.n_parts,
                      w.chunk_counter, w.part},
               out);
         });
}

}  // namespace detail

}  // namespace blake3pp
