#pragma once

// A source whose bytes arrive rather than sit: the window driver for a
// pipe, a socket, or anything else that cannot be seeked.
//
// The file driver reads at a window's offset, and the grid assumes the
// offset is the window's index times its size. A stream has neither: its
// bytes come past once, in order, and how many there are is known only
// when they stop. The pipeline submits reads in grid order, so a driver
// that fills windows in the order admission hands them out delivers the
// same bytes at the same offsets.
//
// The completion half is memory_driver's queued_reads, as in the bench's
// null_driver: a window whose bytes are already in the arena has one job
// left, to report from poll() and not before. The fill happens where the
// pipeline asks for the window's storage, on the driver thread, while the
// pool hashes the windows admitted before it.
//
// Three things end a window, whichever comes first:
//
//   * the window's byte count -- the normal case, and the only one a
//     producer that keeps up ever reaches;
//   * the stream running dry with at least half a window in hand, where
//     early hand-over is on (pipe_driver_options::early_hand_over);
//   * end of stream -- the last window, of whatever length.
//
// The dry signal is a read that comes back short of its ask. It needs no
// clock and no call of its own: a read takes everything queued up to its
// ask, so a short read emptied the queue. A read that comes back whole may
// have left bytes behind, so the loop reads on. POSIX asks for the whole
// window remainder, which no stream buffer fills, so every POSIX read
// reports a drained queue; Windows caps the ask at Reader::read_bound().
//
// Every read blocks. A stream with nothing in it is a producer that is not
// waiting on this reader, so the block holds nothing up; a reader that
// stopped draining a FULL stream would stall the producer.
//
// The cost is the sleep. A pipe write wakes its reader only when the
// reader is asleep, so a reader that drains the pipe and sleeps turns
// every write of a fast producer into a cross-CPU wake-up. On POSIX
// platforms other than Darwin and Emscripten the reader therefore watches
// an empty stream for a bounded, adaptive budget before it sleeps
// (stream_watch). Measured on Linux over 2 GiB from a page-cache file,
// `dd bs=512` and `dd bs=4096` into the stdin path take 25% and 27% less
// wall with the watch, and 13% and 19% less CPU across both processes:
// the sleeps it removes were themselves system time.
//
// A short flush is a multiple of the flush quantum (one part of the
// window grid), which keeps every extent a run of whole, aligned parts:
// tree_reducer admits a range only with a power-of-two size on a multiple
// of that size, and fold_aligned_runs decomposes a part-aligned run into
// exactly those.
//
// One byte of read-ahead is the price of the tree's own rule. A subtree
// handed to the hasher must not contain the message's final chunk, so a
// window may only be inserted once a byte past it is known to exist:
// each fill reads one byte beyond the window and carries it into the
// next. Without it, a stream whose length is a multiple of the window
// would end with its final chunk inside a subtree, which is a wrong digest
// and no error.
//
// Internal to the library. Standard library only: the platform's read
// lives behind stream_reader, whose definition is compiled in
// src/io/stream_reader.cpp.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
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

