#pragma once

// The geometry every I/O entry point shares: how big a window may be, how
// many may be in flight, and the rounding both answers go through. The
// reader, the writer, the pipelines and the public options structs all take
// their defaults and their bounds from here, so a figure appears once.

#include <algorithm>
#include <bit>
#include <cstddef>
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

// The default geometry for a generated file (write_file(), io.hpp): 64 MiB
// windows, two in flight. Not the reader's budget -- this is blake3ppgen's
// measured shape, from four PCIe 5 drives in RAID 0, 32 threads, 32 GiB
// written: 24.2 GiB/s at 64 MiB x 2 and 23.8 at 64 MiB x 4, against 20.7
// at 128 MiB x 4 and 14.8 at 256 MiB x 8. Past a few hundred MiB in flight
// a striped NVMe volume slows down.
inline constexpr std::size_t default_write_window_bytes = 64 * 1024 * 1024;
inline constexpr unsigned default_write_queue_depth = 2;

}  // namespace blake3pp::detail
