#pragma once

// Which side of a finished pipeline run was the constraint, read off
// the counters that run already kept.
//
// This reports. It does not tune: nothing here runs a second geometry,
// predicts a rate, or recommends a number. It takes one run's
// driver_stats plus the wall clock the caller measured around the call
// and says which of the pool, the device and the arena between them was
// the busy one, with the figures it said it from.
//
// Shared by blake3ppsum (reading) and blake3ppgen (writing) and
// asserted by the test suite, so the reading is one piece of arithmetic
// rather than two tools' opinions. Formatting is the tools' own: the
// two directions print different lines.
//
// Standard library only, and no <format>: the test binary includes this
// and is built on toolchains the CLI tools are not.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <blake3pp/trace.hpp>

namespace b3tool {

/// What a run was limited by. The first three are read runs, the fourth
/// is the write side's name for the first, and `undecided` carries two
/// candidates the run could not separate.
enum class reading {
  hash_limited,       // the pool was saturated; I/O geometry will not help
  device_limited,     // the device's queue was full for the run
  arena_limited,      // neither end was busy and the windows ran out
  generator_limited,  // writing: the pool could not fill fast enough
  undecided,
};

[[nodiscard]] constexpr const char* to_string(reading r) noexcept {
  switch (r) {
    case reading::hash_limited: return "hash-limited";
    case reading::device_limited: return "device-limited";
    case reading::arena_limited: return "arena-limited";
    case reading::generator_limited: return "generator-limited";
    case reading::undecided: break;
  }
  return "undecided";
}

/// The bands a share of a resource is read in.
///
/// Not a tuned threshold, and deliberately not one calibrated on a
/// thread count: 64% pool-busy is a starving pool on 32 threads while
/// 88% on 16 is already near its ceiling, so a single cut on the pool
/// figure misreads a small machine. The bands apply to each side
/// separately and the reading comes from comparing the two, which is
/// scale-free; where a figure lands between the bands the run gets no
/// label at all.
struct bands {
  /// At or above this a resource was the wall for the run.
  double saturated = 0.90;
  /// Below this it plainly was not.
  double idle = 0.80;
  /// Two shares closer than this are the same figure. Anything near 1%
  /// in this project is noise; five times that is the smallest band it
  /// uses anywhere.
  double tie = 0.05;
};

/// The figures a reading is made from, and the reading.
struct diagnosis {
  reading label = reading::undecided;
  /// With `label == undecided`, the two the run could not separate.
  reading first = reading::undecided;
  reading second = reading::undecided;

  /// Compressing (reading) or filling (writing), over the pool's whole
  /// capacity for the run: 1.0 is every thread busy start to finish.
  double pool_busy = 0.0;
  /// Writing, the same time over the windows in flight rather than over
  /// the machine: one window is one fill task, so this is the share of
  /// the generator's actual parallelism that was used.
  double fill_busy = 0.0;
  /// How full the device's queue was, as a share of the depth in force.
  /// 1.0 is a queue that was full for the run, so the device's latency
  /// is at its ceiling here and the device is the wall; 1/depth is a
  /// device that served every transfer alone and had capacity to spare.
  ///
  /// Measured as the summed transfer time over the depth times the
  /// wall clock -- the average transfers outstanding, over the slots
  /// there were -- with one correction. A device whose mean
  /// per-transfer time is its own minimum never put one transfer
  /// behind another, however many were outstanding at it: it is
  /// propagating rather than working, its server has capacity to
  /// spare, and concurrency rather than the device is what the run is
  /// short of. That case reads as the one unqueued slot it is.
  ///
  /// The minimum is why the mean is readable at all: a queue's latency
  /// is proportional to its depth by construction, so a latency
  /// compared against anything but its own unqueued value says
  /// nothing.
  ///
  /// The minimum alone is not enough either, which a real drive shows:
  /// a device that saturates below the depth in force serves no
  /// transfer alone after the first, so its mean sits well under depth
  /// times the minimum while its queue is full the whole run. Measured
  /// on a WSL2 virtual disk, 8 MiB windows at depth 4: a mean of
  /// 15.0 ms against a 7.8 ms minimum, with 99% of the slots full. The
  /// mean over the minimum is the no-queueing test, not the
  /// how-queued measure.
  double device_queued = 0.0;
  /// Writing, the share of the windows in flight the device held, which
  /// is directly comparable with fill_busy: a window is either being
  /// filled or being written, so the two shares are of the same thing.
  /// Not set on the read side, where the pool's capacity and the
  /// device's queue are separate resources with no common denominator.
  double device_busy = 0.0;
  /// The driver with a window it could not use. Reading this is
  /// structural and says little (see driver_stats::starved_ns); writing
  /// it is the generator being behind.
  double starved = 0.0;

