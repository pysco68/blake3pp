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
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
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
#include <blake3pp/detail/compress.hpp>
#include <blake3pp/detail/ex_compat.hpp>
#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/detail/driver.hpp>
#include <blake3pp/detail/io_driver.hpp>
#include <blake3pp/detail/tree_reducer.hpp>
#include <blake3pp/dispatch.hpp>
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
    if constexpr (Dir == io_dir::read) {
      // The backend reads exactly what was asked for or reports an
      // error, so the whole window is there.
      ex::set_value(std::move(rcvr_), std::span<const std::byte>(buf_));
    } else {
      ex::set_value(std::move(rcvr_));
    }
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
  // A read hands its window on, a write has nothing left to say.
  using value_signature =
      std::conditional_t<Dir == io_dir::read,
                         ex::set_value_t(std::span<const std::byte>),
                         ex::set_value_t()>;

 public:
  using sender_concept = ex_compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(value_signature,
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
    drv_.wake();
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
  // it happened pass their own report; this one names the state and is
  // the default for the overloads below that take none.
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
      if (stats_ == nullptr) {
        drv_.poll(block);
        continue;
      }
      const std::int64_t t_poll = trace_->now();
      stats_->busy_ns += static_cast<std::uint64_t>(t_poll - t);
      const std::size_t ran = drv_.poll(block);
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
  trace_buffer* trace_ = nullptr;
  driver_stats* stats_ = nullptr;
};

// --------------------------------------------------------------------
// What both scopes do with a stop and a failure.

