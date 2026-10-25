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
//   * the stream running dry with at least half a window in hand -- a
//     producer that is behind must not stall the pool;
//   * end of stream -- the last window, of whatever length.
//
// The hand-over condition is an observable and not a clock. Every read
// blocks, and a read that comes back short of what it asked for is the
// hand-over signal, which costs no call of its own.
//
// Blocking there does not stall the producer: a stream with nothing in
// it is a producer that is not waiting on this reader, while a reader
// that stopped draining a FULL one would stall it. It is not free,
// though. A readiness probe reports an empty queue without sleeping,
// and a blocking read learns the same fact by sleeping. Dropping the
// probe therefore trades calls for wake-ups: measured on Linux at
// 512-byte producer writes, 32% fewer syscalls and 32% more voluntary
// context switches, for 3.6% of wall clock. At 4 KiB writes it is 2.3%,
// and at a producer that keeps up it is nothing -- not because the
// stream stays full, since a 1 MiB pipe cannot fill an 8 MiB ask and
// so nearly every read drains it, but because it refills at once and
// the block returns immediately. What buys the cost back is Windows,
// where the probe has no non-blocking form at all.
//
// A short read means the queue was emptied by it whatever the ask, since
// a read takes everything queued up to what it asked for. The ask
// governs the other branch: a read that comes back WHOLE may have left
// something behind, so the loop reads on rather than handing over. POSIX
// asks for the whole window remainder, which no stream buffer fills, so
// that branch is never taken there and every read reports a drained
// queue; Windows caps the ask at Reader::read_bound(), where it is
// taken. The window lengths come out the same either way: 2 GiB through
// a 1 MiB pipe at 8 MiB windows gives 512 windows, 256 of them flushed
// short, with the cap and without it.
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

// A stream's size while it is still arriving: what file::size() reports
// until the end has been seen, and what the scope reads as "no end
// yet". It is BLAKE3's own message ceiling, so the geometry arithmetic
// around it is the arithmetic a 2^64-byte file would get.
inline constexpr std::uint64_t streaming_size = ~std::uint64_t{0};

// The platform's read, behind a compiled seam.
//
// One call reads what is already there rather than what was asked for:
// a stream's bytes arrive in the producer's own units, and a reader that
// insisted on a full window could not hand one over early.
class stream_reader {
 public:
  struct result {
    std::size_t bytes = 0;
    // The producer closed its end: nothing further will arrive.
    bool eof = false;
  };

  explicit stream_reader(native_stream handle) noexcept : h_(handle) {}

  // Reads into `out`, waiting for the producer's next write if the
  // stream is empty and returning as soon as anything arrives.
  //
  // A count short of `out.size()` is the stream running dry: the queue
  // held less than was asked for, so there is nothing further to take
  // until the producer writes again. That is the driver's hand-over
  // signal, and it comes out of the read the driver was making anyway.
  // @throws std::system_error on a read failure.
  [[nodiscard]] result read_some(std::span<std::byte> out);

  // The most one read may usefully be asked for, or zero where asking
  // for the whole remainder costs nothing.
  //
  // A Windows ReadFile is priced by the range it asks for rather than by
  // the bytes it returns, so an ask of a whole 8 MiB window pays for 8
  // MiB of range in order to receive a pipe's worth. A pipe's buffer is
  // also the most one read of it can ever return, so an ask past it buys
  // nothing: the bound is a Windows anonymous pipe's real capacity, 1
  // MiB. GetNamedPipeInfo is not asked for it -- it answers 4096 on a
  // handle whose buffer is 1 MiB.
  //
  // Measured on this pipe, 2 GiB through the stdin path, against the
  // bound in MiB/s at six producer write sizes (4 KiB to 4 MiB): 256 KiB
  // gives 1026 2507 5357 8870 7186 7468 and 1 MiB gives 973 2260 5010
  // 7497 10708 10587. A smaller bound is up to 15% better where the
  // writes are small and a third slower where they are large, because
  // the ask is then a quarter of what is queued.
  //
  // A POSIX read() copies min(asked, queued) and never touches the rest
  // of the range, so there is no range to pay for and a bound would only
  // cut one call into several. It would also change what the loop hands
  // over: an ask the queue can fill comes back whole, which sends the
  // loop round again instead of flushing, so whole windows go out where
  // half ones did. Measured on Linux, 2 GiB through a 1 MiB pipe at
  // 8 MiB windows, a 1 MiB bound against no bound over six interleaved
  // runs each: level at 512 B and 4 KiB producer writes, and 7% down
  // where the producer keeps up, at 256 full windows against 512 of
  // which half were flushed short.
  [[nodiscard]] static constexpr std::size_t read_bound() noexcept {
#if defined(_WIN32)
    return 1024 * 1024;
#else
    return 0;
#endif
  }

  // Asks the OS for a larger stream buffer, which on Linux is what
  // keeps an 8 MiB window from being 128 hand-offs through a 64 KiB
  // pipe. Best effort: the answer is the buffer in force afterwards, or
  // zero where the platform has no such knob.
  std::size_t widen(std::size_t bytes) noexcept;

 private:
  native_stream h_;
};

// This process's standard input as a stream handle.
[[nodiscard]] native_stream standard_input() noexcept;

