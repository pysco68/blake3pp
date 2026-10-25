#pragma once

// The geometry every I/O entry point shares: how big a window may be, how
// many may be in flight, and the rounding both answers go through. The
// reader, the writer, the pipelines and the public options structs all take
// their defaults and their bounds from here, so a figure appears once.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace blake3pp::detail {

// The deepest queue the engines and the pipeline accept. The ceiling is
// set by memory: a window in flight costs window_bytes of arena plus its
// own tables (a part table and the chaining values its groups fold into,
// and the lane slots its parts write; see parallel.hpp), so 256 of them
// at the default 8 MiB window is over 2 GiB, and at the 1 GiB maximum
// window more than any machine has. NVMe itself takes tens of thousands of
// outstanding commands. The ceiling guards against a typo, and
// BLAKE3PP_MAX_QUEUE_DEPTH raises or lowers it for a caller who knows the
// device and the budget.
#if defined(BLAKE3PP_MAX_QUEUE_DEPTH)
inline constexpr unsigned max_queue_depth = BLAKE3PP_MAX_QUEUE_DEPTH;
#else
inline constexpr unsigned max_queue_depth = 256;
#endif
static_assert(max_queue_depth >= 2,
              "a queue depth below two cannot overlap a read with a hash");

// The largest window or buffer the engines accept, 1 GiB, or less where
// max_queue_depth of them would not fit in size_t. The cap keeps the pool
// size from wrapping and every transfer below the 32-bit length the
// backends hand the kernel (io_uring's sqe.len, WriteFile's DWORD).
inline constexpr std::size_t max_window_bytes =
    std::min<std::size_t>(std::size_t{1} << 30,
                          std::numeric_limits<std::size_t>::max() / max_queue_depth);

// The smallest window any engine or pipeline accepts. At 64 KiB the
// O_DIRECT alignment is trivially satisfied and a submission is worth its
// cost; below it a window is all overhead.
inline constexpr std::size_t min_window_bytes = 64 * 1024;

// The window every entry point defaults to, for files and for spans
// already in memory. Large enough that one window is several subtrees of
// real work per core, small enough that queue_depth of them is an
// ordinary allocation.
//
// A striped volume reads a larger window faster at this depth. That gain
// is the device's: default_queue_depth below carries the no-hashing
// control arm that shows it, and read_plan_budget_bytes carries what a
// larger window would cost in resident arena.
inline constexpr std::size_t default_window_bytes = 8 * 1024 * 1024;

// The windows every read entry point keeps in flight by default.
//
// Read throughput tracks the bytes in flight -- depth times window --
// more closely than either axis on its own. Measured on a four-drive
// PCIe 5 stripe (AMD EPYC 8124P, md raid0 over four NVMe, xfs, Linux
// 7.0, 32 GiB file, best of 2 per cell): cells of equal product agree
// within 0.8 GiB/s at 512 MiB in flight and within 1.0 at 1 GiB, but
// only there -- the spread is 2.4 at 128 MiB and 3.6 at 256 MiB, so
// the axes are redundant at the top of the surface and not across it.
// Neither single-axis step from the 8 MiB x 4 default is worth taking:
// 8 MiB x 16 reads 36.8 GiB/s and 64 MiB x 4 reads 37.2 against 37.4
// for the default, while 8 MiB x 32 reads 40.8, +9.1%, on the same row.
// The best cell is 64 MiB x 16 at 1 GiB in flight; past 2 GiB the
// figure is flat to worse and the raw device rate declines. On a single
// 9 GiB/s local NVMe (GCP z3-highmem-22) the 8-64 MiB x 4-32 grid lands
// within 9.02-9.04 GiB/s in fifteen of sixteen cells, the sixteenth 8.88
// at 64 MiB x qd 32, so geometry buys nothing there.
//
// What a smaller window costs on the stripe is the device's, and the
// no-hashing control arm shows it. Reading 64 GiB cold through O_DIRECT
// with no hash anywhere in the run, the device alone delivers
// 36.5-38.0 GiB/s at an 8 MiB window and depth 4 and 46.4-46.7 at a
// 64 MiB window and the same depth -- 32 MiB against 256 MiB
// outstanding. The pipeline over it reads 38.0-39.3 and 45.1-45.9, which
// is 102-106% and 97-99% of those controls, with the driver thread busy
// 2-7% of its own time at either size. So the window is worth 25% to the
// DEVICE at a depth of four, and a larger default window would recover
// nothing of the pipeline's.
//
// Four is therefore a starting point: a device that gains nothing from
// depth settles there, and 32 MiB in flight is an arena any machine can
// spare. Where depth does pay, it pays an order of magnitude further out,
// and a caller that knows its device states its own figure.
inline constexpr unsigned default_queue_depth = 4;

