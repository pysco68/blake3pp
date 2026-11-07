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
// and the thread that called update_file drives the whole pipeline. The
// compress stage alone leaves it, on the caller's scheduler, and
// continues_on brings the result back through driver_scheduler. The only
// threads the library owns sit below the driver: on the synchronous rung the I/O
// backend performs transfers on lanes of its own, and their callbacks
// still run inside poll() on this thread.
//
// That thread is also the contract with the I/O backends: every
// submit_read, flush and poll happens here, and wake() is the only
// member another thread calls. That is why a scheduler exists at all --
// a pool thread cannot touch the driver, so it parks a node on the run
// queue and nudges the driver awake instead.
//
// Three rules hold the shape together, and all of them are about where a
// completion may run and who may touch what afterwards:
//
//   * callbacks run only inside poll(), which is the I/O backends'
//     contract (src/io/backend.hpp);
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
// A fourth rule covers how a run ends, and it is the reason the stop
// path looks the way it does: the pipeline completes EXACTLY ONCE, with
// the file absorbed, with the first failure a window reported, or
// stopped. Stop is one inplace_stop_source per run, handed to every
// stage through the receivers' environment; asking the device to
// abandon a read is a separate, advisory step that completes nothing.
// A cancelled read is still owed its callback and still holds its
// buffer, so nothing is ever destroyed while an operation is owed --
// which is the same rule as the teardown drain, reached from the other
// direction.
//
// A template over file_driver: io_driver for a real device,
// memory_driver for a span already in memory, the bench's virtual source
// for a pipeline measured with no device under it, and a fake driver in
// the tests.
//
// Internal to the pipeline. Names an execution provider, so
// <blake3pp/io.hpp> must not reach it.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include <blake3pp/detail/driver.hpp>
#include <blake3pp/detail/ex_compat.hpp>
#include <blake3pp/trace.hpp>

namespace blake3pp::detail {

// What the caller's stop does, from whichever thread requested it.
// Both halves are safe from any thread: requesting is atomic, and
// wake() is the one member of a driver another thread may call.
template <class D>
struct request_scope_stop {
  ex::inplace_stop_source* source;
  D* drv;
  void operator()() const noexcept {
    source->request_stop();
    drv->wake();
  }
};

// A type each translation unit has its own of; see window_scope's chain
// builders.
namespace {
struct tu_tag {};
}  // namespace

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
// io_sender: one window's read or write.
//
// One shape serves both directions, because from the pipeline's side the
// two are the same operation: queue it, do not complete inside start(),
// and take whatever poll() reports. The direction decides the buffer's
// constness, which driver call submits it and whether the value
// completion carries the bytes back; the rest only the driver knows.

enum class io_dir { read, write };

template <io_dir Dir>
using io_buffer_t = std::conditional_t<Dir == io_dir::read,
                                       std::span<std::byte>,
                                       std::span<const std::byte>>;

template <file_driver D, class Receiver, io_dir Dir>
class io_op_state : public run_node {
 public:
  using operation_state_concept = ex_compat::operation_state_tag;

  io_op_state(Receiver r, driver_loop<D>* loop, typename D::file* f,
              std::uint64_t off, io_buffer_t<Dir> buf, io_op* op,
              unsigned* owed)
      : rcvr_(std::move(r)), loop_(loop), file_(f), off_(off), buf_(buf),
        op_(op), owed_(owed) {
    run = &deliver_parked;
  }

  io_op_state(const io_op_state&) = delete;
  io_op_state& operator=(const io_op_state&) = delete;

