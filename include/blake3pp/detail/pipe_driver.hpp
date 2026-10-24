#pragma once

// A source whose bytes arrive rather than sit: the window driver for a
// pipe, a socket, or anything else that cannot be seeked.
//
// The file driver the pipeline was written for pread()s at a window's
// offset, and the grid assumes the offset is the window's index times
// its size. A stream has neither: its bytes come past once, in order,
// and how many there are is known only when they stop. What it still
// has is the one property the pipeline needs -- reads are submitted in
// grid order -- so a driver that fills whichever window admission hands
// it, in the order it is handed them, delivers the same bytes at the
// same offsets.
//
// The shape is the bench's null_driver: the completion half is
// memory_driver's queued_reads, because a window whose bytes are
// already in the arena has one job left, to report from poll() and not
// before. Filling the window is the one duty added, and it happens
// where the pipeline asks for the window's storage -- on the driver
// thread, while the pool hashes the windows admitted before it. That
// overlap is the whole point: the stdin path otherwise copies and
// hashes in sequence.
//
// Three things end a window, whichever comes first:
//
//   * the window's byte count -- the normal case, and the only one a
//     producer that keeps up ever reaches;
//   * the flush deadline -- a producer that is behind must not stall
//     the pool or grow its latency without bound;
//   * end of stream -- the last window, of whatever length.
//
// A short flush is a multiple of the flush quantum (one part of the
// window grid), which is what keeps every extent a run of whole,
// aligned parts: tree_reducer admits a range only with a power-of-two
// size on a multiple of that size, and fold_aligned_runs decomposes a
// part-aligned run into exactly those.
//
// One byte of read-ahead is the price of the tree's own rule. A subtree
// handed to the hasher must not contain the message's final chunk, so a
// window may only be inserted once a byte past it is known to exist:
// each fill reads one byte beyond the window and carries it into the
// next. A stream whose length is a multiple of the window would
// otherwise end with its final chunk inside a subtree, which is a wrong
// digest and no error.
//
// Internal to the library. Standard library only: the platform's read
// lives behind stream_reader, whose definition is compiled in
// src/io/stream_reader.cpp.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/contract.hpp>
#include <blake3pp/detail/driver.hpp>
#include <blake3pp/detail/memory_driver.hpp>

namespace blake3pp::detail {

// The descriptor or handle a stream is read through: what the platform
// calls one, with no platform header to say so.
#if defined(_WIN32)
using native_stream = void*;
#else
using native_stream = int;
#endif

// The slowest producer this calls fast, in bytes per second.
//
// The flush deadline is one window at this rate, so a producer at or
// above it fills every window before the timer can fire and pays
// nothing for the timer's existence. The figure is a floor, not a
// tuning constant: 512 MiB/s is above a SATA SSD's sequential read and
// far above what a decompressor, a network transfer or a generator
// script delivers, while a producer below it is already an order of
// magnitude behind the hash -- the Z13 streams 6.9 GiB/s through the
// slowest path measured -- so fragmenting its windows costs nothing
// real and buys a bounded wait.
inline constexpr std::uint64_t pipe_fast_producer_bytes_per_s = 512u << 20;

// The flush deadline for a window of window_bytes: how long a producer
// may leave a window unfinished before what has arrived is hashed
// anyway.
[[nodiscard]] constexpr std::chrono::nanoseconds pipe_flush_after(
    std::size_t window_bytes) noexcept {
  return std::chrono::nanoseconds{
      static_cast<std::int64_t>(static_cast<std::uint64_t>(window_bytes) *
                                1'000'000'000u /
                                pipe_fast_producer_bytes_per_s)};
}

// A stream's size while it is still arriving: what file::size() reports
// until the end has been seen, and what the scope reads as "no end
// yet". It is BLAKE3's own message ceiling, so the geometry arithmetic
// around it is the arithmetic a 2^64-byte file would get.
inline constexpr std::uint64_t streaming_size = ~std::uint64_t{0};

// The platform's read, behind a compiled seam.
//
// One call reads what is already there rather than what was asked for:
// a stream's bytes arrive in the producer's own units, and a reader
// that insisted on a full window could not honour a deadline.
class stream_reader {
 public:
  struct result {
    std::size_t bytes = 0;
    // The producer closed its end: nothing further will arrive.
    bool eof = false;
    // The deadline passed with nothing more available. Not an error,
    // and `bytes` may still be non-zero.
    bool timed_out = false;
  };