// What the driver is allowed to do with a stream.
struct pipe_driver_options {
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
// an empty stream and the end of one without a second process: the
// shipped driver is the alias below it.
template <class Reader>
class basic_pipe_driver {
 public:
  explicit basic_pipe_driver(Reader reader,
                             const pipe_driver_options& opts = {})
      : reader_(std::move(reader)),
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
    least_ = std::max(2 * quantum_, window_bytes / 2 / quantum_ * quantum_);
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
  // producer ever ran dry mid-window. Reported rather than traced: a
  // short flush is the one thing about this driver a caller cannot see
  // from the digest.
  [[nodiscard]] std::uint64_t windows_filled() const noexcept {
    return windows_filled_;
  }
  [[nodiscard]] std::uint64_t short_flushes() const noexcept {
    return short_flushes_;
  }
  [[nodiscard]] std::uint64_t bytes_read() const noexcept { return total_; }
  // The stream buffer the OS granted, or zero where it was not asked.
  [[nodiscard]] std::size_t widened_to() const noexcept { return widened_; }

 private:
  // One window's bytes, and how many of them there are.
  //
  // The loop's target is one byte PAST the window: a window may only be
  // handed over as a subtree once a byte beyond it is known to exist,
  // and that byte becomes the first of the next window. The three ways
  // out are the three ways a window ends.
  //
  // Every read blocks, and every read asks for as much of what is left
  // of the window as the platform prices fairly. No call returns
  // nothing: a read brings back a buffer's worth or a producer's
  // backlog, and one short of its ask carries the dry signal the
  // hand-over wants without a call of its own. Measured on Linux over
  // 2 GiB, the loop reads 2304 times behind a producer that keeps up and
  // 1.65 million times behind 4.19 million 512-byte writes.
  [[nodiscard]] std::size_t fill(std::span<std::byte> out) {
    const std::size_t n = out.size();
    if (eof_ && carry_len_ == 0) {
      return 0;
    }
    std::size_t got = take_carry(out);
    const std::size_t bound = reader_.read_bound();
    bool proved = false;  // a byte past the window is in hand
    while (!eof_ && !proved) {
      const std::span<std::byte> space =
          got < n ? out.subspan(got)
                  : std::span<std::byte>(&probe_, 1);
      const std::size_t ask =
          bound == 0 ? space.size() : std::min(space.size(), bound);
      const typename Reader::result r = reader_.read_some(space.first(ask));
      if (r.eof) {
        eof_ = true;
        break;
      }
      if (got == n) {
        // The window has its bytes; one past it is all that is wanted.
        proved = r.bytes > 0;
        continue;
      }
      got += r.bytes;
      if (r.bytes == ask) {
        // The ask came back whole, so the queue may hold more: read on
        // without asking whether it does.
        continue;
      }
      // The stream ran dry. A prefix that is a whole number of parts
      // can be hashed now; what is left stays in hand, which is also
      // what proves the flushed prefix is not the last window. The
      // floor is compared first because a producer that dribbles takes
      // this branch on every write, and below the floor there is
      // nothing to compute.
      const std::size_t flushed = got < least_ ? 0 : flushable(got, n);
      if (flushed > 0) {
        keep_carry(out, flushed, got);
        ++short_flushes_;
        ++windows_filled_;
        total_ += flushed;
        return flushed;
      }
      // Below the floor, waiting beats fragmenting. The read that
      // follows may sleep, which is what a dribbling producer costs
      // here, but it holds nothing up: a dry stream is a producer that
      // is not waiting on this reader.
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
  // brings itself back to its boundary that way. Zero means nothing
  // worth flushing: the fill waits instead.
  //
  // Half the GRID's window is the floor, whatever this ask is. A
  // hand-over costs a round trip through the pool and what it buys is
  // the overlap between hashing the prefix and reading the rest, so a
  // short prefix is not worth one. An empty stream is not rare: a reader
  // faster than its producer empties it once per buffer, and the buffer
  // is a fraction of the window (1 MiB on Linux once widened, 16 KiB on
  // Darwin).
  //
  // The floor is taken from the grid rather than from `n` because the
  // ask after a short flush is the remainder of the window, and half of
  // THAT would halve again on every flush: the asks would cascade down
  // to one part. Against the grid, the window that brings the grid back
  // to its boundary cannot be flushed at all -- its ask is below the
  // floor -- so it fills, and a run alternates between a flushed window
  // and a whole one.
  //
  // Behind a producer that runs dry, a 2 GiB stream at 8 MiB windows
  // runs on Linux as 512 windows, 256 of them flushed short.
  [[nodiscard]] std::size_t flushable(std::size_t got,
                                      std::size_t n) const noexcept {
    std::size_t whole = got / quantum_ * quantum_;
    if (whole == got) {
      whole = whole >= quantum_ ? whole - quantum_ : 0;
    }
    whole = std::min(whole, n >= 4 * quantum_ ? n - 2 * quantum_ : 0);
    return whole >= least_ ? whole : 0;
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
  std::size_t quantum_ = chunk_size;
  std::size_t least_ = 0;
  std::size_t widen_to_ = 0;
  std::size_t widened_ = 0;
  unsigned windows_ = 0;
  std::size_t window_bytes_ = 0;
  std::size_t carry_len_ = 0;
  std::byte probe_{};
  std::uint64_t size_ = streaming_size;
  std::uint64_t total_ = 0;
  std::uint64_t windows_filled_ = 0;
  std::uint64_t short_flushes_ = 0;
  bool eof_ = false;
};

using pipe_driver = basic_pipe_driver<stream_reader>;

static_assert(file_driver<pipe_driver>);

}  // namespace blake3pp::detail