  std::uint64_t transfers = 0;
  double mean_service_ns = 0.0;
  std::uint64_t min_service_ns = 0;
  std::uint64_t depth = 0;
  std::uint64_t windows_used = 0;
  std::uint64_t windows_planned = 0;
  std::size_t window_bytes = 0;
  /// Reading: the run filled its arena and the arena is no wider than
  /// the queue depth, so every window the pool holds is a read slot the
  /// device cannot use. Always true on the write side, where the two
  /// are the same number.
  bool arena_is_depth = false;

  /// Whether there was anything to read: a run with no transfer landed
  /// never reached the pipeline.
  [[nodiscard]] constexpr bool measured() const noexcept {
    return transfers > 0;
  }

  /// Whether the run turned its queue over often enough to be read. A
  /// run of a few transfers is its own ramp and drain: the first
  /// transfers are served alone and the last leave the queue emptying,
  /// so every share here is a transient. Four turns of the queue is
  /// the floor.
  [[nodiscard]] constexpr bool long_enough() const noexcept {
    return measured() && transfers >= 4 * (depth > 0 ? depth : 1);
  }
};

namespace detail {

[[nodiscard]] inline double share(std::uint64_t ns,
                                 double capacity_ns) noexcept {
  return capacity_ns > 0.0 ? static_cast<double>(ns) / capacity_ns : 0.0;
}

// How full the device's queue was, from a diagnosis whose transfers,
// depth, mean and minimum are already filled in. Both directions
// compute it the same way; see the field for what the two terms are
// and why neither alone would do.
[[nodiscard]] inline double queued(const diagnosis& r,
                                   std::uint64_t device_wait_ns,
                                   double wall_ns, const bands& b) noexcept {
  const auto depth = static_cast<double>(std::max<std::uint64_t>(1, r.depth));
  const double by_wall = std::clamp(share(device_wait_ns, wall_ns * depth),
                                    0.0, 1.0);
  if (r.min_service_ns == 0) {
    return by_wall;
  }
  const double inflation =
      r.mean_service_ns / static_cast<double>(r.min_service_ns);
  // Nothing was ever queued behind anything: one slot's worth, whatever
  // the slots showed.
  return inflation <= 1.0 + b.tie ? std::min(by_wall, 1.0 / depth) : by_wall;
}

}  // namespace detail

/// A read run's reading.
///
/// Three facts decide it, in this order: the pool's busy share, the
/// device's queue, and whether the arena is wider than that queue.
///
/// The third is a relation and not a size. The windows beyond the reads
/// are what the pool draws on while the device serves the next ones, so
/// where there are none the pool runs dry whenever a window is out.
/// That is also the only case driver_stats::starved_ns can fire in,
/// which is why that counter reads "the arena is not wider than the
/// depth" and not "the pool starved" -- a purely hash-limited run shows
/// most of its wall clock in it.
///
/// It is read last because a device whose queue was full for the run is
/// the wall whatever the arena's width: widening the arena then only
/// buffers. So the arena reading is what is left when neither end was
/// busy and every window the run had was out.
///
/// Each figure has a band it is read in, and a run that lands in one
/// gets no label: the two candidates are named instead. A figure near
/// its ceiling without reaching it does not say which of itself and the
/// arena the run was waiting on, and on a narrow machine the answer is
/// often neither.
///
/// @param d          The run's counters.
/// @param wall_ns    Wall clock the caller measured around the call.
/// @param threads    The compute pool's width.
/// @param planned    Window objects the plan allocated, which is the
///                   arena's width in windows.
/// @param window     Bytes per window, after the pipeline's rounding.
[[nodiscard]] inline diagnosis diagnose_read(const blake3pp::driver_stats& d,
                                             double wall_ns, unsigned threads,
                                             unsigned planned,
                                             std::size_t window,
                                             bands b = {}) noexcept {
  diagnosis r;
  r.transfers = d.transfers_landed;
  r.depth = d.queue_depth_final;
  r.windows_used = d.max_windows_in_flight;
  r.windows_planned = planned;
  r.window_bytes = window;
  if (!r.measured() || wall_ns <= 0.0) {
    return r;
  }
  r.mean_service_ns =
      static_cast<double>(d.device_wait_ns) / static_cast<double>(r.transfers);
  r.pool_busy =
      detail::share(d.agent_busy_ns, wall_ns * static_cast<double>(threads));
  r.min_service_ns = d.device_wait_min_ns;
  r.device_queued = detail::queued(r, d.device_wait_ns, wall_ns, b);
  r.starved = detail::share(d.starved_ns, wall_ns);
  r.arena_is_depth = planned > 0 && r.windows_used >= planned &&
                     planned <= r.depth;
  if (!r.long_enough()) {
    return r;
  }
  // The candidate the arena stands in for where it is a lever at all.
  const reading other = r.arena_is_depth ? reading::arena_limited
                                         : reading::device_limited;

  if (r.pool_busy >= b.saturated) {
    r.label = reading::hash_limited;
  } else if (r.device_queued >= b.saturated) {
    r.label = reading::device_limited;
  } else if (r.pool_busy >= b.idle) {
    r.first = reading::hash_limited;
    r.second = other;
  } else if (r.device_queued >= b.idle) {
    r.first = reading::device_limited;
    r.second = r.arena_is_depth ? reading::arena_limited
                                : reading::hash_limited;
  } else if (r.arena_is_depth) {
    r.label = reading::arena_limited;
  } else {
    // Neither end busy and the arena wider than the queue: nothing this
    // reads accounts for the run.
    r.first = reading::device_limited;
    r.second = reading::hash_limited;
  }
  return r;
}

/// A write run's reading.
///
/// The directions are not symmetrical. Writing, the pool FILLS windows
/// and the device drains them, and one window is one fill task, so the
/// windows in flight are the generator's parallelism as well as the
/// device's queue -- the arena and the depth are the same number and
/// there is no third reading between them. The two candidates are the
/// generator and the device, and they share the same windows, so their
/// shares sum to at most one and the reading is which of them held the
/// windows longer.
///
/// @param depth  Windows in flight, which is driver_stats::
///               queue_depth_final.
[[nodiscard]] inline diagnosis diagnose_write(const blake3pp::driver_stats& d,
                                              double wall_ns, unsigned threads,
                                              std::size_t window,
                                              bands b = {}) noexcept {
  diagnosis r;
  r.transfers = d.transfers_landed;
  r.depth = d.queue_depth_final;
  r.windows_used = r.depth;
  r.windows_planned = static_cast<unsigned>(r.depth);
  r.window_bytes = window;
  r.arena_is_depth = true;
  if (!r.measured() || wall_ns <= 0.0) {
    return r;
  }
  const double in_flight =
      wall_ns * static_cast<double>(std::max<std::uint64_t>(1, r.depth));
  r.mean_service_ns =
      static_cast<double>(d.device_wait_ns) / static_cast<double>(r.transfers);
  r.min_service_ns = d.device_wait_min_ns;
  r.pool_busy =
      detail::share(d.agent_busy_ns, wall_ns * static_cast<double>(threads));
  r.fill_busy = detail::share(d.agent_busy_ns, in_flight);
  // The share of the windows in flight the device held, which on this
  // side is directly comparable with fill_busy: the two shares are of
  // the same windows, and a window is either being filled or being
  // written.
  r.device_busy = detail::share(d.device_wait_ns, in_flight);
  r.device_queued = detail::queued(r, d.device_wait_ns, wall_ns, b);
  r.starved = detail::share(d.starved_ns, wall_ns);
  if (!r.long_enough()) {
    return r;
  }

  if (r.device_queued >= b.saturated) {
    r.label = reading::device_limited;
  } else if (r.fill_busy > r.device_busy + b.tie) {
    r.label = reading::generator_limited;
  } else if (r.device_busy > r.fill_busy + b.tie) {
    r.label = reading::device_limited;
  } else {
    r.first = reading::generator_limited;
    r.second = reading::device_limited;
  }
  return r;
}

}  // namespace b3tool
