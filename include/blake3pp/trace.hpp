#pragma once

/// @file
/// Where a file pipeline's time goes: one record per I/O window, and on
/// request one per working agent per window. update_file(),
/// update_stream() and write_file() over a scheduler fill them when a
/// trace_buffer is set in their options and never otherwise, at a cost of
/// one branch per window; with a buffer set, a handful of clock reads per
/// window and per agent.
///
/// The library records; it never allocates, formats or writes. The
/// caller provides the storage, reads it back after the call, and turns
/// it into whatever it needs (blake3pp_bench_file writes Chrome trace
/// JSON). Storage that runs out costs a counted drop, never a stall or a
/// wrong digest. The finest timed unit is one part, a subtree of at least
/// 16 chunks; nothing below that is instrumented.
///
/// diagnose_read() and diagnose_write(), at the end of this header,
/// read a finished run's driver_stats and say which side of the
/// pipeline it was waiting on. Call either once per run; neither
/// formats nor prints.
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
  /// Bit in flags: the window reached the tree as the decomposition of
  /// a run of parts rather than as one subtree, because it is neither a
  /// whole window of the grid nor one that starts where a node of that
  /// size may start.
  ///
  /// Only a stream sets it (blake3ppsum's `-`): a producer that ran dry
  /// before filling a window ends it early, and the window after that one
  /// is short in turn, bringing the grid back onto its boundary. Both
  /// carry this. How many windows ended that way is
  /// stream_report::short_flushes.
  static constexpr std::uint32_t flag_part_run = 1u << 3;

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
  /// flag_parallel, flag_last, flag_short_first, flag_part_run.
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
  /// read or for a window to come back from the pool. The two waits are
  /// summed: this field answers whether the driver thread is the
  /// bottleneck, and for that it does not matter which of them it was
  /// waiting on. device_wait_ns and starved_ns separate them.
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
  // Fields are appended, never inserted: a caller that reads this out of
  // a build it did not compile reads it by offset.
  /// Windows whose transfer landed: reads on a read run, writes on a
  /// write one. Counted on every run, traced or not.
  std::uint64_t transfers_landed;
  /// Bytes those transfers moved.
  std::uint64_t transfer_bytes;
  /// Summed per-window time the device had the transfer, with nothing of
  /// the pipeline's own in it: t_ready - t_wait_begin on a read, the
  /// submission to the completion on a write. Divided by
  /// transfers_landed it is the mean per-window latency; divided again
  /// by the depth in force it is the interval between windows. Counted
  /// on every run.
  std::uint64_t device_wait_ns;
  /// Nanoseconds the pipeline could not use a window it already had,
  /// for the one reason more depth cannot fix. Reading, that is bytes
  /// left and no free window on the pool -- the hash behind the device.
  /// Writing, it is the opposite event: the driver with nothing to
  /// write because every window is still being generated. Counted on
  /// every run.
  ///
  /// Reading, this is NOT a measure of how starved the pool was. It
  /// counts "no free window", which fires structurally whenever the
  /// arena is no wider than the queue depth, so it is nonzero on a run
  /// that is purely hash-limited: a 64 MiB x depth 16 arm at 96%
  /// agent_busy_ns still showed 1.33 s of it. Read it as "the arena is
  /// not wider than the depth" and use agent_busy_ns for the hash
  /// side. Writing it is the clean signal it reads like, because there
  /// the windows in flight are the generator's parallelism.
  std::uint64_t starved_ns;
  /// The queue depth the run finished on: the geometry to pass back as
  /// queue_depth to repeat it.
  std::uint64_t queue_depth_final;
  /// The deepest queue depth the run reached.
  std::uint64_t queue_depth_peak;
  /// Summed over every window and every agent that took part:
  /// nanoseconds the pool spent compressing on a read run, filling on
  /// a write one. Divided by the wall clock and the thread count it is
  /// the pool's busy share, which is the hash side's answer to the
  /// question device_wait_ns answers for the device. Counted on every
  /// run that passes a trace buffer, whether or not it asked for
  /// per-window records.
  ///
  /// starved_ns is not the same measure and does not substitute for
  /// it: see the note on that field.
  std::uint64_t agent_busy_ns;
  /// The shortest of those per-transfer times, which is the device
  /// serving one transfer with nothing queued ahead of it. Zero when
  /// nothing landed.
  ///
  /// It is what makes the mean readable. A queue's latency is
  /// proportional to its depth by construction, so device_wait_ns /
  /// transfers_landed alone cannot say whether the device was the
  /// constraint: against this it can. A mean at depth times this is a
  /// queue that was full for the run, and the device is the wall; a mean
  /// at this is a device that served every transfer alone and had
  /// capacity to spare, whatever the mean's absolute size. Counted on
  /// every run, at one comparison per transfer.
  std::uint64_t device_wait_min_ns;
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

