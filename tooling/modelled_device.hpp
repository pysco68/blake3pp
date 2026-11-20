#pragma once

// A device to put under a driver that has none: the bench's null driver
// and the test drivers, so a control law can be shown a knee that was
// chosen rather than measured.
//
// One FIFO server with a fixed service time per window plus a fixed
// propagation delay: a transfer starts when the server is free, holds
// it for the service time, and lands the latency later. At depth d the
// steady-state rate is min(d x window / (service + latency), window /
// service), so throughput rises linearly to a knee at
// 1 + latency / service and is flat above it. The knee names that depth
// and the latency follows from it.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include <blake3pp/detail/driver.hpp>

namespace b3tool {

class modelled_device {
 public:
  using instant = std::chrono::steady_clock::time_point;

  // No device at all: every transfer lands as soon as it is polled.
  modelled_device() = default;

  // A server that holds a whole window for `service`, behind
  // (knee - 1) service times of propagation delay.
  modelled_device(std::chrono::nanoseconds service, unsigned knee) noexcept
      : service_(service),
        latency_(static_cast<std::int64_t>(knee > 1 ? knee - 1 : 0) *
                 service) {}

  // The server a device of `gib_s` GiB/s is for windows of
  // `window_bytes`, at `knee`.
  [[nodiscard]] static modelled_device at_rate(double gib_s, unsigned knee,
                                               std::size_t window_bytes) {
    if (gib_s <= 0.0) {
      return {};
    }
    const double bytes_per_ns = gib_s * 1024.0 * 1024.0 * 1024.0 / 1e9;
    return {std::chrono::nanoseconds{static_cast<std::int64_t>(std::max(
                1.0, static_cast<double>(window_bytes) / bytes_per_ns))},
            knee};
  }

  [[nodiscard]] bool active() const noexcept {
    return service_ > std::chrono::nanoseconds::zero();
  }
  [[nodiscard]] std::chrono::nanoseconds service() const noexcept {
    return service_;
  }
  [[nodiscard]] std::chrono::nanoseconds latency() const noexcept {
    return latency_;
  }

  // Admits one transfer of `share` of a full window now, and returns when
  // it lands. Called at submission, which is what lets several
  // transfers' service overlap and the rate rise with the depth. One
  // server, FIFO, so the times this hands out are already in order.
  [[nodiscard]] instant admit(double share = 1.0) noexcept {
    const auto held = std::chrono::duration_cast<std::chrono::nanoseconds>(
        service_ * share);
    const instant start = std::max(blake3pp::detail::steady_now(), free_);
    free_ = start + held;
    return free_ + latency_;
  }

  // Waits toward `ready`, in slices no longer than a service time:
  // sleeping the whole way would park the driver thread through the
  // rounds that admit the next windows, which caps the concurrency at
  // whatever one sleep allows. Spins for the last microsecond, which is
  // what keeps a service time of a few hundred microseconds from being
  // quantised by the scheduler's timer slack. Returns whether `ready`
  // has passed.
  bool wait_toward(instant ready) const noexcept {
    using std::chrono::microseconds;
    const std::chrono::nanoseconds left =
        ready - blake3pp::detail::steady_now();
    if (left <= std::chrono::nanoseconds::zero()) {
      return true;
    }
    if (left > microseconds{2}) {
      const std::chrono::nanoseconds slice =
          std::max<std::chrono::nanoseconds>(service_, microseconds{1});
      std::this_thread::sleep_for(std::min<std::chrono::nanoseconds>(
          left - microseconds{1}, slice));
    } else {
      std::this_thread::yield();
    }
    return false;
  }

 private:
  std::chrono::nanoseconds service_{};
  std::chrono::nanoseconds latency_{};
  instant free_{};  // when the server finishes what it holds
};

}  // namespace b3tool