// The read and the write pipeline agree on everything about stopping: one
// stop source for the run, a latch that decides which failure the caller
// hears about, a count of the ones it drops, and a teardown that cannot
// trust the loop. They disagree about what a cancel sweep names and about
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
  // at any point outranks the stop it caused.
  void latch_error(std::error_code ec) noexcept {
    if (!ec_ && !eptr_) {
      ec_ = ec;
    } else {
      ++dropped_failures_;
    }
  }
  void latch_exception(std::exception_ptr e) noexcept {
    if (!ec_ && !eptr_) {
      eptr_ = std::move(e);
    } else {
      ++dropped_failures_;
    }
  }

  // The stop's own completion, for the blocking entry points: they have
  // no receiver to report set_stopped to, and an error_code is the only
  // channel they have.
  void throw_if_stopped(const char* what) const {
    if (stopped_ || stop_requested()) {
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
  bool stopped_ = false;
  // Failures after the first. They are dropped rather than reported --
  // the first one is the cause and the rest are usually what stopping did
  // -- and counted so a run can say how many there were.
  std::uint64_t dropped_failures_ = 0;
  std::error_code ec_{};
  std::exception_ptr eptr_{};
};

// --------------------------------------------------------------------
// The bounded scope.

// steady_clock in nanoseconds, absolute. The trace's own scale is this
// minus its epoch, so a traced run reads the clock once per event and
// not twice. Both pipelines keep their transfer counters on this scale,
// because they are kept whether or not a trace buffer was passed.
[[nodiscard]] inline std::int64_t steady_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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
  // The kernel table the windows compress with, or null to take the
  // hasher's. The expert entry points hand a caller-supplied table
  // straight through; resolving the hasher's variant back to a
  // registered table would quietly replace it with the library's own.
  const kern::kernel_ops* ops = nullptr;
  // A stop the caller owns. The scope watches it for the whole run and
  // requests its own stop from it, so a token requested from any thread
  // reaches the driver through the one member another thread may call.
  // The default token has no state and costs one stop_possible() test at
  // the start of the run, which is what the blocking entry points pass.
  ex::inplace_stop_token stop_token{};
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
//
// The floor is what any run needs; reducer_nodes_for() adds one node per
// window that may be in flight, because windows complete out of order and
// each one can leave a node with no sibling yet. Without that term a deep
// queue would not stall or fail -- admission would simply stop early and
// run fewer windows than the caller asked for, which is the sort of
// silent difference that makes a queue-depth sweep lie.
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

// The storage one run needs: a contiguous run's decomposition, both edge
// windows' reservations, one node per window that may be in flight, and
// one for the window admission is about to take. Never below
// reducer_nodes, so a small run keeps the headroom the default had.
[[nodiscard]] constexpr std::size_t reducer_nodes_for(
    unsigned windows, std::size_t parts) noexcept {
  const std::size_t needed =
      2 * 54 + 2 * nodes_for(parts) + static_cast<std::size_t>(windows) + 1;
  return std::max(reducer_nodes, needed);
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
  return head % direct_alignment == 0;
}

// The geometry of one update_file() call, decided once from the hasher's
// count, the file's size and the caller's requests and handed to the
// driver, the file and the scope alike, so that no two of them round or
// clamp differently.
struct pipeline_plan {
  // The window, as rounded_window_bytes() rounds the request.
  std::size_t window_bytes;
  // Reads the device sees at once: the queue depth as
  // clamp_queue_depth() clamps the request.
  unsigned reads_in_flight;
  // Window objects -- buffer, part table and operation cell -- and so
  // windows in flight at most, reading or on the pool: at least
  // reads_in_flight, as many as the budget buys up to max_queue_depth,
  // and never more than the file has.
  unsigned windows;
  // The short first window that brings the hasher onto the window grid;
  // zero for a hasher already on it.
  std::size_t head_bytes;
  // Whether the file is opened for direct I/O: the caller asked, the
  // grid is one the platform's alignment can serve, and the file is not
  // below min_pipeline_bytes, where buffered reads are the faster ones.
  // A size the filesystem will not give counts as large.
  bool direct_io;
  // Whether this span ends the message. False for a caller streaming a
  // sequence of spans into one hasher: the final window is then an
  // interior one like any other, and goes to the pool as a subtree
  // instead of to the hasher as a tail. A span that does not end the
  // message has to be a whole number of windows, which the scope
  // asserts, since a partial one could not be a subtree.
  bool ends_message = true;
};

// How many windows a file of file_bytes has on this grid, the short
// first one included; saturates at max_queue_depth, which is all a plan
// can use.
[[nodiscard]] constexpr unsigned file_windows(std::uint64_t file_bytes,
                                              std::size_t window_bytes,
                                              std::size_t head_bytes) noexcept {
  if (file_bytes == 0) {
    return 0;
  }
  std::uint64_t n = 0;
  std::uint64_t rest = file_bytes;
  if (head_bytes > 0) {
    n = 1;
    rest -= std::min<std::uint64_t>(head_bytes, rest);
  }
  n += (rest + window_bytes - 1) / window_bytes;
  return n > max_queue_depth ? max_queue_depth : static_cast<unsigned>(n);
}

// file_bytes is the file's size, or zero when unknown: the window count
// is then the budget's, and the open that follows reports why.
//
// window_budget_bytes widens the window objects the plan allocates
// without touching the depth the device sees. Those windows are the
// arena the pool draws its work from; read_plan_budget_bytes() resolves
// the figure, and io_geometry.hpp carries what it buys and what it
// costs. The arena is resident: a window is touched as soon as it is
// read into, so a wide budget costs its own size in pages.
[[nodiscard]] constexpr pipeline_plan plan_pipeline(
    std::uint64_t count, std::size_t window_bytes, unsigned queue_depth,
    bool direct_io, std::uint64_t file_bytes,
    std::size_t window_budget_bytes = 0) noexcept {
  const std::size_t window = rounded_window_bytes(window_bytes);
  const std::size_t head = first_window_bytes(count, window);
  const unsigned reads = clamp_queue_depth(queue_depth);
  const std::size_t by_budget =
      (window_budget_bytes != 0 ? window_budget_bytes
                                : default_window_budget_bytes) /
      window;
  const auto windows = static_cast<unsigned>(std::clamp<std::size_t>(
      std::max<std::size_t>(reads, by_budget), reads, max_queue_depth));
  return {window, reads,
          file_bytes == 0 ? windows
                          : std::min(windows, file_windows(file_bytes, window, head)),
          head,
          direct_io && direct_io_fits(head) && direct_io_worthwhile(file_bytes)};
}


// The same geometry for a source already in memory.
//
// Two of the file plan's inputs mean nothing here: there is no device to
// keep reads in flight for, and the window budget buys nothing when a
// window is a view rather than a buffer. What is left is how many
// windows may be in flight, which is what bounds the reducer and the
// part tables, and the span's own window count.
[[nodiscard]] constexpr pipeline_plan plan_memory_pipeline(
    std::uint64_t count, std::size_t window_bytes,
    std::uint64_t span_bytes) noexcept {
  const std::size_t window = rounded_window_bytes(window_bytes);
  const std::size_t head = first_window_bytes(count, window);
  const unsigned windows =
      span_bytes == 0
          ? 1
          : std::min(max_queue_depth, file_windows(span_bytes, window, head));
  return {window, windows, windows, head, false};
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
// at construction, and a chain may only start when one is free and
// fewer than the plan's reads are still reading. The two bounds are
// separate on purpose: the device wants a few reads in flight, the pool
// wants windows to work on, and one number cannot serve both. The
// reducer is what lets them finish in any order.
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
  std::uint64_t chunks = 0;
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
    // The window objects above are value-initialised, and the fold of a
    // stopped compress stage depends on it: the agents stop claiming
    // parts where the stop found them, and the fold still reads every
    // slot of the part table, including the ones nobody wrote. Those
    // slots are zero because make_unique<window[]> zero-fills the whole
    // window before any constructor runs, and the table has no
    // initialiser of its own to replace that fill with.
    //
    // What the guarantee rests on is a default constructor that is
    // neither user-provided nor deleted, and no standard trait spells
    // that -- is_trivially_default_constructible is already false here,
    // for the members that do carry initialisers -- so the check is the
    // outcome itself.
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
  using base::dropped_failures_;
  using base::ec_;
  using base::eptr_;
  using base::in_flight_;
  using base::latch_error;
  using base::latch_exception;
  using base::quiesce;
  using base::request_stop;
  using base::status_;
  using base::stop_requested;
  using base::transfers_owed_;
  using base::stop_source_;
  using base::stopped_;

  // The cell store is built outside this class, because the cell types
  // are the return types of the chain builders below and those are only
  // nameable once the class is complete.
  template <class Scope, class FullOp, class ReadOnlyOp, class EdgeOp>
  friend struct pipeline_cells;

 public:
  using cv_type = std::array<std::uint32_t, 8>;
  // The window objects are the pool's; the scope only borrows them.
  using window = pipeline_window<Budget>;

  using request_scope_stop = blake3pp::detail::request_scope_stop<D>;
  using outer_stop_callback =
      ex::stop_callback_for_t<ex::inplace_stop_token, request_scope_stop>;

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
        ops_(opts.ops != nullptr ? opts.ops : resolve(h.selected_arch())),
        window_bytes_(plan.window_bytes),
        count_(std::min(plan.windows, host.pool().count())),
        reads_in_flight_(plan.reads_in_flight),
        in_flight_cap_(opts.in_flight_cap == 0
                           ? count_
                           : std::min<unsigned>(opts.in_flight_cap, count_)),
        base_count_(h.count()),
        head_window_bytes_(plan.head_bytes),
        ends_message_(plan.ends_message),
        base_chunk_((h.count() + head_window_bytes_) / chunk_size),
        file_bytes_(file.size()),
        nodes_total_(reducer_nodes_for(plan.windows, Budget.parts())),
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
    assert((ends_message_ || file_bytes_ % window_bytes_ == 0) &&
           "a span that does not end the message must be a whole number "
           "of windows: a partial one cannot be absorbed as a subtree");
    // Checked in every build: a read of window_bytes_ into a slot the pool
    // sized smaller runs off the end of the arena, which the driver hands
    // out by slot without a length of its own.
    require(host.pool().window_bytes() >= window_bytes_,
            "update_file(): a host whose windows are smaller than this run "
            "plans to read");
    loop_.record_into(trace_);
    // One operation cell per window this run may hold at once. The cells
    // are the scope's; the windows are not, so a borrowed window carries
    // the index of the cell it was lent with. Handed out lowest first,
    // which keeps a run that never fills the pool on the same few cells.
    free_cells_ = std::make_unique<unsigned[]>(count_);
    for (unsigned i = 0; i < count_; ++i) {
      free_cells_[i] = count_ - 1 - i;
    }
    free_cell_count_ = count_;
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
  // that was already stopped -- which stopped_ tells apart.
  [[nodiscard]] bool begin(stop_watch& watch) {
    if (file_bytes_ == 0) {
      return false;
    }
    if (opts_stop_token_.stop_possible()) {
      if (opts_stop_token_.stop_requested()) {
        // Stopped before anything was submitted. Nothing to cancel,
        // nothing to absorb, and the hasher is untouched.
        stopped_ = true;
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
    note_pool_wait_end();
    if (trace_ != nullptr) {
      driver_stats& d = trace_->driver();
      // The rung is the file's, which closes with the run: copied here so a
      // trace read afterwards still names it.
      set_read_backend(d, file_.name());
      d.admission_stalls = admission_stalls_;
      d.max_pending = max_pending_;
      d.max_reads_in_flight = max_reading_;
      d.max_windows_in_flight = max_windows_;
      d.transfers_landed = transfers_landed_;
      d.transfer_bytes = transfer_bytes_;
      d.device_wait_ns = device_wait_ns_;
      d.starved_ns = starved_ns_;
      d.queue_depth_final = reads_in_flight_;
      d.queue_depth_peak = reads_in_flight_;
      d.agent_busy_ns = agent_busy_ns_.load(std::memory_order_relaxed);
      d.device_wait_min_ns = transfers_landed_ > 0 ? device_min_ns_ : 0;
    }
    if (eptr_) {
      return outcome::exception;
    }
    if (ec_) {
      return outcome::error;
    }
    if (stopped_ || stop_requested()) {
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
  [[nodiscard]] bool stopped() const noexcept {
    return stopped_ || stop_requested();
  }

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
          status_, [&] { admit_round(cells); }, [this] { on_stall(); });
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
        if (next_offset_ >= file_bytes_) {
          break;
        }
        // An edge window reserves room for a whole decomposition rather
        // than for one node, so its own cost is planned before it is
        // admitted.
        const std::size_t cost = this->next_window_cost();
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
        const unsigned cell = free_cells_[free_cell_count_ - 1];
        bool is_full = false;
        bool is_edge_fanout = false;
        try {
          take_next(*w);
          claim_record(*w);
          // An edge window is the short first one or the last one, each
          // with its own cell, since a three-window file has both in
          // flight at once; one with nothing to fan out is only a read.
          is_full = !w->last && !w->short_first;
          is_edge_fanout = !is_full && w->plan.parts >= 2;
          if (is_full) {
            prepare_compress(*w);
            cells.full[cell].emplace(typename Cells::connect_full{this, w});
          } else if (is_edge_fanout) {
            (w->last ? cells.last_edge : cells.first_edge)
                .emplace(typename Cells::connect_edge{this, w});
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
        --free_cell_count_;
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
          ex::start(*(w->last ? cells.last_edge : cells.first_edge));
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
  void on_stall() const { this->report_stall(); }

 private:

  // The loop the base's quiesce drives, which is the host's.
  [[nodiscard]] driver_loop<D>& loop() noexcept { return loop_; }
  // What the base's teardown reclaims transfers from: this run's file,
  // never the whole driver, which may be serving other pipelines.
  [[nodiscard]] typename D::file& file() noexcept { return file_; }

  // Where the next window sits in the file: what take_next() hands out,
  // computed from the scope's cursor alone so a window can be planned
  // before it is taken.
  struct geometry {
    std::uint64_t offset = 0;
    std::size_t bytes = 0;
    bool last = false;
    bool short_first = false;
  };



  // What the node buffers hold: the worst case over any run, which is
  // what fold_aligned_runs asserts against.
  static constexpr std::size_t edge_node_cap = 2 * 54;

 public:
  // The three chain builders, the receiver they complete and the
  // environment they carry are public because the cell store that
  // connects them lives outside this class: the cell types are these
  // builders' return types, which cannot be named until the class is
  // complete.

  // What every stage below the scope reads the scope's stop from; the
  // adaptors forward it down the chain.
  struct scope_env {
    ex::inplace_stop_token token;
    [[nodiscard]] ex::inplace_stop_token query(
        ex::get_stop_token_t) const noexcept {
      return token;
    }
  };

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

    [[nodiscard]] scope_env get_env() const noexcept {
      return {scope->stop_source_.get_token()};
    }
    void set_value() && noexcept { scope->on_done(*w); }
    void set_error(std::error_code ec) && noexcept { scope->on_error(*w, ec); }
    void set_error(std::exception_ptr e) && noexcept {
      scope->on_exception(*w, std::move(e));
    }
    void set_stopped() && noexcept { scope->on_stopped(*w); }
  };

  static_assert(std::copy_constructible<scope_receiver>);

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
                          [this, &w](std::span<const std::byte>) {
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
                    [this, &w](std::span<const std::byte>) noexcept {
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
                          [this, &w](std::span<const std::byte>) {
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

  [[nodiscard]] auto edge_compress_stage(window& w) {
    read_landed(w);
    absorb_head(w);
    const window_plan& p = w.plan;
    w.compress.prepare_edge(ops_, w.view().data() + p.head_bytes,
                            p.part_chunks, p.parts, p.first_chunk,
                            h_.key_words(), h_.mode_flags(), trace_, w.index,
                            file_index_);
    const std::span<tree_reducer::node> out(edge_nodes_[w.plan.edge_slot]);
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
    require(nodes <= edge_node_cap,
            "an edge window produced more nodes than its slot holds");
    w.nodes = nodes;
    for (std::size_t i = 0; i < nodes; ++i) {
      const tree_reducer::node& n = edge_nodes_[w.plan.edge_slot][i];
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
  // reading. Timed on the transitions, so a starved stretch costs two
  // clock reads however many rounds it spans.
  //
  // The windows in flight are not part of this. An empty pool means
  // every window this run may hold is already out, so a term comparing
  // the count against its cap would be false wherever the rest is true,
  // and the counter would stay at zero on every run.
  void note_pool_wait() noexcept {
    const bool starved = pool_.empty() && !stop_requested() &&
                         next_offset_ < file_bytes_ &&
                         reading_ < reads_in_flight_;
    if (starved) {
      if (pool_starved_at_ == 0) {
        pool_starved_at_ = steady_now_ns();
      }
      return;
    }
    note_pool_wait_end();
  }

  // Closes an open starved interval, from the round that ends it or
  // from the end of the run.
  void note_pool_wait_end() noexcept {
    if (pool_starved_at_ != 0) {
      starved_ns_ += static_cast<std::uint64_t>(
          std::max<std::int64_t>(0, steady_now_ns() - pool_starved_at_));
      pool_starved_at_ = 0;
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
        std::to_string(in_flight_) + " windows are in flight (" +
        std::to_string(reading_) + " reading, of " +
        std::to_string(reads_in_flight_) + " allowed) holding " +
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
    g.last = ends_message_ && next_offset_ + g.bytes >= file_bytes_;
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
        part_chunks<Budget>(static_cast<std::size_t>(chunks_with_tail));
    const std::uint64_t k = (chunks_with_tail - 1) / p;
    return k < 2 ? 0 : static_cast<std::size_t>(k);
  }

  // Takes the next window's geometry and plan into w and advances.
  //
  // Not noexcept: window_storage_for() checks the geometry against the
  // plan, and admission runs before the window is counted in flight, so
  // the throw unwinds a round that owes nothing.
  void take_next(window& w) {
    w.cancel_asked = false;
    w.view_ready = false;
    const geometry g = next_geometry();
    w.plan = plan_window(g);
    w.offset = g.offset;
    w.bytes = g.bytes;
    w.last = g.last;
    w.short_first = g.short_first;
    w.chunks = w.bytes / chunk_size;
    w.index = index_++;
    w.nodes = 0;
    const window_storage storage =
        loop_.driver().window_storage_for(file_, w.slot, w.offset, w.bytes);
    w.read_into = storage.read_into;
    w.bytes_view = storage.view;
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
    const std::size_t part = part_chunks<Budget>(chunks_left);
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
        part_chunks<Budget>(static_cast<std::size_t>(chunks_with_tail));
    p.parts = parts;
    p.tail_bytes = bytes - parts * p.part_chunks * chunk_size;
    p.node_cost = nodes_for(parts);
    p.edge_slot = 1;
    return p;
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

  void on_done(window& w) noexcept { finish_window(w); }

  void on_error(window& w, std::error_code ec) noexcept {
    latch_error(ec);
    request_stop();
    finish_window(w);
  }

  void on_exception(window& w, std::exception_ptr e) noexcept {
    latch_exception(std::move(e));
    request_stop();
    finish_window(w);
  }

  // A stage that stopped rather than failed. Reached only after stop was
  // requested, except from a provider that stops a bulk on its own,
  // which this handles the same way rather than relying on it not to.
  void on_stopped(window& w) noexcept {
    stopped_ = true;
    request_stop();
    finish_window(w);
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
    if (stop_requested() || next_offset_ >= file_bytes_) {
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
    free_cells_[free_cell_count_++] = w.cell;
    pool_.give_back(&w);
  }

  void claim_record(window& w) noexcept {
    // Unconditional: the device counters are differences of this
    // timestamp and the one read_landed() takes, so they cannot be a
    // tracing caller's privilege. Two clock reads per window, which at
    // an 8 MiB window and 46 GiB/s is about 12,000 a second.
    w.t_read_begin = steady_now_ns();
    w.rec = trace_ != nullptr ? trace_->claim_window() : nullptr;
    if (w.rec != nullptr) {
      w.rec->index = w.index;
      w.rec->bytes = w.bytes;
      w.rec->flags = (w.last ? window_record::flag_last : 0) |
                     (w.short_first ? window_record::flag_short_first : 0);
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
    const std::int64_t t = steady_now_ns();
    ++transfers_landed_;
    transfer_bytes_ += w.bytes;
    const auto service =
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, t - w.t_read_begin));
    device_wait_ns_ += service;
    device_min_ns_ = std::min(device_min_ns_, service);
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
  std::size_t window_bytes_ = 0;
  unsigned count_ = 0;            // window objects, and windows in flight at most
  unsigned reads_in_flight_ = 0;  // windows reading at most: what the device sees
  unsigned in_flight_cap_ = 0;
  std::uint64_t base_count_ = 0;        // the hasher's byte count at start
  std::size_t head_window_bytes_ = 0;   // the short first window, or zero
  bool ends_message_ = true;
  std::uint64_t base_chunk_ = 0;        // where the full windows' grid starts
  std::uint64_t file_bytes_ = 0;
  // The storage this run allocated, and what admission may use of it.
  std::size_t nodes_total_ = reducer_nodes;
  std::size_t reducer_capacity_ = reducer_nodes;
  ex::inplace_stop_token opts_stop_token_{};
  std::unique_ptr<tree_reducer::node[]> nodes_;
  tree_reducer reducer_;
  // Where an edge window's fold leaves its nodes: one buffer for the
  // first window and one for the last, since a three-window file has
  // both in flight at once.
  std::array<std::array<tree_reducer::node, edge_node_cap>, 2> edge_nodes_{};

  // What this scope currently holds, so the cancel sweep names its own
  // reads and not another pipeline's sharing the same pool.
  window* taken_ = nullptr;
  std::unique_ptr<unsigned[]> free_cells_;
  unsigned free_cell_count_ = 0;
  window* last_window_ = nullptr;
  std::uint64_t next_offset_ = 0;
  std::uint64_t index_ = 0;
  unsigned reading_ = 0;  // windows whose read has not landed
  unsigned max_reading_ = 0;
  unsigned max_windows_ = 0;
  std::size_t in_flight_nodes_ = 0;
  std::uint64_t admission_stalls_ = 0;
  std::size_t max_pending_ = 0;
  // What the device did, kept on every run. They are the reason the two
  // window timestamps are unconditional.
  std::uint64_t transfers_landed_ = 0;
  std::uint64_t transfer_bytes_ = 0;
  std::uint64_t device_wait_ns_ = 0;
  std::uint64_t starved_ns_ = 0;
  // The pool's summed compress time over the run. Written by the agent
  // that finished a window, read by the driver after everything has
  // joined, so it is atomic where the other counters above are not.
  std::atomic<std::uint64_t> agent_busy_ns_{0};
  // The shortest transfer the device served, which is its latency with
  // nothing queued ahead. One comparison per landed read.
  std::uint64_t device_min_ns_ = ~std::uint64_t{0};
  // When the pipeline last found the pool empty with work to start;
  // zero when it is not starved. One clock read per transition, not per
  // round.
  std::int64_t pool_starved_at_ = 0;
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
      : full(std::make_unique<std::optional<full_op>[]>(count)) {}

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
  // One cell of each shape per edge window: the first and the last can
  // be in flight together on a file of three windows, and either may
  // turn out to have no parts worth fanning out.
  std::optional<edge_op> first_edge;
  std::optional<read_only_op> first_read;
  std::optional<edge_op> last_edge;
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

// Definition-site concept checks, as the I/O backends carry for theirs.
// io_driver is the model every provider has to host; an operation state
// only becomes a type once a receiver is named, which is what
// probe_receiver is for.
namespace pipeline_conformance {
using probe = ex_compat::probe_receiver;

static_assert(ex::sender<read_sender<io_driver>>);
static_assert(ex::operation_state<io_op_state<io_driver, probe, io_dir::read>>);
static_assert(ex::sender<write_sender<io_driver>>);
static_assert(ex::operation_state<io_op_state<io_driver, probe, io_dir::write>>);
// The declared adaptor over senders with no surprises of their own, so
// the asserts are about the adaptor and not about whatever it happens to
// wrap: the two value shapes the pipeline gates, and the valueless one
// the write side forwards.
using declared_cv_child = decltype(ex::just(std::array<std::uint32_t, 8>{}));
using declared_cv =
    declared_sender<declared_cv_child, std::array<std::uint32_t, 8>>;
static_assert(ex::sender<declared_cv>);
static_assert(ex::operation_state<
              declared_op<declared_cv_child, probe,
                          std::array<std::uint32_t, 8>>>);
using declared_n_child = decltype(ex::just(std::size_t{}));
static_assert(ex::sender<declared_sender<declared_n_child, std::size_t>>);
static_assert(
    ex::operation_state<declared_op<declared_n_child, probe, std::size_t>>);
using declared_void_child = decltype(ex::just());
static_assert(ex::sender<declared_sender<declared_void_child>>);
static_assert(ex::operation_state<declared_op<declared_void_child, probe>>);
static_assert(ex::sender<schedule_sender<io_driver>>);
static_assert(ex::operation_state<schedule_op<io_driver, probe>>);
static_assert(ex::scheduler<driver_scheduler<io_driver>>);
}  // namespace pipeline_conformance

}  // namespace blake3pp::detail
