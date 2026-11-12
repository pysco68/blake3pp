#pragma once

// The file bench's stand-in for a device: a file_driver whose reads land
// without touching storage, optionally behind a modelled device with a
// chosen service time and knee, so the pipeline's engine ceiling and its
// control law can be measured apart from any real drive.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <thread>
#include <vector>

#include <blake3pp/detail/driver.hpp>
#include <blake3pp/detail/memory_driver.hpp>

namespace b3bench {

// A device to put under the null driver, so a control law can be shown
// a knee that was chosen rather than measured.
//
// One FIFO server with a fixed service time per window plus a fixed
// propagation delay: a transfer starts when the server is free, holds
// it for `service_ns`, and lands `latency_ns` later. At depth d the
// steady-state rate is min(d x window / (service + latency), window /
// service), so throughput rises linearly to a knee at
// 1 + latency / service and is flat above it. `knee` names that depth
// and the latency follows from it.
struct null_device {
  double rate_gib_s = 0.0;  // 0: complete immediately, no device at all
  unsigned knee = 1;
  std::int64_t service_ns = 0;
  std::int64_t latency_ns = 0;

  [[nodiscard]] bool active() const noexcept { return rate_gib_s > 0.0; }

  void settle(std::size_t window_bytes) noexcept {
    if (!active()) {
      return;
    }
    const double bytes_per_ns = rate_gib_s * 1024.0 * 1024.0 * 1024.0 / 1e9;
    service_ns = static_cast<std::int64_t>(
        std::max(1.0, static_cast<double>(window_bytes) / bytes_per_ns));
    latency_ns = static_cast<std::int64_t>(knee > 1 ? knee - 1 : 0) *
                 service_ns;
  }
};

// The pipeline with no device under it, and no bytes either.
//
// The file is VIRTUAL: it reports whatever size the run asks for, while
// the driver owns only windows x window_bytes of buffer and every window
// reuses its slot's slice. Reads complete with whatever is already there,
// so what this measures is the engine, the fan-out and the fold, with
// the device removed -- the ceiling a real one is measured against.
//
// That is NOT the same benchmark as hashing a real span:
// the bytes here are a handful of resident windows, not a working set.
// See bench/README.md.
//
// The completion half is memory_driver's, because a source with nothing
// to read has exactly one job -- report from poll() and not before.
// Given a null_device it paces those reports instead, so the synthetic
// arms can be run against a device whose knee was chosen rather than
// measured.
class null_driver {
 public:
  // The pool is sized and faulted in at construction, before the caller
  // starts its clock. Leaving it to reserve() would put a quarter of a
  // gigabyte of first-touch page faults inside the measurement at 64 MiB
  // windows, which is exactly the shape of a pipeline regression that is
  // not one.
  null_driver(const blake3pp::detail::io_driver_options&, unsigned,
              std::uint64_t size, std::byte filler, std::size_t pool_bytes,
              null_device dev = {})
      : size_(size), dev_(dev), pool_(pool_bytes, filler) {}
  null_driver(const null_driver&) = delete;
  null_driver& operator=(const null_driver&) = delete;

  class file {
   public:
    explicit file(null_driver& drv) noexcept : drv_(&drv) {}
    [[nodiscard]] std::uint64_t size() const noexcept { return drv_->size_; }
    [[nodiscard]] std::string_view name() const noexcept { return "null"; }
    // The sink has no length to trim: the write scope calls this at the
    // ends of a run and the answer is the same either way.
    void set_size(std::uint64_t) noexcept {}

   private:
    null_driver* drv_;
  };

  void reserve(unsigned windows, std::size_t window_bytes) {
    const std::size_t want = window_bytes * windows;
    if (pool_.size() < want) {
      pool_.assign(want, filler_);
    }
    window_stride_ = window_bytes;
  }

  // Each slot owns one slice, which every window through that slot
  // reuses: the source has no distinct bytes, and separate slots keep
  // the reads from overlapping.
  [[nodiscard]] blake3pp::detail::window_storage window_storage_for(
      file&, unsigned slot, std::uint64_t, std::size_t bytes) noexcept {
    const std::span<std::byte> slice{pool_.data() + slot * window_stride_,
                                     bytes};
    return {slice, std::span<const std::byte>(slice)};
  }

