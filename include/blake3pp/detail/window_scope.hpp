#pragma once

// The bounded scope: the windows of one run, how many of them are in
// flight, and the cells their sender chains live in, over the loop and
// senders of driver_loop.hpp and the plan of pipeline_plan.hpp.
// run_window_pipeline() runs one open file through it.
//
// Internal to the pipeline. Names an execution provider, so
// <blake3pp/io.hpp> must not reach it.

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/compress.hpp>
#include <blake3pp/detail/contract.hpp>
#include <blake3pp/detail/driver.hpp>
#include <blake3pp/detail/driver_loop.hpp>
#include <blake3pp/detail/ex_compat.hpp>
#include <blake3pp/detail/io_geometry.hpp>
#include <blake3pp/detail/pipeline_plan.hpp>
#include <blake3pp/detail/tree_reducer.hpp>
#include <blake3pp/detail/window_planner.hpp>
#include <blake3pp/dispatch.hpp>
#include <blake3pp/trace.hpp>

namespace blake3pp::detail {

// --------------------------------------------------------------------
// The bounded scope.

// What the scope needs that the public options do not already say.
struct pipeline_options {
  // The requests plan_pipeline() rounds and clamps; the scope itself
  // reads only the plan.
  std::size_t window_bytes = default_window_bytes;
  unsigned queue_depth = default_queue_depth;
  // The blocking entry points' request: run_window_pipeline() turns it
  // into the tracer the scope is instantiated with. The scope itself
  // reads only its tracer, so a caller that constructs a scope passes
  // the policy and this field is not consulted.
  trace_buffer* trace = nullptr;
  // What a window record names as its file: the file's index in a
  // many-files operation, zero otherwise.
  std::uint32_t file_index = 0;
  // Windows allowed in flight at once, capped to the window count; 0
  // means the window count. The tests run the whole matrix at 1 first,
  // where every insert arrives in file order, before letting the real
  // count expose the out-of-order path.
  unsigned in_flight_cap = 0;
  // Reducer nodes the scope will use, 0 for all of them. A test lowers
  // it to watch admission throttle; it has to stay above the
  // decomposition size of the file it is used on (see the liveness
  // argument at admit_more()), which only a caller that knows the file
  // can judge.
  std::size_t reducer_capacity = 0;
  // A stop the caller owns. The scope watches it for the whole run and
  // requests its own stop from it, so a token requested from any thread
  // reaches the driver through the one member another thread may call.
  // The default token has no state and costs one stop_possible() test at
  // the start of the run, which is what the blocking entry points pass.
  ex::inplace_stop_token stop_token{};
};

// One window of a run: where its bytes are, the part table its compress
// stage fills, and the read operation the driver owes. Allocated by the
// pool rather than the scope, so a context can hold one set of these
// and lend them to whichever pipeline is running.
template <stack_budget Budget>
struct pipeline_window {
  // Where this window's bytes live, as the driver says: what the read
  // fills, and what the pipeline reads back. For an arena the two name
  // the same bytes; for a source already in memory the first is empty,
  // because there is nothing to move.
  std::span<std::byte> read_into;
  std::span<const std::byte> bytes_view;
  // Whether bytes_view's CONTENT is defined yet. Set when the read
  // completes, cleared when the window is taken for the next one: an
  // arena window names its slice from the moment it is taken, and what
  // is in it belongs to the driver until poll() says otherwise.
  bool view_ready = false;
  window_compress<Budget> compress;
  std::uint64_t index = 0;
  std::uint64_t offset = 0;
  std::size_t chunks = 0;
  std::size_t bytes = 0;
  window_record* rec = nullptr;
  // When the pipeline asked the driver for this window's bytes, on
  // steady_clock. Kept whether or not a trace buffer was given: the
  // device counters driver_stats reports are differences of this and
  // the landing, and a counter that only worked for a tracing caller
  // would be a different pipeline from the one everyone else runs.
  std::int64_t t_read_begin = 0;
  // The pool's free list and the scope's list of what it currently
  // holds. A window is on exactly one of them, never both.
  pipeline_window* next_free = nullptr;
  pipeline_window* next_taken = nullptr;
  unsigned slot = 0;      // which window of the pool this is
  unsigned cell = 0;      // which operation cell of the borrowing scope
  bool last = false;
  bool short_first = false;
  bool reading = false;      // submitted, and the read has not landed
  bool cancel_asked = false;  // the sweep has already asked for this read
  window_plan plan;
  std::size_t nodes = 0;  // nodes the fold produced

  // Where this window's fold leaves its nodes when the window is a run
  // of parts rather than one subtree.
  //
  // Per window, not one buffer per edge. A seekable file has at most two
  // such windows -- the short first one and the last -- but a stream has
  // no such bound: once a producer has left one window short the grid is
  // off its boundary, and every window after it is a run of parts until
  // the next one brings the grid back. Any of them may be in flight at
  // once, so the storage belongs to the window.
  //
  // The bound is this run's own worst case rather than the message
  // ceiling's: fold_aligned_runs() produces one ascending and one
  // descending chain of nodes, at most one per level of the run, and
  // window_compress::prepare_edge() refuses a run of more parts than the
  // budget's table holds.
  static constexpr std::size_t node_cap = nodes_for(Budget.parts());
  std::array<tree_reducer::node, node_cap> fold_nodes{};

  // Where this window's bytes are, which is settled when the window is
  // taken. Only the ADDRESS is: use this to record where a later stage
  // will read, never to read now.
  [[nodiscard]] const std::byte* at() const noexcept {
    return bytes_view.data();
  }

  // The window's bytes themselves, for everything that compresses or
  // absorbs them. The assert is the contract the arena needs and the
  // in-memory source satisfies trivially; it is live in the sanitizer
  // lanes.
  [[nodiscard]] std::span<const std::byte> view() const noexcept {
    assert(view_ready &&
           "a window's bytes are only defined once its read has "
           "completed: the view names the storage from the moment the "
           "window is taken, the content arrives in poll()");
    return bytes_view;
  }
  // The read's operation, alive for as long as the window object: the
  // driver may still owe this read after the chain's cell is gone, and
  // the stop sweep names it from here. Last, so the fields the agents
  // touch keep their offsets.
  io_op op{};
};

// The window storage of a run: the driver's arena, the window objects
// over it and their part tables, allocated and faulted once. A scope
// borrows windows from here and gives them back; the pool never gives
// out the same window twice.
//
// It is separate from the scope because the storage outlives any one
// run: a context holds one pool and lends it to whichever pipeline is
// running, so several files can share an arena instead of each faulting
// in its own.
template <stack_budget Budget>
class window_pool {
 public:
  using window = pipeline_window<Budget>;

