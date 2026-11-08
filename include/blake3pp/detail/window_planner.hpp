#pragma once

// Where each window of a run sits and how it splits between the hasher
// and the pool, worked out from the plan and a cursor alone: no driver,
// scheduler or tracer, so a window can be costed before it is taken and
// the arithmetic can be tested without running a pipeline.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/compress.hpp>
#include <blake3pp/detail/contract.hpp>
#include <blake3pp/detail/pipeline_plan.hpp>

namespace blake3pp::detail {

// How a window is split three ways. `head` is absorbed by the hasher
// on the driver the moment the read lands, `parts` full parts go to
// the pool and come back as nodes, and `tail` waits for the reducer to
// drain. A full window is all parts and no ends. Decided by
// plan_window() from the geometry, with nothing else touched, which is
// what lets admission cost a window that has not been built.
struct window_plan {
  std::uint64_t first_chunk = 0;  // where its contribution to the tree starts
  std::size_t head_bytes = 0;
  std::size_t tail_bytes = 0;
  std::size_t parts = 0;
  std::size_t part_chunks = 0;
  std::size_t node_cost = 1;   // reducer slots admission reserves
};
static_assert(std::is_trivially_copyable_v<window_plan>);

// Where the next window sits in the source: what the planner hands out,
// computed from its cursor alone so a window can be planned before it
// is taken.
struct window_geometry {
  std::uint64_t offset = 0;
  std::size_t bytes = 0;
  bool last = false;
  bool short_first = false;
};

// The windows of one run, in order. Owns the cursor: the scope asks for
// the next geometry, lets the driver answer for a stream, and advances
// by what the window turned out to hold.
template <stack_budget Budget>
class window_planner {
 public:
  // @throws std::logic_error for a plan the source cannot be cut by.
  window_planner(const pipeline_plan& plan, std::uint64_t base_count,
                 std::uint64_t file_bytes)
      : window_bytes_(plan.window_bytes),
        base_count_(base_count),
        head_window_bytes_(plan.head_bytes),
        ends_message_(plan.ends_message),
        streaming_(plan.streaming),
        grid_parts_(plan.window_bytes / chunk_size /
                    part_chunks<Budget>(plan.window_bytes / chunk_size)),
        base_chunk_((base_count + head_window_bytes_) / chunk_size),
        file_bytes_(file_bytes) {
    require(ends_message_ || streaming_ || file_bytes_ % window_bytes_ == 0,
            "run_window_pipeline(): a span that does not end the message "
            "must be a whole number of windows: a partial one cannot be "
            "absorbed as a subtree");
    // A stream has to start on the grid. The short first window is read
    // whole or not at all -- its length is what plan_short_first splits
    // into a head for the hasher and parts for the pool -- and a
    // producer that left it short would leave the grid off every part
    // boundary for the rest of the run. Every caller streams into a
    // hasher that is on a window boundary, which is every fresh one.
    require(!streaming_ || head_window_bytes_ == 0,
            "update_stream(): a hasher part-way through a window cannot be "
            "brought onto the grid from a stream");
  }

  // A source whose length arrives with its bytes; see pipeline_plan.
  [[nodiscard]] bool streaming() const noexcept { return streaming_; }
  // The source's length as last seen; a stream's grows as it is read.
  [[nodiscard]] std::uint64_t file_bytes() const noexcept {
    return file_bytes_;
  }
  // Every byte has been handed out.
  [[nodiscard]] bool exhausted() const noexcept {
    return next_offset_ >= file_bytes_;
  }

  [[nodiscard]] window_geometry next_geometry() const noexcept {
    window_geometry g;
    g.offset = next_offset_;
    g.short_first = g.offset == 0 && head_window_bytes_ > 0;
    // A window ends at the next boundary of the grid the full windows
    // sit on, which on a seekable source is always one whole window
    // away. On a stream it need not be: a producer that left a window
    // short put the cursor between two boundaries, and the window after
    // it asks for the remainder -- the short first window's trick,
    // applied mid-stream, which is what keeps the grid from drifting for
    // the rest of the run.
    const std::uint64_t past_head =
        next_offset_ - std::min<std::uint64_t>(next_offset_, head_window_bytes_);
    const std::size_t to_boundary =
        static_cast<std::size_t>(past_head % window_bytes_);
    const std::size_t want =
        g.short_first ? head_window_bytes_ : window_bytes_ - to_boundary;
    g.bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(want, file_bytes_ - next_offset_));
    g.last = ends_message_ && next_offset_ + g.bytes >= file_bytes_;
    return g;
  }

