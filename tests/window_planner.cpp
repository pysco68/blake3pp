// The window planner on its own: where each window of a run sits and how
// it splits between the hasher and the pool, checked without a driver, a
// scheduler or a byte hashed. The walks assert the shape the tree needs:
// the hasher's bytes are a prefix and a suffix of the message, every node
// between them is an aligned power-of-two range, and the final chunk is
// never inside one. The end-to-end digests in file_pipeline.cpp and
// pipe_source.cpp rest on exactly that.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/pipeline_plan.hpp>
#include <blake3pp/detail/stack_budget.hpp>
#include <blake3pp/detail/window_planner.hpp>
#include <doctest/doctest.h>

namespace {

using blake3pp::chunk_size;
using blake3pp::default_stack_budget;
using blake3pp::stack_budget;
using blake3pp::detail::nodes_for;
using blake3pp::detail::pipeline_plan;
using blake3pp::detail::plan_pipeline;
using blake3pp::detail::window_geometry;
using blake3pp::detail::window_kind;
using blake3pp::detail::window_plan;
using blake3pp::detail::window_planner;

// Four parts per window: few, wide parts, so the short first and the
// last window reach their "fewer than two parts" cases at sizes the
// default budget would fan out.
constexpr stack_budget narrow_budget{4 * stack_budget::bytes_per_part};

// A stream's size until its end has been seen (pipe_driver.hpp's
// streaming_size).
constexpr std::uint64_t no_end_yet = ~std::uint64_t{0};

// How far a walk has got through the message.
struct walk_state {
  std::uint64_t covered = 0;  // message bytes accounted for, base included
  bool node_seen = false;     // the pool has been handed a node
  bool tail_seen = false;     // the hasher has taken the message's end
};

// One window's plan against the tree's rules, in walk order.
template <stack_budget Budget>
void check_window(const window_geometry& g, const window_plan& p,
                  std::uint64_t base_count, std::size_t window_bytes,
                  walk_state& s) {
  const std::uint64_t pos = base_count + g.offset;
  REQUIRE(pos == s.covered);
  REQUIRE(g.bytes > 0);
  REQUIRE(!s.tail_seen);
  // The hasher takes a head only at its own cursor, which is where the
  // short first window starts, and a tail only at the message's end.
  if (!g.short_first) {
    CHECK(p.head_bytes == 0);
  }
  if (!g.last) {
    CHECK(p.tail_bytes == 0);
  }
  if (p.head_bytes > 0) {
    CHECK(!s.node_seen);
  }
  switch (p.kind) {
    case window_kind::subtree: {
      const std::uint64_t chunks = g.bytes / chunk_size;
      CHECK(p.parts == 0);
      CHECK(p.node_cost == 1);
      CHECK(g.bytes == window_bytes);
      CHECK(std::has_single_bit(chunks));
      CHECK(p.first_chunk * chunk_size == pos);
      CHECK(p.first_chunk % chunks == 0);
      s.node_seen = true;
      break;
    }
    case window_kind::part_run:
      CHECK(p.parts >= 2);
      CHECK(p.parts <= Budget.parts());
      CHECK(std::has_single_bit(p.part_chunks));
      CHECK(p.node_cost == nodes_for(p.parts));
      CHECK(p.head_bytes + p.parts * p.part_chunks * chunk_size +
                p.tail_bytes ==
            g.bytes);
      CHECK(p.first_chunk * chunk_size == pos + p.head_bytes);
      CHECK(p.first_chunk % p.part_chunks == 0);
      s.node_seen = true;
      break;
    case window_kind::read_only:
      CHECK(p.parts == 0);
      CHECK(p.head_bytes + p.tail_bytes == g.bytes);
      CHECK((g.short_first || g.last));
      break;
  }
  if (g.last) {
    // A node holding the final chunk would be finalised as an interior
    // one, so the hasher always keeps some of the last window.
    CHECK(p.tail_bytes > 0);
    s.tail_seen = true;
  }
  s.covered = pos + g.bytes;
}

// Plans a seekable file of `size` bytes into a hasher at `count`, checks
// every window, and returns the kinds in order.
template <stack_budget Budget>
std::vector<window_kind> walk_file(std::uint64_t count,
                                   std::size_t window_request,
                                   std::uint64_t size) {
  const pipeline_plan plan =
      plan_pipeline(count, window_request, 4, false, size);
  CAPTURE(count);
  CAPTURE(plan.window_bytes);
  CAPTURE(size);
  window_planner<Budget> planner(plan, count, size);
  walk_state s{count};
  std::vector<window_kind> kinds;
  while (!planner.exhausted()) {
    const window_geometry g = planner.next_geometry();
    CAPTURE(g.offset);
    const window_plan p = planner.plan_window(g);
    CHECK(g.short_first == (g.offset == 0 && plan.head_bytes > 0));
    CHECK(planner.next_window_cost() == p.node_cost);
    check_window<Budget>(g, p, count, plan.window_bytes, s);
    planner.advance(g.bytes);
    kinds.push_back(p.kind);
  }
  CHECK(s.covered == count + size);
  CHECK(s.tail_seen);
  return kinds;
}

// A stream planner on a 256 KiB grid: sixteen parts of sixteen chunks.
constexpr std::size_t stream_window = 256 * 1024;
constexpr std::size_t part_bytes = 16 * chunk_size;

pipeline_plan stream_plan() {
  pipeline_plan plan = plan_pipeline(0, stream_window, 4, false, 0);
  plan.streaming = true;
  return plan;
}

}  // namespace

