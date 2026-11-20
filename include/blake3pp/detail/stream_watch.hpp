#pragma once

// The wait before a read of a stream that is empty: watch it for a
// bounded, adaptive budget, then sleep.
//
// A template over the platform, which supplies four operations --
//
//   now()    the monotonic clock, as a steady_clock time_point;
//   probe()  whether a read would return now, without waiting;
//   sleep()  wait until a read would return;
//   relax()  one iteration of a busy wait;
//
// -- so the tests drive the loop with a scripted clock and scripted
// arrivals and assert on what it counted, and the shipped reader
// supplies the real ones (stream_reader.cpp). The rule can only ever
// cost time, never a byte, so a digest test cannot see a defect in it;
// the counters are what make its shape observable.
//
// The watch replaces a sleep and a wake-up with a few probes, so it pays
// only where a probe costs less than the sleep it removes. Two platforms
// read without it:
//
//   * Darwin. A zero-timeout poll() costs the caller an involuntary
//     context switch whenever anything else is runnable, data queued or
//     not, so the probe that opens every wait is already a loss.
//   * Windows. PeekNamedPipe costs 1.50 us a call, and a watch built on it
//     lost every one of 42 counterbalanced pairs.
//
// Such a platform skips wait() entirely rather than running it with a
// zero budget (stream_reader.cpp carries the measurements).
//
// The budget moves in steps of an eighth of the ceiling and never jumps:
// a probe that finds data grows it, a budget that times out is halved,
// and a sleep that a full-ceiling watch would have avoided grows it.
// Eight consecutive hits re-arm a fully decayed budget. A smaller step
// notices a producer that speeds up more slowly. A step as large as the
// ceiling pays a full wasted watch on every long gap of a producer whose
// gaps straddle the ceiling; measured, that rule lost 5% to a reader that
// only sleeps. The floor is one probe interval: the smallest budget that
// still makes one probe before the sleep.
//
// By default the ceiling is 16 us and the watch is paced to one probe
// per microsecond, so a full budget is sixteen probes and a write is
// noticed at most a microsecond late. Measured on Linux against probe intervals
// of 4 and 16 us, 1 us was the fastest at 4 KiB producer writes and level
// at 512-byte ones.
//
// This is the only clock in the stream path, and it decides nothing the
// hasher can see: it chooses how the thread waits, watching or sleeping,
// for the same readiness. Nothing here ends a window.
//
// One instance per reader, touched only by the thread that owns the
// driver; a reader shared between threads would race here.
//
// Standard library only, so the pipe bench's drain, which links no
// library, runs the same rule.

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace blake3pp::detail {

class stream_watch {
 public:
  // The two figures the rule is tuned by: the ceiling, and the pace of
  // the probes. The library runs the defaults; the pipe bench's drain runs
  // others to measure them.
  struct bounds {
    std::chrono::nanoseconds ceiling{16'000};
    std::chrono::nanoseconds probe_interval{1'000};
  };

  stream_watch() noexcept : stream_watch(bounds{}) {}
  explicit stream_watch(bounds b) noexcept
      : ceiling_(b.ceiling),
        probe_interval_(b.probe_interval),
        step_(std::max(std::chrono::nanoseconds{1}, b.ceiling / 8)),
        floor_(b.probe_interval),
        budget_(b.ceiling) {}

  // What the waits did. `probes` counts every probe, the one each wait
  // opens with included; `caught` the waits a probe ended; `slept` the
  // waits that reached the sleep.
  struct counters {
    std::uint64_t probes = 0;
    std::uint64_t caught = 0;
    std::uint64_t slept = 0;
  };

  // Returns once a read would return something. May throw whatever the
  // platform's probe or sleep throws.
  template <class Platform>
  void wait(Platform& p) {
    ++stats_.probes;
    if (p.probe()) {
      return;
    }
    const std::chrono::steady_clock::time_point t0 = p.now();
    const std::chrono::nanoseconds budget = budget_;
    std::chrono::steady_clock::time_point last_probe = t0;
    for (;;) {
      std::chrono::steady_clock::time_point t = p.now();
      while (t - last_probe < probe_interval_) {
        p.relax();
        t = p.now();
      }
      last_probe = t;
      ++stats_.probes;
      if (p.probe()) {
        budget_ = std::min(ceiling_, budget + step_);
        ++stats_.caught;
        return;
      }
      // The probe before the budget: a budget of one interval makes
      // exactly one probe, as the floor promises.
      if (t - t0 >= budget) {
        break;
      }
    }
    budget_ = std::max(floor_, budget / 2);
    p.sleep();
    ++stats_.slept;
    if (p.now() - t0 < ceiling_) {
      budget_ = std::min(ceiling_, budget + step_);
    }
  }

  [[nodiscard]] std::chrono::nanoseconds budget() const noexcept {
    return budget_;
  }
  [[nodiscard]] const counters& stats() const noexcept { return stats_; }

  [[nodiscard]] std::chrono::nanoseconds ceiling() const noexcept {
    return ceiling_;
  }
  [[nodiscard]] std::chrono::nanoseconds probe_interval() const noexcept {
    return probe_interval_;
  }
  // A budget's move on a hit or a short sleep, an eighth of the ceiling.
  [[nodiscard]] std::chrono::nanoseconds step() const noexcept {
    return step_;
  }
  // The smallest budget, one probe interval.
  [[nodiscard]] std::chrono::nanoseconds floor() const noexcept {
    return floor_;
  }

 private:
  std::chrono::nanoseconds ceiling_;
  std::chrono::nanoseconds probe_interval_;
  std::chrono::nanoseconds step_;
  std::chrono::nanoseconds floor_;
  std::chrono::nanoseconds budget_;
  counters stats_{};
};

}  // namespace blake3pp::detail
