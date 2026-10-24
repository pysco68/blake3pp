// blake3pp::diagnose_read() and blake3pp::diagnose_write() as a consumer
// sees them: one public header, counters filled in by hand, no pipeline
// and no tooling header in sight.
//
// The pipeline-driven cases are in file_pipeline.cpp and
// write_pipeline.cpp, which produce each reading from the synthetic
// device rather than describing it. What this file asserts is that the
// facility is reachable and correct through <blake3pp/trace.hpp> alone,
// which is the contract a caller outside this repository has.

#include <cstddef>
#include <cstdint>
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