// The wait before a read of a stream that is empty: watch it for a
// bounded, adaptive budget, then sleep.
//
// A template over the platform, which supplies four operations --
//
//   now()    the monotonic clock, in nanoseconds;
//   probe()  whether a read would return now, without waiting;
//   sleep()  wait until a read would return;
//   relax()  one iteration of a busy wait;
//
// -- so the tests drive the loop with a scripted clock and scripted
// arrivals and assert on what it counted, and the shipped reader
// supplies the real ones (stream_reader.cpp). The rule can only ever
// cost time, never a byte, so a digest test cannot see a defect in it;
// the counters are what make its shape observable.
//
// The watch replaces a sleep and a wake-up with a few probes, so it pays
// only where a probe costs less than the sleep it removes. Two platforms
// read without it:
//
//   * Darwin. A zero-timeout poll() costs the caller an involuntary
//     context switch whenever anything else is runnable, data queued or
//     not, so the probe that opens every wait is already a loss.
//   * Windows. PeekNamedPipe costs 1.50 us a call, and a watch built on it
//     lost every one of 42 counterbalanced pairs.
//
// Such a platform skips wait() entirely rather than running it with a
// zero budget (stream_reader.cpp carries the measurements).
//
// The budget moves in steps of an eighth of the ceiling and never jumps:
// a probe that finds data grows it, a budget that times out is halved,
// and a sleep that a full-ceiling watch would have avoided grows it.
// Eight consecutive hits re-arm a fully decayed budget. A smaller step
// notices a producer that speeds up more slowly. A step as large as the
// ceiling pays a full wasted watch on every long gap of a producer whose
// gaps straddle the ceiling; measured, that rule lost 5% to a reader that
// only sleeps. The floor is one probe interval: the smallest budget that
// still makes one probe before the sleep.
//
// The ceiling is 16 us and the watch is paced to one probe per
// microsecond, so a full budget is sixteen probes and a write is noticed
// at most a microsecond late. Measured on Linux against probe intervals
// of 4 and 16 us, 1 us was the fastest at 4 KiB producer writes and level
// at 512-byte ones.
//
// This is the only clock in the stream path, and it decides nothing the
// hasher can see: it chooses how the thread waits, watching or sleeping,
// for the same readiness. Nothing here ends a window.
//
// One instance per reader, touched only by the thread that owns the
// driver; a reader shared between threads would race here.
class stream_watch {
 public:
  static constexpr std::int64_t ceiling_ns = 16 * 1000;
  static constexpr std::int64_t probe_interval_ns = 1000;
  static constexpr std::int64_t step_ns = ceiling_ns / 8;
  static constexpr std::int64_t floor_ns = probe_interval_ns;

  // What the waits did. `probes` counts every probe, the one each wait
  // opens with included; `caught` the waits a probe ended; `slept` the
  // waits that reached the sleep.
  struct counters {
    std::uint64_t probes = 0;
    std::uint64_t caught = 0;
    std::uint64_t slept = 0;
  };

  // Returns once a read would return something. May throw whatever the
  // platform's probe or sleep throws.
  template <class Platform>
  void wait(Platform& p) {
    ++stats_.probes;
    if (p.probe()) {
      return;
    }
    const std::int64_t t0 = p.now();
    const std::int64_t budget = budget_;
    std::int64_t last_probe = t0;
    for (;;) {
      std::int64_t t = p.now();
      while (t - last_probe < probe_interval_ns) {
        p.relax();
        t = p.now();
      }
      last_probe = t;
      ++stats_.probes;
      if (p.probe()) {
        budget_ = std::min(ceiling_ns, budget + step_ns);
        ++stats_.caught;
        return;
      }
      // The probe before the budget: a budget of one interval makes
      // exactly one probe, as the floor promises.
      if (t - t0 >= budget) {
        break;
      }
    }
    budget_ = std::max(floor_ns, budget / 2);
    p.sleep();
    ++stats_.slept;
    if (p.now() - t0 < ceiling_ns) {
      budget_ = std::min(ceiling_ns, budget + step_ns);
    }
  }

  [[nodiscard]] std::int64_t budget() const noexcept { return budget_; }
  [[nodiscard]] const counters& stats() const noexcept { return stats_; }

 private:
  std::int64_t budget_ = ceiling_ns;
  counters stats_{};
};

#if defined(_WIN32)
// The bound on a read's ask, learned from what the reads return.
//
// A Windows ReadFile is priced by the range it asks for (see
// stream_reader::read_bound()), so asking for range a read cannot fill
// costs time, and asking for too little costs a whole further ReadFile.
// The bound errs high: it is the largest of the last eight observations
// rounded up to a power of two, and it falls only once eight reads in a
// row have shown a smaller queue.
//
// An observation is what a read returned, unless the window's remainder
// capped the ask and the read came back whole: that read says nothing
// about the queue. Reads the bound capped are observations too. The
// rounding keeps the bound from collapsing. The one-byte read-ahead moves
// the reads off the producer's write boundaries, so the bound can settle
// a byte short of a write; each write then takes a full read and a
// one-byte one, and a rule that learned only from those reads falls to a
// single byte.
//
// Eight is the window. Every window boundary produces a read the
// remainder caps, so the bound has to survive that; eight also bounds how
// long a producer that slows down leaves the bound too high.
//
// Growth has to be probed for, because the ask caps what a read can
// return and an observation can therefore never exceed the bound. Eight
// reads in a row that the bound capped and that came back whole double
// it: one over-asked read in nine, and the only way a producer that
// speeds up is noticed.
//
// A run starts unbounded, so the first read asks for the whole window
// remainder and measures the queue from what comes back: one over-asked
// read per stream. A producer that fills every ask leaves the bound at
// zero, and the stream is never bounded.
class read_ask_bound {
 public:
  static constexpr unsigned window_reads = 8;

