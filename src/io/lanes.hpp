#pragma once

// The threads that perform blocking transfers off the polling thread
// (io_lanes), the queue of discrete transfers that rides them
// (deferred_pump), and the intrusive FIFO both and the engines queue
// operations on (deferred_ops).
// Internal to src/io/, never installed.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <blake3pp/detail/driver.hpp>

namespace blake3pp::detail::io_impl {

// An intrusive FIFO of operations linked through one of their own
// pointers, next_deferred unless another is named: the queue inside
// deferred_pump, the aio engine's list of requests not yet offered to
// the kernel, and the GCD engine's list of landed transfers. Not
// synchronized; deferred_pump and the GCD engine guard theirs under a
// mutex.
template <class Op, Op* Op::*Link = &Op::next_deferred>
class deferred_ops {
 public:
  void push(Op& op) noexcept {
    op.*Link = nullptr;
    if (tail_ == nullptr) {
      head_ = tail_ = &op;
    } else {
      tail_->*Link = &op;
      tail_ = &op;
    }
  }

  // The next one to serve, or null. Removed from the queue by the act of
  // taking it: the caller owes its callback either way.
  [[nodiscard]] Op* take() noexcept {
    Op* const op = head_;
    if (op == nullptr) {
      return nullptr;
    }
    head_ = op->*Link;
    if (head_ == nullptr) {
      tail_ = nullptr;
    }
    return op;
  }

  [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

  // The oldest queued op, left queued; the rest follow its link. For a
  // backend that OFFERS a batch to the OS and only takes off the queue
  // what the OS accepted: reading the queue and emptying it have to be
  // separable for that, since a refusal must leave the order as it was.
  [[nodiscard]] Op* front() const noexcept { return head_; }

  // Forgets every queued op: none was ever issued, so there is nothing
  // to wait for, and none gets its callback.
  void clear() noexcept { head_ = tail_ = nullptr; }

  // Forgets the queued ops `drop` accepts, on the same terms: for a file
  // closing with transfers it could not issue, which name its handles
  // and its share of the count. Returns how many were dropped.
  template <class Pred>
  std::size_t remove_if(Pred drop) noexcept {
    return remove_first_if(std::numeric_limits<std::size_t>::max(), drop);
  }

  // The same over the oldest `n` queued ops only: for a batch offered from
  // the front of the queue, whose answer concerns those and leaves the
  // rest untouched and in order, without walking them.
  template <class Pred>
  std::size_t remove_first_if(std::size_t n, Pred drop) noexcept {
    std::size_t dropped = 0;
    Op* prev = nullptr;
    for (Op* op = head_; op != nullptr && n > 0; --n) {
      Op* const next = op->*Link;
      if (drop(*op)) {
        (prev == nullptr ? head_ : prev->*Link) = next;
        if (tail_ == op) {
          tail_ = prev;
        }
        ++dropped;
      } else {
        prev = op;
      }
      op = next;
    }
    return dropped;
  }

 private:
  Op* head_ = nullptr;
  Op* tail_ = nullptr;
};

// --- Blocking-I/O lanes, and the deferred list that rides them -------------

// A lane is one thread that performs blocking transfers, so the thread
// that calls poll() does not have to.
//
// The pipeline admits windows, returns finished windows to the pool and
// feeds the reducer between two polls, so a whole window's pread inside
// poll() stalls all of it for as long as the device takes. With the
// pread inside poll(), 855 ms of a 1040 ms run was finished hash work
// waiting for the polling thread, against 8 ms through io_uring on the
// same file.
//
// A lane performs the transfer and nothing else. The callback still runs
// only inside poll(), on the thread that called it.

// The most lanes one context runs.
//
// One lane takes the transfer off the polling thread. Each further lane
// is another transfer the device can have at once on a rung that would
// otherwise see exactly one, whatever the queue depth says.
//
// Four, because the transfers here are whole windows (8 MiB by default)
// and large sequential transfers stop gaining at a shallow depth: 64 MiB
// writes at depth 2 and depth 4 measured within 2% of each other, 24.2
// against 23.8 GiB/s on four PCIe 5 drives (cli/gen.cpp). Each lane is a
// thread the hash pool, already sized to the machine, does not get.
inline constexpr unsigned max_io_lanes = 4;

// Lanes for a context whose caller asked for `max_inflight` transfers at
// once. More lanes than transfers can never help, so the depth is a
// ceiling as much as the constant is.
[[nodiscard]] inline unsigned io_lanes_for(unsigned max_inflight) noexcept {
  return std::clamp(max_inflight, 1u, max_io_lanes);
}

// Owns a few lanes and the rules for owning them, and nothing about what
// they carry: the deferred pump (below) runs a queue of discrete transfers
// over them.
//
// Every member is for the polling thread except stopping() and
// wake_poller(), which are what a lane's own body calls.
class io_lanes {
 public:
  // The context's wake(), its one member another thread may call. A lane
  // releases a blocked poll() through it, through whatever that context
  // blocks on: an eventfd, a kqueue user event, a completion port, an
  // event handle.
  using wake_fn = void (*)(void*) noexcept;
  // One lane's whole life. It returns once stopping() reads true.
  using body_fn = void (*)(void*) noexcept;
  // How stop() gets a lane out of whatever it waits on: a condition
  // variable notify. Called after the stop flag is set and before the
  // join, if start() has been given one.
  using unblock_fn = void (*)(void*) noexcept;