  // Queues the transfer and returns: no flush, because the loop flushes
  // once for every operation it started this round, and no completion,
  // because the backend only ever calls back from inside poll().
  //
  // A transfer that could not be queued owes no callback, and neither
  // does one that was never submitted because stop had already been
  // requested. Completing the receiver from start() would run the rest
  // of the window chain inside whatever started this one, so both park
  // on the run queue and arrive like every other completion: a
  // system_error as its code, anything else as the exception itself.
  void start() & noexcept {
    if (stop_token().stop_requested()) {
      stopped_ = true;
      loop_->publish(this);
      return;
    }
    bind_completion<&io_op_state::on_transfer_done>(*op_, *this);
    try {
      if constexpr (Dir == io_dir::read) {
        loop_->driver().submit_read(*file_, off_, buf_, *op_);
      } else {
        loop_->driver().submit_write(*file_, off_, buf_, *op_);
      }
      // From here the driver owes this transfer a callback. A scope
      // winding down has to tell the chains waiting on the driver from the
      // chains running on the pool: the first it can reclaim, the second
      // it has to wait out.
      ++*owed_;
    } catch (const std::system_error& e) {
      ec_ = e.code();
      loop_->publish(this);
    } catch (...) {
      eptr_ = std::current_exception();
      loop_->publish(this);
    }
  }

 private:
  using token_type = ex::stop_token_of_t<ex::env_of_t<Receiver>>;

  [[nodiscard]] token_type stop_token() const noexcept {
    return ex::get_stop_token(ex::get_env(rcvr_));
  }

  static void deliver_parked(run_node* n) noexcept {
    auto* const self = static_cast<io_op_state*>(n);
    if (self->stopped_) {
      ex::set_stopped(std::move(self->rcvr_));
    } else if (self->eptr_) {
      ex::set_error(std::move(self->rcvr_), std::move(self->eptr_));
    } else {
      ex::set_error(std::move(self->rcvr_), self->ec_);
    }
  }

  // A cancelled transfer is only a stop if this chain's token says so.
  // The backends report the outcome, not the cause: operation_canceled
  // from a device nobody asked to cancel is an I/O error like any other,
  // and a cancel that lost the race brings data, which is used.
  void on_transfer_done(std::error_code ec) noexcept {
    --*owed_;
    if (ec) {
      if (ec == std::errc::operation_canceled &&
          stop_token().stop_requested()) {
        ex::set_stopped(std::move(rcvr_));
        return;
      }
      ex::set_error(std::move(rcvr_), ec);
      return;
    }
    // A read reports success only once the whole window is there: the
    // backend reads exactly what was asked for or reports an error.
    ex::set_value(std::move(rcvr_));
  }

  Receiver rcvr_;
  driver_loop<D>* loop_;
  typename D::file* file_;
  std::uint64_t off_;
  io_buffer_t<Dir> buf_;
  // The caller's, and outliving this state: the transfer is still owed
  // when a stopped chain's cell is re-emplaced, and the scope has to be
  // able to name it to cancel it.
  io_op* op_;
  // The scope's count of transfers the driver owes it: raised when a
  // submit has succeeded, lowered by the completion, so a chain that
  // parked without submitting is never counted.
  unsigned* owed_;
  std::error_code ec_{};
  std::exception_ptr eptr_{};
  bool stopped_ = false;
};

template <file_driver D, io_dir Dir>
class io_sender {
  // Both directions complete with no value: the bytes are in the buffer
  // the caller named when it built the sender.
 public:
  using sender_concept = ex_compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(ex::set_value_t(),
                                    ex::set_error_t(std::error_code),
                                    ex::set_error_t(std::exception_ptr),
                                    ex::set_stopped_t());

  io_sender(driver_loop<D>* loop, typename D::file* f, std::uint64_t off,
            io_buffer_t<Dir> buf, io_op* op, unsigned* owed) noexcept
      : loop_(loop), file_(f), off_(off), buf_(buf), op_(op), owed_(owed) {}

  template <class Receiver>
  [[nodiscard]] io_op_state<D, Receiver, Dir> connect(Receiver r) const {
    return {std::move(r), loop_, file_, off_, buf_, op_, owed_};
  }