  // Zero means unbounded: ask for the whole window remainder.
  [[nodiscard]] std::size_t bound() const noexcept { return bound_; }

  // One read, by what it asked for and what it returned.
  void note(std::size_t asked, std::size_t got) noexcept {
    const bool whole = got == asked;
    if (whole && (bound_ == 0 || asked < bound_)) {
      // Unbounded and filled, or capped by the window's remainder: the
      // queue held at least the ask, and that is all this read shows.
      return;
    }
    recent_[at_ % window_reads] = got;
    ++at_;
    const std::size_t seen = *std::ranges::max_element(recent_);
    bound_ = seen > max_bound ? max_bound : std::bit_ceil(seen);
    if (!whole) {
      capped_ = 0;
      return;
    }
    if (++capped_ < window_reads) {
      return;
    }
    capped_ = 0;
    bound_ = bound_ > max_bound / 2 ? max_bound : bound_ * 2;
  }

 private:
  // Large enough never to bind a real window; small enough for bit_ceil.
  static constexpr std::size_t max_bound = std::size_t{1} << 40;

  std::array<std::size_t, window_reads> recent_{};
  unsigned at_ = 0;
  unsigned capped_ = 0;
  std::size_t bound_ = 0;
};
#endif

// The platform's read, behind a compiled seam.
//
// One call reads what is already there rather than what was asked for:
// a stream's bytes arrive in the producer's own units, and a reader that
// insisted on a full window could not hand one over early.
class stream_reader {
 public:
  struct result {
    // May be zero with eof false: a zero-length message the producer
    // wrote to a Windows message-mode pipe, which the fill loop treats as
    // a read that moved nothing and asks again. A POSIX read() never returns
    // it, because zero there is the end of the stream.
    std::size_t bytes = 0;
    // The producer closed its end: nothing further will arrive.
    bool eof = false;
  };

  // @throws std::system_error (EBADF) for a handle that names no stream:
  // a negative descriptor, or a null or INVALID_HANDLE_VALUE handle.
  // poll() skips a negative descriptor and reports nothing ready, so a
  // wait on one would never return.
  explicit stream_reader(native_stream handle)
      : h_(checked(handle))
#if defined(_WIN32)
        ,
        is_pipe_(handle_is_pipe(handle))
#endif
  {
  }

  // Reads into `out`, waiting for the producer's next write if the
  // stream is empty and returning as soon as anything arrives.
  //
  // A count short of `out.size()` is the stream running dry: the queue
  // held less than was asked for, so there is nothing further to take
  // until the producer writes again. That is the driver's hand-over
  // signal, and it comes out of the read the driver was making anyway.
  //
  // On POSIX platforms other than Darwin and Emscripten, an empty stream
  // is watched for a bounded, adaptive budget before the call sleeps in
  // the kernel (stream_watch); Windows, Darwin and Emscripten block in
  // the read. The budget decides only how long the call watches before
  // it sleeps, never what it returns: what the queue holds when the read
  // is made.
  // @throws std::system_error on a read failure.
  [[nodiscard]] result read_some(std::span<std::byte> out);

  // The watch's budget and counters, for a driver that reports them.
  [[nodiscard]] const stream_watch& watch() const noexcept { return watch_; }

  // The most one read may usefully be asked for, or zero where asking
  // for the whole remainder costs nothing. Asked before every read,
  // because on Windows it is an observation and not a constant.
  //
  // A Windows ReadFile is priced by the range it asks for rather than by
  // the bytes it returns: in a sweep of fixed bounds, a 256 KiB bound beat
  // a 1 MiB one at small producer writes and lost at large ones.
  // read_ask_bound above is the rule that keeps the ask near what the
  // pipe delivers.
  //
  // A POSIX read() copies min(asked, queued) and never touches the rest
  // of the range, so there is no range to pay for and a bound would only
  // cut one call into several. It would also change what the loop hands
  // over: an ask the queue can fill comes back whole, which sends the
  // loop round again instead of flushing. Measured on Linux, a 1 MiB bound
  // was 7% slower than none where the producer keeps up. POSIX therefore
  // answers zero.
  [[nodiscard]] std::size_t read_bound() const noexcept;