  void submit_read(file& f, std::uint64_t, std::span<std::byte> buf,
                   blake3pp::detail::io_op& op) noexcept {
    submit(f, buf.size(), op);
  }
  // The sink half: the bytes are already where they were generated, so
  // the only work is reporting from poll() and not before.
  void submit_write(file& f, std::uint64_t, std::span<const std::byte> buf,
                    blake3pp::detail::io_op& op) noexcept {
    submit(f, buf.size(), op);
  }
  void flush() noexcept {}

  std::size_t poll(bool block) {
    if (!dev_.active()) {
      return reads_.poll(block);
    }
    release_ready();
    if (block && reads_.in_flight() == 0 && !timed_.empty()) {
      wait_until(timed_.front().ready);
      release_ready();
    }
    // Only ever park on the waiter with nothing of the device's own
    // still to land: the waiter is woken by the pool, not by the clock.
    return reads_.poll(block && timed_.empty());
  }

  void wake() noexcept { reads_.wake(); }
  void cancel(blake3pp::detail::io_op& op) noexcept {
    for (auto it = timed_.begin(); it != timed_.end(); ++it) {
      if (it->op == &op) {
        reads_.push(op, it->owner);
        timed_.erase(it);
        break;
      }
    }
    reads_.cancel(op);
  }
  [[nodiscard]] std::size_t in_flight() const noexcept {
    return reads_.in_flight() + timed_.size();
  }
  void drain() noexcept {
    release_all();
    reads_.drain();
  }
  // Nothing here is in the OS's hands; one file's share is its own queued
  // operations, as on the memory driver.
  void drain(file& f) noexcept {
    release_all();
    reads_.drain(&f);
  }

 private:
  struct timed_op {
    blake3pp::detail::io_op* op;
    const void* owner;
    std::int64_t ready;
  };

  [[nodiscard]] static std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  void submit(file& f, std::size_t bytes, blake3pp::detail::io_op& op) noexcept {
    if (!dev_.active()) {
      reads_.push(op, &f);
      return;
    }
    // One server, FIFO, so the ready times this produces are already in
    // order and the queue is a plain deque.
    const double share = dev_.service_ns == 0 || window_stride_ == 0
                             ? 1.0
                             : static_cast<double>(bytes) /
                                   static_cast<double>(window_stride_);
    const auto service =
        static_cast<std::int64_t>(static_cast<double>(dev_.service_ns) * share);
    const std::int64_t start = std::max(now_ns(), server_free_);
    server_free_ = start + service;
    timed_.push_back({&op, &f, server_free_ + dev_.latency_ns});
    op.live = true;
  }

  void release_ready() noexcept {
    const std::int64_t t = now_ns();
    while (!timed_.empty() && timed_.front().ready <= t) {
      reads_.push(*timed_.front().op, timed_.front().owner);
      timed_.pop_front();
    }
  }

  void release_all() noexcept {
    while (!timed_.empty()) {
      reads_.push(*timed_.front().op, timed_.front().owner);
      timed_.pop_front();
    }
  }

  // Waits for one completion, in slices no longer than a service time:
  // sleeping the whole way would park the driver thread through the
  // rounds that admit the next windows, which caps the concurrency at
  // whatever one sleep allows. Spins for the last microsecond, which is
  // what keeps a service time of a few hundred microseconds from being
  // quantised by the scheduler's timer slack.
  void wait_until(std::int64_t ready) const noexcept {
    const std::int64_t slice = std::max<std::int64_t>(dev_.service_ns, 1000);
    const std::int64_t left = ready - now_ns();
    if (left <= 0) {
      return;
    }
    if (left > 2000) {
      std::this_thread::sleep_for(
          std::chrono::nanoseconds(std::min(left - 1000, slice)));
    } else {
      std::this_thread::yield();
    }
  }

  std::uint64_t size_;
  std::byte filler_{};
  std::size_t window_stride_ = 0;
  null_device dev_{};
  std::int64_t server_free_ = 0;
  std::deque<timed_op> timed_;
  std::vector<std::byte> pool_;
  blake3pp::detail::queued_reads reads_;
};

static_assert(blake3pp::detail::file_driver<null_driver>);

}  // namespace b3bench