  // The driver had the last word on a stream window's length, and on
  // where the message ends: a stream learns that only by reaching it, so
  // the size is re-read for every window rather than taken once at the
  // start.
  [[nodiscard]] window_geometry observe(window_geometry g, std::size_t bytes,
                                        std::uint64_t file_bytes) noexcept {
    file_bytes_ = file_bytes;
    g.bytes = bytes;
    g.last = ends_message_ && g.offset + g.bytes >= file_bytes_;
    return g;
  }

  // Moves the cursor past a window that has been taken.
  void advance(std::size_t bytes) noexcept { next_offset_ += bytes; }

  // What the next window would reserve in the reducer, from the same
  // plan the window will get.
  [[nodiscard]] std::size_t next_window_cost() const noexcept {
    const std::size_t planned = plan_window(next_geometry()).node_cost;
    if (!streaming_) {
      return planned;
    }
    // The grid asks, the driver answers, and a window it ends early
    // reserves a whole decomposition where a full one reserves a single
    // node. Admission has to have cleared whatever arrives, so it
    // reserves the worst case over any length the ask could come back
    // as -- which is also what reducer_nodes_for() sized the storage
    // for.
    return std::max(planned, nodes_for(grid_parts_));
  }

  // Decides a window's three parts. A window that is both ends of the
  // file is the hasher's whole: it is misaligned at the front and
  // carries the final chunk at the back, and nothing between them is
  // worth a subtree. For every window but the short first one the
  // contribution starts at the window itself; the short one's starts
  // wherever its head ends, which plan_short_first works out.
  [[nodiscard]] window_plan plan_window(
      const window_geometry& g) const noexcept {
    window_plan p;
    p.first_chunk =
        base_chunk_ +
        (g.offset - std::min<std::uint64_t>(g.offset, head_window_bytes_)) /
            chunk_size;
    if (g.last && g.short_first) {
      p.tail_bytes = g.bytes;
      return p;
    }
    if (g.last) {
      return plan_last(g.bytes, p);
    }
    if (g.short_first) {
      return plan_short_first(g.bytes, p);
    }
    // One subtree: a whole window of the grid, starting where a node of
    // that size may start. Every interior window of a seekable source is
    // this, and so is every window of a stream whose producer kept up.
    const std::uint64_t chunks = g.bytes / chunk_size;
    if (g.bytes == window_bytes_ && chunks > 0 &&
        p.first_chunk % chunks == 0) {
      return p;
    }
    return plan_mid(g.bytes, p);
  }

  // What a window a stream ended early has to be for its bytes to reach
  // the tree at all, checked in every build.
  //
  // A run that is not a whole number of aligned parts yields nodes
  // claiming ranges that are not nodes of the tree: the sizes still sum
  // to the message, the reducer's own disjointness check still passes,
  // and the digest is simply wrong. Nothing downstream can catch it, so
  // it is caught here.
  // @throws std::logic_error if the driver flushed a window the grid
  //         cannot decompose.
  void check_streamed_window(const window_geometry& g,
                             const window_plan& p) const {
    if (g.last || g.short_first) {
      return;  // the hasher's own ends; plan_last and plan_short_first
    }
    require(g.bytes % chunk_size == 0,
            "a stream flushed a window that is not a whole number of "
            "chunks");
    if (p.parts == 0) {
      // One subtree: a whole window of the grid, on a boundary a node of
      // that size may start at.
      require(g.bytes == window_bytes_,
              "a stream flushed a window that is neither a whole window "
              "of the grid nor a run of two or more parts");
      return;
    }
    require(p.parts >= 2,
            "a stream flushed fewer than two parts: a window of one part "
            "is not worth a round trip through the pool");
    require(g.bytes == p.parts * p.part_chunks * chunk_size,
            "a stream flushed a window its parts do not cover exactly");
    require(p.first_chunk % p.part_chunks == 0,
            "a stream flushed a window that does not start on a part "
            "boundary");
  }

 private:
  // A window the grid did not fill, in the middle of a message.
  //
  // None of it can go to the hasher. The hasher takes bytes in order and
  // every window before this one is still a pending node, so a head
  // absorbed here would land at the wrong position -- which is what
  // separates this from the short FIRST window, whose head is at the
  // hasher's own cursor by construction. All of it goes to the pool as
  // whole parts instead, and comes back as the decomposition of that run.
  //
  // What makes that legal is the flush rule: every window a stream ends
  // early is a multiple of one part of the grid, so the run starts on a
  // part boundary and ends on one. check_streamed_window() checks it,
  // because a run that is not is a wrong digest and no error.
  [[nodiscard]] window_plan plan_mid(std::size_t bytes,
                                     window_plan p) const noexcept {
    const std::size_t chunks = static_cast<std::size_t>(bytes / chunk_size);
    const std::size_t part = part_chunks<Budget>(chunks);
    if (chunks < part) {
      // Too short to be even one part, and nothing here may reach the
      // hasher. Left for check_streamed_window() to refuse.
      return p;
    }
    p.part_chunks = part;
    p.parts = chunks / part;
    p.node_cost = nodes_for(p.parts);
    return p;
  }