  // The arena is the driver's, so that it outlives every read the
  // teardown has to drain; the window objects are this pool's.
  template <file_driver D>
  window_pool(D& drv, unsigned windows, std::size_t window_bytes)
      : count_(windows),
        window_bytes_(window_bytes),
        windows_(std::make_unique<window[]>(windows)) {
    // The fold of a stopped compress stage depends on a fresh window's
    // part table being zero: the agents stop claiming parts where the
    // stop found them, and the fold still reads every slot, including the
    // ones nobody wrote. The table zero-initialises itself
    // (window_compress::cvs in compress.hpp). No standard trait states
    // that, so the check is the outcome itself.
    static_assert(
        [] {
          const window probe{};
          for (const auto& part : probe.compress.cvs) {
            for (const std::uint32_t word : part) {
              if (word != 0) {
                return false;
              }
            }
          }
          return true;
        }(),
        "a window's part table must value-initialise to zero: the fold "
        "of a stopped compress stage reads part slots no agent claimed");
    drv.reserve(count_, window_bytes_);
    for (unsigned i = count_; i > 0; --i) {
      windows_[i - 1].slot = i - 1;
      push_free(&windows_[i - 1]);
    }
  }

  window_pool(const window_pool&) = delete;
  window_pool& operator=(const window_pool&) = delete;

  [[nodiscard]] unsigned count() const noexcept { return count_; }
  [[nodiscard]] std::size_t window_bytes() const noexcept {
    return window_bytes_;
  }
  [[nodiscard]] bool empty() const noexcept { return free_ == nullptr; }

  // The next free window, or null when every one is lent out. Borrowing
  // is the admission point a scope stalls on.
  [[nodiscard]] window* take() noexcept {
    window* const w = free_;
    if (w != nullptr) {
      free_ = w->next_free;
      w->next_free = nullptr;
    }
    return w;
  }

  void give_back(window* w) noexcept { push_free(w); }

 private:
  void push_free(window* w) noexcept {
    w->next_free = free_;
    free_ = w;
  }

  unsigned count_ = 0;
  std::size_t window_bytes_ = 0;
  std::unique_ptr<window[]> windows_;
  window* free_ = nullptr;
};

// What a run borrows rather than owns: the driver, the one run queue
// every stage publishes into, and the window pool. Whoever holds this
// outlives every run on it -- a blocking call builds one for its single
// run, a context keeps one for its lifetime and lends it to each
// pipeline it starts.
//
// There is exactly one loop per host, because the loop IS the thread
// that may touch the driver: several pipelines sharing a host share that
// thread, and each is driven a round at a time.
template <stack_budget Budget, file_driver D>
class pipeline_host {
 public:
  pipeline_host(D& drv, unsigned windows, std::size_t window_bytes)
      : drv_(drv), loop_(drv), pool_(drv, windows, window_bytes) {}

  pipeline_host(const pipeline_host&) = delete;
  pipeline_host& operator=(const pipeline_host&) = delete;

  [[nodiscard]] D& driver() noexcept { return drv_; }
  [[nodiscard]] driver_loop<D>& loop() noexcept { return loop_; }
  [[nodiscard]] window_pool<Budget>& pool() noexcept { return pool_; }