  explicit stream_reader(native_stream handle) noexcept : h_(handle) {}

  // Reads into `out`, returning as soon as anything arrives.
  //
  // `deadline_ns` is a steady_clock point in nanoseconds, or
  // no_deadline to wait as long as it takes.
  // @throws std::system_error on a read failure.
  [[nodiscard]] result read_some(std::span<std::byte> out,
                                 std::int64_t deadline_ns);

  // Asks the OS for a larger stream buffer, which on Linux is what
  // keeps an 8 MiB window from being 128 hand-offs through a 64 KiB
  // pipe. Best effort: the answer is the buffer in force afterwards, or
  // zero where the platform has no such knob.
  std::size_t widen(std::size_t bytes) noexcept;

  static constexpr std::int64_t no_deadline = -1;

 private:
  native_stream h_;
};

// This process's standard input as a stream handle.
[[nodiscard]] native_stream standard_input() noexcept;

// Now, as the deadlines are expressed.
[[nodiscard]] inline std::int64_t stream_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// What the driver is allowed to do with a stream.
struct pipe_driver_options {
  // How long a window may sit unfinished before what has arrived is
  // flushed short. Zero takes pipe_flush_after(window_bytes), which is
  // resolved in reserve() once the window size is known.
  std::chrono::nanoseconds flush_after{0};
  // The alignment every short flush is rounded down to, in bytes: one
  // part of the window grid, which the caller computes from its stack
  // budget (part_chunks<Budget>). Zero takes one chunk, which is legal
  // for the reducer only when the parts happen to divide it, so a
  // caller that fans windows out states its own.
  std::size_t flush_quantum = 0;
  // Ask the OS for this much stream buffer. Zero leaves it alone.
  std::size_t widen_to = 0;
};

// A stream as a file_driver, over any reader with stream_reader's shape.
//
// The reader is a template parameter so the tests can script arrivals,
// deadlines and the end of the stream without a second process: the
// shipped driver is the alias below it.
template <class Reader>
class basic_pipe_driver {
 public:
  explicit basic_pipe_driver(Reader reader,
                             const pipe_driver_options& opts = {})
      : reader_(std::move(reader)), flush_after_(opts.flush_after),
        quantum_(opts.flush_quantum == 0 ? chunk_size : opts.flush_quantum),
        widen_to_(opts.widen_to) {
    require(quantum_ % chunk_size == 0,
            "pipe_driver: a flush quantum that is not a multiple of the "
            "chunk size");
  }

  basic_pipe_driver(const basic_pipe_driver&) = delete;
  basic_pipe_driver& operator=(const basic_pipe_driver&) = delete;

  // The stream, named as a file. One per driver: a stream has one
  // position, so two of them over one driver would interleave.
  class file {
   public:
    explicit file(basic_pipe_driver& drv) noexcept : drv_(&drv) {}

    // streaming_size until the end has been seen, the real total
    // afterwards. The scope re-reads this after every window for
    // exactly that reason.
    [[nodiscard]] std::uint64_t size() const noexcept { return drv_->size_; }
    [[nodiscard]] std::string_view name() const noexcept { return "pipe"; }

   private:
    basic_pipe_driver* drv_;
  };

