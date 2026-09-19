#pragma once

/// @file
/// Where a file pipeline's time goes: one record per I/O window, and on
/// request one per working agent per window. update_file() fills them
/// when a trace_buffer is set in its options and never otherwise, at a
/// cost of one branch per window; with a buffer set, a handful of clock
/// reads per window and per agent.
///
/// The library records; it never allocates, formats or writes. The
/// caller provides the storage, reads it back after the call, and turns
/// it into whatever it needs (blake3pp_bench_file writes Chrome trace
/// JSON). Storage that runs out costs a counted drop, never a stall or a
/// wrong digest. The finest timed unit is one part, a subtree of at least
/// 16 chunks; nothing below that is instrumented.
///
/// Standard library only, C++20.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace blake3pp {

/// One I/O window from the reader handing it out to the reader taking
/// it back. Timestamps are nanoseconds on std::chrono::steady_clock
/// relative to the trace_buffer's construction, so a plain difference is
/// a duration.
struct window_record {
  /// Bit in flags: the window was fanned out over the scheduler.
  static constexpr std::uint32_t flag_parallel = 1u << 0;
  /// Bit in flags: the window reaches the end of the file.
  static constexpr std::uint32_t flag_last = 1u << 1;

  /// Position of the window within this update_file() call, from 0.
  std::uint64_t index;
  /// Bytes in the window.
  std::uint64_t bytes;
  /// Before the pipeline asked the reader for the window.
  std::int64_t t_wait_begin;
  /// The reader handed the window out; its bytes are in memory.
  std::int64_t t_ready;
  /// The compute is done: every agent has joined, or hasher::update()
  /// returned when the window was hashed inline.
  std::int64_t t_joined;
  /// The chaining values are in the hasher. Equals t_joined for an inline
  /// window, which has no separate absorb step.
  std::int64_t t_absorbed;
  /// The window's buffer is back with the reader.
  std::int64_t t_released;
  /// Summed over the agents that took part: nanoseconds spent compressing.
  std::uint64_t agent_busy_ns;
  /// Bulk invocations that compressed at least one part.
  std::uint32_t agents_active;
  /// flag_parallel, flag_last.
  std::uint32_t flags;
};

/// One bulk invocation that compressed at least one part of a window.
/// Opt-in: written only when the trace_buffer was given agent storage.
struct agent_record {
  /// window_record::index of the window this belongs to.
  std::uint64_t window;
  /// The first part started (same clock and epoch as window_record).
  std::int64_t t_begin;
  /// From the first part starting to the last part ending.
  std::uint64_t busy_ns;
  /// The shortest part this invocation compressed.
  std::uint64_t min_part_ns;
  /// The longest part this invocation compressed.
  std::uint64_t max_part_ns;
  /// Parts this invocation compressed.
  std::uint32_t parts;
  /// Chunks per part (the same for every part of a window).
  std::uint32_t part_chunks;
  /// The CPU the invocation ran on at t_begin (sched_getcpu() on Linux);
  /// UINT32_MAX where the platform cannot say.
  std::uint32_t cpu;
  /// Zero.
  std::uint32_t reserved;
};

static_assert(std::is_trivially_copyable_v<window_record>);
static_assert(std::is_trivially_copyable_v<agent_record>);

/// Caller-owned storage for the records of one or more update_file()
/// calls, handed in through file_io_options::trace.
///
/// Records are claimed in order until the storage is full; every claim
/// beyond that is counted as a drop. Window records are claimed by the
/// thread driving the pipeline; agent records by any thread, through one
/// atomic increment. A claimed record has exactly one writer, and nothing
/// reads the records until the update_file() call has returned. That is
/// the whole concurrency contract.
///
/// @code
/// std::vector<blake3pp::window_record> windows(file_bytes / window_bytes + 1);
/// blake3pp::trace_buffer trace(windows);
/// blake3pp::update_file(h, path, {.trace = &trace});
/// for (const auto& w : trace.windows()) {
///   // w.t_ready - w.t_wait_begin: the wait for the read
/// }
/// @endcode
class trace_buffer {
 public:
  /// @param windows  Storage for window records; one per window.
  /// @param agents   Storage for agent records; empty leaves agent
  ///                 recording off (wants_agents() is false).
  explicit trace_buffer(std::span<window_record> windows,
                        std::span<agent_record> agents = {}) noexcept
      : windows_(windows), agents_(agents),
        epoch_(std::chrono::steady_clock::now()) {}

  trace_buffer(const trace_buffer&) = delete;
  trace_buffer& operator=(const trace_buffer&) = delete;

  /// The next window record, zeroed, or nullptr when the storage is full
  /// (counted in dropped_windows()). One thread claims window records.
  [[nodiscard]] window_record* claim_window() noexcept {
    if (n_windows_ >= windows_.size()) {
      ++dropped_windows_;
      return nullptr;
    }
    window_record* const r = &windows_[n_windows_++];
    *r = window_record{};
    return r;
  }

  /// The next agent record, zeroed, or nullptr when the storage is full
  /// (counted in dropped_agents()) or when no agent storage was given
  /// (not counted). Any thread; one atomic increment.
  [[nodiscard]] agent_record* claim_agent() noexcept {
    if (agents_.empty()) {
      return nullptr;
    }
    const std::size_t i = next_agent_.fetch_add(1, std::memory_order_relaxed);
    if (i >= agents_.size()) {
      dropped_agents_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    agent_record* const r = &agents_[i];
    *r = agent_record{};
    return r;
  }

  /// Whether agent records are wanted: agent storage was given.
  [[nodiscard]] bool wants_agents() const noexcept { return !agents_.empty(); }

  /// Nanoseconds since construction on std::chrono::steady_clock.
  [[nodiscard]] std::int64_t now() const noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - epoch_)
        .count();
  }

  /// The epoch as absolute steady_clock nanoseconds (CLOCK_MONOTONIC on
  /// Linux), so timestamps taken by other tools on the same clock align
  /// with the records: theirs minus this is on the records' scale.
  [[nodiscard]] std::int64_t epoch_ns() const noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               epoch_.time_since_epoch())
        .count();
  }

  /// The window records claimed so far, in claim order.
  [[nodiscard]] std::span<const window_record> windows() const noexcept {
    return windows_.first(n_windows_);
  }

  /// The agent records claimed so far, in claim order.
  [[nodiscard]] std::span<const agent_record> agents() const noexcept {
    // The claim index overshoots the storage once it is full.
    return agents_.first(std::min(next_agent_.load(std::memory_order_relaxed),
                                  agents_.size()));
  }

  /// Window claims refused for lack of storage.
  [[nodiscard]] std::uint64_t dropped_windows() const noexcept {
    return dropped_windows_;
  }

  /// Agent claims refused for lack of storage.
  [[nodiscard]] std::uint64_t dropped_agents() const noexcept {
    return dropped_agents_.load(std::memory_order_relaxed);
  }

  /// Forgets every record and drop; the epoch stays, so later timestamps
  /// remain comparable with earlier ones. Not concurrent with writers.
  void clear() noexcept {
    n_windows_ = 0;
    dropped_windows_ = 0;
    next_agent_.store(0, std::memory_order_relaxed);
    dropped_agents_.store(0, std::memory_order_relaxed);
  }

 private:
  std::span<window_record> windows_;
  std::span<agent_record> agents_;
  std::chrono::steady_clock::time_point epoch_;
  std::size_t n_windows_ = 0;
  std::uint64_t dropped_windows_ = 0;
  std::atomic<std::size_t> next_agent_{0};
  std::atomic<std::uint64_t> dropped_agents_{0};
};

}  // namespace blake3pp