 private:
  D& drv_;
  driver_loop<D> loop_;
  window_pool<Budget> pool_;
};

template <class Scope, class FullOp, class ReadOnlyOp, class EdgeOp>
struct pipeline_cells;

// What a caller of window_scope spells. The op types come from the
// scope's own chain builders, and carrying them through the alias is
// what keeps the instantiation internal to this translation unit.
template <class Scope>
using pipeline_cells_for = pipeline_cells<
    Scope,
    ex::connect_result_t<decltype(std::declval<Scope&>().window_chain(
                             std::declval<typename Scope::window&>())),
                         typename Scope::scope_receiver>,
    ex::connect_result_t<decltype(std::declval<Scope&>().read_only_chain(
                             std::declval<typename Scope::window&>())),
                         typename Scope::scope_receiver>,
    ex::connect_result_t<decltype(std::declval<Scope&>().edge_chain(
                             std::declval<typename Scope::window&>())),
                         typename Scope::scope_receiver>>;

// Several window chains in flight over one file, all on the caller's
// thread.
//
// The shape of one window:
//
//     io.read(window)            // completes on the driver
//       | let_value(compress)    // provider bulk, then fold, on the pool
//       | continues_on(driver)   // back to the driver
//       | then(reduce_into_tree) // order-free
//
// and the scope is what decides how many of those exist at once: a
// window is a buffer, a part table and an operation cell, all allocated
// at construction, and a chain may only start when one is free and
// fewer than the plan's reads are still reading. The two bounds are
// separate on purpose: the device wants a few reads in flight, the pool
// wants windows to work on, and one number cannot serve both. The
// reducer is what lets them finish in any order.
template <tracer Tracer, stack_budget Budget, file_driver D, class Scheduler>
class window_scope
    : public pipeline_scope_base<window_scope<Tracer, Budget, D, Scheduler>,
                                 D> {
  // The base drives this scope's loop and sweep, both of which are its
  // own business and nobody else's.
  friend class pipeline_scope_base<window_scope, D>;

  // The base holds the stop source, the failure latch and the in-flight
  // count; these bring them in unqualified.
  using base = pipeline_scope_base<window_scope, D>;
  using base::ec_;
  using base::eptr_;
  using base::in_flight_;
  using base::latch_error;
  using base::latch_exception;
  using base::quiesce;
  using base::request_stop;
  using base::status_;
  using base::stop_requested;
  using base::transfers_;
  using base::transfers_owed_;
  using base::stop_source_;

  // The cell store is built outside this class, because the cell types
  // are the return types of the chain builders below and those are only
  // nameable once the class is complete.
  template <class Scope, class FullOp, class ReadOnlyOp, class EdgeOp>
  friend struct pipeline_cells;

 public:
  using cv_type = std::array<std::uint32_t, 8>;
  // The window objects are the pool's; the scope only borrows them.
  using window = pipeline_window<Budget>;

  using typename base::outer_stop_callback;
  using typename base::request_scope_stop;
  using scope_receiver = typename base::template receiver<window>;
  static_assert(std::copy_constructible<scope_receiver>);

  // What a caller holds for the length of a run so the stop stays
  // watched: begin() fills it, end() empties it, and it must outlive
  // every window in flight.
  using stop_watch = std::optional<outer_stop_callback>;


  // The host is the caller's: a blocking entry point builds one beside
  // the scope, a context keeps one and lends it to every pipeline it
  // runs. Either way it outlives the scope, because a read the teardown
  // drains is still targeting its arena.
  window_scope(hasher& h, pipeline_host<Budget, D>& host,
               typename D::file& file, Scheduler sched,
               const pipeline_plan& plan, const pipeline_options& opts,
               Tracer tracer = Tracer{})
      : h_(h),
        loop_(host.loop()),
        pool_(host.pool()),
        file_(file),
        sched_(std::move(sched)),
        trace_(tracer.buffer()),
        file_index_(opts.file_index),
        // The hasher's own table, never one resolved from its variant:
        // that would quietly replace a caller-supplied table with the
        // library's.
        ops_(kernels(h)),
        count_(std::min(plan.windows, host.pool().count())),
        reads_in_flight_(plan.reads_in_flight),
        in_flight_cap_(opts.in_flight_cap == 0
                           ? count_
                           : std::min<unsigned>(opts.in_flight_cap, count_)),
        planner_(plan, h.count(), file.size()),
        nodes_total_(
            reducer_nodes_for(plan.windows, Budget.parts(), plan.streaming)),
        reducer_capacity_(opts.reducer_capacity == 0
                              ? nodes_total_
                              : std::min(opts.reducer_capacity, nodes_total_)),
        opts_stop_token_(opts.stop_token),
        nodes_(std::make_unique<tree_reducer::node[]>(nodes_total_)),
        reducer_(ops_, h.key_words(), h.mode_flags(),
                 std::span<tree_reducer::node>(nodes_.get(), nodes_total_)) {
    // Where each window's bytes actually are is the driver's answer, and
    // it is asked per window, because a source already in memory has a
    // different view for every offset.
    // Checked in every build: a read of a window into a slot the pool
    // sized smaller runs off the end of the arena, which the driver hands
    // out by slot without a length of its own.
    require(host.pool().window_bytes() >= plan.window_bytes,
            "update_file(): a host whose windows are smaller than this run "
            "plans to read");
    // One operation cell per window this run may hold at once. The cells
    // are the scope's; the windows are not, so a borrowed window carries
    // the index of the cell it was lent with. Handed out lowest first,
    // which keeps a run that never fills the pool on the same few cells.
    // Reserved to the cell count once, so release() never reallocates.
    free_cells_.reserve(count_);
    for (unsigned i = 0; i < count_; ++i) {
      free_cells_.push_back(count_ - 1 - i);
    }
  }

  // Over a file the caller opened itself, planned from the requests in
  // opts; what the tests construct directly.
  window_scope(hasher& h, pipeline_host<Budget, D>& host,
               typename D::file& file, Scheduler sched,
               const pipeline_options& opts)
      : window_scope(h, host, file, std::move(sched),
                     plan_pipeline(h.count(), opts.window_bytes,
                                   opts.queue_depth, false, file.size()),
                     opts) {}

  window_scope(const window_scope&) = delete;
  window_scope& operator=(const window_scope&) = delete;

  // Everything still borrowed goes back to the pool.
  //
  // The last window is held past the end of the run on purpose -- its
  // tail is the message end, absorbed after every other window -- and a
  // failed or stopped run may still hold whatever it had when it gave
  // up. Nothing is in flight here: the run either ended with nothing
  // outstanding or was quiesced, both of which are checked above this
  // point. A pool that outlives the scope would otherwise lose a window
  // per run, and with it eventually every window it has.
  // Nothing of this run may still be running: a window given back here
  // while a pool thread is inside its compress would be lent to another
  // pipeline and written into from two places. run() ends with nothing
  // outstanding or winds down through quiesce(), and quiesce() only
  // returns once every chain has either completed or been reclaimed from
  // the driver -- which leaves it counted, hence the comparison rather
  // than a zero.
  ~window_scope() {
    assert(in_flight_ == transfers_owed_ &&
           "a window was still on the pool when its scope went");
    give_back_all();
  }

  // The capacity the default storage has to clear, written from what the
  // scope actually reserves:
  //
  //   2 * 54                 the windows already read, as a contiguous
  //                          run, decompose into at most one node per
  //                          level twice over
  //   2 * nodes_for(parts)   the two edge windows -- a short first one
  //                          and the last one can be in flight together
  //                          -- each reserving what its own run of parts
  //                          can decompose into
  //
  // nodes_for is logarithmic in the part count, so even an absurd stack
  // budget leaves this far below the storage.
  static_assert(reducer_nodes > 2 * 54 + 2 * nodes_for(Budget.parts()),
                "reducer_nodes must clear a contiguous run's decomposition "
                "plus both edge windows' reservations, or admission stalls");

  // Watches the caller's stop and says whether there is a run at all.
  //
  // The callback does the two things that are safe from a thread that is
  // not the driver's: request this scope's stop, and nudge the driver
  // out of a blocking poll. Everything the stop then causes -- the
  // cancels, the admission halt, the completion -- happens on the driver
  // thread, in its own round.
  //
  // Destroying the watch is the hand-off back: an inplace_stop_callback's
  // destructor waits for a callback already running, so past that
  // object's lifetime no outer stop can still be arriving. The caller
  // owns it, because it has to outlive every window in flight.
  //
  // False means there is nothing to drive: an empty file, or a token
  // that was already stopped -- which stopped() tells apart.
  [[nodiscard]] bool begin(stop_watch& watch) {
    if (planner_.file_bytes() == 0) {
      return false;
    }
    if (opts_stop_token_.stop_possible()) {
      if (opts_stop_token_.stop_requested()) {
        // Stopped before anything was submitted. Nothing to cancel,
        // nothing to absorb, and the hasher is untouched.
        request_stop();
        return false;
      }
      watch.emplace(opts_stop_token_,
                    request_scope_stop{&stop_source_, &loop_.driver()});
    }
    return true;
  }

  // How a finished run ended, for a caller that has somewhere to report
  // it. The blocking entry points turn these into exceptions; a sender
  // turns them into completion signals.
  enum class outcome { value, error, exception, stopped };

  // The tail of a run: stop watching, record what the round counters
  // saw, and -- only if the run actually produced a tree -- absorb it
  // into the hasher.
  //
  // A stopped or failed run absorbs nothing: the reducer's nodes are
  // discarded with it, and the last window is not taken into the hasher.
  // What the hasher already took -- the heads of the edge windows that
  // had landed -- stays.
  [[nodiscard]] outcome end(stop_watch& watch) {
    // Whatever this run still holds goes back before it reports, on
    // every path. Holding one window past the completion is what turns a
    // shared pool into a deadlock: with more runs than windows, each
    // would keep one and none could ever start.
    struct give_back_guard {
      window_scope* scope;
      ~give_back_guard() { scope->give_back_all(); }
    } const guard{this};
    // Nothing is in flight, so no stop can still be on its way into a
    // stage; the watch is done before anything is reported.
    watch.reset();

    // A run that ends while the pool is empty has an open starved
    // interval, and the counter is reported from here.
    transfers_.end_starved();
    if (trace_ != nullptr) {
      driver_stats& d = trace_->driver();
      // The rung is the file's, which closes with the run: copied here so a
      // trace read afterwards still names it.
      set_read_backend(d, file_.name());
      d.admission_stalls = admission_stalls_;
      d.max_pending = max_pending_;
      d.max_reads_in_flight = max_reading_;
      d.max_windows_in_flight = max_windows_;
      d.queue_depth_final = reads_in_flight_;
      d.queue_depth_peak = reads_in_flight_;
      d.agent_busy_ns = agent_busy_ns_.load(std::memory_order_relaxed);
      transfers_.report(d, loop_.driver());
    }
    if (eptr_) {
      return outcome::exception;
    }
    if (ec_) {
      return outcome::error;
    }
    if (stop_requested()) {
      return outcome::stopped;
    }
    // Every complete window is a subtree of the final tree and goes in
    // through the reducer; the last one carries the message end, so it
    // can only ever be hashed by the hasher itself, and last.
    reducer_.drain_into(h_);
    if (last_window_ != nullptr) {
      window& w = *last_window_;
      // Only the tail: the parts before it are already in the tree.
      h_.update(std::span<const std::byte>(
          w.view().data() + (w.bytes - w.plan.tail_bytes),
          w.plan.tail_bytes));
      if (w.rec != nullptr) {
        // The last window is never folded, so its join and its absorb
        // are the same instant: the hasher took it whole.
        w.rec->t_joined = w.rec->t_absorbed = trace_->now();
      }
      stamp_released(w);
    }
    return outcome::value;
  }

  // The error a failed run carries, for the caller that has to report
  // it. Only meaningful for the matching outcome.
  [[nodiscard]] std::error_code error() const noexcept { return ec_; }
  [[nodiscard]] std::exception_ptr exception() const noexcept {
    return eptr_;
  }

  // What the loop reads to decide whether this run is over and whether
  // anything is still owed. A caller driving several runs on one loop
  // sums the second across them and ends when every first is set.
  [[nodiscard]] const loop_status& status() const noexcept { return status_; }

  // Waits out every window still in flight without running another
  // round, for a caller whose round threw: the cells cannot be
  // destroyed while a pool thread is about to publish into one. What
  // the run held goes back to the pool here, since end() will not be
  // called for it; a window kept past the failure is what starves the
  // runs sharing the pool.
  void abandon() noexcept {
    quiesce();
    give_back_all();
  }

  // Whether a stop has taken this run, for a caller reporting a run that
  // never started.
  [[nodiscard]] bool stopped() const noexcept { return stop_requested(); }

  // How many windows this run may hold at once, which is how many
  // operation cells its caller has to provide.
  [[nodiscard]] unsigned count() const noexcept { return count_; }

  // Drives the whole file on this thread and absorbs it into the hasher.
  //
  // Ends exactly once, and only after everything in flight has finished:
  // with the file in the hasher, with the first failure any window
  // reported, or with operation_canceled if the run was stopped and
  // nothing failed. A stopped or failed run absorbs nothing further --
  // the reducer's nodes are discarded and the last window is not taken
  // -- and the hasher keeps whatever it had already absorbed.
  void run() {
    stop_watch watch;
    if (!begin(watch)) {
      throw_if_stopped();
      return;
    }
    // The operation cells are a local, so an exception leaving run_until
    // would destroy them while a window can still be on the pool, about
    // to publish into one. The handler waits those windows out first;
    // the cells are still alive inside it.
    pipeline_cells_for<window_scope> cells(count_);
    try {
      loop_.run_until(
          status_, [&] { admit_round(cells); }, [this] { this->report_stall(); },
          trace_);
    } catch (...) {
      quiesce();
      throw;
    }
    assert(in_flight_ == 0 && "run_until returned with a window in flight");
    switch (end(watch)) {
      case outcome::exception:
        std::rethrow_exception(eptr_);
      case outcome::error:
        throw std::system_error(ec_, "blake3pp: reading a window");
      case outcome::stopped:
        throw_if_stopped();
        break;
      case outcome::value:
        break;
    }
  }

  // How often a window that had a free buffer and work left to do was
  // held back because the reducer had no node for it. Zero on any run
  // whose capacity is the default; the admission test is what reads it.
  [[nodiscard]] std::uint64_t admission_stalls() const noexcept {
    return admission_stalls_;
  }

 private:
  void throw_if_stopped() const {
    base::throw_if_stopped("blake3pp: the file pipeline was stopped");
  }

 public:
  // One round's worth of starts: every window that may start, started
  // before the one flush that follows. The loop calls this once per
  // round, which is what lets several pipelines share a loop -- each is
  // offered a round in turn and admits what it can.
  //
  // The cell types and the conversion functions that fill them are
  // run()'s locals, which is why they arrive as template parameters.
  template <class Cells>
  void admit_round(Cells& cells) {
    {
      if (stop_requested()) {
        sweep_cancels();
      }
      // Every window that can start, started before the one flush this
      // round: the reads of a whole batch reach the OS in one call.
      // A window holds a read slot from its submit until its read lands,
      // and a buffer until it has been reduced; the device is fed by the
      // first bound and the pool by the second.
      // `pool_.empty()` is the fourth wait condition: with several
      // pipelines on one context the windows may all be lent out, and
      // this one resumes when any of them gives one back.
      while (!stop_requested() && in_flight_ < in_flight_cap_ &&
             reading_ < reads_in_flight_ && !pool_.empty()) {
        if (planner_.exhausted()) {
          break;
        }
        // An edge window reserves room for a whole decomposition rather
        // than for one node, so its own cost is planned before it is
        // admitted.
        const std::size_t cost = planner_.next_window_cost();
        if (!admit_more(cost)) {
          break;
        }
        window* const w = pool_.take();
        // Built, then counted. Everything up to the connect is this
        // window's own state, and anything that throws there -- an
        // operation state a provider allocates, the lane table a window
        // shape needs, a plan the geometry contradicts -- gives the window
        // straight back, rather than leaving a count this run can never
        // work off or a window a shared pool never offers again. Starting
        // is last, because from then on the chain may complete and will
        // count itself down.
        const unsigned cell = free_cells_.back();
        bool is_full = false;
        bool is_edge_fanout = false;
        try {
          take_next(*w);
          if (w->bytes == 0) {
            // An empty stream: its end is only visible as a window with
            // nothing in it, and take_next() has just recorded where it
            // is. Any other stream ends on a window that holds bytes,
            // because the driver reads one byte past each window
            // (pipe_driver.hpp). Nothing to start, and the window goes
            // straight back.
            w->rec = nullptr;
            w->op.live = false;
            pool_.give_back(w);
            break;
          }
          claim_record(*w);
          // Three shapes. A full window is one subtree and inserts one
          // node. A window with ends the hasher owns -- the short first
          // one, the last one -- or one a stream left off the grid is a
          // run of parts and inserts their decomposition; it uses the
          // cell its own window index names, because a stream can have
          // several in flight at once. An end with nothing worth
          // fanning out is only a read, and the hasher takes it whole.
          is_full = !w->last && !w->short_first && w->plan.parts == 0;
          is_edge_fanout = !is_full && w->plan.parts >= 2;
          if (is_full) {
            prepare_compress(*w);
            cells.full[cell].emplace(typename Cells::connect_full{this, w});
          } else if (is_edge_fanout) {
            cells.edge[cell].emplace(typename Cells::connect_edge{this, w});
          } else {
            (w->last ? cells.last_read : cells.first_read)
                .emplace(typename Cells::connect_read{this, w});
          }
        } catch (...) {
          w->rec = nullptr;
          w->op.live = false;
          pool_.give_back(w);
          throw;
        }
        w->cell = cell;
        free_cells_.pop_back();
        w->next_taken = taken_;
        taken_ = w;
        ++in_flight_;
        w->reading = true;
        ++reading_;
        max_reading_ = std::max(max_reading_, reading_);
        max_windows_ = std::max(max_windows_, in_flight_);
        in_flight_nodes_ += w->plan.node_cost;
        status_.outstanding = in_flight_;
        if (w->last) {
          last_window_ = w;
        }
        if (is_full) {
          ex::start(*cells.full[w->cell]);
        } else if (is_edge_fanout) {
          ex::start(*cells.edge[w->cell]);
        } else {
          ex::start(*(w->last ? cells.last_read : cells.first_read));
        }
      }
      note_pool_wait();
      // A stop that arrives with nothing in flight ends the run here.
      // Without this the loop would reach the stall report, which exists
      // for a pipeline nobody can wake, and this one is over.
      settle();
    }
  }

  // What the loop does when it would sleep with nothing owed: for one
  // pipeline that is a configuration it cannot run.

 private:

  // The loop the base's quiesce drives, which is the host's.
  [[nodiscard]] driver_loop<D>& loop() noexcept { return loop_; }
  // What the base's teardown reclaims transfers from: this run's file,
  // never the whole driver, which may be serving other pipelines.
  [[nodiscard]] typename D::file& file() noexcept { return file_; }

 public:
  // The three chain builders, the receiver they complete and the
  // environment they carry are public because the cell store that
  // connects them lives outside this class: the cell types are these
  // builders' return types, which cannot be named until the class is
  // complete.

  // What every stage below the scope reads the scope's stop from; the
  // adaptors forward it down the chain.
  // One window, start to finish. The middle line is the compress stage;
  // everything around it is the same for every window.
  // The three chain builders are templates on a tag that is a different
  // type in every translation unit (an anonymous namespace), for the
  // reason RULE 6 in ex_compat.hpp gives one layer down: their return
  // types carry a provider's internal-linkage sender types, while a
  // non-template member's mangled name does not, so without the tag two
  // units' builders have one name and the linker keeps one body for
  // layouts that may differ.
  template <class Tag = tu_tag>
  [[nodiscard]] auto window_chain(window& w) {
    return ex::then(
        ex::continues_on(
            ex::let_value(loop_.read(file_, w.offset,
                                     w.read_into, w.op, transfers_owed_),
                          [this, &w]() {
                            return compress_stage(w);
                          }),
            loop_.scheduler()),
        [this, &w](const cv_type& cv) { reduce_into_tree(w, cv); });
  }

  // An edge window with nothing worth fanning out: read it, hold it, and
  // let the hasher take the whole thing at the end. Small last windows
  // land here, and so does a file below one window.
  template <class Tag = tu_tag>
  [[nodiscard]] auto read_only_chain(window& w) {
    return ex::then(loop_.read(file_, w.offset, w.read_into, w.op, transfers_owed_),
                    [this, &w]() noexcept {
                      read_landed(w);
                      // A short first window with nothing worth fanning
                      // out is all head, and the hasher takes it now; a
                      // last window is all tail, and the hasher takes it
                      // once the reducer has drained.
                      absorb_head(w);
                      stamp_joined(w);
                    });
  }

  // An edge window that has full parts in the middle: the parts go to
  // the pool like any other window's, but what comes back is the
  // canonical decomposition of that run rather than one chaining value,
  // because the window's ends belong to the hasher.
  //
  // Same three stages as the full window's chain and the same rules; it
  // exists separately because its value type is a node count, and
  // generalising the full window's chain to carry either would put a
  // branch on the path every window takes.
  template <class Tag = tu_tag>
  [[nodiscard]] auto edge_chain(window& w) {
    return ex::then(
        ex::continues_on(
            ex::let_value(loop_.read(file_, w.offset, w.read_into, w.op, transfers_owed_),
                          [this, &w]() {
                            return edge_compress_stage(w);
                          }),
            loop_.scheduler()),
        [this, &w](std::size_t nodes) { insert_edge_nodes(w, nodes); });
  }

 private:

  // The bytes an edge window owes the hasher before anything else can
  // reach it. Safe at any point before drain_into, because no reducer
  // node reaches the hasher until then -- and it has to happen before
  // the nodes do, since it is what puts the hasher on the boundary they
  // are numbered from.
  void absorb_head(window& w) noexcept {
    if (w.plan.head_bytes > 0) {
      h_.update(
          std::span<const std::byte>(w.view().data(), w.plan.head_bytes));
    }
  }

  // The two compress stages return provider senders too and are not
  // tagged, so two units share their names and the linker keeps one body
  // for both units' calls. That is harmless because the body builds the
  // same non-polymorphic layout in every unit: RULE 6's failure
  // (ex_compat.hpp) needs a layout that differs between units, met
  // through a vptr. A stage whose sender could differ by unit takes the
  // tag too.
  [[nodiscard]] auto edge_compress_stage(window& w) {
    read_landed(w);
    absorb_head(w);
    const window_plan& p = w.plan;
    w.compress.prepare_edge(ops_, w.view().data() + p.head_bytes,
                            p.part_chunks, p.parts, p.first_chunk,
                            h_.key_words(), h_.mode_flags(), trace_, w.index,
                            file_index_);
    const std::span<tree_reducer::node> out(w.fold_nodes);
    const ex::inplace_stop_token tok = stop_source_.get_token();
    return stamped<std::size_t>(
        w, tok, compress_edge_on<Tracer::traced>(sched_, w.compress, out, tok));
  }

  // On the driver, like every other insert. Not noexcept: the contract
  // checks below are the kind the chain carries as set_error(exception_ptr)
  // for run() to rethrow (contract.hpp), which a noexcept frame would turn
  // into a terminate.
  void insert_edge_nodes(window& w, std::size_t nodes) {
    // fold_aligned_runs() wrote these, bounded by the span it was given;
    // reading more than that span holds would walk off the array.
    require(nodes <= window::node_cap,
            "an edge window produced more nodes than its own storage holds");
    w.nodes = nodes;
    for (std::size_t i = 0; i < nodes; ++i) {
      const tree_reducer::node& n = w.fold_nodes[i];
      // Admission reserved a whole decomposition for this window, so a
      // refusal is capacity arithmetic that disagrees with itself. Checked
      // in every build: dropping the node silently is a subtree missing
      // from the tree, which is a wrong digest and no error.
      require(reducer_.insert(n.first_chunk, n.chunks, n.cv),
              "the reducer refused an edge window's node, which admission "
              "had reserved room for");
    }
    max_pending_ = std::max(max_pending_, reducer_.pending());
    if (w.rec != nullptr) {
      w.rec->t_absorbed = trace_->now();
    }
  }

  [[nodiscard]] auto compress_stage(window& w) {
    read_landed(w);
    const ex::inplace_stop_token tok = stop_source_.get_token();
    return stamped<cv_type>(
        w, tok, compress_on<Tracer::traced>(sched_, w.compress, tok));
  }

  // A compress stage's tail: stopped if the run was, and, when tracing,
  // the agents' record stamped on the way through.
  template <class Value, class Stage>
  [[nodiscard]] auto stamped(window& w, ex::inplace_stop_token tok,
                             Stage&& stage) {
    if constexpr (Tracer::traced) {
      return stopped_if<Value>(
          tok, ex::then(std::forward<Stage>(stage),
                        [this, &w](Value v) noexcept {
                          stamp_agents(w);
                          return v;
                        }));
    } else {
      return stopped_if<Value>(tok, std::forward<Stage>(stage));
    }
  }

  // The one stall more depth cannot help: bytes left to read, a free
  // read slot, and no window on the pool. That is the hash behind the
  // device, and the windows already in flight are buffering rather than
  // reading.
  //
  // The windows in flight are not part of this. An empty pool means
  // every window this run may hold is already out, so a term comparing
  // the count against its cap would be false wherever the rest is true,
  // and the counter would stay at zero on every run.
  void note_pool_wait() noexcept {
    transfers_.starved(pool_.empty() && !stop_requested() &&
                       !planner_.exhausted() &&
                       reading_ < reads_in_flight_);
  }

  // Why no window could start. Reached only from the driver loop, with
  // nothing in flight, so throwing here unwinds a pipeline that owes no
  // callbacks.
  [[noreturn]] void report_stall() const {
    throw std::logic_error(
        "blake3pp: the file pipeline cannot start a window and nothing is "
        "in flight. The reducer holds " +
        std::to_string(reducer_.pending()) + " of " +
        std::to_string(reducer_capacity_) + " nodes, " +
        std::to_string(in_flight_) + " windows are in flight (" +
        std::to_string(reading_) + " reading, of " +
        std::to_string(reads_in_flight_) + " allowed) holding " +
        std::to_string(in_flight_nodes_) + " reserved nodes, and the next "
        "window reserves " + std::to_string(planner_.next_window_cost()) +
        " more. The capacity has to exceed what the windows already read "
        "decompose into plus that reservation.");
  }

  // Whether one more window may start.
  //
  // Every chain in flight will insert exactly one node when it finishes,
  // so a window may only start while the reducer has a node free for it.
  // That is what makes insert() total here: it can refuse only when the
  // storage is full and no merge happened, and this rule leaves room for
  // each chain before it starts.
  //
  // A full window inserts one node when it finishes and reserves one. An
  // edge window inserts the decomposition of its run of parts instead,
  // up to 2 * 54 nodes, and reserves that many from the moment it
  // starts: reserving one and inserting a hundred is how the refusal
  // this rule exists to prevent would come back.
  //
  // The rule cannot stall the pipeline. Reads start in file order, so
  // when nothing is in flight the windows that have finished are a
  // contiguous run from the beginning, and a contiguous run of aligned
  // subtrees decomposes into at most one ascending and one descending
  // chain of nodes, one per level: 2 * 54 at BLAKE3's 2^64-byte limit.
  // The capacity is above that plus what one edge window reserves, so
  // with nothing in flight there is always a window that may start.
  [[nodiscard]] bool admit_more(std::size_t cost) noexcept {
    if (reducer_.pending() + in_flight_nodes_ + cost <= reducer_capacity_) {
      return true;
    }
    ++admission_stalls_;
    return false;
  }

  // Takes the next window's geometry and plan into w and advances.
  //
  // Not noexcept: window_storage_for() checks the geometry against the
  // plan, and admission runs before the window is counted in flight, so
  // the throw unwinds a round that owes nothing.
  void take_next(window& w) {
    w.cancel_asked = false;
    w.view_ready = false;
    // Before the driver is asked, not after: the device counters are
    // differences of this timestamp and the one read_landed() takes, and
    // for a stream the asking IS the transfer -- the driver fills the
    // window where it names it, because the window's length is what the
    // subtrees are planned from and a stream only knows the length once
    // it has the bytes. One clock read per window either way.
    w.t_read_begin = steady_now_ns();
    window_geometry g = planner_.next_geometry();
    const window_storage storage =
        loop_.driver().window_storage_for(file_, w.slot, g.offset, g.bytes);
    if (planner_.streaming()) {
      g = planner_.observe(g, storage.view.size(), file_.size());
    }
    w.plan = planner_.plan_window(g);
    w.offset = g.offset;
    w.bytes = g.bytes;
    w.last = g.last;
    w.short_first = g.short_first;
    w.chunks = w.bytes / chunk_size;
    w.index = index_++;
    w.nodes = 0;
    w.read_into = storage.read_into;
    w.bytes_view = storage.view;
    if (planner_.streaming() && w.bytes > 0) {
      planner_.check_streamed_window(g, w.plan);
    }
    planner_.advance(w.bytes);
  }

  // Runs at admission, BEFORE the read: what it records is where the
  // compress stage will find the window, and that stage only starts once
  // the read has completed. Hence at() rather than view().
  // Not noexcept: prepare() grows the lane table for this window's shape.
  void prepare_compress(window& w) {
    const bool fanned =
        w.compress.prepare(ops_, w.at(), w.chunks,
                           w.plan.first_chunk, h_.key_words(), h_.mode_flags(),
                           trace_, w.index, file_index_);
    // update_file only reaches the pipeline for windows big enough to fan
    // out; a window that is not is exactly what the fallback path exists
    // for.
    assert(fanned && "a window too small to fan out reached the pipeline");
    (void)fanned;
  }

  // Not noexcept, for the reason insert_edge_nodes() gives.
  void reduce_into_tree(window& w, const cv_type& cv) {
    // Admission reserved this node before the window started; the same
    // reasoning as the edge path's, and the same consequence if it is
    // wrong.
    require(reducer_.insert(w.plan.first_chunk, w.chunks, cv),
            "the reducer refused a window's node, which admission had "
            "reserved room for");
    max_pending_ = std::max(max_pending_, reducer_.pending());
    if (w.rec != nullptr) {
      w.rec->t_absorbed = trace_->now();
    }
  }

  // What every completion but a successful one owes: the read slot back
  // if the read never landed, the window off the in-flight count, and
  // the reducer nodes it reserved released, since they are now either in
  // the reducer or never coming.
  void finish_window(window& w) noexcept {
    release_read_slot(w);
    --in_flight_;
    in_flight_nodes_ -= w.plan.node_cost;
    if (&w != last_window_) {
      release(w);
    }
    settle();
  }

  void settle() noexcept {
    status_.outstanding = in_flight_;
    if (in_flight_ > 0) {
      return;
    }
    if (stop_requested() || planner_.exhausted()) {
      status_.done = true;
    }
  }

  // Asks the device to abandon the reads that have not landed. On the
  // driver thread, once per read: the sweep runs every round while stop
  // is requested, and a read that has already been asked for is not
  // asked again.
  //
  // Nothing completes here. A cancelled read still owes its callback and
  // still holds its buffer, and only poll() can deliver either.
  void sweep_cancels() noexcept {
    for (window* w = taken_; w != nullptr; w = w->next_taken) {
      // Only a window whose read the driver actually has: a window
      // admitted in the same round that a stop arrived may have parked
      // without submitting, and latching it here would stop the sweep
      // asking for the read that follows.
      if (w->reading && w->op.live && !w->cancel_asked) {
        w->cancel_asked = true;
        loop_.driver().cancel(w->op);
      }
    }
  }

  // Returns every window this scope still holds. Legal only with
  // nothing in flight: the run either ended with nothing outstanding or
  // was quiesced, both of which are established before this is reached.
  //
  // What is normally left here is the last window, which is held past
  // the end of the run on purpose -- its tail is the message end,
  // absorbed after every other window -- plus whatever a failed or
  // stopped run still had when it gave up.
  void give_back_all() noexcept {
    while (taken_ != nullptr) {
      window* const w = taken_;
      taken_ = w->next_taken;
      w->next_taken = nullptr;
      w->reading = false;
      w->rec = nullptr;
      // Nothing is in flight here, and a drain forgets without a callback,
      // so the driver owes this operation nothing -- whoever takes the
      // window next must not read a transfer that was forgotten.
      w->op.live = false;
      pool_.give_back(w);
    }
    last_window_ = nullptr;
  }

  void release(window& w) noexcept {
    stamp_released(w);
    // Off this scope's list before it goes back on the pool's: a window
    // is on exactly one of the two.
    window** link = &taken_;
    while (*link != nullptr && *link != &w) {
      link = &(*link)->next_taken;
    }
    assert(*link == &w && "a window released twice, or never taken");
    *link = w.next_taken;
    w.next_taken = nullptr;
    free_cells_.push_back(w.cell);
    pool_.give_back(&w);
  }

  void claim_record(window& w) noexcept {
    // t_read_begin was stamped by take_next(), before the driver was
    // asked for the window: see the note there.
    w.rec = trace_ != nullptr ? trace_->claim_window() : nullptr;
    if (w.rec != nullptr) {
      w.rec->index = w.index;
      w.rec->bytes = w.bytes;
      w.rec->flags =
          (w.last ? window_record::flag_last : 0) |
          (w.short_first ? window_record::flag_short_first : 0) |
          (!w.last && !w.short_first && w.plan.parts > 0
               ? window_record::flag_part_run
               : 0);
      w.rec->slot = w.slot;
      w.rec->file = file_index_;
      w.rec->t_wait_begin = w.t_read_begin - trace_->epoch_ns();
    }
  }

  // The read is in memory: the window gives its read slot back and
  // keeps its buffer. On the driver, inside the read's completion.
  void read_landed(window& w) noexcept {
    w.view_ready = true;
    release_read_slot(w);
    const std::int64_t t = transfers_.land(w.t_read_begin, w.bytes);
    if (w.rec != nullptr) {
      w.rec->t_ready = t - trace_->epoch_ns();
    }
  }

  void release_read_slot(window& w) noexcept {
    if (w.reading) {
      w.reading = false;
      --reading_;
    }
  }

  // A window that never reaches the pool has no separate join: its
  // compute is the hasher's, at the end.
  void stamp_joined(window& w) noexcept {
    if (w.rec != nullptr) {
      w.rec->t_joined = trace_->now();
    }
  }

  void stamp_agents(window& w) noexcept {
    const std::uint64_t busy =
        w.compress.pt.busy_ns.load(std::memory_order_relaxed);
    // Outside the record check: the pool's busy share is what says
    // whether the hash is the constraint, and a caller may want it
    // without paying for one record per window. The window's agents
    // have already summed their own time into this counter, so this is
    // one relaxed add per fanned window on the agent that finished it;
    // the driver reads it back once, in end().
    agent_busy_ns_.fetch_add(busy, std::memory_order_relaxed);
    if (w.rec != nullptr) {
      w.rec->t_joined = trace_->now();
      w.rec->agent_busy_ns = busy;
      w.rec->agents_active =
          w.compress.pt.active.load(std::memory_order_relaxed);
      w.rec->flags |= window_record::flag_parallel;
    }
  }

  void stamp_released(window& w) noexcept {
    if (w.rec != nullptr) {
      w.rec->t_released = trace_->now();
    }
  }

  hasher& h_;
  driver_loop<D>& loop_;
  window_pool<Budget>& pool_;
  typename D::file& file_;
  Scheduler sched_;
  trace_buffer* trace_ = nullptr;
  std::uint32_t file_index_ = 0;
  const kern::kernel_ops* ops_ = nullptr;
  unsigned count_ = 0;            // window objects, and windows in flight at most
  unsigned reads_in_flight_ = 0;  // windows reading at most: what the device sees
  unsigned in_flight_cap_ = 0;
  window_planner<Budget> planner_;
  // The storage this run allocated, and what admission may use of it.
  std::size_t nodes_total_ = reducer_nodes;
  std::size_t reducer_capacity_ = reducer_nodes;
  ex::inplace_stop_token opts_stop_token_{};
  std::unique_ptr<tree_reducer::node[]> nodes_;
  tree_reducer reducer_;

  // What this scope currently holds, so the cancel sweep names its own
  // reads and not another pipeline's sharing the same pool.
  window* taken_ = nullptr;
  std::vector<unsigned> free_cells_;
  window* last_window_ = nullptr;
  std::uint64_t index_ = 0;
  unsigned reading_ = 0;  // windows whose read has not landed
  unsigned max_reading_ = 0;
  unsigned max_windows_ = 0;
  std::size_t in_flight_nodes_ = 0;
  std::uint64_t admission_stalls_ = 0;
  std::size_t max_pending_ = 0;
  // The pool's summed compress time over the run. Written by the agent
  // that finished a window, read by the driver after everything has
  // joined, so it is atomic where the other counters above are not.
  std::atomic<std::uint64_t> agent_busy_ns_{0};
};

// The connected chains of one run: one cell per window the scope may
// hold at once, plus one of each edge shape, re-emplaced per use.
// optional does the destroy-then-construct and the conversion functions
// do the construct-in-place, which is what lets an immovable operation
// state live in a container.
//
// It lives outside window_scope because the cell types are the return
// types of that class's chain builders, which cannot be named until the
// class is complete. Whoever drives a run owns one of these and must
// keep it alive until nothing is in flight: a window on the pool is
// about to publish into a cell.
//
// The three operation types are template parameters, not member aliases.
// A provider's sender types have INTERNAL linkage (stdexec defines __sexpr
// inside an anonymous namespace), so naming them here gives this template
// internal linkage too and every translation unit gets its own.
//
// Parameterised on Scope alone it would have external linkage while its
// members differed per translation unit, and the linker would then let
// one unit's destructor run over another's object -- which the
// sanitizer lane reports as a vptr mismatch, correctly.
template <class Scope, class FullOp, class ReadOnlyOp, class EdgeOp>
struct pipeline_cells {
  using window = typename Scope::window;
  using receiver = typename Scope::scope_receiver;
  using full_op = FullOp;
  using read_only_op = ReadOnlyOp;
  using edge_op = EdgeOp;