  // The arena is this driver's, like the file driver's, because the
  // teardown rule -- no read outstanding before the bytes go -- is the
  // owner's to keep. Zero-initialised, so the pages are resident
  // before the first window rather than faulted in under the clock.
  void reserve(unsigned windows, std::size_t window_bytes) {
    const std::size_t want = static_cast<std::size_t>(windows) * window_bytes;
    if (arena_.size() < want) {
      arena_.assign(want, std::byte{});
    }
    windows_ = windows;
    window_bytes_ = window_bytes;
    carry_.resize(2 * quantum_);
    if (flush_after_.count() == 0) {
      flush_after_ = pipe_flush_after(window_bytes);
    }
    if (widen_to_ != 0) {
      widened_ = reader_.widen(widen_to_);
    }
  }

  // Where window `slot` lives -- and, for this driver, the point at
  // which its bytes arrive.
  //
  // The fill happens here rather than in submit_read() because the
  // window's length is what the pipeline plans its subtrees from, and
  // a stream only knows the length once it has the bytes. Returning a
  // shorter span than was asked for is how the driver says "this is
  // all this window gets", and the scope re-derives the window from it.
  //
  // `read_into` comes back empty: the bytes have already moved, so the
  // read that follows has nothing to do but be reported from poll().
  // @throws std::system_error on a read failure, which unwinds a round
  //         that owes nothing.
  [[nodiscard]] window_storage window_storage_for(file&, unsigned slot,
                                                  std::uint64_t,
                                                  std::size_t bytes) {
    require(slot < windows_ && bytes <= window_bytes_,
            "pipe_driver::window_storage_for(): a window outside the "
            "reserved geometry");
    const std::span<std::byte> slice{
        arena_.data() + static_cast<std::size_t>(slot) * window_bytes_, bytes};
    const std::size_t got = fill(slice);
    return {{}, std::span<const std::byte>(slice.first(got))};
  }

  // The bytes are where they will be read from, so the only work left
  // is reporting from poll() and not before: completing inside a submit
  // would run the rest of one window's chain inside the start of
  // another.
  void submit_read(file& f, std::uint64_t, std::span<std::byte>,
                   io_op& op) noexcept {
    reads_.push(op, &f);
  }

  void flush() noexcept {}
  std::size_t poll(bool block) { return reads_.poll(block); }
  void wake() noexcept { reads_.wake(); }
  void cancel(io_op& op) noexcept { reads_.cancel(op); }
  [[nodiscard]] std::size_t in_flight() const noexcept {
    return reads_.in_flight();
  }
  void drain() noexcept { reads_.drain(); }
  void drain(file& f) noexcept { reads_.drain(&f); }

  // What the fills did, for a caller that wants to know whether the
  // deadline ever had to fire. Reported rather than traced: a short
  // flush is the one thing about this driver a caller cannot see from
  // the digest.
  [[nodiscard]] std::uint64_t windows_filled() const noexcept {
    return windows_filled_;
  }
  [[nodiscard]] std::uint64_t timer_flushes() const noexcept {
    return timer_flushes_;
  }
  [[nodiscard]] std::uint64_t bytes_read() const noexcept { return total_; }
  // The stream buffer the OS granted, or zero where it was not asked.
  [[nodiscard]] std::size_t widened_to() const noexcept { return widened_; }
  [[nodiscard]] std::chrono::nanoseconds flush_after() const noexcept {
    return flush_after_;
  }