 private:
  driver_loop<D>* loop_;
  typename D::file* file_;
  std::uint64_t off_;
  io_buffer_t<Dir> buf_;
  io_op* op_;
  unsigned* owed_;
};

template <file_driver D>
using read_sender = io_sender<D, io_dir::read>;
template <file_driver D>
using write_sender = io_sender<D, io_dir::write>;

// --------------------------------------------------------------------
// declared: an adaptor that states its completions instead of asking its
// child, and gates its value on a stop token.
//
// let_value has to know how its child completes, and asks without an
// environment; stdexec's bulk, and a chain containing schedule() on a
// pool, cannot answer that -- they would be asked in turn, with no
// environment, and fail to compile. Connecting asks with the real
// environment, which is where the child is happy to answer. So the
// completions here are DECLARED: what a scheduler and a bulk of noexcept
// bodies can produce, with a channel the child never sends costing
// nothing.
//
// The token turns an arriving value into a stop, which is what a stage
// whose agents stopped part-way needs: the reducer must not take a
// chaining value folded from parts that were never compressed. Only the
// value channel is intercepted -- an error still reaches the scope as an
// error, which is what lets a failure outrank the stop it caused -- and
// the token is read when the value arrives, after the stage's own work
// has finished, so a stop requested while the agents were running is
// still seen. A default token never stops, which is how a chain that
// only needs the declared signatures asks for a plain forward.
//
// Env is named rather than deduced from Op: the operation state holds
// this receiver's operation by value, so it is still incomplete when its
// type is worked out, and a deduced get_env() would need it whole.
template <class Env, class Op, class... Values>
struct declared_receiver {
  using receiver_concept = ex_compat::receiver_tag;
  // RULE 5: a real constructor, and copyable.
  explicit declared_receiver(Op* o) noexcept : op(o) {}
  Op* op;

  // Forwarded, not replaced: the stage below this reads its stop token
  // out of the scope's environment through here.
  [[nodiscard]] Env get_env() const noexcept { return op->downstream_env(); }
  void set_value(Values... v) && noexcept {
    op->value_arrived(std::move(v)...);
  }
  void set_error(std::exception_ptr e) && noexcept {
    op->error_arrived(std::move(e));
  }
  void set_stopped() && noexcept { op->stop_arrived(); }
};

template <class Sender, class Receiver, class... Values>
class declared_op {
 public:
  using operation_state_concept = ex_compat::operation_state_tag;
  using inner_receiver =
      declared_receiver<ex::env_of_t<Receiver>, declared_op, Values...>;

  declared_op(Sender s, Receiver r, ex::inplace_stop_token t)
      : rcvr_(std::move(r)), token_(t),
        inner_(ex::connect(std::move(s), inner_receiver{this})) {}

  declared_op(const declared_op&) = delete;
  declared_op& operator=(const declared_op&) = delete;

  void start() & noexcept { ex::start(inner_); }

  [[nodiscard]] ex::env_of_t<Receiver> downstream_env() const noexcept {
    return ex::get_env(rcvr_);
  }
  void value_arrived(Values... v) noexcept {
    if (token_.stop_requested()) {
      ex::set_stopped(std::move(rcvr_));
    } else {
      ex::set_value(std::move(rcvr_), std::move(v)...);
    }
  }
  void error_arrived(std::exception_ptr e) noexcept {
    ex::set_error(std::move(rcvr_), std::move(e));
  }
  void stop_arrived() noexcept { ex::set_stopped(std::move(rcvr_)); }

 private:
  Receiver rcvr_;
  ex::inplace_stop_token token_;
  ex::connect_result_t<Sender, inner_receiver> inner_;
};

template <class Sender, class... Values>
class declared_sender {
 public:
  using sender_concept = ex_compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(ex::set_value_t(Values...),
                                    ex::set_error_t(std::exception_ptr),
                                    ex::set_stopped_t());

  declared_sender(Sender s, ex::inplace_stop_token t)
      : sndr_(std::move(s)), token_(t) {}

