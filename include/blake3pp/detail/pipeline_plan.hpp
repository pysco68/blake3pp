#pragma once

// How a run is cut into windows: the reducer's node budget, the first
// window's size, and the plans plan_pipeline() and plan_memory_pipeline()
// make. Names no execution provider, so a translation unit that only
// plans (src/io_hash.cpp) does not pull one in.

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/driver.hpp>
#include <blake3pp/detail/io_geometry.hpp>

namespace blake3pp::detail {

// Pending nodes the reducer may hold.
//
// The scope admits a window only while a node is free for it, so the
// count is what bounds how far ahead of the tree the reads may run. It
// has to clear the worst a contiguous run of windows can decompose into
// -- one ascending and one descending chain of aligned nodes, one per
// level, 54 levels at BLAKE3's 2^64-byte limit -- plus the one node the
// next window needs, or the scope could reach a state where no window
// may start; see admit_more().
//
// The floor is what any run needs; reducer_nodes_for() adds one node per
// window that may be in flight, because windows complete out of order and
// each one can leave a node with no sibling yet. Without that term a deep
// queue would not stall or fail -- admission would simply stop early and
// run fewer windows than the caller asked for, which is the sort of
// silent difference that makes a queue-depth sweep lie.
inline constexpr std::size_t reducer_nodes = 256;

// The most nodes a run of `parts` equal parts can decompose into: one
// ascending chain and one descending one, at most one node per level of
// the run. An edge window reserves this rather than the worst case over
// any run, which is what keeps a small reducer usable: a blanket 2 * 54
// would make an edge window unadmittable below that capacity, and the
// pipeline would stall rather than run slowly.
[[nodiscard]] constexpr std::size_t nodes_for(std::size_t parts) noexcept {
  return 2 * static_cast<std::size_t>(std::bit_width(parts));
}

// The storage one run needs: a contiguous run's decomposition, both edge
// windows' reservations, one node per window that may be in flight, and
// one for the window admission is about to take. Never below
// reducer_nodes, so a small run keeps the headroom the default had.
// On a stream every window may be an edge window: once the producer has
// left one short, the grid is off its boundary and the window after it
// is a run of parts rather than one subtree -- so each window in flight
// reserves a whole decomposition instead of a single node, and so does
// the one admission is about to take.
[[nodiscard]] constexpr std::size_t reducer_nodes_for(
    unsigned windows, std::size_t parts, bool streaming = false) noexcept {
  const std::size_t per_window = streaming ? nodes_for(parts) : 1;
  const std::size_t edges = streaming ? 0 : 2 * nodes_for(parts);
  const std::size_t needed =
      2 * max_cv_stack_depth + edges +
      (static_cast<std::size_t>(windows) + 1) * per_window;
  return std::max(reducer_nodes, needed);
}

// How many bytes of a file have to be read before its windows line up
// with the hasher's own window grid.
//
// A hasher part-way through a message -- the second file of several
// hashed as one -- sits at some count that is not a multiple of the
// window. Reading one short window first brings it to a boundary, and
// every window after that is a subtree like any other. Zero means the
// hasher is already on a boundary, which is every fresh hasher.
[[nodiscard]] constexpr std::size_t first_window_bytes(
    std::uint64_t count, std::size_t window_bytes) noexcept {
  const std::size_t offset = static_cast<std::size_t>(count % window_bytes);
  return (window_bytes - offset) % window_bytes;
}

// Whether the platform's direct-I/O path can serve a file whose windows
// start at `head` bytes in.
//
// Direct I/O wants aligned offsets as well as aligned lengths, and the
// windows after a short first one start at head + n * window -- so an
// unaligned head misaligns every window of the file, not just the first.
// Degrading read by read would send every one of them to the synchronous
// rung; a buffered file keeps them all asynchronous. So the choice is per
// file, and it is made once.
[[nodiscard]] constexpr bool direct_io_fits(std::size_t head) noexcept {
  return head % direct_alignment == 0;
}

// The geometry of one update_file() call, decided once from the hasher's
// count, the file's size and the caller's requests and handed to the
// driver, the file and the scope alike, so that no two of them round or
// clamp differently.
struct pipeline_plan {
  // The window, as rounded_window_bytes() rounds the request.
  std::size_t window_bytes;
  // Reads the device sees at once: the queue depth as
  // clamp_queue_depth() clamps the request.
  unsigned reads_in_flight;
  // Window objects -- buffer, part table and operation cell -- and so
  // windows in flight at most, reading or on the pool: at least
  // reads_in_flight, as many as the budget buys up to max_queue_depth,
  // and never more than the file has.
  unsigned windows;
  // The short first window that brings the hasher onto the window grid;
  // zero for a hasher already on it.
  std::size_t head_bytes;
  // Whether the file is opened for direct I/O: the caller asked, the
  // grid is one the platform's alignment can serve, and the file is not
  // below min_pipeline_bytes, where buffered reads are the faster ones.
  // A size the filesystem will not give counts as large.
  bool direct_io;
  // Whether this span ends the message. False for a caller streaming a
  // sequence of spans into one hasher: the final window is then an
  // interior one like any other, and goes to the pool as a subtree
  // instead of to the hasher as a tail. A span that does not end the
  // message has to be a whole number of windows, which the scope
  // checks in every build, since a partial one could not be a subtree.
  bool ends_message = true;
  // Whether the source is a stream: a pipe, a socket, anything whose
  // bytes arrive rather than sit. Its length is not known in advance,
  // and a window's own length is whatever the driver put in it rather
  // than what the grid asked for. The scope then re-reads file.size()
  // after every window, takes each window's length from the storage
  // the driver handed back, and treats a window the grid did not fill
  // as a run of whole parts rather than as one subtree. False for every
  // seekable source, where none of that is paid for.
  bool streaming = false;
};

// How many windows a file of file_bytes has on this grid, the short
// first one included; saturates at max_queue_depth, which is all a plan
// can use.
[[nodiscard]] constexpr unsigned file_windows(std::uint64_t file_bytes,
                                              std::size_t window_bytes,
                                              std::size_t head_bytes) noexcept {
  if (file_bytes == 0) {
    return 0;
  }
  std::uint64_t n = 0;
  std::uint64_t rest = file_bytes;
  if (head_bytes > 0) {
    n = 1;
    rest -= std::min<std::uint64_t>(head_bytes, rest);
  }
  n += ceil_div(rest, window_bytes);
  return n > max_queue_depth ? max_queue_depth : static_cast<unsigned>(n);
}

// file_bytes is the file's size, or zero when unknown: the window count
// is then the budget's, and the open that follows reports why.
//
// window_budget_bytes widens the window objects the plan allocates
// without touching the depth the device sees. Those windows are the
// arena the pool draws its work from; read_plan_budget_bytes() resolves
// the figure, and io_geometry.hpp carries what it buys and what it
// costs. The arena is resident: a window is touched as soon as it is
// read into, so a wide budget costs its own size in pages.
[[nodiscard]] constexpr pipeline_plan plan_pipeline(
    std::uint64_t count, std::size_t window_bytes, unsigned queue_depth,
    bool direct_io, std::uint64_t file_bytes,
    std::size_t window_budget_bytes = 0) noexcept {
  const std::size_t window = rounded_window_bytes(window_bytes);
  const std::size_t head = first_window_bytes(count, window);
  const unsigned reads = clamp_queue_depth(queue_depth);
  const std::size_t by_budget =
      (window_budget_bytes != 0 ? window_budget_bytes
                                : default_window_budget_bytes) /
      window;
  const auto windows = static_cast<unsigned>(std::clamp<std::size_t>(
      std::max<std::size_t>(reads, by_budget), reads, max_queue_depth));
  return {window, reads,
          file_bytes == 0 ? windows
                          : std::min(windows, file_windows(file_bytes, window, head)),
          head,
          direct_io && direct_io_fits(head) && direct_io_worthwhile(file_bytes)};
}


// The same geometry for a source already in memory.
//
// Two of the file plan's inputs mean nothing here: there is no device to
// keep reads in flight for, and the window budget buys nothing when a
// window is a view rather than a buffer. What is left is how many
// windows may be in flight, which is what bounds the reducer and the
// part tables, and the span's own window count.
[[nodiscard]] constexpr pipeline_plan plan_memory_pipeline(
    std::uint64_t count, std::size_t window_bytes,
    std::uint64_t span_bytes) noexcept {
  const std::size_t window = rounded_window_bytes(window_bytes);
  const std::size_t head = first_window_bytes(count, window);
  const unsigned windows =
      span_bytes == 0
          ? 1
          : std::min(max_queue_depth, file_windows(span_bytes, window, head));
  return {window, windows, windows, head, false};
}

}  // namespace blake3pp::detail