  explicit pipeline_cells(unsigned count)
      : full(std::make_unique<std::optional<full_op>[]>(count)),
        edge(std::make_unique<std::optional<edge_op>[]>(count)) {}

  pipeline_cells(const pipeline_cells&) = delete;
  pipeline_cells& operator=(const pipeline_cells&) = delete;

  // Builds a cell's operation state in place: an operation state does
  // not move, so the optional takes a conversion that constructs it
  // where it will live.
  template <class Op, auto Chain>
  struct connect_as {
    Scope* scope;
    window* w;
    operator Op() const {
      return ex::connect((scope->*Chain)(*w), receiver{scope, w});
    }
  };
  using connect_full = connect_as<full_op, &Scope::template window_chain<>>;
  using connect_read =
      connect_as<read_only_op, &Scope::template read_only_chain<>>;
  using connect_edge = connect_as<edge_op, &Scope::template edge_chain<>>;

  std::unique_ptr<std::optional<full_op>[]> full;
  // A window that is a run of parts rather than one subtree uses the
  // same cell index as a full one would: the first and the last can be
  // in flight together on a file of three windows, and a stream whose
  // producer left a window short puts every window after it on this
  // shape until the grid comes back to its boundary.
  std::unique_ptr<std::optional<edge_op>[]> edge;
  // The two windows that can have no parts worth fanning out, which are
  // the file's own ends and nothing in between.
  std::optional<read_only_op> first_read;
  std::optional<read_only_op> last_read;
};

// Runs one open file through the pipeline on the calling thread.
//
// Tracing is a policy type below this point, because the window chain's
// type has to be settled before the run starts; here is where the
// runtime request becomes one of the two policies, once per run.
template <stack_budget Budget, file_driver D, class Scheduler>
void run_window_pipeline(hasher& h, pipeline_host<Budget, D>& host,
                         typename D::file& file, Scheduler&& sched,
                         const pipeline_plan& plan,
                         const pipeline_options& opts) {
  using sched_type = std::remove_cvref_t<Scheduler>;
  if (opts.trace != nullptr) {
    window_scope<buffer_tracer, Budget, D, sched_type> scope(
        h, host, file, std::forward<Scheduler>(sched), plan, opts,
        buffer_tracer{opts.trace});
    scope.run();
  } else {
    window_scope<null_tracer, Budget, D, sched_type> scope(
        h, host, file, std::forward<Scheduler>(sched), plan, opts);
    scope.run();
  }
}

// The same over a driver with no host yet: one is built for this run
// alone, which is what a blocking call does. A caller that runs several
// files keeps its own host and uses the overload above.
template <stack_budget Budget, file_driver D, class Scheduler>
void run_window_pipeline(hasher& h, D& drv, typename D::file& file,
                         Scheduler&& sched, const pipeline_plan& plan,
                         const pipeline_options& opts) {
  pipeline_host<Budget, D> host(drv, plan.windows, plan.window_bytes);
  run_window_pipeline<Budget>(h, host, file, std::forward<Scheduler>(sched),
                              plan, opts);
}

// The same over a driver whose file is already open, planned from the
// options alone: what the tests and the bench call, where the open mode
// was the caller's.
template <stack_budget Budget, file_driver D, class Scheduler>
void run_window_pipeline(hasher& h, D& drv, typename D::file& file,
                         Scheduler&& sched, const pipeline_options& opts) {
  run_window_pipeline<Budget>(
      h, drv, file, std::forward<Scheduler>(sched),
      plan_pipeline(h.count(), opts.window_bytes, opts.queue_depth, false,
                    file.size()),
      opts);
}

}  // namespace blake3pp::detail