  // Spells a context's wake() as one of those:
  //   lanes_(io_lanes::waker<my_context>(), this, io_lanes_for(depth))
  template <class Ctx>
  [[nodiscard]] static constexpr wake_fn waker() noexcept {
    return +[](void* p) noexcept { static_cast<Ctx*>(p)->wake(); };
  }

  // `owner` must outlive this. Every consumer declares this as a member
  // of the owner, so the destructor's join runs before the owner goes.
  io_lanes(wake_fn wake, void* owner, unsigned lanes) noexcept
      : wake_(wake), owner_(owner), want_(std::max(1u, lanes)) {}

  io_lanes(const io_lanes&) = delete;
  io_lanes& operator=(const io_lanes&) = delete;

  // Joins. A lane writes into a buffer the caller owns, so nothing
  // holding one of those buffers may be released before this returns.
  // Each consumer declares this after everything a lane touches.
  ~io_lanes() { stop(); }

  // Starts the lanes, once, on first use: a context whose engine took
  // everything never creates a thread, and setup that throws before the
  // first transfer leaves none behind.
  //
  // False means not one lane could be started, and the caller performs
  // the work on the polling thread instead, so a machine that will not
  // give a thread still completes a transfer it already accepted.
  bool start(body_fn body, unblock_fn unblock, void* cookie) noexcept {
    if (started_) {
      return !lanes_.empty();
    }
    started_ = true;
    unblock_ = unblock;
    cookie_ = cookie;
    try {
      lanes_.reserve(want_);
    } catch (...) {  // not one lane, which the result reports
      return false;
    }
    for (unsigned i = 0; i < want_; ++i) {
      try {
        lanes_.emplace_back([body, cookie] { body(cookie); });
      } catch (...) {  // NOLINT(bugprone-empty-catch): reported by the result
        break;
      }
    }
    return !lanes_.empty();
  }

  // Whether start() has run and started at least one lane.
  [[nodiscard]] bool running() const noexcept {
    return started_ && !lanes_.empty();
  }

  // Lanes that exist, or the number start() will ask for. Never zero, so
  // a caller dividing by it does not have to check: a context with no
  // lanes serves one transfer at a time on the polling thread.
  [[nodiscard]] unsigned count() const noexcept {
    return started_ ? std::max(1u, static_cast<unsigned>(lanes_.size()))
                    : want_;
  }

  // A lane's body returns when this reads true.
  [[nodiscard]] bool stopping() const noexcept {
    return stopping_.load(std::memory_order_acquire);
  }

  // From a lane, when it has something for the polling thread.
  void wake_poller() noexcept { wake_(owner_); }

  // Flag, unblock, join. Idempotent, and what the destructor does.
  void stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    if (unblock_ != nullptr) {
      unblock_(cookie_);
    }
    for (std::thread& t : lanes_) {
      if (t.joinable()) {
        t.join();
      }
    }
    lanes_.clear();
  }

 private:
  wake_fn wake_;
  void* owner_;
  unblock_fn unblock_ = nullptr;
  void* cookie_ = nullptr;
  unsigned want_;
  std::atomic<bool> stopping_{false};
  bool started_ = false;
  std::vector<std::thread> lanes_;
};

// Performs, on io_lanes, the transfers a context could not hand to its
// asynchronous engine: a piece NO_BUFFERING cannot take, a submission the
// kernel refused, or no engine at all.
//
// A queue of discrete transfers of known length, completing in any order,
// which is what a pipeline submits. A source whose read decides its own
// length cannot be one of these and takes a lane of its own.
//
// The operation type needs `next_deferred`, `cancelled` and what
// performed_transfer carries. One link serves both the queue and the done
// list, because an operation is on exactly one of them.
template <class Op>
class deferred_pump {
 public:
  // Performs one transfer and says how it went. Runs on a lane, so it may
  // touch nothing but the operation and the file the operation names.
  // noexcept: a lane has no caller to catch an exception.
  using perform_fn = std::error_code (*)(Op&) noexcept;

