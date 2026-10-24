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
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <span>
#include <string_view>
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
  /// Bit in flags: the short window that brings a hasher already
  /// part-way through a message back onto a window boundary.
  static constexpr std::uint32_t flag_short_first = 1u << 2;

  /// Position of the window within this call, from 0.
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
  /// flag_parallel, flag_last, flag_short_first.
  std::uint32_t flags;
  /// Which window object -- buffer, part table and operation cell --
  /// carried this window. The pipeline reuses a fixed set of them, so
  /// this is the track a window's phases belong on when several are in
  /// flight; zero on the sequential path, which has one.
  std::uint32_t slot;
  /// The file's index in a many-files operation (async_update_files),
  /// which fills one buffer with every file's windows; zero for a single
  /// file.
  std::uint32_t file;
};

/// One piece of a window's work on one agent: a bulk invocation that
/// compressed at least one part, or the fold of the window's chaining
/// values that ran on the agent that finished last (parts == 0), or on
/// the write side one window's fill. Opt-in: written only when the
/// trace_buffer was given agent storage.
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
  /// Parts this invocation compressed; zero marks the fold that followed
  /// the bulk, whose busy_ns is the fold's own time.
  std::uint32_t parts;
  /// Chunks per part (the same for every part of a window).
  std::uint32_t part_chunks;
  /// The CPU the invocation ran on at t_begin (sched_getcpu() on Linux);
  /// UINT32_MAX where the platform cannot say.
  std::uint32_t cpu;
  /// window_record::file of the window this belongs to.
  std::uint32_t file;
};

/// What the thread driving the pipeline spent its own time on.
///
/// The stage shares of a window do not sum to the wall clock once
/// several windows are in flight, so this is the number that says
/// whether the driver is the bottleneck: busy against parked is the
/// thread's own split, and the loop counters say what it was doing when
/// it was busy.
///
/// Written only by the driver thread, and only while it is inside its
/// own loop.
struct driver_stats {
  /// Nanoseconds the driver spent outside a blocking poll: submitting,
  /// reaping, running completions and inserting into the reducer.
  std::uint64_t busy_ns;
  /// Nanoseconds the driver spent inside a blocking poll, waiting for a
  /// read or for a window to come back from the pool.
  std::uint64_t parked_ns;
  /// Times round the loop: one drain, one round of starts, one flush and
  /// one poll each.
  std::uint64_t iterations;
  /// poll() calls, blocking and not.
  std::uint64_t polls;
  /// Of those, the ones that blocked.
  std::uint64_t blocking_polls;
  /// Blocking polls released without a completion of their own: a wake
  /// from a thread handing a finished window back. One per window is the
  /// expected shape; many more means the driver is being woken while it
  /// still had work in hand.
  std::uint64_t wakes;
  /// Completions run off the run queue: one per window coming back from
  /// the pool, plus any deferred failure.
  std::uint64_t queue_runs;
  /// Windows the scope held back because the reducer had no node free.
  std::uint64_t admission_stalls;
  /// The most subtrees the reducer held at once: how far ahead of the
  /// tree the reads ran, and what the node storage has to cover.
  std::uint64_t max_pending;
  /// The most reads submitted and not yet landed at once: what the
  /// device saw, bounded by the queue depth.
  std::uint64_t max_reads_in_flight;
  /// The most windows in flight at once -- reading, on the pool or
  /// waiting to reduce -- bounded by the window objects the plan
  /// allocated.
  std::uint64_t max_windows_in_flight;
  /// The rung the reads actually engaged, NUL-padded: "io_uring+direct",
  /// "pread", "readfile" and the rest, with the reason appended when an
  /// asynchronous engine was asked for and refused. Copied rather than
  /// referenced, so it outlives the file. Read it through
  /// read_backend(), which trims the padding.
  std::array<char, 56> read_backend;
};

/// The engaged read rung as text; empty when nothing recorded one.
[[nodiscard]] inline std::string_view read_backend(
    const driver_stats& d) noexcept {
  const std::size_t n = static_cast<std::size_t>(
      std::find(d.read_backend.begin(), d.read_backend.end(), '\0') -
      d.read_backend.begin());
  return {d.read_backend.data(), n};
}

/// Records `name` in `d`, truncated to what the field holds.
inline void set_read_backend(driver_stats& d, std::string_view name) noexcept {
  d.read_backend = {};
  const std::size_t n = std::min(name.size(), d.read_backend.size() - 1);
  std::copy_n(name.begin(), n, d.read_backend.begin());
}

static_assert(std::is_trivially_copyable_v<window_record>);
static_assert(std::is_trivially_copyable_v<agent_record>);
static_assert(std::is_trivially_copyable_v<driver_stats>);

/// Caller-owned storage for the records of one or more update_file()
/// calls, handed in through file_io_options::trace.
///
/// Records are claimed in order until the storage is full; every claim
/// beyond that is counted as a drop. Window records are claimed by the
/// thread driving the pipeline; agent records by any thread, through one
/// atomic increment. A claimed record has exactly one writer, and nothing
/// reads the records until the update_file() call has returned.
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

  /// Where the driver thread's own time went. Only the pipeline path
  /// fills this in; the sequential window loop, which has no driver
  /// thread, leaves it zeroed.
  [[nodiscard]] const driver_stats& driver() const noexcept {
    return driver_;
  }

  /// The driver's own record, for the thread that owns the loop.
  [[nodiscard]] driver_stats& driver() noexcept { return driver_; }

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
    driver_ = driver_stats{};
    next_agent_.store(0, std::memory_order_relaxed);
    dropped_agents_.store(0, std::memory_order_relaxed);
  }

 private:
  std::span<window_record> windows_;
  std::span<agent_record> agents_;
  std::chrono::steady_clock::time_point epoch_;
  std::size_t n_windows_ = 0;
  std::uint64_t dropped_windows_ = 0;
  driver_stats driver_{};
  std::atomic<std::size_t> next_agent_{0};
  std::atomic<std::uint64_t> dropped_agents_{0};
};

/// How a pipeline run records, decided by type.
///
/// The blocking entry points take a `trace_buffer*` in their options and
/// choose between the two policies below at run time, once per call. A
/// sender cannot: its operation state is a type fixed when the receiver
/// is connected, so the senders in <blake3pp/async_io.hpp> take the policy
/// as an argument instead. `null_tracer` is the default and records
/// nothing, at no cost; `buffer_tracer` records into the buffer it names.
///
/// @code
/// blake3pp::trace_buffer trace(windows);
/// auto s = blake3pp::async::async_update_file(ctx, h, path, sched, {},
///                                             blake3pp::buffer_tracer{&trace});
/// @endcode
struct null_tracer {
  static constexpr bool traced = false;
  [[nodiscard]] constexpr trace_buffer* buffer() const noexcept {
    return nullptr;
  }
};

struct buffer_tracer {
  static constexpr bool traced = true;
  /// Not owned: the buffer has to outlive every operation started with
  /// this tracer, and nothing reads its records until they have completed.
  trace_buffer* buf = nullptr;
  [[nodiscard]] constexpr trace_buffer* buffer() const noexcept {
    return buf;
  }
};

/// What the pipeline asks of a tracing policy: whether its runs are
/// instrumented, decided at compile time, and the buffer they record into.
template <class T>
concept tracer = std::is_nothrow_copy_constructible_v<T> &&
                 requires(const T& t) {
                   { T::traced } -> std::convertible_to<bool>;
                   { t.buffer() } noexcept -> std::same_as<trace_buffer*>;
                 };

static_assert(tracer<null_tracer>);
static_assert(tracer<buffer_tracer>);

}  // namespace blake3pp