TEST_CASE("a seekable file's windows tile the message as the tree needs") {
  for (const std::size_t window : {std::size_t{64} * 1024,
                                   std::size_t{1} << 20}) {
    const std::uint64_t w = window;
    // Hashers on a boundary, inside a chunk, on a chunk but off a part,
    // and one short of a window; files from one byte to several windows
    // with every kind of last window.
    const std::uint64_t counts[] = {
        0, 1, 1000, 1024, 3 * 1024 + 5, 16 * 1024, w - 1, w, 7 * w + 12345};
    const std::uint64_t sizes[] = {1,         1023,          1024,
                                   1025,      16 * 1024,     33 * 1024 + 1,
                                   w - 1,     w,             w + 1,
                                   3 * w,     3 * w + 1,     5 * w + 40 * 1024};
    for (const std::uint64_t count : counts) {
      for (const std::uint64_t size : sizes) {
        (void)walk_file<default_stack_budget>(count, window, size);
        (void)walk_file<narrow_budget>(count, window, size);
      }
    }
  }
}

TEST_CASE("a fresh hasher's file is subtrees, then the hasher's tail") {
  constexpr std::size_t w = 64 * 1024;
  using enum window_kind;
  // A last window of five chunks has no two whole parts before its final
  // chunk, so the hasher takes all of it.
  CHECK(walk_file<default_stack_budget>(0, w, 3 * w + 5000) ==
        std::vector{subtree, subtree, subtree, read_only});
  // One of 41 chunks has two parts of sixteen before the final chunk.
  CHECK(walk_file<default_stack_budget>(0, w, 3 * w + 40 * 1024 + 3) ==
        std::vector{subtree, subtree, subtree, part_run});
  // A file inside one window is the hasher's whole.
  CHECK(walk_file<default_stack_budget>(0, w, 5000) ==
        std::vector{read_only});
}

TEST_CASE("a hasher part-way through a message starts with a short window") {
  constexpr std::size_t w = 64 * 1024;
  const pipeline_plan plan = plan_pipeline(1000, w, 4, false, 4 * w);
  REQUIRE(plan.head_bytes == w - 1000);
  const window_planner<default_stack_budget> planner(plan, 1000, 4 * w);
  const window_geometry g = planner.next_geometry();
  CHECK(g.short_first);
  CHECK(g.bytes == w - 1000);
  const window_plan p = planner.plan_window(g);
  // 24 bytes finish the hasher's chunk, at chunk 1; fifteen more chunks
  // reach the part boundary at chunk 16, and the three parts after that
  // end on the window boundary.
  CHECK(p.kind == window_kind::part_run);
  CHECK(p.head_bytes == 24 + 15 * chunk_size);
  CHECK(p.first_chunk == 16);
  CHECK(p.part_chunks == 16);
  CHECK(p.parts == 3);
  CHECK(p.tail_bytes == 0);
}