  template <class Receiver>
  [[nodiscard]] declared_op<Sender, Receiver, Values...> connect(
      Receiver r) const {
    return {sndr_, std::move(r), token_};
  }

 private:
  Sender sndr_;
  ex::inplace_stop_token token_;
};

// The value types are named rather than deduced: deducing them would
// mean asking the child how it completes, which is the one thing this
// must not do.
template <class... Values, class Sender>
[[nodiscard]] auto declared(Sender&& s) {
  return declared_sender<std::remove_cvref_t<Sender>, Values...>{
      std::forward<Sender>(s), {}};
}

// The same adaptor with a token: the point in a stage where a value
// becomes a stop.
template <class... Values, class Sender>
[[nodiscard]] auto stopped_if(ex::inplace_stop_token token, Sender&& s) {
  return declared_sender<std::remove_cvref_t<Sender>, Values...>{
      std::forward<Sender>(s), token};
}

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
  explicit driver_loop(D& drv) noexcept : drv_(drv) {}
  driver_loop(const driver_loop&) = delete;
  driver_loop& operator=(const driver_loop&) = delete;

  // The operation is the caller's: it has to outlive the chain that owns
  // this sender, because a read the driver still owes survives the cell
  // its chain lived in.
  // `owed` is the scope's count of transfers this driver owes it, which
  // its teardown reads: see pipeline_scope_base::quiesce().
  [[nodiscard]] read_sender<D> read(typename D::file& f, std::uint64_t off,
                                    std::span<std::byte> buf, io_op& op,
                                    unsigned& owed) noexcept {
    return read_sender<D>{this, &f, off, buf, &op, &owed};
  }

  [[nodiscard]] write_sender<D> write(typename D::file& f, std::uint64_t off,
                                      std::span<const std::byte> buf,
                                      io_op& op, unsigned& owed) noexcept {
    return write_sender<D>{this, &f, off, buf, &op, &owed};
  }

  [[nodiscard]] driver_scheduler<D> scheduler() noexcept {
    return driver_scheduler<D>{this};
  }