/// Caller-owned storage for the records of one or more pipeline runs,
/// handed in through the options' `trace` field (file_io_options,
/// stream_io_options, file_write_options).
///
/// Records are claimed in order until the storage is full; every claim
/// beyond that is counted as a drop. Window records are claimed by the
/// thread driving the pipeline; agent records by any thread, through one
/// atomic increment. A claimed record has exactly one writer, and nothing
/// reads the records until the call has returned.
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

// --- Reading a finished run off its counters --------------------------------
//
// driver_stats says where a run's time went; these say which side of the
// pipeline the run was waiting on. One call at the end of a run, never
// per window; the bodies are in src/diagnosis.cpp.

/// What a finished pipeline run was limited by.
///
/// The first four are read runs, the fifth is the write side's name for
/// the first, and `undecided` carries two candidates the run could not
/// separate (diagnosis::first and diagnosis::second).
///
/// Enumerators are appended, never inserted, reordered or renamed, the
/// same rule detail/arch.def and detail/io_engine.def follow: a caller
/// may switch over this against a library build it did not compile.
enum class bottleneck {
  /// The pool was saturated. The hash is the constraint and I/O geometry
  /// will not move it.
  hash,
  /// The device's queue was full for the run, at the bytes the run kept
  /// outstanding (diagnosis::bytes_in_flight).
  device,
  /// Neither end was busy and the run ran out of windows: every window
  /// the pool holds is a transfer slot the device cannot use.
  arena,
  /// Neither end was busy, the arena was wider than the queue, and the
  /// device served every transfer as if it were alone, so what the run
  /// never had more of is bytes outstanding.
  ///
  /// Only the test suite's synthetic device is known to produce it.
  in_flight,
  /// Writing: the pool could not fill windows fast enough to keep the
  /// device busy.
  generator,
  /// No reading: the run was too short to read, had nothing to read, or
  /// landed between two candidates.
  undecided,
};

/// `b` as text: "hash-limited", "device-limited", "arena-limited",
/// "in-flight-limited", "generator-limited", "undecided".
///
/// These strings are API. They are what blake3ppsum and blake3ppgen
/// print under --diag, alongside the read-backend strings of
/// read_backend(), which are already read out of benchmark output by
/// hand and by script. A string changes only when the value it names
/// changes meaning.
[[nodiscard]] const char* to_string(bottleneck b) noexcept;

/// The bands a share of a resource is read in.
///
/// Every entry point below takes these, so a caller can set its own.
/// They are not calibrated on a thread count: 64% pool-busy is a starving
/// pool on 32 threads while 88% on 16 is already near its ceiling, so a
/// single cut on the pool figure misreads a small machine. The bands apply
/// to each side separately and the reading comes from comparing the two,
/// which is scale-free. Where a figure lands between the bands the run
/// gets no label.
struct bands {
  /// At or above this a resource was the wall for the run.
  double saturated = 0.90;
  /// Below this it plainly was not.
  double idle = 0.80;
  /// Two shares closer than this are the same figure.
  double tie = 0.05;
};