  // The short window that brings the hasher back to a window boundary.
  //
  // Its front belongs to the hasher: the bytes to the next chunk
  // boundary, then whole chunks up to a part boundary, which is under
  // one part in total and is absorbed on the driver the moment the read
  // lands. What follows is whole parts, aligned in the message's own
  // chunk space, and goes to the pool like any other window's.
  [[nodiscard]] window_plan plan_short_first(std::size_t bytes,
                                             window_plan p) const noexcept {
    const std::uint64_t count = base_count_;
    const std::size_t to_chunk = static_cast<std::size_t>(
        (chunk_size - count % chunk_size) % chunk_size);
    const std::size_t chunks_left =
        bytes > to_chunk ? (bytes - to_chunk) / chunk_size : 0;
    if (chunks_left < 2) {
      p.head_bytes = bytes;
      return p;
    }
    const std::size_t part = part_chunks<Budget>(chunks_left);
    const std::uint64_t chunk_at = (count + to_chunk) / chunk_size;
    const std::size_t to_part =
        static_cast<std::size_t>((part - chunk_at % part) % part) * chunk_size;
    const std::size_t head = to_chunk + to_part;
    const std::size_t parts =
        bytes > head ? (bytes - head) / (part * chunk_size) : 0;
    if (parts < 2) {
      p.head_bytes = bytes;
      return p;
    }
    p.head_bytes = head;
    p.part_chunks = part;
    p.parts = parts;
    p.first_chunk = (count + head) / chunk_size;
    p.node_cost = nodes_for(parts);
    // The parts cover the window exactly: the head lands on a part
    // boundary and the window ends on a window boundary, and the part
    // size divides the window because both are powers of two and the
    // part is the smaller.
    assert(bytes == head + parts * part * chunk_size &&
           "a short first window's parts must cover it exactly");
    return p;
  }

  // The last window carries the message's final chunk, and a merged
  // range must never contain it -- so the part that holds it, and
  // everything after, stays with the hasher. Everything before that is
  // ordinary full parts and goes to the pool like any other window's.
  //
  // k is how many whole parts sit entirely before the final chunk. Below
  // two of them there is nothing worth a round trip through the pool and
  // the window is all tail, which is also the shape of a file that fits
  // inside one window. The hasher's share is then up to two parts rather
  // than the one the driver rule allows elsewhere: the final part is its
  // by definition, and the one before it is not worth splitting out
  // alone.
  [[nodiscard]] window_plan plan_last(std::size_t bytes,
                                      window_plan p) const noexcept {
    const std::uint64_t chunks_with_tail =
        (static_cast<std::uint64_t>(bytes) + chunk_size - 1) / chunk_size;
    if (chunks_with_tail < 2) {
      p.tail_bytes = bytes;
      return p;
    }
    const std::size_t part =
        part_chunks<Budget>(static_cast<std::size_t>(chunks_with_tail));
    const std::uint64_t k = (chunks_with_tail - 1) / part;
    if (k < 2) {
      p.tail_bytes = bytes;
      return p;
    }
    p.part_chunks = part;
    p.parts = static_cast<std::size_t>(k);
    p.tail_bytes = bytes - p.parts * part * chunk_size;
    p.node_cost = nodes_for(p.parts);
    return p;
  }

  std::size_t window_bytes_ = 0;
  std::uint64_t base_count_ = 0;        // the hasher's byte count at start
  std::size_t head_window_bytes_ = 0;   // the short first window, or zero
  bool ends_message_ = true;
  bool streaming_ = false;
  // Parts in a full window of the grid: what a window a stream ended
  // early reserves in the reducer, since admission has to clear the
  // length the driver turns out to hand back and not just the one it
  // was asked for.
  std::size_t grid_parts_ = 1;
  std::uint64_t base_chunk_ = 0;        // where the full windows' grid starts
  std::uint64_t file_bytes_ = 0;
  std::uint64_t next_offset_ = 0;
};

}  // namespace blake3pp::detail
