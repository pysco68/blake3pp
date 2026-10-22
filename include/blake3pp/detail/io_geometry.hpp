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
inline constexpr std::size_t default_window_bytes = 8 * 1024 * 1024;

// The windows every read entry point keeps in flight by default. Two is
// the minimum that overlaps a read with a hash; the measured sweet spot on
// every device so far is four to eight, and past that a window costs its
// arena without hiding more latency.
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