// The window the reader actually uses for a requested size: a power of
// two (so every full window is a subtree-aligned unit) between
// min_window_bytes and max_window_bytes. Anything that sizes storage per
// window must use the same rounding.
[[nodiscard]] constexpr std::size_t rounded_window_bytes(
    std::size_t requested) noexcept {
  return std::bit_floor(
      std::clamp<std::size_t>(requested, min_window_bytes, max_window_bytes));
}

// The queue depth the engines and the pipeline actually use for a
// requested one: at least two, so one window can be read while another
// is hashed, and at most max_queue_depth.
[[nodiscard]] constexpr unsigned clamp_queue_depth(unsigned requested) noexcept {
  return std::clamp(requested, 2u, max_queue_depth);
}

// Window objects a stream's run allocates.
//
// A stream has one position, so exactly one window is ever being filled:
// depth buys nothing here, and the count is only what lets the pool and
// the reducer run ahead of the fill. Four is one being filled, one on the
// pool, and two holding completions that wait in the reducer for a
// sibling. stream_io_options::windows overrides it.
inline constexpr unsigned default_stream_windows = 4;

// Buffer memory a read plan spends on window objects, which is both the
// reads the device has outstanding and the windows a pool has work in.
//
// This memory is RESIDENT. A window is touched as soon as it is read
// into, and a run reaches the whole arena within the first few turns of
// the queue, so the cost of a budget is the budget: a 1 GiB arena is a
// gigabyte of pages, not a gigabyte of address space.
//
// What it buys is the pool being fed. The windows beyond the reads are
// the work the hash side has in hand while the device serves the next
// ones, and on a fast device they are worth more than the depth is.
// Measured on the four-drive PCIe 5 stripe (AMD EPYC 8124P, md raid0
// over four NVMe, xfs, Linux 7.0, 32 threads, 64 GiB file, the depth
// held at four reads so the two axes are separate):
//
//   window   arena             GiB/s   pool busy
//   64 MiB   256 MiB (4 win)    37.0     64%
//   64 MiB   384 MiB (6 win)    41.5     76%
//   64 MiB   512 MiB (8 win)    45.9     86%
//   64 MiB     1 GiB (16 win)   45.6     85%
//   64 MiB     2 GiB (32 win)   44.4     85%
//    8 MiB    32 MiB (4 win)    30.2     53%
//    8 MiB    64 MiB (8 win)    37.6     71%
//    8 MiB   128 MiB (16 win)   38.9     75%
//    8 MiB   256 MiB (32 win)   39.3     75%
//    8 MiB     1 GiB (128 win)  38.9     76%
//    8 MiB     2 GiB (256 win)  38.5     74%
//
// So the return flattens at a WINDOW COUNT and not at a number of
// bytes: eight windows at 64 MiB and sixteen at 8 MiB, past which the
// figure is flat and then declines. A byte budget alone gets that wrong
// in one direction: divided by a large window it buys too few windows,
// and 128 MiB at a 64 MiB window buys four, 19% below eight.
//
// The pool-busy column is the mechanism, and narrowing the pool is what
// shows it. The same 8 MiB row on half the machine:
//
//   threads   arena              GiB/s   pool busy
//        32    32 MiB (4 win)     30.2     54%
//        32    64 MiB (8 win)     37.8     71%
//        32   128 MiB (16 win)    38.9     75%
//        32   256 MiB (32 win)    39.5     76%
//        16    32 MiB (4 win)     31.0     88%
//        16    64 MiB (8 win)     36.1     98%
//        16   128 MiB (16 win)    35.0     99%
//        16   256 MiB (32 win)    34.6     99%
//
// Sixteen threads are nearly saturated on four windows, are saturated on
// eight and cannot use more. Thirty-two stand at 54% on four and keep
// gaining to sixteen. The arena's job is to keep the consumer fed, so
// its size is the consumer's question.
//
// (The 16-thread arms are the noisiest here: the two passes of the
// eight-window cell read 34.6 and 37.5. The flatness above eight windows
// survives that, the exact plateau figure does not.)
//
// Hence: one window per threads_per_window hardware threads, which keeps
// a laptop from spending a server's arena, floored at 128 MiB
// (default_window_budget_bytes). Together they land on the measured knee
// at both window sizes -- eight windows at 64 MiB, sixteen at 8 MiB --
// and on four threads they give the plan the floor alone gives, at any
// window size.
// The write side resolves its depth the same way
// (resolve_write_queue_depth), and for the same reason: the parallelism
// that has to be kept fed is the machine's, not the device's.
//
// Windows per thread is not itself constant -- a larger window is more
// work, so fewer of them are needed -- which is why the floor and the
// count are both there and the larger wins. Above the 32 threads
// measured here the count is an extrapolation, and
// max_window_budget_bytes is what bounds it.
//
// A device that is already saturated gains nothing from any of this. On
// the single 9 GiB/s NVMe above the whole geometry grid is one number, so
// there the arena is memory spent for no return.
inline constexpr unsigned threads_per_window = 4;
// The floor, and what a caller with no thread count to offer gets. At
// the default 8 MiB window it is sixteen windows, the count the stripe's
// own 8 MiB row flattens at.
inline constexpr std::size_t default_window_budget_bytes =
    std::size_t{128} << 20;
