#pragma once

// The completion half every source without I/O shares: reads remembered
// at submit and reported from poll().
//
// Internal to the library. Standard library only; it names no execution
// provider.

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <utility>

#include <blake3pp/detail/contract.hpp>
#include <blake3pp/detail/driver.hpp>

namespace blake3pp::detail {

// The completion half of a source that needs no I/O.
//
// A read of bytes already in memory has nothing to do, but it may not
// say so from submit_read(): the scope starts the next window from
// inside a completion, so completing inline would run one window's chain
// inside the start of another -- the recursion the pipeline's second
// rule exists to prevent. Reads are remembered here and reported from
// poll(), which is the one place callbacks may run.
//
// Shared by every such source: memory_driver, pipe_driver, and the
// bench's virtual source, which differ only in where a window's bytes
// come from.
class queued_reads {
 public:
  queued_reads() = default;
  queued_reads(const queued_reads&) = delete;
  queued_reads& operator=(const queued_reads&) = delete;

  void push(io_op& op, const void* owner) noexcept {
    // Constructed fresh over whatever the last read left, which is the
    // one way the caller's storage is ever filled.
    op_node& n = op.emplace_native<op_node>();
    n.next = nullptr;
    n.owner = owner;
    n.cancelled = false;
    if (tail_ == nullptr) {
      head_ = tail_ = &op;
    } else {
      node_of(*tail_).next = &op;
      tail_ = &op;
    }
    ++queued_;
    op.live = true;  // queued: this source owes the callback
  }

  // The bytes are already there, so a cancel always wins: nothing has
  // been done that a completion could prefer over it. An op this source
  // does not owe is not one to cancel -- its storage holds no node.
  void cancel(io_op& op) noexcept {
    if (op.live) {
      node_of(op).cancelled = true;
    }
  }

  // Everything queued, in submission order. There is no work to pace,
  // and pacing it would only add this source's own cost to a
  // measurement.
  std::size_t poll(bool block) {
    if (head_ == nullptr) {
      if (block) {
        while (!waiter_.take()) {
          waiter_.sleep();
        }
      }
      return 0;
    }
    std::size_t ran = 0;
    io_op* op = head_;
    head_ = tail_ = nullptr;
    queued_ = 0;
    while (op != nullptr) {
      io_op* const next = node_of(*op).next;
      const bool cancelled = node_of(*op).cancelled;
      op->live = false;
      op->done(op, cancelled
                       ? std::make_error_code(std::errc::operation_canceled)
                       : std::error_code{});
      ++ran;
      op = next;
    }
    return ran;
  }

  void wake() noexcept { waiter_.wake(); }

  [[nodiscard]] std::size_t in_flight() const noexcept { return queued_; }

  // Queued reads are forgotten, and none gets its callback: the
  // contract's teardown drain for a source with nothing in flight. The
  // live flag goes down with them, because nothing is owed afterwards.
  void drain() noexcept {
    // The link is read before the flag goes down: native<op_node>() is only
    // valid while the driver owes the operation, which is what live says.
    io_op* op = head_;
    while (op != nullptr) {
      io_op* const next = node_of(*op).next;
      op->live = false;
      op = next;
    }
    head_ = tail_ = nullptr;
    queued_ = 0;
  }

  // One owner's reads, forgotten the same way. The others stay queued and
  // still get their callbacks, which is what a shared driver needs.
  void drain(const void* owner) noexcept {
    io_op** link = &head_;
    tail_ = nullptr;
    while (*link != nullptr) {
      io_op& op = **link;
      op_node& n = node_of(op);
      if (n.owner == owner) {
        *link = n.next;
        op.live = false;
        --queued_;
      } else {
        tail_ = &op;
        link = &n.next;
      }
    }
  }

 private:
  // The queue links live in the caller's operation, like every backend's
  // do, so nothing is allocated per read either.
  struct op_node {
    io_op* next = nullptr;
    // Which file queued it, so one file's reads can be forgotten without
    // touching another's: several spans may share one driver.
    const void* owner = nullptr;
    bool cancelled = false;
  };
  static_assert(sizeof(op_node) <= io_op::storage_size);
  static_assert(alignof(op_node) <= io_op::storage_align);

  [[nodiscard]] static op_node& node_of(io_op& op) noexcept {
    return op.native<op_node>();
  }

  // The only shared state: a pool thread handing a window back wakes a
  // driver parked in poll(). Everything else here is the driver's own
  // thread by contract.
  class waiter {
   public:
    void wake() noexcept {
      {
        const std::lock_guard<std::mutex> lock(m_);
        signalled_ = true;
      }
      cv_.notify_one();
    }
    [[nodiscard]] bool take() noexcept {
      const std::lock_guard<std::mutex> lock(m_);
      return std::exchange(signalled_, false);
    }
    void sleep() {
      std::unique_lock<std::mutex> lock(m_);
      cv_.wait(lock, [this] { return signalled_; });
    }

   private:
    std::mutex m_;
    std::condition_variable cv_;
    bool signalled_ = false;
  };

  io_op* head_ = nullptr;
  io_op* tail_ = nullptr;
  std::size_t queued_ = 0;
  waiter waiter_;
};

}  // namespace blake3pp::detail