  [[nodiscard]] D& driver() noexcept { return drv_; }
  // For a const reporter asking the driver what its engine offered.
  [[nodiscard]] const D& driver() const noexcept { return drv_; }

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
    const publishing pin{this};
    queue_.push(n);
    drv_.wake();
  }

  // A thread that hands run_until's caller something and then still has
  // to reach this loop, the way publish() does, holds one of these across
  // both: run_until does not return until it is gone. The hand-over must
  // come after the constructor, which is what makes the count visible to
  // a caller that saw the hand-over.
  class publishing {
   public:
    explicit publishing(driver_loop* loop) noexcept : loop_(loop) {
      loop_->publishers_.fetch_add(1, std::memory_order_relaxed);
    }
    publishing(const publishing&) = delete;
    publishing& operator=(const publishing&) = delete;
    ~publishing() { loop_->publishers_.fetch_sub(1, std::memory_order_release); }

   private:
    driver_loop* loop_;
  };

  // The state that cannot resolve itself: about to sleep with no read
  // owed, nothing to run and nothing outstanding. Callers that know why
  // it happened pass their own report; this one names the state and is
  // the default for the overloads below that take none.
  [[noreturn]] static void stall() {
    throw std::logic_error(
        "blake3pp: the driver loop would block with no read in flight, an "
        "empty run queue and nothing outstanding: nothing could wake it");
  }

  // Drives everything until the pipeline says it is done, which happens
  // inside a completion this loop itself ran. Blocks only when there is
  // nothing to run, so the thread sleeps in the kernel rather than
  // spinning, and only ever here.
  //
  // before_flush runs once per round, after the completions and before
  // the submit: whatever it starts joins the same flush, so a batch of
  // windows reaches the OS in one call rather than one each.
  //
  // `trace` receives what the thread spends its time on during this call;
  // null records nothing and costs one branch per poll. It is given per
  // call because an async context's loop outlives the operations it runs,
  // and their buffers with them.
  template <class BeforeFlush, class OnStall>
  void run_until(const loop_status& status, BeforeFlush&& before_flush,
                 OnStall&& on_stall, trace_buffer* trace = nullptr) {
    // Also on the way out of a throwing poll(): the caller's next act is
    // to destroy the operation cells, and a thread inside publish() is
    // still reading this loop.
    const publisher_guard guard{this};
    driver_stats* const stats = trace != nullptr ? &trace->driver() : nullptr;
    // The driver's own split of busy against parked, which is what says
    // whether this thread is the bottleneck once the stage shares of a
    // window no longer sum to the wall clock. One clock read per poll,
    // and none at all without a trace buffer.
    std::int64_t t = stats != nullptr ? trace->now() : 0;
    for (;;) {
      const std::size_t ran_queue = drain();
      before_flush();
      if (stats != nullptr) {
        stats->queue_runs += ran_queue;
        ++stats->iterations;
      }
      if (status.done) {
        if (stats != nullptr) {
          stats->busy_ns += static_cast<std::uint64_t>(trace->now() - t);
        }
        return;
      }
      drv_.flush();
      const bool block = queue_.empty();
      // What in_flight() exists for. Sleeping with no
      // read owed by the driver, nothing to run and nothing outstanding
      // anywhere else is a pipeline nobody will ever wake, so it is
      // reported rather than entered -- and reported in every build,
      // because the shapes that reach it are configurations, not bugs a
      // sanitizer lane would find. Nothing is in flight at this point,
      // which is what makes unwinding from here safe.
      if (block && drv_.in_flight() == 0 && status.outstanding == 0) {
        on_stall();
      }
      if (stats == nullptr) {
        drv_.poll(block);
        continue;
      }
      const std::int64_t t_poll = trace->now();
      stats->busy_ns += static_cast<std::uint64_t>(t_poll - t);
      const std::size_t ran = drv_.poll(block);
      t = trace->now();
      ++stats->polls;
      if (block) {
        ++stats->blocking_polls;
        stats->parked_ns += static_cast<std::uint64_t>(t - t_poll);
        if (ran == 0) {
          // Nothing of the driver's own finished, so what released this
          // poll was a wake from a thread handing a window back.
          ++stats->wakes;
        }
      } else {
        stats->busy_ns += static_cast<std::uint64_t>(t - t_poll);
      }
    }
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
    return ran;
  }

  // One flush and one poll, for a caller winding the pipeline down
  // outside run_until. Returns the callbacks the poll ran, and throws
  // what poll() throws.
  std::size_t poll_once(bool block = true) {
    drv_.flush();
    return drv_.poll(block);
  }

 private:
  D& drv_;
  run_queue queue_;
  std::atomic<unsigned> publishers_{0};
};

// --------------------------------------------------------------------
// What both scopes do with a stop and a failure.

// The read and the write pipeline agree on everything about stopping: one
// stop source for the run, a latch that decides which failure the caller
// hears about, and a teardown that cannot trust the loop. They disagree about what a cancel sweep names and about
// when a run is done, so those two stay in the scope. The members below
// are protected, which is how the scope keeps naming them directly.
//
// Derived must provide loop() and sweep_cancels().
template <class Derived, class D>
class pipeline_scope_base {
 protected:
  [[nodiscard]] bool stop_requested() const noexcept {
    return stop_source_.stop_requested();
  }

  // Requesting is what reaches the stages: a receiver's environment hands
  // this source's token down its chain.
  void request_stop() noexcept { stop_source_.request_stop(); }

  // The first failure is the one the caller hears about. Stop and error
  // race by construction -- requesting stop is what makes the other
  // windows finish, and one of them may fail on the way out -- so the
  // latch decides: whatever was latched first wins, and a failure latched
  // at any point outranks the stop it caused. Failures after the first
  // are dropped: the first is the cause, and the rest are usually what
  // stopping did.
  void latch_error(std::error_code ec) noexcept {
    if (!ec_ && !eptr_) {
      ec_ = ec;
    }
  }
  void latch_exception(std::exception_ptr e) noexcept {
    if (!ec_ && !eptr_) {
      eptr_ = std::move(e);
    }
  }

