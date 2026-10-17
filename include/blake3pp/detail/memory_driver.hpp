#pragma once

// A source whose windows are already in memory, and the completion
// machinery every such source shares.
//
// The pipeline was written for a device: a window is a buffer the driver
// fills and hands back from poll(). A span needs none of that, and the
// cheapest way to say so is a driver whose windows are VIEWS of the
// caller's bytes -- nothing is allocated, nothing is copied, and a read
// is the act of noticing that the bytes are already there.
//
// What it still does is complete from poll(), on the thread that called
// it. That is not ceremony: the scope starts the next window from inside
// a completion, so a read that completed inside submit_read() would run
// the rest of one window's chain inside the start of another, which is
// the recursion the pipeline's second rule exists to prevent. The queue
// below is one round's worth of noticing, deferred to the one place
// callbacks are allowed to run.
//
// Internal to the library. Standard library only; it names no execution
// provider, and the driver concept it models names no driver.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

#include <algorithm>
#include <condition_variable>
#include <mutex>
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
// Shared by every such source: the in-memory driver below, and the
// bench's virtual source, which differ only in where a window's bytes
// come from.
class queued_reads {
 public:
  queued_reads() = default;
  queued_reads(const queued_reads&) = delete;
  queued_reads& operator=(const queued_reads&) = delete;

  void push(io_op& op) noexcept {
    // Constructed fresh over whatever the last read left, which is the
    // one way the caller's storage is ever filled.
    op_node& n = op.emplace_native<op_node>();
    n.next = nullptr;
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
  // contract's teardown drain for a source with nothing in flight.
  void drain() noexcept {
    head_ = tail_ = nullptr;
    queued_ = 0;
  }

 private:
  // The queue links live in the caller's operation, like every backend's
  // do, so nothing is allocated per read either.
  struct op_node {
    io_op* next = nullptr;
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

// A source whose windows are views of bytes the caller already has:
// nothing is allocated, nothing is copied, and a read is the act of
// noticing that the bytes are there.
class memory_driver {
 public:
  // The bytes of one "file". Several may share a driver, which is what
  // a caller hashing a sequence of spans through one scope needs.
  class file {
   public:
    file(memory_driver&, std::span<const std::byte> bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] std::uint64_t size() const noexcept {
      return static_cast<std::uint64_t>(bytes_.size());
    }
    [[nodiscard]] std::string_view name() const noexcept { return "memory"; }

   private:
    friend class memory_driver;
    std::span<const std::byte> bytes_;
  };

  memory_driver() = default;
  memory_driver(const memory_driver&) = delete;
  memory_driver& operator=(const memory_driver&) = delete;

  // Nothing is owned, so there is nothing to size. The geometry is kept
  // only to check that windows ask for slots the plan accounted for;
  // kept in every build, so the type has one layout whatever NDEBUG a
  // translation unit was compiled with.
  void reserve(unsigned windows,
               [[maybe_unused]] std::size_t window_bytes) noexcept {
    windows_ = windows;
  }

  // A window of a span is a subspan of it. Nothing to read into, because
  // the bytes never move.
  // @throws std::logic_error for a slot or a window the geometry does not
  //         cover: past the end this hands back a view of the caller's own
  //         memory beyond the span being hashed.
  [[nodiscard]] window_storage window_storage_for(
      file& f, [[maybe_unused]] unsigned slot, std::uint64_t off,
      std::size_t bytes) {
    // Checked in every build: the subspan below is a view into the
    // caller's own memory, and a window past its end is a read of whatever
    // follows it -- which on this driver is the caller's address space
    // rather than an arena of ours.
    require(slot < windows_, "a window outside the reserved geometry");
    require(off + bytes <= f.bytes_.size(),
            "window_storage_for(): a window past the end of the span being "
            "hashed");
    return {{}, f.bytes_.subspan(static_cast<std::size_t>(off), bytes)};
  }

  void submit_read(file&, std::uint64_t, std::span<std::byte>,
                   io_op& op) noexcept {
    reads_.push(op);
  }

  void flush() noexcept {}
  std::size_t poll(bool block) { return reads_.poll(block); }
  void wake() noexcept { reads_.wake(); }
  void cancel(io_op& op) noexcept { reads_.cancel(op); }
  [[nodiscard]] std::size_t in_flight() const noexcept {
    return reads_.in_flight();
  }
  void drain() noexcept { reads_.drain(); }
  // One file's share of it. Every queued read of this driver is a view of
  // bytes the caller already has, so none is in anyone else's hands and
  // dropping them all is dropping this file's.
  void drain(file&) noexcept { reads_.drain(); }

 private:
  queued_reads reads_;
  [[maybe_unused]] unsigned windows_ = 0;
};

static_assert(file_driver<memory_driver>);

}  // namespace blake3pp::detail
