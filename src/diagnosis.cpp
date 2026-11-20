// Which side of a finished pipeline run was the constraint, read off the
// counters that run already kept.
//
// This reports. It does not tune: nothing here runs a second geometry,
// predicts a rate, or recommends a number. It takes one run's
// driver_stats plus the wall clock the caller measured around the call
// and says which of the pool, the device and the arena between them was
// the busy one, with the figures it said it from.
//
// Out of line because the two entry points run once at the end of a run,
// never per window, and <blake3pp/trace.hpp> is on the include path of
// detail/compress.hpp and the umbrella header. The declarations and the
// contract are there; the band comparisons are here.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <blake3pp/trace.hpp>

namespace blake3pp {
namespace {

using fractional_ns = std::chrono::duration<double, std::nano>;

// A time as a share of a capacity in time: a dimensionless ratio.
[[nodiscard]] double share(std::chrono::nanoseconds part,
                           fractional_ns capacity) noexcept {
  return capacity > fractional_ns::zero() ? fractional_ns(part) / capacity
                                          : 0.0;
}

// Whether the device ever put one transfer behind another, from a
// diagnosis whose mean and minimum are already filled in. A mean within
// the tie band of the minimum is a server that served every transfer as
// if it were alone.
[[nodiscard]] bool unqueued(const diagnosis& r, const bands& b) noexcept {
  return r.min_service > std::chrono::nanoseconds::zero() &&
         r.mean_service <= fractional_ns(r.min_service) * (1.0 + b.tie);
}

// How full the device's queue was, from a diagnosis whose transfers,
// depth, mean, minimum and device_unqueued are already filled in. Both
// directions compute it the same way; see diagnosis::device_queued for
// what the two terms are and why neither alone would do.
[[nodiscard]] double queued(const diagnosis& r,
                            std::chrono::nanoseconds device_wait,
                            fractional_ns wall) noexcept {
  const auto depth =
      static_cast<double>(std::max<unsigned>(1, r.device_depth));
  const double by_wall =
      std::clamp(share(device_wait, wall * depth), 0.0, 1.0);
  // Nothing was ever queued behind anything: one slot's worth, whatever
  // the slots showed.
  return r.device_unqueued ? std::min(by_wall, 1.0 / depth) : by_wall;
}

// The transfers the device actually had at once: what the engine offered
// where it could say, and the depth the pipeline asked for where it
// could not. Never zero, because it is a denominator.
[[nodiscard]] unsigned device_depth_of(const driver_stats& d,
                                       std::uint64_t depth) noexcept {
  // Both arrive 64-bit, because driver_stats and diagnosis keep every
  // count that wide. A pipeline's depths are bounded by max_queue_depth,
  // but the counters are the caller's to fill in, so the narrowing
  // saturates rather than wrapping a depth past UINT_MAX to zero.
  const std::uint64_t said = d.device_depth;
  const std::uint64_t got = said != 0 ? std::min(said, depth) : depth;
  return static_cast<unsigned>(std::clamp<std::uint64_t>(
      got, 1, std::numeric_limits<unsigned>::max()));
}

// What both sides read off the counters the same way: the geometry, and
// with a run to measure, the device's service, the pool's share, the
// starved share and the lanes' queueing. False when there is nothing to
// measure -- no transfer landed, or no wall clock -- and the diagnosis
// then reports only its geometry.
bool measure(diagnosis& r, const driver_stats& d, fractional_ns wall,
             unsigned threads, std::size_t window, const bands& b) noexcept {
  r.transfers = d.transfers_landed;
  r.depth = d.queue_depth_final;
  r.device_depth = device_depth_of(d, r.depth);
  r.window_bytes = window;
  r.bytes_in_flight = r.device_depth * static_cast<std::uint64_t>(window);
  if (!r.measured() || wall <= fractional_ns::zero()) {
    return false;
  }
  const auto transfers = static_cast<double>(r.transfers);
  r.mean_service = fractional_ns(d.device_wait) / transfers;
  r.min_service = d.device_wait_min;
  r.device_unqueued = unqueued(r, b);
  r.pool_busy = share(d.agent_busy, wall * static_cast<double>(threads));
  r.device_queued = queued(r, d.device_wait, wall);
  r.starved = share(d.starved, wall);
  r.mean_lane_wait = fractional_ns(d.lane_wait) / transfers;
  return true;
}

}  // namespace

const char* to_string(bottleneck b) noexcept {
  switch (b) {
    case bottleneck::hash: return "hash-limited";
    case bottleneck::device: return "device-limited";
    case bottleneck::arena: return "arena-limited";
    case bottleneck::in_flight: return "in-flight-limited";
    case bottleneck::generator: return "generator-limited";
    case bottleneck::undecided: break;
  }
  return "undecided";
}

diagnosis diagnose_read(const driver_stats& d,
                        std::chrono::duration<double, std::nano> wall,
                        unsigned threads, unsigned planned,
                        std::size_t window, bands b) noexcept {
  diagnosis r;
  r.windows_used = d.max_windows_in_flight;
  r.windows_planned = planned;
  if (!measure(r, d, wall, threads, window, b)) {
    return r;
  }
  r.arena_is_depth =
      planned > 0 && r.windows_used >= planned && planned <= r.depth;
  if (!r.long_enough()) {
    return r;
  }
  // The candidate the arena stands in for where it is a lever at all.
  const bottleneck other =
      r.arena_is_depth ? bottleneck::arena : bottleneck::device;

  if (r.pool_busy >= b.saturated) {
    r.label = bottleneck::hash;
  } else if (r.device_queued >= b.saturated) {
    r.label = bottleneck::device;
  } else if (r.pool_busy >= b.idle) {
    r.first = bottleneck::hash;
    r.second = other;
  } else if (r.device_queued >= b.idle) {
    r.first = bottleneck::device;
    r.second = r.arena_is_depth ? bottleneck::arena : bottleneck::hash;
  } else if (r.arena_is_depth) {
    r.label = bottleneck::arena;
  } else if (r.device_unqueued) {
    // Neither end busy, the arena wider than the queue, and no transfer
    // ever behind another: the pool had windows to spare, the device's
    // server had capacity to spare, and what the run never had more of
    // is bytes outstanding. Naming the device or the hash here would
    // name the two candidates this run has already ruled out.
    r.label = bottleneck::in_flight;
  } else {
    // The device did queue and neither share reached its band: a run
    // between the two, and nothing here separates them.
    r.first = bottleneck::device;
    r.second = bottleneck::hash;
  }
  return r;
}

diagnosis diagnose_write(const driver_stats& d,
                         std::chrono::duration<double, std::nano> wall,
                         unsigned threads, std::size_t window,
                         bands b) noexcept {
  diagnosis r;
  const bool measured = measure(r, d, wall, threads, window, b);
  // The write side's arena is its queue: every window in flight is one
  // the device or the generator holds.
  r.windows_used = r.depth;
  r.windows_planned = r.depth;
  r.arena_is_depth = true;
  if (!measured) {
    return r;
  }
  const fractional_ns in_flight =
      wall * static_cast<double>(std::max<std::uint64_t>(1, r.depth));
  r.fill_busy = share(d.agent_busy, in_flight);
  // The share of the windows in flight the device held, which on this
  // side is directly comparable with fill_busy: the two shares are of
  // the same windows, and a window is either being filled or being
  // written.
  r.device_busy = share(d.device_wait, in_flight);
  if (!r.long_enough()) {
    return r;
  }

  if (r.device_queued >= b.saturated) {
    r.label = bottleneck::device;
  } else if (r.fill_busy > r.device_busy + b.tie) {
    r.label = bottleneck::generator;
  } else if (r.device_busy > r.fill_busy + b.tie) {
    r.label = bottleneck::device;
  } else {
    r.first = bottleneck::generator;
    r.second = bottleneck::device;
  }
  return r;
}

}  // namespace blake3pp