TEST_CASE("a span that does not end the message is all subtrees") {
  constexpr std::size_t w = 64 * 1024;
  pipeline_plan plan = plan_pipeline(0, w, 4, false, 4 * w);
  plan.ends_message = false;
  window_planner<default_stack_budget> planner(plan, 0, 4 * w);
  unsigned windows = 0;
  while (!planner.exhausted()) {
    const window_geometry g = planner.next_geometry();
    CHECK(!g.last);
    CHECK(planner.plan_window(g).kind == window_kind::subtree);
    planner.advance(g.bytes);
    ++windows;
  }
  CHECK(windows == 4);
  // A partial window could not be a subtree, and nothing else may hold
  // the bytes of a span the message continues past.
  CHECK_THROWS_AS(window_planner<default_stack_budget>(plan, 0, 4 * w + 1),
                  std::logic_error);
}

TEST_CASE("a stream's short windows are runs of parts until the grid is back") {
  window_planner<default_stack_budget> planner(stream_plan(), 0, no_end_yet);
  walk_state s;
  const auto take = [&](std::size_t got, std::uint64_t size) {
    const window_geometry g =
        planner.observe(planner.next_geometry(), got, size);
    const window_plan p = planner.plan_window(g);
    planner.check_streamed_window(g, p);
    check_window<default_stack_budget>(g, p, 0, stream_window, s);
    planner.advance(g.bytes);
    return p;
  };
  // Admission reserves for whatever the driver hands back, which on a
  // stream may be a run of up to a whole grid window's parts.
  CHECK(planner.next_window_cost() == nodes_for(16));

  CHECK(planner.next_geometry().bytes == stream_window);
  CHECK(take(stream_window, no_end_yet).kind == window_kind::subtree);

  // A producer that ran dry: ten parts flushed early, and the window
  // after them asks for the six that bring the grid back to its boundary.
  const window_plan flushed = take(10 * part_bytes, no_end_yet);
  CHECK(flushed.kind == window_kind::part_run);
  CHECK(flushed.parts == 10);
  CHECK(planner.next_geometry().bytes == 6 * part_bytes);
  const window_plan rest = take(6 * part_bytes, no_end_yet);
  CHECK(rest.kind == window_kind::part_run);
  CHECK(rest.parts == 6);

  CHECK(planner.next_geometry().bytes == stream_window);
  CHECK(take(stream_window, no_end_yet).kind == window_kind::subtree);

  // The end: the driver has seen it, and the size is what was read. Two
  // parts sit before the final chunk of 40000 bytes.
  const std::uint64_t end = s.covered + 40000;
  const window_plan last = take(40000, end);
  CHECK(last.kind == window_kind::part_run);
  CHECK(last.parts == 2);
  CHECK(planner.exhausted());
  CHECK(planner.file_bytes() == end);
  CHECK(s.tail_seen);
}

TEST_CASE("a stream window the grid cannot decompose is refused") {
  window_planner<default_stack_budget> planner(stream_plan(), 0, no_end_yet);
  const auto refused = [&](std::size_t got) {
    const window_geometry g =
        planner.observe(planner.next_geometry(), got, no_end_yet);
    CHECK_THROWS_AS(planner.check_streamed_window(g, planner.plan_window(g)),
                    std::logic_error);
  };
  SUBCASE("not a whole number of chunks") { refused(10 * part_bytes + 1); }
  SUBCASE("one part") { refused(part_bytes); }
  SUBCASE("less than one part") { refused(8 * chunk_size); }
}

TEST_CASE("a stream cannot start off the grid") {
  pipeline_plan plan = stream_plan();
  plan.head_bytes = 1000;
  CHECK_THROWS_AS(window_planner<default_stack_budget>(plan, 0, no_end_yet),
                  std::logic_error);
}
