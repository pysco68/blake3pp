#pragma once

// A source that performs no I/O at all: a read completes at the next
// poll() and never touches the buffer. It exists to measure what the
// pipeline costs with the device removed -- the engine, the fan-out and
// the fold alone -- which is the ceiling every real device is measured
// against. Callers that need defined content fill the buffers once
// before they start; nothing here will overwrite them.
//
// A file has a size and no path, which is why the contract describes
// C::file's construction rather than requiring one spelling of it.
// Internal to src/io/, never installed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "io/backend.hpp"

namespace blake3pp::detail::io_impl {

class null_context {
 public:
  struct read_op : read_op_base {
    read_op* next_queued = nullptr;
  };

  class file {
   public:
    file(null_context&, std::uint64_t size) noexcept : size_(size) {}

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::string_view name() const noexcept { return "null"; }

   private:
    std::uint64_t size_ = 0;
  };

  null_context(const reader_context_options&, unsigned) noexcept {}
  null_context(const null_context&) = delete;
  null_context& operator=(const null_context&) = delete;
  // Queued reads simply never complete if the context dies first, which
  // is the contract's "drain without running callbacks" for a source
  // that has nothing in flight in the first place.

  void submit_read(file&, std::uint64_t, std::span<std::byte>,
                   read_op& op) noexcept {
    op.next_queued = nullptr;
    if (tail_ == nullptr) {
      head_ = tail_ = &op;
    } else {
      tail_->next_queued = &op;
      tail_ = &op;
    }
    queued_++;
  }

  void flush() noexcept {}

  // Completes everything queued: there is no work to pace, and pacing it
  // would put this source's own cost in the measurement.
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
    read_op* op = head_;
    head_ = tail_ = nullptr;
    queued_ = 0;
    while (op != nullptr) {
      read_op* const next = op->next_queued;
      op->done(op, {});
      ran++;
      op = next;
    }
    return ran;
  }

  void wake() noexcept { waiter_.wake(); }

  [[nodiscard]] std::size_t in_flight() const noexcept { return queued_; }

 private:
  read_op* head_ = nullptr;
  read_op* tail_ = nullptr;
  std::size_t queued_ = 0;
  poll_waiter waiter_;
};

static_assert(reader_context<null_context>);

}  // namespace blake3pp::detail::io_impl