  deferred_pump(io_lanes::wake_fn wake, void* owner, perform_fn perform,
                unsigned lanes) noexcept
      : perform_(perform), lanes_(wake, owner, lanes) {}

  deferred_pump(const deferred_pump&) = delete;
  deferred_pump& operator=(const deferred_pump&) = delete;

  // Hands one transfer over. From the polling thread only.
  void push(Op& op) noexcept {
    std::unique_lock<std::mutex> lock(m_);
    if (!lanes_.running()) {
      lanes_.start(&lane_body, &lane_unblock, this);
    }
    accrue();
    queue_.push(op);
    ++queued_;
    if (!lanes_.running()) {
      return;  // the polling thread will serve it: see run_one_here()
    }
    lock.unlock();
    work_.notify_one();
  }

  // Every transfer the lanes have finished, as one list linked through
  // next_deferred, LIFO -- which the contract permits, since a caller
  // matches a completion to its window by the operation and not by order.
  // Each carries its result in result(). From the polling thread
  // only.
  [[nodiscard]] Op* take_done() noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    return std::exchange(done_, nullptr);
  }

  // Serves one queued transfer on the CALLING thread, and only for a
  // context that got no lanes. Returns the operation, performed, or null.
  // From the polling thread only.
  [[nodiscard]] Op* run_one_here() noexcept {
    Op* op = nullptr;
    bool cancelled = false;
    {
      const std::lock_guard<std::mutex> lock(m_);
      if (lanes_.running() || queue_.empty()) {
        return nullptr;
      }
      op = queue_.take();
      accrue();
      --queued_;
      cancelled = op->cancelled;
    }
    perform_one(perform_, *op, cancelled);
    return op;
  }

  // Hands `report` every transfer the lanes finished, then the one
  // run_one_here() serves for a context that got no lanes. Returns how
  // many it reported. From the polling thread only.
  template <class Report>
  std::size_t report_finished(Report&& report) noexcept {
    std::size_t ran = 0;
    for (Op* op = take_done(); op != nullptr;) {
      Op* const next = op->next_deferred;
      report(*op);
      ++ran;
      op = next;
    }
    if (Op* const op = run_one_here(); op != nullptr) {
      report(*op);
      ++ran;
    }
    return ran;
  }

