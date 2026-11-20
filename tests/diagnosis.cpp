// blake3pp::diagnose_read() and blake3pp::diagnose_write() as a consumer
// sees them: one public header, counters filled in by hand, no pipeline
// and no tooling header in sight.
//
// The readings are asserted here, against counters whose shares are
// stated. The pipeline-driven cases in file_pipeline.cpp and
// write_pipeline.cpp run the synthetic device on a real clock, which load
// moves, so they assert the geometry and that the counters cohere. This
// file also holds the facility to <blake3pp/trace.hpp> alone, the
// contract a caller outside this repository has.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <blake3pp/trace.hpp>
#include <doctest/doctest.h>

namespace {

constexpr double wall_ns = 1e9;
constexpr unsigned threads = 8;
constexpr std::uint64_t depth = 4;
constexpr std::size_t window = 8u << 20;

// A run of `transfers` transfers at `depth`, with the pool busy
// `pool_share` of its capacity and the device holding `device_share` of
// its queue. `min_ns` is the device's unqueued service time: equal to the
// mean marks a device that never put one transfer behind another.
[[nodiscard]] blake3pp::driver_stats counters(std::uint64_t transfers,
                                              double pool_share,
                                              double device_share,
                                              std::uint64_t min_ns) {
  blake3pp::driver_stats d{};
  d.transfers_landed = transfers;
  d.queue_depth_final = depth;
  d.max_windows_in_flight = depth;
  d.agent_busy_ns =
      static_cast<std::uint64_t>(pool_share * wall_ns * threads);
  d.device_wait_ns = static_cast<std::uint64_t>(
      device_share * wall_ns * static_cast<double>(depth));
  d.device_wait_min_ns = min_ns;
  return d;
}

[[nodiscard]] std::string_view label(blake3pp::bottleneck b) {
  return blake3pp::to_string(b);
}

}  // namespace

