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
// A template over file_driver: io_driver for a real device, the bench's
// driver over null_context for a pipeline measured with no device under
// it, and a fake driver in the tests.
//
// Internal to the pipeline. Names an execution provider, so
// <blake3pp/io.hpp> must not reach it.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
#include <utility>

#include <blake3pp/detail/ex_compat.hpp>
#include <blake3pp/detail/io_driver.hpp>

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
  // window chain is a pool thread. The push and the wake are the entire
  // hand-off back to the driver; the wake is one write to the driver's
  // wake primitive per window, and it is what stops a parked poll from
  // sleeping through a ready window.
  void start() & noexcept {
    loop_->queue().push(this);
    loop_->driver().wake();
  }

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
  void start() & noexcept {
    op_.done = &on_read_done;
    op_.owner = this;
    try {
      loop_->driver().submit_read(*file_, off_, buf_, op_);
    } catch (const std::system_error& e) {
      fail(e.code());
    } catch (...) {
      fail(std::make_error_code(std::errc::io_error));
    }
  }

 private:
  // A read that could not be queued owes no callback, but completing the
  // receiver from start() would run the rest of the window chain inside
  // whatever started this one. The run queue is already the way back
  // onto the loop, so the failure takes it and arrives like every other
  // completion.
  void fail(std::error_code ec) noexcept {
    ec_ = ec;
    loop_->queue().push(this);
    loop_->driver().wake();
  }

  static void deliver_submit_failure(run_node* n) noexcept {
    auto* const self = static_cast<read_op_state*>(n);
    ex::set_error(std::move(self->rcvr_), self->ec_);
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
  io_read_op op_{};
};

template <file_driver D>
class read_sender {
 public:
  using sender_concept = ex_compat::sender_tag;
  BLAKE3PP_EX_COMPLETION_SIGNATURES(
      ex::set_value_t(std::span<const std::byte>),
      ex::set_error_t(std::error_code));

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
  [[nodiscard]] run_queue& queue() noexcept { return queue_; }

  // Drives everything until the pipeline says it is done, which happens
  // inside a completion this loop itself ran. Blocks only when there is
  // nothing to run, so the thread sleeps in the kernel rather than
  // spinning, and only ever here.
  void run_until(const loop_status& status) {
    for (;;) {
      drain();
      if (status.done) {
        return;
      }
      drv_->flush();
      const bool block = queue_.empty();
      // What in_flight() was kept across Phase 2b for. Sleeping with no
      // read owed by the driver, nothing to run and nothing outstanding
      // anywhere else is a pipeline nobody will ever wake.
      assert((!block || drv_->in_flight() > 0 || status.outstanding > 0) &&
             "the driver is about to block with no read in flight, an empty "
             "run queue and no chain outstanding: nothing can wake it");
      drv_->poll(block);
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

 private:
  D* drv_;
  run_queue queue_;
};

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