  // The stop's own completion, for the blocking entry points: they have
  // no receiver to report set_stopped to, and an error_code is the only
  // channel they have.
  void throw_if_stopped(const char* what) const {
    if (stop_requested()) {
      throw std::system_error(
          std::make_error_code(std::errc::operation_canceled), what);
    }
  }

  // The stop path for a loop that can no longer be trusted to run.
  //
  // Reached when an exception has left the run, so the normal round --
  // request, sweep, let the completions arrive -- cannot be continued:
  // the poll that threw may throw again. The first two steps are the same
  // as any stop's; what differs is the wait behind them. Nothing on the
  // pool may publish into an operation cell once the cells are destroyed,
  // so every chain in flight is waited out here, with the cells alive.
  //
  // Which chains can be hurried and which have to be waited for is the
  // whole of it. A chain waiting on a transfer is the driver's, and the
  // driver can be told to give that transfer up; a chain on the pool or
  // on the run queue comes back on its own and nothing can shorten it.
  // transfers_owed_ counts the first kind, so in_flight_ above it means
  // something is still running somewhere this scope cannot reach, and the
  // only safe answer is to wait.
  //
  // The error the caller is already unwinding with is the one it keeps:
  // whatever a poll throws here is dropped, and a driver that throws or
  // reports nothing that many times is not asked again.
  static constexpr unsigned max_fruitless_polls = 8;

  void quiesce() noexcept {
    Derived& self = static_cast<Derived&>(*this);
    request_stop();
    // Same sweep as a normal round's: the transfers still owed are what
    // this is waiting for, and asking the device to give them back is what
    // shortens the wait. Advisory, so a backend that cannot is no worse
    // off than before.
    self.sweep_cancels();
    unsigned fruitless = 0;
    for (;;) {
      self.loop().drain();
      if (in_flight_ == 0) {
        break;
      }
      assert(transfers_owed_ <= in_flight_);
      if (fruitless < max_fruitless_polls) {
        // Blocking only while the driver has something to hand back.
        // With nothing of anyone's in flight there is nothing for a sleep
        // to wait for: the chains left are on the pool, and the loop's own
        // run queue is where they arrive.
        const bool owed = self.loop().driver().in_flight() != 0;
        std::size_t ran = 0;
        try {
          ran = self.loop().poll_once(owed);
        } catch (...) {
          ran = 0;
        }
        if (ran == 0) {
          ++fruitless;
          std::this_thread::yield();
        } else {
          fruitless = 0;
        }
        continue;
      }
      if (in_flight_ == transfers_owed_) {
        // Only transfers are left, so the driver is asked for them back:
        // this file's, because the driver may be serving other pipelines
        // whose transfers are not this scope's to forget. It runs no
        // callback of THIS scope's, so nothing publishes into a cell
        // afterwards and the chains stay counted -- there is nothing left
        // that could count them down. A sibling file's completion may be
        // reported from inside it, on this thread, where the backends say
        // so.
        self.loop().driver().drain(self.file());
        break;
      }
      std::this_thread::yield();
    }
    self.loop().settle_publishers();
  }

  loop_status status_{};
  unsigned in_flight_ = 0;
  // Of those, the ones waiting on a transfer the driver owes. Kept by the
  // operation states (io_op_state), which are the only code that knows
  // whether a submit happened.
  unsigned transfers_owed_ = 0;
  // The one stop of the run. Every window receiver's environment answers
  // with its token, and the scope's own admission, sweep and settle read
  // it: there is no second flag that could disagree with it.
  ex::inplace_stop_source stop_source_;
  std::error_code ec_{};
  std::exception_ptr eptr_{};
};

}  // namespace blake3pp::detail