TEST_SUITE("diagnosis") {

TEST_CASE("a saturated pool reads as hash-limited") {
  const auto d = counters(64, 0.95, 0.02, 1'000'000);
  const auto r = blake3pp::diagnose_read(d, wall_ns, threads, 4, window);
  REQUIRE(r.measured());
  REQUIRE(r.long_enough());
  CHECK(r.label == blake3pp::bottleneck::hash);
  CHECK(label(r.label) == "hash-limited");
  CHECK(r.pool_busy == doctest::Approx(0.95).epsilon(0.01));
  // The geometry the reading was taken at, which the tools print.
  CHECK(r.bytes_in_flight == depth * window);
}

TEST_CASE("a full queue over an idle pool reads as device-limited") {
  // The mean service time is far above the unqueued minimum, so the
  // device really did put transfers behind one another.
  const auto d = counters(64, 0.10, 0.95, 10'000'000);
  const auto r = blake3pp::diagnose_read(d, wall_ns, threads, 4, window);
  CHECK(r.label == blake3pp::bottleneck::device);
  CHECK(label(r.label) == "device-limited");
  CHECK_FALSE(r.device_unqueued);
}

TEST_CASE("a full queue under a wide arena reads as device-limited") {
  // The shape tests/file_pipeline.cpp drives through the real pipeline
  // with a slow device: more windows planned than the queue is deep, so
  // the arena cannot be the constraint, and the queue held for the run.
  // That run's queue share is a share of the wall clock, which load
  // moves; here the share is stated.
  const auto d = counters(64, 0.10, 0.95, 10'000'000);
  const auto r = blake3pp::diagnose_read(d, wall_ns, threads, 16, window);
  REQUIRE(r.long_enough());
  CHECK_FALSE(r.arena_is_depth);
  CHECK_FALSE(r.device_unqueued);
  CHECK(r.label == blake3pp::bottleneck::device);
}

TEST_CASE("a saturated pool that starves the driver still reads as hash-limited") {
  // A pool that is the constraint leaves the driver with no free window
  // most of the run, so starved_ns is most of the wall clock. starved_ns
  // does not measure pool starvation, and a reading built on it would
  // call this arena-limited. tests/file_pipeline.cpp produces this shape
  // with a zero-copy source; the shares are stated here because load
  // moves the pool's share of a real clock.
  auto d = counters(64, 0.95, 0.02, 1'000'000);
  d.starved_ns = static_cast<std::uint64_t>(0.9 * wall_ns);
  const auto r = blake3pp::diagnose_read(d, wall_ns, threads, 16, window);
  REQUIRE(r.long_enough());
  CHECK(r.starved > 0.5);
  CHECK_FALSE(r.arena_is_depth);
  CHECK(r.label == blake3pp::bottleneck::hash);
}

TEST_CASE("an arena at the queue depth reads as arena-limited") {
  const auto d = counters(64, 0.10, 0.50, 10'000'000);
  const auto r = blake3pp::diagnose_read(d, wall_ns, threads, 4, window);
  REQUIRE(r.arena_is_depth);
  CHECK(r.label == blake3pp::bottleneck::arena);
  CHECK(label(r.label) == "arena-limited");
}

TEST_CASE("a wide arena over an unqueued device reads as in-flight-limited") {
  // 16 windows planned against a depth of 4, and a mean service time at
  // the minimum: the pool had windows to spare and the device served
  // every transfer alone.
  const auto d = counters(64, 0.10, 0.016, 1'000'000);
  const auto r = blake3pp::diagnose_read(d, wall_ns, threads, 16, window);
  REQUIRE(r.device_unqueued);
  CHECK_FALSE(r.arena_is_depth);
  CHECK(r.label == blake3pp::bottleneck::in_flight);
  CHECK(label(r.label) == "in-flight-limited");
}

TEST_CASE("a narrow arena over an unqueued device still reads as arena-limited") {
  // Arena against in-flight: both have an idle pool and a device that
  // served every transfer alone, and the arena's width against the queue
  // depth separates them. Four windows planned against a depth of four,
  // so the pool has nothing spare and the arena is the constraint.
  //
  // This is the shape tests/file_pipeline.cpp drives through the real
  // pipeline. The reading turns on unqueued(), a 5% band around the
  // minimum, and a run on a real clock cannot promise a mean that close
  // to its own minimum, so the reading is asserted here.
  const auto d = counters(64, 0.10, 0.016, 1'000'000);
  const auto r = blake3pp::diagnose_read(d, wall_ns, threads, depth, window);
  REQUIRE(r.device_unqueued);
  REQUIRE(r.arena_is_depth);
  CHECK(r.label == blake3pp::bottleneck::arena);
  CHECK(label(r.label) == "arena-limited");
}

TEST_CASE("a run shorter than four turns of the queue gets no reading") {
  const auto d = counters(4, 0.95, 0.95, 1'000'000);
  const auto r = blake3pp::diagnose_read(d, wall_ns, threads, 4, window);
  CHECK(r.measured());
  CHECK_FALSE(r.long_enough());
  CHECK(r.label == blake3pp::bottleneck::undecided);
  CHECK(label(r.label) == "undecided");
}

TEST_CASE("a default-constructed diagnosis has no reading to give") {
  const blake3pp::diagnosis empty;
  CHECK_FALSE(empty.measured());
  CHECK(empty.label == blake3pp::bottleneck::undecided);
}

TEST_CASE("a pool between the bands is undecided between hash and the arena or device") {
  // The pool at 85% is past the idle band and short of saturation. The
  // device's queue share is 50%, under the idle band, and its mean
  // service of 31 ms is well above the 10 ms minimum, so it did queue
  // but neither end is decided. The second candidate is the arena when
  // the arena is the depth, and the device otherwise.
  const auto d = counters(64, 0.85, 0.50, 10'000'000);
  const auto narrow = blake3pp::diagnose_read(d, wall_ns, threads, depth, window);
  REQUIRE(narrow.long_enough());
  CHECK(narrow.label == blake3pp::bottleneck::undecided);
  CHECK(narrow.first == blake3pp::bottleneck::hash);
  CHECK(narrow.second == blake3pp::bottleneck::arena);

  const auto wide = blake3pp::diagnose_read(d, wall_ns, threads, 16, window);
  REQUIRE(wide.long_enough());
  CHECK(wide.label == blake3pp::bottleneck::undecided);
  CHECK(wide.first == blake3pp::bottleneck::hash);
  CHECK(wide.second == blake3pp::bottleneck::device);
}

TEST_CASE("a device between the bands is undecided between device and the arena or hash") {
  // The device at 85% of its queue, the pool idle: the device is the
  // first candidate, and the second is whichever the arena leaves.
  const auto d = counters(64, 0.10, 0.85, 10'000'000);
  const auto narrow = blake3pp::diagnose_read(d, wall_ns, threads, depth, window);
  REQUIRE(narrow.long_enough());
  CHECK(narrow.label == blake3pp::bottleneck::undecided);
  CHECK(narrow.first == blake3pp::bottleneck::device);
  CHECK(narrow.second == blake3pp::bottleneck::arena);

  const auto wide = blake3pp::diagnose_read(d, wall_ns, threads, 16, window);
  REQUIRE(wide.long_enough());
  CHECK(wide.label == blake3pp::bottleneck::undecided);
  CHECK(wide.first == blake3pp::bottleneck::device);
  CHECK(wide.second == blake3pp::bottleneck::hash);
}

TEST_CASE("a queued device between the bands with a wide arena is undecided device or hash") {
  // Neither share reaches its band, the arena is wider than the queue,
  // and the device did queue: the final branch of diagnose_read().
  const auto d = counters(64, 0.10, 0.60, 10'000'000);
  const auto r = blake3pp::diagnose_read(d, wall_ns, threads, 16, window);
  REQUIRE(r.long_enough());
  CHECK_FALSE(r.arena_is_depth);
  CHECK_FALSE(r.device_unqueued);
  CHECK(r.label == blake3pp::bottleneck::undecided);
  CHECK(r.first == blake3pp::bottleneck::device);
  CHECK(r.second == blake3pp::bottleneck::hash);
}

TEST_CASE("a write run whose fill and device shares tie is undecided between generator and device") {
  // Fill share 2 x 0.30 = 0.60 of the windows in flight, device share
  // 0.60: within the tie band of each other, and the device's queue
  // share 0.60 is below saturation.
  const auto d = counters(64, 0.30, 0.60, 10'000'000);
  const auto r = blake3pp::diagnose_write(d, wall_ns, threads, window);
  REQUIRE(r.long_enough());
  CHECK(r.label == blake3pp::bottleneck::undecided);
  CHECK(r.first == blake3pp::bottleneck::generator);
  CHECK(r.second == blake3pp::bottleneck::device);
}

TEST_CASE("the write side names the generator and the device") {
  // Writing, the windows in flight are the generator's parallelism as
  // well as the device's queue, so the reading is which of the two held
  // them longer.
  const auto filling = counters(64, 0.30, 0.016, 1'000'000);
  const auto g = blake3pp::diagnose_write(filling, wall_ns, threads, window);
  CHECK(g.label == blake3pp::bottleneck::generator);
  CHECK(label(g.label) == "generator-limited");
  CHECK(g.arena_is_depth);

  const auto draining = counters(64, 0.02, 0.95, 10'000'000);
  const auto s = blake3pp::diagnose_write(draining, wall_ns, threads, window);
  CHECK(s.label == blake3pp::bottleneck::device);
}

TEST_CASE("the bands are the caller's to set") {
  // A pool at 85% is undecided under the defaults and saturated once the
  // caller moves the band under it.
  const auto d = counters(64, 0.85, 0.50, 10'000'000);
  CHECK(blake3pp::diagnose_read(d, wall_ns, threads, 4, window).label ==
        blake3pp::bottleneck::undecided);
  CHECK(blake3pp::diagnose_read(d, wall_ns, threads, 4, window,
                                {.saturated = 0.80, .idle = 0.70})
            .label == blake3pp::bottleneck::hash);
}

}  // TEST_SUITE

// The engaged rung is copied into the record, so it outlives the string
// it was recorded from (the file that names it closes with the run). A
// name of the field's full length comes back whole; the engines assert
// at compile time that theirs never exceed it, and a longer one from
// elsewhere is cut to the field.
TEST_CASE("a read backend name is copied whole, up to the field's length") {
  const std::string want(blake3pp::max_read_backend_length, 'x');
  blake3pp::driver_stats d{};
  CHECK(blake3pp::read_backend(d).empty());
  {
    const std::string source = want;
    blake3pp::set_read_backend(d, source);
  }
  CHECK(blake3pp::read_backend(d) == want);
  blake3pp::set_read_backend(d, want + "yz");
  CHECK(blake3pp::read_backend(d) == want);
}