// The ceiling. Past two gigabytes the stripe's read figure declines and
// its raw device rate with it, so a wide machine asking for a wide
// window is not given an arena nothing was measured to use.
inline constexpr std::size_t max_window_budget_bytes = std::size_t{2} << 30;

// The window budget a read plan is given: one window per
// threads_per_window threads of `threads`, between the floor and the
// ceiling. Every read entry point resolves it this way, so a plan made
// from a caller's options is the same plan whichever entry point made
// it.
//
// `threads` of zero is a caller with no pool size to offer -- a plan
// made straight from an options struct, as the tests and the bench make
// one -- and gets the floor.
[[nodiscard]] constexpr std::size_t read_plan_budget_bytes(
    unsigned threads = 0,
    std::size_t window_bytes = default_window_bytes) noexcept {
  if (threads == 0) {
    return default_window_budget_bytes;
  }
  const std::size_t windows =
      std::max<std::size_t>(1, threads / threads_per_window);
  return std::clamp<std::size_t>(rounded_window_bytes(window_bytes) * windows,
                                 default_window_budget_bytes,
                                 max_window_budget_bytes);
}

// The size below which a message is small: a span of fewer bytes is
// hashed without the pipeline (parallel.hpp has the measurement), and a
// file of fewer bytes is read through the page cache whatever the caller
// asked, because the pipeline it would feed is skipped and the cache
// bypass costs more than it saves there. Measured on four PCIe 5 drives
// in RAID 0, 1000 MiB of files per row, cold, direct against buffered:
// 1.41 against 1.49 GB/s at 64 KiB, 1.99 against 2.18 at 96, 4.48
// against 4.79 at 256, and the other way at 1 MiB, 10.50 against 9.21.
inline constexpr std::size_t min_pipeline_bytes = 512 * 1024;

// Whether a file of file_bytes is read through the cache bypass when the
// caller asked for it: zero is a size the filesystem would not give, and
// counts as large. Every reading entry point asks this, so a file is
// opened the same way whichever one opens it.
[[nodiscard]] constexpr bool direct_io_worthwhile(
    std::uint64_t file_bytes) noexcept {
  return file_bytes == 0 || file_bytes >= min_pipeline_bytes;
}

// The default geometry for a generated file (write_file(), io.hpp).
//
// One window's fill is sequential, so on the write side the windows in
// flight are the whole parallelism and the queue depth is the one lever:
// with the device removed, 2 / 4 / 8 / 16 / 32 in flight generate 4.1 /
// 8.3 / 15.8 / 34.8 / 62.9 GiB/s on 32 cores and the window size moves
// that by a few percent across a 16x range. End to end on four PCIe 5
// drives in RAID 0 the same machine writes 6.5 GiB/s at four in flight
// and 31.0 at thirty-two, 98% of what the device sustains. So the depth
// follows the hardware thread count when none is asked for, and the
// window is small enough that a deep queue stays within a bytes-in-flight
// budget, past which a striped volume slows down (20.7 GiB/s at 512 MiB
// in flight and 14.8 at 2 GiB, against 24.2 at 128 MiB, on the same
// drives).
inline constexpr std::size_t default_write_window_bytes = 8 * 1024 * 1024;
// Zero: resolve_write_queue_depth() decides from the thread count.
inline constexpr unsigned default_write_queue_depth = 0;
// Bytes in flight an automatic depth may reach: 32 windows at the default
// size.
inline constexpr std::size_t default_write_budget_bytes = std::size_t{256} << 20;
// What the sequential write_file() forms use for an automatic depth: one
// buffer fills while the other drains, and a single producer cannot use
// more.
inline constexpr unsigned sequential_write_queue_depth = 2;

// The depth a write over a scheduler uses: the requested one, clamped
// like every other, or for a request of zero one window per thread of
// `threads`, bounded so that depth x window stays within the budget.
//
// One window's fill is sequential and one window is one fill task, so
// the depth is the generator's parallelism as well as the device's
// queue: below the thread count the pool is under-used.
[[nodiscard]] constexpr unsigned resolve_write_queue_depth(
    unsigned requested, std::size_t window_bytes, unsigned threads) noexcept {
  if (requested != 0) {
    return clamp_queue_depth(requested);
  }
  const std::size_t window = rounded_window_bytes(window_bytes);
  const std::size_t by_budget = default_write_budget_bytes / window;
  const std::size_t want = std::min<std::size_t>(std::max(1u, threads), by_budget);
  return clamp_queue_depth(static_cast<unsigned>(
      std::min<std::size_t>(want, std::numeric_limits<unsigned>::max())));
}

}  // namespace blake3pp::detail
