#pragma once

// The sender side of the file pipeline: a read as a sender, the driver
// as a scheduler, and the loop that drives both on the caller's thread.
//
// One window in flight is one sender chain,
//
//     io.read(window, offset)
//       | let_value(compress_on(sched))
//       | continues_on(io.scheduler())
//       | then(reduce_into_tree)
//
// and the whole pipeline runs on the thread that called update_file: the
// library owns no thread. Only the compress stage leaves it, and
// continues_on brings the result back through driver_scheduler.
//
// The one thread is also the contract with the I/O backends: every
// submit_read, flush and poll happens here, and wake() is the only
// member another thread calls. That is why a scheduler exists at all --
// a pool thread cannot touch the driver, so it parks a node on the run
// queue and nudges the driver awake instead.
//
// Three rules hold the shape together, and all of them are about where a
// completion may run and who may touch what afterwards:
//
//   * callbacks run only inside poll(), which is the I/O contract from
//     Phase 2a (src/io/backend.hpp);
//   * nothing completes inside a start(), which is this header's;
//   * once a node is on the run queue, the thread that put it there
//     touches neither the node nor the operation state it lives in ever
//     again.
//
// The second follows from the first plus who calls start(). The scope
// starts the next window from inside its own receiver, so a sender that
// completed its receiver from start() would run that receiver inside
// itself -- recursing through the scope's bookkeeping while the earlier
// completion is still halfway through it, to a depth set by how many
// windows happen to be ready. A read that cannot even be queued
// therefore parks its failure on the run queue and is completed from
// the loop, like every other completion.
//
// The third is what a publishing thread owes the driver. The push is the
// hand-off, and from that instant the driver may run the continuation,
// re-emplace the operation cell the node lived in, finish the file and
// return to a caller that destroys everything. Only the loop itself may
// be touched after a push -- its publisher count and its driver's wake
// -- and the loop is what waits those threads out before run_until
// returns. ThreadSanitizer is the only thing that catches a violation.
//
// A template over file_driver: io_driver for a real device, the bench's
// driver over null_context for a pipeline measured with no device under
// it, and a fake driver in the tests.
//
// Internal to the pipeline. Names an execution provider, so
// <blake3pp/io.hpp> must not reach it.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
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
#include <thread>
#include <type_traits>
#include <utility>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/ex_compat.hpp>
#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/detail/io_driver.hpp>
#include <blake3pp/detail/tree_reducer.hpp>
#include <blake3pp/dispatch.hpp>
#include <blake3pp/trace.hpp>

namespace blake3pp::detail {

// A node on the driver's run queue. Intrusive, so the queue allocates
// nothing: the operation states that carry one are allocated once with
// their window and reused for the whole file.
struct run_node {
  run_node* next = nullptr;
  void (*run)(run_node*) noexcept = nullptr;
};

// Many producers, one consumer: pool threads push, the driver drains.
// The only shared structure in the pipeline; everything else belongs to
// the driver thread.
class run_queue {
 public:
  run_queue() = default;
  run_queue(const run_queue&) = delete;
  run_queue& operator=(const run_queue&) = delete;

  // Any thread. The release on the exchange publishes whatever the node
  // was filled with to the drain's acquire.
  //
  // The node is written only before the exchange that publishes it: a
  // failed compare_exchange has published nothing, and a successful one
  // hands the node to the driver, which may run it and re-emplace the
  // operation cell it lives in before this call has even returned.
  // Nothing here touches n afterwards, and nothing anywhere else may
  // either.
  void push(run_node* n) noexcept {
    run_node* head = head_.load(std::memory_order_relaxed);
    do {
      n->next = head;
    } while (!head_.compare_exchange_weak(
        head, n, std::memory_order_release, std::memory_order_relaxed));
  }

  [[nodiscard]] bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) == nullptr;
  }

  // Driver thread. Takes everything pushed so far, in push order: the
  // pushes build a stack, and reversing it once per drain costs one walk
  // and keeps windows coming back in the order they finished rather than
  // inverted, which is what makes a trace readable.
  [[nodiscard]] run_node* take() noexcept {
    run_node* n = head_.exchange(nullptr, std::memory_order_acquire);
    run_node* fifo = nullptr;
    while (n != nullptr) {
      run_node* const next = n->next;
      n->next = fifo;
      fifo = n;
      n = next;
    }
    return fifo;
  }

 private:
  std::atomic<run_node*> head_{nullptr};
};

template <file_driver D>
class driver_loop;
template <file_driver D>
class driver_scheduler;

// --------------------------------------------------------------------
// driver_scheduler: work parked on the run queue, run by the driver.

template <file_driver D, class Receiver>
class schedule_op : public run_node {
 public:
  using operation_state_concept = ex_compat::operation_state_tag;

  schedule_op(Receiver r, driver_loop<D>* loop)
      : rcvr_(std::move(r)), loop_(loop) {
    run = [](run_node* self) noexcept {
      // RULE 2: the CPO, never the member.
      ex::set_value(std::move(static_cast<schedule_op*>(self)->rcvr_));
    };
  }

  schedule_op(const schedule_op&) = delete;
  schedule_op& operator=(const schedule_op&) = delete;

  // Runs on whichever thread finished the work before it, which for the
  // window chain is a pool thread. Handing the node to the loop is the
  // entire hand-off back to the driver, and it is the last thing this
  // operation state does: publishing it is what allows the driver to
  // finish the run and destroy it.
  void start() & noexcept { loop_->publish(this); }

 private:
  Receiver rcvr_;
  driver_loop<D>* loop_;
};

template <file_driver D>
struct schedule_env {
  driver_loop<D>* loop;
  [[nodiscard]] driver_scheduler<D> query(
      ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept;
};

template <file_driver D>
class schedule_sender {
 public:
  using sender_concept = ex_compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(ex::set_value_t());

  explicit schedule_sender(driver_loop<D>* loop) noexcept : loop_(loop) {}

  template <class Receiver>
  [[nodiscard]] schedule_op<D, Receiver> connect(Receiver r) const {
    return {std::move(r), loop_};
  }