 private:
  // One window's bytes, and how many of them there are.
  //
  // The loop's target is one byte PAST the window: a window may only be
  // handed over as a subtree once a byte beyond it is known to exist,
  // and that byte becomes the first of the next window. The three ways
  // out are the three ways a window ends.
  [[nodiscard]] std::size_t fill(std::span<std::byte> out) {
    const std::size_t n = out.size();
    if (eof_ && carry_len_ == 0) {
      return 0;
    }
    std::size_t got = take_carry(out);
    std::int64_t deadline = deadline_from_now();
    bool proved = false;  // a byte past the window is in hand
    while (!eof_ && !proved) {
      const std::span<std::byte> space =
          got < n ? out.subspan(got)
                  : std::span<std::byte>(&probe_, 1);
      const typename Reader::result r = reader_.read_some(space, deadline);
      if (r.eof) {
        eof_ = true;
        break;
      }
      if (got < n) {
        got += r.bytes;
      } else {
        proved = r.bytes != 0;
      }
      if (r.timed_out && !proved) {
        // The producer is behind. A prefix that is a whole number of
        // parts can be hashed now; what is left stays in hand, which is
        // also what proves the flushed prefix is not the last window.
        const std::size_t flushed = flushable(got, n);
        if (flushed > 0) {
          keep_carry(out, flushed, got);
          ++timer_flushes_;
          ++windows_filled_;
          total_ += flushed;
          return flushed;
        }
        // Less than a flush is worth: a producer this far behind is
        // better waited for than fragmented.
        deadline = deadline_from_now();
      }
    }
    if (proved) {
      carry_[0] = probe_;
      carry_len_ = 1;
      ++windows_filled_;
      total_ += n;
      return n;
    }
    // End of stream: this window is the last, whatever its length, and
    // the hasher takes its tail. An empty stream fills none.
    size_ = total_ + got;
    if (got > 0) {
      ++windows_filled_;
    }
    total_ += got;
    return got;
  }

  [[nodiscard]] std::int64_t deadline_from_now() const noexcept {
    return stream_now_ns() + flush_after_.count();
  }

  // The carry from the previous window, which belongs at the front of
  // this one.
  std::size_t take_carry(std::span<std::byte> out) noexcept {
    if (carry_len_ == 0) {
      return 0;
    }
    const std::size_t n = std::min(carry_len_, out.size());
    std::memcpy(out.data(), carry_.data(), n);
    // out is never shorter than the carry in practice -- the smallest
    // window the grid asks for is one quantum -- but a caller that asks
    // for less must not lose bytes.
    if (n < carry_len_) {
      std::memmove(carry_.data(), carry_.data() + n, carry_len_ - n);
    }
    carry_len_ -= n;
    return n;
  }

  // How much of `got` may be flushed now, out of a window asked for `n`
  // bytes: whole parts, with something kept back at both ends.
  //
  // One part stays in hand because a flush must be provably not the
  // message's end, and two parts of the ASK stay unflushed because the
  // window after this one gets the remainder of the ask -- the grid
  // brings itself back to its boundary that way -- and a window of one
  // part is a round trip through the pool for 16 KiB. Zero means
  // nothing worth flushing: the fill waits instead.
  [[nodiscard]] std::size_t flushable(std::size_t got,
                                      std::size_t n) const noexcept {
    std::size_t whole = got / quantum_ * quantum_;
    if (whole == got) {
      whole = whole >= quantum_ ? whole - quantum_ : 0;
    }
    whole = std::min(whole, n >= 4 * quantum_ ? n - 2 * quantum_ : 0);
    return whole >= 2 * quantum_ ? whole : 0;
  }

  // What a short flush leaves behind, for the window after it.
  void keep_carry(std::span<std::byte> out, std::size_t flushed,
                  std::size_t got) noexcept {
    carry_len_ = got - flushed;
    require(carry_len_ <= carry_.size(),
            "pipe_driver: a short flush left more in hand than the carry "
            "holds");
    std::memcpy(carry_.data(), out.data() + flushed, carry_len_);
  }

  Reader reader_;
  queued_reads reads_;
  std::vector<std::byte> arena_;
  std::vector<std::byte> carry_;
  std::chrono::nanoseconds flush_after_{0};
  std::size_t quantum_ = chunk_size;
  std::size_t widen_to_ = 0;
  std::size_t widened_ = 0;
  unsigned windows_ = 0;
  std::size_t window_bytes_ = 0;
  std::size_t carry_len_ = 0;
  std::byte probe_{};
  std::uint64_t size_ = streaming_size;
  std::uint64_t total_ = 0;
  std::uint64_t windows_filled_ = 0;
  std::uint64_t timer_flushes_ = 0;
  bool eof_ = false;
};

using pipe_driver = basic_pipe_driver<stream_reader>;

static_assert(file_driver<pipe_driver>);

}  // namespace blake3pp::detail