  // Flags a transfer no lane has taken yet, which is the whole of a cancel
  // here: once a lane has one the flag arrives too late and the transfer
  // runs to completion, which is what the contract's advisory cancel
  // allows. Under the lock, because the flag is what a lane reads when it
  // takes the transfer.
  void cancel(Op& op) noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    op.cancelled = true;
  }

  [[nodiscard]] bool queue_empty() const noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    return queue_.empty();
  }

  // Whether queued work waits for the polling thread itself: no lane runs
  // to take it, so only run_one_here() will, and a poll that blocked
  // instead would wait for what it alone can do.
  [[nodiscard]] bool needs_poller() const noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    return !lanes_.running() && !queue_.empty();
  }

  // Forgets the queued transfers `drop` accepts -- for a file closing with
  // transfers it never issued, which name its handles and its share of
  // the count. None was ever performed, so there is nothing to wait for
  // and none gets its callback. Returns how many.
  template <class Pred>
  std::size_t remove_queued_if(Pred drop) noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    accrue();
    const std::size_t dropped = queue_.remove_if(drop);
    queued_ -= std::min(dropped, queued_);
    return dropped;
  }

  // The same for transfers the lanes finished but nobody has reported.
  // Call it after quiesce(), or a transfer still in a lane's hands lands
  // on the list behind this sweep.
  template <class Pred>
  std::size_t remove_done_if(Pred drop) noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    std::size_t dropped = 0;
    Op** link = &done_;
    while (*link != nullptr) {
      Op* const op = *link;
      if (drop(*op)) {
        *link = op->next_deferred;
        op->next_deferred = nullptr;
        ++dropped;
      } else {
        link = &op->next_deferred;
      }
    }
    return dropped;
  }

  // Waits until no lane holds a transfer, so the buffers they were
  // writing into may be released. It does NOT stop a lane picking up what
  // is still queued: a caller reclaiming one file clears that file's
  // queue first, and a caller draining the context clears the whole
  // queue first.
  void quiesce() noexcept {
    std::unique_lock<std::mutex> lock(m_);
    idle_.wait(lock, [this] { return busy_ == 0; });
  }

  // Forgets everything queued and everything finished, reporting none of
  // it: the context-wide teardown drain. Waits the lanes out in the
  // middle, because a transfer one of them holds is writing into memory
  // the caller is about to reuse.
  void clear() noexcept {
    {
      const std::lock_guard<std::mutex> lock(m_);
      accrue();
      queue_.clear();
      queued_ = 0;
    }
    quiesce();
    const std::lock_guard<std::mutex> lock(m_);
    done_ = nullptr;
  }

  // The transfers this has at the device at once, which is the device's
  // queue depth on this rung.
  [[nodiscard]] unsigned depth() const noexcept { return lanes_.count(); }

  // Summed time transfers spent queued here waiting for a lane, with none
  // of the device in it: the measure of too few lanes, as opposed to a
  // device that is the wall.
  [[nodiscard]] std::uint64_t lane_wait_ns() const noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    // Whatever is queued right now has been waiting since the last
    // change, and a caller reading this at the end of a run has usually
    // just drained: accrue that stretch too rather than losing it.
    return lane_wait_ns_ + waiting_since_change();
  }

 private:
  static void lane_body(void* p) noexcept {
    static_cast<deferred_pump*>(p)->run();
  }

  // Takes the lock before notifying, so a lane cannot be between its
  // predicate and its wait when the stop flag is set.
  static void lane_unblock(void* p) noexcept {
    auto* const self = static_cast<deferred_pump*>(p);
    { const std::lock_guard<std::mutex> lock(self->m_); }
    self->work_.notify_all();
  }

  // The transfer, charged to the device around itself: the wait for a
  // lane is already over, so this measures the device alone. Two clock
  // reads per transfer.
  static void perform_one(perform_fn perform, Op& op,
                          bool cancelled) noexcept {
    if (cancelled) {
      op.set_result(std::make_error_code(std::errc::operation_canceled));
      return;
    }
    op.device_begin();
    op.set_result(perform(op));
    op.device_end();
  }

  void run() noexcept {
    for (;;) {
      Op* op = nullptr;
      bool cancelled = false;
      {
        std::unique_lock<std::mutex> lock(m_);
        work_.wait(lock, [this] {
          return lanes_.stopping() || !queue_.empty();
        });
        if (lanes_.stopping()) {
          // Queued transfers are the caller's to forget: a stop only
          // happens after a drain, which has cleared them, and performing
          // one now would write into memory that may already be gone.
          return;
        }
        op = queue_.take();
        accrue();
        --queued_;
        cancelled = op->cancelled;
        ++busy_;
      }
      perform_one(perform_, *op, cancelled);
      {
        const std::lock_guard<std::mutex> lock(m_);
        op->next_deferred = done_;
        done_ = op;
        if (--busy_ == 0) {
          idle_.notify_all();
        }
      }
      lanes_.wake_poller();
    }
  }

  // What everything currently queued has waited since the length last
  // changed. Under the lock.
  [[nodiscard]] std::uint64_t waiting_since_change() const noexcept {
    if (queued_ == 0) {
      return 0;
    }
    const std::int64_t d = steady_now_ns() - since_;
    return d > 0 ? static_cast<std::uint64_t>(d) * queued_ : 0;
  }

  // Little's law, kept incrementally: the summed wait of every transfer
  // that passes through the queue is the integral of the queue's length
  // over time. So one clock read per change of length is enough and no
  // transfer needs a stamp of its own. Under the lock, before the length
  // changes.
  void accrue() noexcept {
    lane_wait_ns_ += waiting_since_change();
    since_ = steady_now_ns();
  }

  mutable std::mutex m_;
  std::condition_variable work_;  // a transfer was queued, or stop() ran
  std::condition_variable idle_;  // the last busy lane finished
  deferred_ops<Op> queue_;
  Op* done_ = nullptr;
  perform_fn perform_;
  std::size_t queued_ = 0;
  std::size_t busy_ = 0;
  std::uint64_t lane_wait_ns_ = 0;
  std::int64_t since_ = 0;
  // Declared last of the state they touch, and joined by its own
  // destructor before any of it goes.
  io_lanes lanes_;
};

}  // namespace blake3pp::detail::io_impl