/// The figures a reading is made from, and the reading.
struct diagnosis {
  bottleneck label = bottleneck::undecided;
  /// With `label == undecided`, the two the run could not separate.
  bottleneck first = bottleneck::undecided;
  bottleneck second = bottleneck::undecided;

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
  /// The minimum alone is not enough either. A device that saturates
  /// below the depth in force serves no transfer alone after the first,
  /// so its mean sits well under depth times the minimum while its queue
  /// is full the whole run. The mean over the minimum is therefore the
  /// no-queueing test, and the share of slots is the how-queued measure.
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
  /// Bytes the run kept outstanding at the device: the depth times the
  /// window. This is the quantity a device responds to, and the depth
  /// alone does not state it -- a full queue of four 8 MiB reads is
  /// 32 MiB outstanding and a full queue of four 64 MiB reads is 256,
  /// and a striped volume answers the two differently. Measured on a
  /// four-drive PCIe 5 stripe with no hashing in the picture, reading
  /// 64 GiB cold through O_DIRECT: 36.5-38.0 GiB/s at 32 MiB
  /// outstanding, 46.4-46.7 at 256 MiB, 53.8-53.9 at 1 GiB. So a
  /// device-limited reading is a reading at this number, and a reader
  /// who is not told it cannot tell a device at its ceiling from a
  /// device answering a narrow geometry.
  std::uint64_t bytes_in_flight = 0;
  /// The device never put one transfer behind another: its mean
  /// per-transfer time is within the tie band of its own minimum. Its
  /// server had capacity to spare whatever its queue looked like, so
  /// concurrency rather than the device is what the run was short of.
  bool device_unqueued = false;
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
/// Where the arena was NOT the run's bound and neither end was busy,
/// the fourth reading applies: the device served every transfer as if
/// it were alone, so what the run never had more of is bytes
/// outstanding -- the depth times the window.
///
/// Each figure has a band it is read in, and a run that lands in one
/// gets no label: the two candidates are named instead. A figure near
/// its ceiling without reaching it does not say which of itself and the
/// arena the run was waiting on, and on a narrow machine the answer is
/// often neither.
///
/// @code
/// std::vector<blake3pp::window_record> windows(count);
/// blake3pp::trace_buffer trace(windows);
/// const auto t0 = std::chrono::steady_clock::now();
/// blake3pp::update_file(h, path, {.trace = &trace});
/// const double wall_ns = std::chrono::duration<double, std::nano>(
///     std::chrono::steady_clock::now() - t0).count();
/// const auto d = blake3pp::diagnose_read(trace.driver(), wall_ns, threads,
///                                        planned, window_bytes);
/// std::puts(blake3pp::to_string(d.label));
/// @endcode
///
/// @param d          The run's counters.
/// @param wall_ns    Wall clock the caller measured around the call.
/// @param threads    The compute pool's width.
/// @param planned    Window objects the plan allocated, which is the
///                   arena's width in windows.
/// @param window     Bytes per window, after the pipeline's rounding.
/// @param b          The bands each share is read in.
[[nodiscard]] diagnosis diagnose_read(const driver_stats& d, double wall_ns,
                                      unsigned threads, unsigned planned,
                                      std::size_t window,
                                      bands b = {}) noexcept;

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
/// The windows in flight come from driver_stats::queue_depth_final.
///
/// @param d          The run's counters.
/// @param wall_ns    Wall clock the caller measured around the call.
/// @param threads    The compute pool's width.
/// @param window     Bytes per window, after the pipeline's rounding.
/// @param b          The bands each share is read in.
[[nodiscard]] diagnosis diagnose_write(const driver_stats& d, double wall_ns,
                                       unsigned threads, std::size_t window,
                                       bands b = {}) noexcept;

}  // namespace blake3pp