  // Asks the OS for a larger stream buffer, which on Linux is what
  // keeps an 8 MiB window from being 128 hand-offs through a 64 KiB
  // pipe. Best effort: the answer is the buffer in force afterwards, or
  // zero where the platform has no such knob or the handle is not a pipe.
  std::size_t widen(std::size_t bytes) noexcept;

 private:
  // Waits until a read would return something -- bytes, the end, or an
  // error -- watching first and sleeping after.
  void wait_readable();

  [[nodiscard]] static native_stream checked(native_stream h) {
#if defined(_WIN32)
    const bool names_none =
        h == nullptr ||
        h == reinterpret_cast<native_stream>(static_cast<std::intptr_t>(-1));
#else
    const bool names_none = h < 0;
#endif
    if (names_none) {
      throw std::system_error(
          std::make_error_code(std::errc::bad_file_descriptor),
          "reading a stream");
    }
    return h;
  }

  native_stream h_;
#if defined(_WIN32)
  // Whether the handle is a pipe, which decides what a read that moved
  // nothing means (see read_some()). Asked once, at construction.
  static bool handle_is_pipe(native_stream h) noexcept;
  // Waits until a PIPE_NOWAIT pipe has bytes queued or its writer has
  // gone; see read_some().
  void wait_for_data();
  bool is_pipe_ = false;
#endif
  stream_watch watch_;
#if defined(_WIN32)
  read_ask_bound ask_bound_;
#endif
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
  // Hand a window over when the producer runs dry before filling it,
  // instead of waiting for the rest of it. Off leaves fill() two exits
  // rather than three: a window ends when it is full, or when the stream
  // does.
  //
  // On by default here. The shipped value is per platform and is set in
  // parallel_io.hpp (stream_hands_over_early); the driver has no platform
  // fork.
  bool early_hand_over = true;
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
        widen_to_(opts.widen_to),
        early_hand_over_(opts.early_hand_over) {
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
    // Two exits are expressed as a floor no window can reach, so the
    // fill loop needs no branch of its own for them.
    least_ = early_hand_over_
                 ? std::max(2 * quantum_,
                            window_bytes / 2 / quantum_ * quantum_)
                 : ~std::size_t{0};
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
  // The reader, for what it counted (stream_reader::watch()).
  [[nodiscard]] const Reader& reader() const noexcept { return reader_; }
  // Whether a window may end when the stream runs dry, as this driver
  // was configured.
  [[nodiscard]] bool hands_over_early() const noexcept {
    return early_hand_over_;
  }

 private:
  // One window's bytes, and how many of them there are.
  //
  // The loop's target is one byte PAST the window: a window may only be
  // handed over as a subtree once a byte beyond it is known to exist,
  // and that byte becomes the first of the next window. The three ways
  // out are the three ways a window ends.
  //
  // Every read blocks, and asks for as much of what is left of the window
  // as Reader::read_bound() allows. A read returns at least one byte,
  // except a zero-length message on a Windows message-mode pipe, which
  // the loop takes as a short read. A read short of its ask is the dry
  // signal the hand-over acts on.
  [[nodiscard]] std::size_t fill(std::span<std::byte> out) {
    const std::size_t n = out.size();
    if (eof_ && carry_len_ == 0) {
      return 0;
    }
    std::size_t got = take_carry(out);
    bool proved = false;  // a byte past the window is in hand
    while (!eof_ && !proved) {
      const std::span<std::byte> space =
          got < n ? out.subspan(got)
                  : std::span<std::byte>(&probe_, 1);
      // Asked per read rather than once per window: on Windows the bound
      // is what the reads have shown the pipe delivers, so it moves.
      const std::size_t bound = reader_.read_bound();
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
  bool early_hand_over_ = true;
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