  // continues_on asks a sender where it completes; answering with the
  // driver's own scheduler is what lets the window chain say "back on
  // the driver" and be believed.
  [[nodiscard]] schedule_env<D> get_env() const noexcept { return {loop_}; }

 private:
  driver_loop<D>* loop_;
};

// RULE 3 supplies get_forward_progress_guarantee: weakly_parallel is the
// honest answer, since this scheduler advances only while its owner is
// inside run_until.
template <file_driver D>
class driver_scheduler : public ex_compat::weakly_parallel_scheduler {
 public:
  using scheduler_concept = ex_compat::scheduler_tag;

  driver_scheduler() noexcept = default;
  explicit driver_scheduler(driver_loop<D>* loop) noexcept : loop_(loop) {}

  [[nodiscard]] schedule_sender<D> schedule() const noexcept {
    return schedule_sender<D>{loop_};
  }

  bool operator==(const driver_scheduler&) const noexcept = default;

 private:
  driver_loop<D>* loop_ = nullptr;
};

template <file_driver D>
driver_scheduler<D> schedule_env<D>::query(
    ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept {
  return driver_scheduler<D>{loop};
}

// --------------------------------------------------------------------
// read_sender: one window's read.

template <file_driver D, class Receiver>
class read_op_state : public run_node {
 public:
  using operation_state_concept = ex_compat::operation_state_tag;

  read_op_state(Receiver r, driver_loop<D>* loop, typename D::file* f,
                std::uint64_t off, std::span<std::byte> buf)
      : rcvr_(std::move(r)), loop_(loop), file_(f), off_(off), buf_(buf) {
    run = &deliver_submit_failure;
  }

  read_op_state(const read_op_state&) = delete;
  read_op_state& operator=(const read_op_state&) = delete;

  // Queues the read and returns: no flush, because the loop flushes once
  // for every read it started this round, and no completion, because the
  // backend only ever calls back from inside poll().
  //
  // A read that could not be queued owes no callback, but completing the
  // receiver from start() would run the rest of the window chain inside
  // whatever started this one. The run queue is already the way back
  // onto the loop, so the failure takes it and arrives like every other
  // completion: a system_error as its code, anything else as the
  // exception itself.
  void start() & noexcept {
    op_.done = &on_read_done;
    op_.owner = this;
    try {
      loop_->driver().submit_read(*file_, off_, buf_, op_);
    } catch (const std::system_error& e) {
      ec_ = e.code();
      loop_->publish(this);
    } catch (...) {
      eptr_ = std::current_exception();
      loop_->publish(this);
    }
  }

 private:
  static void deliver_submit_failure(run_node* n) noexcept {
    auto* const self = static_cast<read_op_state*>(n);
    if (self->eptr_) {
      ex::set_error(std::move(self->rcvr_), std::move(self->eptr_));
    } else {
      ex::set_error(std::move(self->rcvr_), self->ec_);
    }
  }

  static void on_read_done(io_read_op* op, std::error_code ec) noexcept {
    auto* const self = static_cast<read_op_state*>(op->owner);
    if (ec) {
      ex::set_error(std::move(self->rcvr_), ec);
    } else {
      // The backend reads exactly what was asked for or reports an
      // error, so the whole window is there.
      ex::set_value(std::move(self->rcvr_),
                    std::span<const std::byte>(self->buf_));
    }
  }

  Receiver rcvr_;
  driver_loop<D>* loop_;
  typename D::file* file_;
  std::uint64_t off_;
  std::span<std::byte> buf_;
  std::error_code ec_{};
  std::exception_ptr eptr_{};
  io_read_op op_{};
};

template <file_driver D>
class read_sender {
 public:
  using sender_concept = ex_compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(
      ex::set_value_t(std::span<const std::byte>),
      ex::set_error_t(std::error_code),
      ex::set_error_t(std::exception_ptr));

  read_sender(driver_loop<D>* loop, typename D::file* f, std::uint64_t off,
              std::span<std::byte> buf) noexcept
      : loop_(loop), file_(f), off_(off), buf_(buf) {}

  template <class Receiver>
  [[nodiscard]] read_op_state<D, Receiver> connect(Receiver r) const {
    return {std::move(r), loop_, file_, off_, buf_};
  }

 private:
  driver_loop<D>* loop_;
  typename D::file* file_;
  std::uint64_t off_;
  std::span<std::byte> buf_;
};

// --------------------------------------------------------------------
// The loop.

// The two facts the loop needs from the pipeline above it, both written
// only from completions the loop itself ran.
//
// `outstanding` is what makes a blocking poll safe. A window that has
// been read but whose compress stage is still on the pool is owed by
// nobody the driver can see: in_flight() is back to zero and the run
// queue is empty, yet a wake is certainly coming. Counting chains rather
// than reads is the difference between sleeping until that wake and
// asserting that nothing can arrive.
struct loop_status {
  bool done = false;
  unsigned outstanding = 0;
};

template <file_driver D>
class driver_loop {
 public:
  explicit driver_loop(D& drv) noexcept : drv_(&drv) {}
  driver_loop(const driver_loop&) = delete;
  driver_loop& operator=(const driver_loop&) = delete;

  [[nodiscard]] read_sender<D> read(typename D::file& f, std::uint64_t off,
                                    std::span<std::byte> buf) noexcept {
    return read_sender<D>{this, &f, off, buf};
  }

  [[nodiscard]] driver_scheduler<D> scheduler() noexcept {
    return driver_scheduler<D>{this};
  }

  [[nodiscard]] D& driver() noexcept { return *drv_; }

  // Where to record what this thread spends its time on; null records
  // nothing, which is the default and costs one branch per poll.
  void record_into(trace_buffer* trace) noexcept {
    trace_ = trace;
    stats_ = trace != nullptr ? &trace->driver() : nullptr;
  }

  // Hands one node to the driver, from any thread.
  //
  // Everything after the push touches this loop and nothing else: the
  // node is the driver's from that instant, and so is the operation
  // state it lives in, which may already have been destroyed and
  // re-emplaced for the next window. The driver's wake is reached
  // through the loop for the same reason, and the publisher count is
  // what keeps the loop and the driver alive long enough to reach it --
  // run_until does not return while a thread is between the two.
  void publish(run_node* n) noexcept {
    publishers_.fetch_add(1, std::memory_order_relaxed);
    queue_.push(n);
    drv_->wake();
    publishers_.fetch_sub(1, std::memory_order_release);
  }

  // Drives everything until the pipeline says it is done, which happens
  // inside a completion this loop itself ran. Blocks only when there is
  // nothing to run, so the thread sleeps in the kernel rather than
  // spinning, and only ever here.
  //
  // before_flush runs once per round, after the completions and before
  // the submit: whatever it starts joins the same flush, so a batch of
  // windows reaches the OS in one call rather than one each.
  // The state that cannot resolve itself: about to sleep with no read
  // owed, nothing to run and nothing outstanding. Callers that know why
  // it happened pass their own; this one at least names the state.
  [[noreturn]] static void stall() {
    throw std::logic_error(
        "blake3pp: the driver loop would block with no read in flight, an "
        "empty run queue and nothing outstanding: nothing could wake it");
  }

  template <class BeforeFlush, class OnStall>
  void run_until(const loop_status& status, BeforeFlush&& before_flush,
                 OnStall&& on_stall) {
    // Also on the way out of a throwing poll(): the caller's next act is
    // to destroy the operation cells, and a thread inside publish() is
    // still reading this loop.
    const publisher_guard guard{this};
    // The driver's own split of busy against parked, which is what says
    // whether this thread is the bottleneck once the stage shares of a
    // window no longer sum to the wall clock. One clock read per poll,
    // and none at all without a trace buffer.
    std::int64_t t = stats_ != nullptr ? trace_->now() : 0;
    for (;;) {
      drain();
      before_flush();
      if (stats_ != nullptr) {
        ++stats_->iterations;
      }
      if (status.done) {
        if (stats_ != nullptr) {
          stats_->busy_ns += static_cast<std::uint64_t>(trace_->now() - t);
        }
        return;
      }
      drv_->flush();
      const bool block = queue_.empty();
      // What in_flight() was kept across Phase 2b for. Sleeping with no
      // read owed by the driver, nothing to run and nothing outstanding
      // anywhere else is a pipeline nobody will ever wake, so it is
      // reported rather than entered -- and reported in every build,
      // because the shapes that reach it are configurations, not bugs a
      // sanitizer lane would find. Nothing is in flight at this point,
      // which is what makes unwinding from here safe.
      if (block && drv_->in_flight() == 0 && status.outstanding == 0) {
        on_stall();
      }
      if (stats_ == nullptr) {
        drv_->poll(block);
        continue;
      }
      const std::int64_t t_poll = trace_->now();
      stats_->busy_ns += static_cast<std::uint64_t>(t_poll - t);
      const std::size_t ran = drv_->poll(block);
      t = trace_->now();
      ++stats_->polls;
      if (block) {
        ++stats_->blocking_polls;
        stats_->parked_ns += static_cast<std::uint64_t>(t - t_poll);
        if (ran == 0) {
          // Nothing of the driver's own finished, so what released this
          // poll was a wake from a thread handing a window back.
          ++stats_->wakes;
        }
      } else {
        stats_->busy_ns += static_cast<std::uint64_t>(t - t_poll);
      }
    }
  }

  template <class BeforeFlush>
  void run_until(const loop_status& status, BeforeFlush&& before_flush) {
    run_until(status, std::forward<BeforeFlush>(before_flush), &stall);
  }

  void run_until(const loop_status& status) {
    run_until(status, [] {}, &stall);
  }

  // Waits out the threads that are inside publish(). One eventfd write
  // each, so this spins for as long as a syscall and only where the run
  // is already over.
  struct publisher_guard {
    driver_loop* loop;
    ~publisher_guard() { loop->settle_publishers(); }
  };

  void settle_publishers() noexcept {
    while (publishers_.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }
  }

  // The two halves of one round, for a caller winding the pipeline down
  // outside run_until: what is parked runs, and nothing else; then one
  // flush and one blocking poll, which throws what poll() throws.
  // Runs everything parked on the queue, once. A node may be pushed
  // again by its own completion, so the successor is read before the
  // node runs.
  std::size_t drain() noexcept {
    std::size_t ran = 0;
    for (run_node* n = queue_.take(); n != nullptr;) {
      run_node* const next = n->next;
      n->run(n);
      ++ran;
      n = next;
    }
    if (stats_ != nullptr) {
      stats_->queue_runs += ran;
    }
    return ran;
  }

  void poll_once() {
    drv_->flush();
    drv_->poll(true);
  }

 private:
  D* drv_;
  run_queue queue_;
  std::atomic<unsigned> publishers_{0};
  trace_buffer* trace_ = nullptr;
  driver_stats* stats_ = nullptr;
};

// --------------------------------------------------------------------
// The bounded scope.

// What the scope needs that the public options do not already say.
struct pipeline_options {
  // The requests plan_pipeline() rounds and clamps; the scope itself
  // reads only the plan.
  std::size_t window_bytes = 8 * 1024 * 1024;
  unsigned queue_depth = 4;
  trace_buffer* trace = nullptr;
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
};

// Pending nodes the reducer may hold.
//
// The scope admits a window only while a node is free for it, so the
// count is what bounds how far ahead of the tree the reads may run. It
// has to clear the worst a contiguous run of windows can decompose into
// -- one ascending and one descending chain of aligned nodes, one per
// level, 54 levels at BLAKE3's 2^64-byte limit -- plus the one node the
// next window needs, or the scope could reach a state where no window
// may start; see admit_more().
inline constexpr std::size_t reducer_nodes = 256;

// The most nodes a run of `parts` equal parts can decompose into: one
// ascending chain and one descending one, at most one node per level of
// the run. An edge window reserves this rather than the worst case over
// any run, which is what keeps a small reducer usable: a blanket 2 * 54
// would make an edge window unadmittable below that capacity, and the
// pipeline would stall rather than run slowly.
[[nodiscard]] constexpr std::size_t nodes_for(std::size_t parts) noexcept {
  return 2 * static_cast<std::size_t>(std::bit_width(parts));
}

// How many bytes of a file have to be read before its windows line up
// with the hasher's own window grid.
//
// A hasher part-way through a message -- the second file of several
// hashed as one -- sits at some count that is not a multiple of the
// window. Reading one short window first brings it to a boundary, and
// every window after that is a subtree like any other. Zero means the
// hasher is already on a boundary, which is every fresh hasher.
[[nodiscard]] constexpr std::size_t first_window_bytes(
    std::uint64_t count, std::size_t window_bytes) noexcept {
  const std::size_t offset = static_cast<std::size_t>(count % window_bytes);
  return (window_bytes - offset) % window_bytes;
}

// Whether the platform's direct-I/O path can serve a file whose windows
// start at `head` bytes in.
//
// Direct I/O wants aligned offsets as well as aligned lengths, and the
// windows after a short first one start at head + n * window -- so an
// unaligned head misaligns every window of the file, not just the first.
// Degrading read by read would put every one of them on the synchronous
// path inside poll(), on the driver thread; a buffered file keeps them
// all asynchronous. So the choice is per file, and it is made once.
[[nodiscard]] constexpr bool direct_io_fits(std::size_t head) noexcept {
  return head % io_driver::direct_alignment == 0;
}

// The geometry of one update_file() call, decided once from the hasher's
// count and the caller's requests and handed to the driver, the file and
// the scope alike, so that no two of them round or clamp differently.
struct pipeline_plan {
  // The window, as rounded_window_bytes() rounds the request.
  std::size_t window_bytes;
  // Windows in flight at most, and window objects allocated: the queue
  // depth as clamp_queue_depth() clamps the request.
  unsigned queue_depth;
  // The short first window that brings the hasher onto the window grid;
  // zero for a hasher already on it.
  std::size_t head_bytes;
  // Whether the file is opened for direct I/O: the caller asked, and the
  // grid is one the platform's alignment can serve.
  bool direct_io;
};

[[nodiscard]] constexpr pipeline_plan plan_pipeline(
    std::uint64_t count, std::size_t window_bytes, unsigned queue_depth,
    bool direct_io) noexcept {
  const std::size_t window = rounded_window_bytes(window_bytes);
  const std::size_t head = first_window_bytes(count, window);
  return {window, clamp_queue_depth(queue_depth), head,
          direct_io && direct_io_fits(head)};
}


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
// at construction, and a chain may only start when one is free. The
// reducer is what lets them finish in any order.
template <bool Traced, stack_budget Budget, file_driver D, class Scheduler>
class window_scope {
 public:
  using cv_type = std::array<std::uint32_t, 8>;

  window_scope(hasher& h, D& drv, typename D::file& file, Scheduler sched,
               const pipeline_plan& plan, const pipeline_options& opts)
      : h_(h),
        loop_(drv),
        file_(file),
        sched_(std::move(sched)),
        trace_(opts.trace),
        ops_(resolve(h.selected_arch())),
        window_bytes_(plan.window_bytes),
        count_(plan.queue_depth),
        in_flight_cap_(opts.in_flight_cap == 0
                           ? count_
                           : std::min<unsigned>(opts.in_flight_cap, count_)),
        base_count_(h.count()),
        head_window_bytes_(plan.head_bytes),
        base_chunk_((h.count() + head_window_bytes_) / chunk_size),
        file_bytes_(file.size()),
        reducer_capacity_(opts.reducer_capacity == 0
                              ? reducer_nodes
                              : std::min(opts.reducer_capacity, reducer_nodes)),
        windows_(std::make_unique<window[]>(count_)),
        nodes_(std::make_unique<tree_reducer::node[]>(reducer_nodes)),
        reducer_(ops_, h.key_words(), h.mode_flags(),
                 std::span<tree_reducer::node>(nodes_.get(), reducer_nodes)) {
    // The one buffer allocation of the whole run, owned by the driver so
    // that it outlives every read the teardown has to drain.
    const std::span<std::byte> pool =
        drv.allocate(window_bytes_ * static_cast<std::size_t>(count_));
    loop_.record_into(trace_);
    for (unsigned i = count_; i > 0; --i) {
      window& w = windows_[i - 1];
      w.buffer = pool.subspan(static_cast<std::size_t>(i - 1) * window_bytes_,
                              window_bytes_);
      w.slot = i - 1;
      push_free(&w);
    }
  }

  // Over a file the caller opened itself, planned from the requests in
  // opts; what the tests construct directly.
  window_scope(hasher& h, D& drv, typename D::file& file, Scheduler sched,
               const pipeline_options& opts)
      : window_scope(h, drv, file, std::move(sched),
                     plan_pipeline(h.count(), opts.window_bytes,
                                   opts.queue_depth, false),
                     opts) {}

  window_scope(const window_scope&) = delete;
  window_scope& operator=(const window_scope&) = delete;

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

  // Drives the whole file on this thread and absorbs it into the hasher.
  // Throws the first error any window reported, once, after everything in
  // flight has finished.
  void run() {
    if (file_bytes_ == 0) {
      return;
    }
    // The operation cells: fixed storage for one connected chain per
    // window, re-emplaced per use. optional does the destroy-then-construct
    // and the conversion functions below do the construct-in-place, which
    // is what lets an immovable operation state live in a container.
    using full_op = ex::connect_result_t<
        decltype(std::declval<window_scope&>().window_chain(
            std::declval<window&>())),
        scope_receiver>;
    using read_only_op = ex::connect_result_t<
        decltype(std::declval<window_scope&>().read_only_chain(
            std::declval<window&>())),
        scope_receiver>;
    using edge_op = ex::connect_result_t<
        decltype(std::declval<window_scope&>().edge_chain(
            std::declval<window&>())),
        scope_receiver>;

    struct connect_full {
      window_scope* scope;
      window* w;
      operator full_op() const {
        return ex::connect(scope->window_chain(*w), scope_receiver{scope, w});
      }
    };
    struct connect_read {
      window_scope* scope;
      window* w;
      operator read_only_op() const {
        return ex::connect(scope->read_only_chain(*w),
                           scope_receiver{scope, w});
      }
    };
    struct connect_edge {
      window_scope* scope;
      window* w;
      operator edge_op() const {
        return ex::connect(scope->edge_chain(*w), scope_receiver{scope, w});
      }
    };

    auto cells = std::make_unique<std::optional<full_op>[]>(count_);
    // One cell of each shape per edge window: the first and the last can
    // be in flight together on a file of three windows, and either may
    // turn out to have no parts worth fanning out.
    std::optional<edge_op> first_edge_cell;
    std::optional<read_only_op> first_read_cell;
    std::optional<edge_op> last_edge_cell;
    std::optional<read_only_op> last_read_cell;

    // The cells are locals, so an exception leaving run_until would
    // destroy them while a window can still be on the pool, about to
    // publish into one. The handler waits those windows out first; the
    // cells are still alive inside it.
    try {
      run_windows(cells, first_edge_cell, first_read_cell, last_edge_cell,
                  last_read_cell, connect_full{}, connect_read{},
                  connect_edge{});
    } catch (...) {
      quiesce();
      throw;
    }
    assert(in_flight_ == 0 && "run_until returned with a window in flight");

    if (trace_ != nullptr) {
      trace_->driver().admission_stalls = admission_stalls_;
      trace_->driver().max_pending = max_pending_;
    }
    if (eptr_) {
      std::rethrow_exception(eptr_);
    }
    if (ec_) {
      throw std::system_error(ec_, "blake3pp: reading a window");
    }
    // Every complete window is a subtree of the final tree and goes in
    // through the reducer; the last one carries the message end, so it
    // can only ever be hashed by the hasher itself, and last.
    reducer_.drain_into(h_);
    if (last_window_ != nullptr) {
      window& w = *last_window_;
      // Only the tail: the parts before it are already in the tree.
      h_.update(std::span<const std::byte>(
          w.buffer.data() + (w.bytes - w.plan.tail_bytes), w.plan.tail_bytes));
      if (w.rec != nullptr) {
        // The last window is never folded, so its join and its absorb
        // are the same instant: the hasher took it whole.
        w.rec->t_joined = w.rec->t_absorbed = trace_->now();
      }
      stamp_released(w);
    }
  }

  // How often a window that had a free buffer and work left to do was
  // held back because the reducer had no node for it. Zero on any run
  // whose capacity is the default; the admission test is what reads it.
  [[nodiscard]] std::uint64_t admission_stalls() const noexcept {
    return admission_stalls_;
  }

 private:
  // The loop proper: starts every window that may start before each
  // flush, until the pipeline reports itself done. The cell types and
  // the conversion functions that fill them are run()'s locals, which
  // is why they arrive as template parameters.
  template <class Cells, class EdgeCell, class ReadCell, class ConnectFull,
            class ConnectRead, class ConnectEdge>
  void run_windows(Cells& cells, EdgeCell& first_edge_cell,
                   ReadCell& first_read_cell, EdgeCell& last_edge_cell,
                   ReadCell& last_read_cell, ConnectFull, ConnectRead,
                   ConnectEdge) {
    loop_.run_until(status_, [&] {
      // Every window that can start, started before the one flush this
      // round: the reads of a whole batch reach the OS in one call.
      while (!stop_ && in_flight_ < in_flight_cap_ && free_ != nullptr) {
        if (next_offset_ >= file_bytes_) {
          break;
        }
        // An edge window reserves room for a whole decomposition rather
        // than for one node, so what it costs has to be known before it
        // is admitted.
        // The cost is the window's own, not a worst case: plan it
        // before admitting it, since an edge window reserves a whole
        // decomposition.
        const std::size_t cost = this->next_window_cost();
        if (!admit_more(cost)) {
          break;
        }
        window* const w = free_;
        take_next(*w);
        free_ = w->next_free;
        claim_record(*w);
        ++in_flight_;
        in_flight_nodes_ += w->plan.node_cost;
        status_.outstanding = in_flight_;
        if (w->last) {
          last_window_ = w;
        }
        if (!w->last && !w->short_first) {
          prepare_compress(*w);
          cells[w->slot].emplace(ConnectFull{this, w});
          ex::start(*cells[w->slot]);
        } else if (w->plan.parts >= 2) {
          // An edge window: the short first one or the last one, each
          // with its own cell, since a three-window file has both in
          // flight at once.
          auto& cell = w->last ? last_edge_cell : first_edge_cell;
          cell.emplace(ConnectEdge{this, w});
          ex::start(*cell);
        } else {
          auto& cell = w->last ? last_read_cell : first_read_cell;
          cell.emplace(ConnectRead{this, w});
          ex::start(*cell);
        }
      }
    }, [this] { this->report_stall(); });
  }

  // Waits out every window still in flight after an exception has left
  // the loop, so that nothing on the pool can publish into an operation
  // cell once the cells are destroyed. No new window starts; what is
  // parked runs, and a blocking poll brings the rest back. The error the
  // caller is already unwinding with is the one it keeps: whatever a
  // poll throws here is dropped, and after max_failed_polls of them the
  // driver is not asked again. From then on the run queue alone is
  // watched, since a window on the pool publishes without the driver's
  // help, and the reads the driver still owes are left to its own
  // teardown drain, which runs no callback and touches no operation.
  static constexpr unsigned max_failed_polls = 8;

  void quiesce() noexcept {
    stop_ = true;
    unsigned failed_polls = 0;
    for (;;) {
      loop_.drain();
      if (in_flight_ == 0) {
        break;
      }
      if (failed_polls < max_failed_polls) {
        try {
          loop_.poll_once();
        } catch (...) {
          ++failed_polls;
        }
        continue;
      }
      // Every chain the driver does not owe as a read is on the pool or
      // on the run queue, and comes back on its own.
      if (in_flight_ <= loop_.driver().in_flight()) {
        break;
      }
      std::this_thread::yield();
    }
    loop_.settle_publishers();
  }

  // Where the next window sits in the file: what take_next() hands out,
  // computed from the scope's cursor alone so a window can be planned
  // before it is taken.
  struct geometry {
    std::uint64_t offset = 0;
    std::size_t bytes = 0;
    bool last = false;
    bool short_first = false;
  };

  // How a window is split three ways. `head` is absorbed by the hasher
  // on the driver the moment the read lands, `parts` full parts go to
  // the pool and come back as nodes, and `tail` waits for the reducer to
  // drain. A full window is all parts and no ends. Decided by
  // plan_window() from the geometry, with nothing else touched, which is
  // what lets admission cost a window that has not been built.
  struct window_plan {
    std::uint64_t first_chunk = 0;  // where its contribution to the tree starts
    std::size_t head_bytes = 0;
    std::size_t tail_bytes = 0;
    std::size_t parts = 0;
    std::size_t part_chunks = 0;
    std::size_t node_cost = 1;   // reducer slots admission reserves
    unsigned edge_slot = 0;      // which node buffer an edge window uses
  };
  static_assert(std::is_trivially_copyable_v<window_plan>);

  struct window {
    std::span<std::byte> buffer;
    window_compress<Budget> compress;
    std::uint64_t index = 0;
    std::uint64_t offset = 0;
    std::uint64_t chunks = 0;
    std::size_t bytes = 0;
    window_record* rec = nullptr;
    window* next_free = nullptr;
    unsigned slot = 0;
    bool last = false;
    bool short_first = false;
    window_plan plan;
    std::size_t nodes = 0;  // nodes the fold produced
  };

  // What the node buffers hold: the worst case over any run, which is
  // what fold_aligned_runs asserts against.
  static constexpr std::size_t edge_node_cap = 2 * 54;

  // The scope's receiver is a handle: the scope and the window it speaks
  // for, nothing else. It must stay copy-constructible, because beman's
  // continues_on stores the receiver by copy from an lvalue
  // (continues_on.hpp, state_type), and the chain below runs through
  // exactly that adaptor.
  struct scope_receiver {
    using receiver_concept = ex_compat::receiver_tag;
    scope_receiver(window_scope* s, window* win) noexcept
        : scope(s), w(win) {}
    window_scope* scope;
    window* w;

    void set_value() && noexcept { scope->on_done(*w); }
    void set_error(std::error_code ec) && noexcept { scope->on_error(*w, ec); }
    void set_error(std::exception_ptr e) && noexcept {
      scope->on_exception(*w, std::move(e));
    }
    void set_stopped() && noexcept {
      scope->on_error(*w,
                      std::make_error_code(std::errc::operation_canceled));
    }
  };

  static_assert(std::copy_constructible<scope_receiver>);

  // One window, start to finish. Phase 4 replaces the middle line and
  // nothing else.
  [[nodiscard]] auto window_chain(window& w) {
    return ex::then(
        ex::continues_on(
            ex::let_value(loop_.read(file_, w.offset,
                                     w.buffer.first(w.bytes)),
                          [this, &w](std::span<const std::byte>) {
                            return compress_stage(w);
                          }),
            loop_.scheduler()),
        [this, &w](const cv_type& cv) noexcept { reduce_into_tree(w, cv); });
  }

  // An edge window with nothing worth fanning out: read it, hold it, and
  // let the hasher take the whole thing at the end. Small last windows
  // land here, and so does a file below one window.
  [[nodiscard]] auto read_only_chain(window& w) {
    return ex::then(loop_.read(file_, w.offset, w.buffer.first(w.bytes)),
                    [this, &w](std::span<const std::byte>) noexcept {
                      stamp_ready(w);
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
  [[nodiscard]] auto edge_chain(window& w) {
    return ex::then(
        ex::continues_on(
            ex::let_value(loop_.read(file_, w.offset, w.buffer.first(w.bytes)),
                          [this, &w](std::span<const std::byte>) {
                            return edge_compress_stage(w);
                          }),
            loop_.scheduler()),
        [this, &w](std::size_t nodes) noexcept {
          insert_edge_nodes(w, nodes);
        });
  }

  // The bytes an edge window owes the hasher before anything else can
  // reach it. Safe at any point before drain_into, because no reducer
  // node reaches the hasher until then -- and it has to happen before
  // the nodes do, since it is what puts the hasher on the boundary they
  // are numbered from.
  void absorb_head(window& w) noexcept {
    if (w.plan.head_bytes > 0) {
      h_.update(
          std::span<const std::byte>(w.buffer.data(), w.plan.head_bytes));
    }
  }

  [[nodiscard]] auto edge_compress_stage(window& w) {
    stamp_ready(w);
    absorb_head(w);
    const window_plan& p = w.plan;
    w.compress.prepare_edge(ops_, w.buffer.data() + p.head_bytes,
                            p.part_chunks, p.parts, p.first_chunk,
                            h_.key_words(), h_.mode_flags(), trace_, w.index);
    const std::span<tree_reducer::node> out(edge_nodes_[w.plan.edge_slot]);
    if constexpr (Traced) {
      return ex::then(compress_edge_on<true>(sched_, w.compress, out),
                      [this, &w](std::size_t nodes) noexcept {
                        stamp_agents(w);
                        return nodes;
                      });
    } else {
      return compress_edge_on<false>(sched_, w.compress, out);
    }
  }

  // On the driver, like every other insert.
  void insert_edge_nodes(window& w, std::size_t nodes) noexcept {
    w.nodes = nodes;
    for (std::size_t i = 0; i < nodes; ++i) {
      const tree_reducer::node& n = edge_nodes_[w.plan.edge_slot][i];
      const bool inserted = reducer_.insert(n.first_chunk, n.chunks, n.cv);
      // Admission reserved a whole decomposition for this window.
      assert(inserted && "the reducer refused an edge window's node");
      (void)inserted;
    }
    max_pending_ = std::max(max_pending_, reducer_.pending());
    if (w.rec != nullptr) {
      w.rec->t_absorbed = trace_->now();
    }
  }

  [[nodiscard]] auto compress_stage(window& w) {
    stamp_ready(w);
    if constexpr (Traced) {
      return ex::then(compress_on<true>(sched_, w.compress),
                      [this, &w](const cv_type& cv) noexcept {
                        if (w.rec != nullptr) {
                          w.rec->t_joined = trace_->now();
                          w.rec->agent_busy_ns =
                              w.compress.pt.busy_ns.load(
                                  std::memory_order_relaxed);
                          w.rec->agents_active =
                              w.compress.pt.active.load(
                                  std::memory_order_relaxed);
                          w.rec->flags |= window_record::flag_parallel;
                        }
                        return cv;
                      });
    } else {
      return compress_on<false>(sched_, w.compress);
    }
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
        std::to_string(in_flight_) + " windows are in flight holding " +
        std::to_string(in_flight_nodes_) + " reserved nodes, and the next "
        "window reserves " + std::to_string(next_window_cost()) +
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

  [[nodiscard]] geometry next_geometry() const noexcept {
    geometry g;
    g.offset = next_offset_;
    g.short_first = g.offset == 0 && head_window_bytes_ > 0;
    const std::size_t want = g.short_first ? head_window_bytes_ : window_bytes_;
    g.bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(want, file_bytes_ - next_offset_));
    g.last = next_offset_ + g.bytes >= file_bytes_;
    return g;
  }

  // What the next window would reserve in the reducer, from the same
  // plan the window will get.
  [[nodiscard]] std::size_t next_window_cost() const noexcept {
    return plan_window(next_geometry()).node_cost;
  }

  // Decides a window's three parts. A window that is both ends of the
  // file is the hasher's whole: it is misaligned at the front and
  // carries the final chunk at the back, and nothing between them is
  // worth a subtree. For every window but the short first one the
  // contribution starts at the window itself; the short one's starts
  // wherever its head ends, which plan_short_first works out.
  [[nodiscard]] window_plan plan_window(const geometry& g) const noexcept {
    window_plan p;
    p.first_chunk =
        base_chunk_ +
        (g.offset - std::min<std::uint64_t>(g.offset, head_window_bytes_)) /
            chunk_size;
    if (g.last && g.short_first) {
      p.tail_bytes = g.bytes;
      return p;
    }
    if (g.last) {
      return plan_last(g.bytes, p);
    }
    if (g.short_first) {
      return plan_short_first(g.bytes, p);
    }
    return p;
  }

  // How many whole parts of a last window of `bytes` sit entirely before
  // its final chunk; below two there is nothing worth fanning out.
  [[nodiscard]] std::size_t last_window_parts(
      std::size_t bytes) const noexcept {
    const std::uint64_t chunks_with_tail =
        (static_cast<std::uint64_t>(bytes) + chunk_size - 1) / chunk_size;
    if (chunks_with_tail < 2) {
      return 0;
    }
    const std::size_t p =
        window_part_chunks<Budget>(static_cast<std::size_t>(chunks_with_tail));
    const std::uint64_t k = (chunks_with_tail - 1) / p;
    return k < 2 ? 0 : static_cast<std::size_t>(k);
  }

  // Takes the next window's geometry and plan into w and advances.
  void take_next(window& w) noexcept {
    const geometry g = next_geometry();
    w.plan = plan_window(g);
    w.offset = g.offset;
    w.bytes = g.bytes;
    w.last = g.last;
    w.short_first = g.short_first;
    w.chunks = w.bytes / chunk_size;
    w.index = index_++;
    w.nodes = 0;
    next_offset_ += w.bytes;
  }

  // The short window that brings the hasher back to a window boundary.
  //
  // Its front belongs to the hasher: the bytes to the next chunk
  // boundary, then whole chunks up to a part boundary, which is under
  // one part in total and is absorbed on the driver the moment the read
  // lands. What follows is whole parts, aligned in the message's own
  // chunk space, and goes to the pool like any other window's.
  [[nodiscard]] window_plan plan_short_first(std::size_t bytes,
                                             window_plan p) const noexcept {
    const std::uint64_t count = base_count_;
    const std::size_t to_chunk = static_cast<std::size_t>(
        (chunk_size - count % chunk_size) % chunk_size);
    const std::size_t chunks_left =
        bytes > to_chunk ? (bytes - to_chunk) / chunk_size : 0;
    if (chunks_left < 2) {
      p.head_bytes = bytes;
      return p;
    }
    const std::size_t part = window_part_chunks<Budget>(chunks_left);
    const std::uint64_t chunk_at = (count + to_chunk) / chunk_size;
    const std::size_t to_part =
        static_cast<std::size_t>((part - chunk_at % part) % part) * chunk_size;
    const std::size_t head = to_chunk + to_part;
    const std::size_t parts =
        bytes > head ? (bytes - head) / (part * chunk_size) : 0;
    if (parts < 2) {
      p.head_bytes = bytes;
      return p;
    }
    p.head_bytes = head;
    p.part_chunks = part;
    p.parts = parts;
    p.first_chunk = (count + head) / chunk_size;
    p.node_cost = nodes_for(parts);
    p.edge_slot = 0;
    // The parts cover the window exactly: the head lands on a part
    // boundary and the window ends on a window boundary, and the part
    // size divides the window because both are powers of two and the
    // part is the smaller.
    assert(bytes == head + parts * part * chunk_size &&
           "a short first window's parts must cover it exactly");
    return p;
  }

  // The last window carries the message's final chunk, and a merged
  // range must never contain it -- so the part that holds it, and
  // everything after, stays with the hasher. Everything before that is
  // ordinary full parts and goes to the pool like any other window's.
  //
  // k is how many whole parts sit entirely before the final chunk. Below
  // two of them there is nothing worth a round trip through the pool and
  // the window is all tail, which is also the shape of a file that fits
  // inside one window. The hasher's share is then up to two parts rather
  // than the one the driver rule allows elsewhere: the final part is its
  // by definition, and the one before it is not worth splitting out
  // alone.
  [[nodiscard]] window_plan plan_last(std::size_t bytes,
                                      window_plan p) const noexcept {
    const std::size_t parts = last_window_parts(bytes);
    if (parts == 0) {
      p.tail_bytes = bytes;
      return p;
    }
    const std::uint64_t chunks_with_tail =
        (static_cast<std::uint64_t>(bytes) + chunk_size - 1) / chunk_size;
    p.part_chunks =
        window_part_chunks<Budget>(static_cast<std::size_t>(chunks_with_tail));
    p.parts = parts;
    p.tail_bytes = bytes - parts * p.part_chunks * chunk_size;
    p.node_cost = nodes_for(parts);
    p.edge_slot = 1;
    return p;
  }

  void prepare_compress(window& w) noexcept {
    const bool fanned =
        w.compress.prepare(ops_, w.buffer.data(), w.chunks,
                           w.plan.first_chunk, h_.key_words(), h_.mode_flags(),
                           trace_, w.index);
    // update_file only reaches the pipeline for windows big enough to fan
    // out; a window that is not is exactly what the fallback path exists
    // for.
    assert(fanned && "a window too small to fan out reached the pipeline");
    (void)fanned;
  }

  void reduce_into_tree(window& w, const cv_type& cv) noexcept {
    const bool inserted = reducer_.insert(w.plan.first_chunk, w.chunks, cv);
    // Admission reserved this node before the window started.
    assert(inserted && "the reducer refused a window admission had room for");
    (void)inserted;
    max_pending_ = std::max(max_pending_, reducer_.pending());
    if (w.rec != nullptr) {
      w.rec->t_absorbed = trace_->now();
    }
  }

  void on_done(window& w) noexcept {
    --in_flight_;
    // Whatever this window reserved in the reducer is now either in it
    // or never coming.
    in_flight_nodes_ -= w.plan.node_cost;
    if (&w != last_window_) {
      release(w);
    }
    settle();
  }

  void on_error(window& w, std::error_code ec) noexcept {
    if (!ec_) {
      ec_ = ec;
    }
    stop_ = true;
    --in_flight_;
    in_flight_nodes_ -= w.plan.node_cost;
    if (&w != last_window_) {
      release(w);
    }
    settle();
  }

  void on_exception(window& w, std::exception_ptr e) noexcept {
    if (!eptr_) {
      eptr_ = std::move(e);
    }
    stop_ = true;
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
    if (stop_ || next_offset_ >= file_bytes_) {
      status_.done = true;
    }
  }

  void release(window& w) noexcept {
    stamp_released(w);
    push_free(&w);
  }

  void push_free(window* w) noexcept {
    w->next_free = free_;
    free_ = w;
  }

  void claim_record(window& w) noexcept {
    w.rec = trace_ != nullptr ? trace_->claim_window() : nullptr;
    if (w.rec != nullptr) {
      w.rec->index = w.index;
      w.rec->bytes = w.bytes;
      w.rec->flags = (w.last ? window_record::flag_last : 0) |
                     (w.short_first ? window_record::flag_short_first : 0);
      w.rec->slot = w.slot;
      w.rec->t_wait_begin = trace_->now();
    }
  }

  void stamp_ready(window& w) noexcept {
    if (w.rec != nullptr) {
      w.rec->t_ready = trace_->now();
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
    if (w.rec != nullptr) {
      w.rec->t_joined = trace_->now();
      w.rec->agent_busy_ns =
          w.compress.pt.busy_ns.load(std::memory_order_relaxed);
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
  driver_loop<D> loop_;
  typename D::file& file_;
  Scheduler sched_;
  trace_buffer* trace_ = nullptr;
  const kern::kernel_ops* ops_ = nullptr;
  std::size_t window_bytes_ = 0;
  unsigned count_ = 0;
  unsigned in_flight_cap_ = 0;
  std::uint64_t base_count_ = 0;        // the hasher's byte count at start
  std::size_t head_window_bytes_ = 0;   // the short first window, or zero
  std::uint64_t base_chunk_ = 0;        // where the full windows' grid starts
  std::uint64_t file_bytes_ = 0;
  std::size_t reducer_capacity_ = reducer_nodes;
  std::unique_ptr<window[]> windows_;
  std::unique_ptr<tree_reducer::node[]> nodes_;
  tree_reducer reducer_;
  // Where an edge window's fold leaves its nodes: one buffer for the
  // first window and one for the last, since a three-window file has
  // both in flight at once.
  std::array<std::array<tree_reducer::node, edge_node_cap>, 2> edge_nodes_{};

  loop_status status_{};
  window* free_ = nullptr;
  window* last_window_ = nullptr;
  std::uint64_t next_offset_ = 0;
  std::uint64_t index_ = 0;
  unsigned in_flight_ = 0;
  std::size_t in_flight_nodes_ = 0;
  std::uint64_t admission_stalls_ = 0;
  std::size_t max_pending_ = 0;
  bool stop_ = false;
  std::error_code ec_{};
  std::exception_ptr eptr_{};
};

// Runs one open file through the pipeline on the calling thread.
//
// Tracing is a template parameter below this point, because the window
// chain's type has to be settled before the run starts; here is where the
// runtime choice becomes a compile-time one, once per run.
template <stack_budget Budget, file_driver D, class Scheduler>
void run_window_pipeline(hasher& h, D& drv, typename D::file& file,
                         Scheduler&& sched, const pipeline_plan& plan,
                         const pipeline_options& opts) {
  using sched_type = std::remove_cvref_t<Scheduler>;
  if (opts.trace != nullptr) {
    window_scope<true, Budget, D, sched_type> scope(
        h, drv, file, std::forward<Scheduler>(sched), plan, opts);
    scope.run();
  } else {
    window_scope<false, Budget, D, sched_type> scope(
        h, drv, file, std::forward<Scheduler>(sched), plan, opts);
    scope.run();
  }
}

// The same over a driver whose file is already open, planned from the
// options alone: what the tests and the bench call, where the open mode
// was the caller's.
template <stack_budget Budget, file_driver D, class Scheduler>
void run_window_pipeline(hasher& h, D& drv, typename D::file& file,
                         Scheduler&& sched, const pipeline_options& opts) {
  run_window_pipeline<Budget>(
      h, drv, file, std::forward<Scheduler>(sched),
      plan_pipeline(h.count(), opts.window_bytes, opts.queue_depth, false),
      opts);
}

// Definition-site concept checks, as the I/O backends carry for theirs.
// io_driver is the model every provider has to host; an operation state
// only becomes a type once a receiver is named, which is what
// probe_receiver is for.
namespace pipeline_conformance {
using probe = ex_compat::probe_receiver;

static_assert(ex::sender<read_sender<io_driver>>);
static_assert(ex::operation_state<read_op_state<io_driver, probe>>);
static_assert(ex::sender<schedule_sender<io_driver>>);
static_assert(ex::operation_state<schedule_op<io_driver, probe>>);
static_assert(ex::scheduler<driver_scheduler<io_driver>>);
}  // namespace pipeline_conformance

}  // namespace blake3pp::detail
