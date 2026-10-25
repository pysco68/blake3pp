#pragma once

/// @file
/// File hashing driven by a scheduler: the intersection of io.hpp, which
/// windows a file at storage speed, and parallel.hpp, which hashes
/// subtrees across threads.
///
/// It is its own header because that intersection is the only part of the
/// file API needing an execution provider. io.hpp on its own compiles
/// against the standard library alone, and parallel.hpp on its own knows
/// nothing about files.
///
/// Every full window is a power-of-2, subtree-aligned run of chunks, so
/// its chaining values drop into the hasher through the same
/// push_subtree_cv seam the in-memory parallel engine uses. The final
/// window (which may be partial and contains the message end) goes
/// through hasher::update to keep ROOT finalization correct.
///
/// Same shape as io.hpp: `update_file()` is the primitive, `hash_file()` the
/// one-shot convenience, each with a throwing and a std::error_code form.
/// `update_stream()` feeds a hasher from a pipe, a socket or standard
/// input through the same pipeline.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/file_pipeline.hpp>
#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/detail/pipe_driver.hpp>
#include <blake3pp/detail/write_pipeline.hpp>
#include <blake3pp/io.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/trace.hpp>

namespace blake3pp {

namespace detail {
#if defined(BLAKE3PP_TEST_PIPELINE_STOP)
// The stop the blocking entry points hand the pipeline.
//
// These entry points block, so they have no receiver whose stop token
// they could honour; async_io.hpp is where a caller brings its own. The
// test executable compiles this header with a seam that installs one
// anyway, so the stop path can be exercised without the async API. The
// shipped library has neither the pointer nor the branch: the other
// definition returns a token with no state, which the scope tests once
// per run.
inline ex::inplace_stop_source*& installed_stop_source() noexcept {
  static ex::inplace_stop_source* src = nullptr;
  return src;
}

[[nodiscard]] inline ex::inplace_stop_token blocking_stop_token() noexcept {
  ex::inplace_stop_source* const src = installed_stop_source();
  return src != nullptr ? src->get_token() : ex::inplace_stop_token{};
}
#else
[[nodiscard]] inline ex::inplace_stop_token blocking_stop_token() noexcept {
  return {};
}
#endif
}  // namespace detail

/// Generates a file of exactly `size` bytes, filling each window over a
/// scheduler and writing it through the platform's fastest path.
///
/// `fill` is called as `fill(std::span<std::byte> out, std::uint64_t
/// offset)` on the scheduler's agents, once per window, and must fill
/// every byte of `out`. Windows are filled and written concurrently and
/// in no particular order, so a generator must depend on its offset and
/// nothing else.
///
/// `fill` may throw. The exception stops the run and comes back out of
/// this call, and the file is left as far as it got, as after an I/O
/// failure. The `std::error_code` overload reports it as
/// not_enough_memory, like a provider's exception, so a caller whose
/// generator can fail takes the throwing form.
///
/// On success the file is exactly `size` bytes. On failure the file is
/// left as far as it got: what lies past the last landed window is
/// unspecified, and it is NOT truncated, because a length invented for
/// bytes that were never produced would hide the failure.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param path   The file to create or truncate.
/// @param size   How many bytes to generate.
/// @param fill   The generator, called per window on the pool.
/// @param sched  Where the fills run.
/// @param opts   The geometry.
/// @return The bytes written (size) and the rung the file was written
///         through, as write_report says.
/// @throws std::system_error on I/O failure, and whatever the execution
///         provider raises.
template <class Scheduler, class Fill>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
write_report write_file(const std::filesystem::path& path, std::uint64_t size,
                        Fill&& fill, Scheduler&& sched,
                        const file_write_options& opts = {}) {
  // The machine's width, which is what the automatic depth is one
  // window per, and what the scope is told the generator's parallelism
  // is. A caller whose pool is narrower than the machine states its
  // own depth (file_write_options::queue_depth).
  const unsigned threads = std::thread::hardware_concurrency();
  const unsigned depth = detail::resolve_write_queue_depth(
      opts.queue_depth, opts.window_bytes, threads);
  detail::io_driver drv({.async = opts.async,
                         .offload_submit = opts.offload_submit,
                         .ioring = opts.ioring,
                         .registered_buffers = opts.registered_buffers},
                        depth);
  detail::io_driver::file file(
      drv, path,
      detail::io_driver::open_options{.write = true,
                                      .direct_io = opts.direct_io,
                                      .preallocate_bytes = size,
                                      .waive_zero_fill = opts.waive_zero_fill});
  detail::run_write_pipeline(drv, file, std::forward<Scheduler>(sched),
                             std::forward<Fill>(fill), size,
                             {.window_bytes = opts.window_bytes,
                              .queue_depth = depth,
                              .trace = opts.trace,
                              .generator_threads = threads});
  return {.bytes = size, .backend = std::string(file.name())};
}

/// write_file() over a scheduler, reporting failure through ec instead of
/// throwing.
///
/// An I/O failure arrives as its own error code. An allocation failure,
/// an exception from the generator and whatever the execution provider
/// raises all read as not_enough_memory, as in every ec overload here.
/// @param path   The file to create or truncate.
/// @param size   How many bytes to generate.
/// @param fill   The generator, called per window on the pool.
/// @param sched  Where the fills run.
/// @param ec     Cleared on success; the failure otherwise.
/// @param opts   The geometry.
/// @return The report, or a default one when ec is set.
template <class Scheduler, class Fill>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
write_report write_file(const std::filesystem::path& path, std::uint64_t size,
                        Fill&& fill, Scheduler&& sched, std::error_code& ec,
                        const file_write_options& opts = {}) noexcept {
  return detail::with_error_code(ec, [&] {
    return write_file(path, size, std::forward<Fill>(fill),
                      std::forward<Scheduler>(sched), opts);
  });
}

/// Streams a file into a hasher with every complete window fanned out
/// over a scheduler; the final window is absorbed by h itself.
///
/// Same contract as io.hpp's update_file(): h keeps its mode, stays open
/// for more input and for every finalize form, and files hash in
/// sequence. A hasher already part-way through a message is brought back
/// onto a window boundary by one short first window, whose head goes
/// through `h.update()` and whose remainder fans out like any other; every
/// window after it is offloaded whole. The digest is the same wherever the
/// hasher started, and only how much of the first window is parallel
/// varies.
/// @tparam Budget     What a window's part table may take; see
///                    stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param h      The hasher to stream into.
/// @param path   The file to read.
/// @param sched  Where the subtree reductions run.
/// @param opts   The pipeline knobs.
/// @throws std::system_error on I/O failure, and whatever the execution
///         provider raises. A run that was stopped throws
///         std::errc::operation_canceled; h is then left wherever the
///         stop found it, as it is after a failed read, and is still
///         usable for further input.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
void update_file(hasher& h, const std::filesystem::path& path,
                 Scheduler&& sched, const file_io_options& opts = {}) {
  // The plan is made once here and is the only geometry the driver, the
  // file and the scope see. Its direct_io is the caller's wish only where
  // the grid is aligned: an unaligned head misaligns every window after
  // it and would send each read to the synchronous rung.
  //
  // The size is read before the open because the plan decides the open
  // mode and sizes the window objects to the file; a size the filesystem
  // cannot give leaves the count to the budget, and the open reports why.
  std::error_code size_ec;
  const std::uintmax_t size = std::filesystem::file_size(path, size_ec);
  // The plan's window objects are the arena the pool draws work from.
  // Their number follows the machine's thread count
  // (read_plan_budget_bytes), while the engine's queue is sized for the
  // reads that can actually be outstanding, which is the depth.
  const detail::pipeline_plan plan = detail::plan_pipeline(
      h.count(), opts.window_bytes, opts.queue_depth, opts.direct_io,
      size_ec ? 0 : static_cast<std::uint64_t>(size),
      detail::read_plan_budget_bytes(std::thread::hardware_concurrency(),
                                     opts.window_bytes));
  detail::io_driver drv({.async = opts.async,
                         .offload_submit = opts.offload_submit,
                         .ioring = opts.ioring,
                         .registered_buffers = opts.registered_buffers},
                        plan.reads_in_flight);
  detail::io_driver::file file(drv, path, plan.direct_io);
  detail::run_window_pipeline<Budget>(
      h, drv, file, std::forward<Scheduler>(sched), plan,
      {.window_bytes = opts.window_bytes,
       .queue_depth = opts.queue_depth,
       .trace = opts.trace,
       .stop_token = detail::blocking_stop_token()});
}

/// Streams a file into a hasher over a scheduler, reporting failure
/// through ec instead of throwing.
/// @tparam Budget     What a window's part table may take; see
///                    stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param h      The hasher to stream into.
/// @param path   The file to read.
/// @param sched  Where the subtree reductions run.
/// @param ec     Cleared on success; the error otherwise.
/// @param opts   The pipeline knobs.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
void update_file(hasher& h, const std::filesystem::path& path,
                 Scheduler&& sched, std::error_code& ec,
                 const file_io_options& opts = {}) noexcept {
  detail::with_error_code(ec, [&] {
    update_file<Budget>(h, path, std::forward<Scheduler>(sched), opts);
  });
}

/// One-shot digest of a file over the full pipeline: a hasher shaped by
/// opts (SIMD variant, and plain, keyed or key-derivation mode), the file
/// streamed through it multi-core, finalized.
/// @tparam Budget     What a window's part table may take; see
///                    stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param path   The file to hash.
/// @param sched  Where the subtree reductions run.
/// @param opts   The hasher's variant and mode, and the pipeline knobs.
/// @throws std::system_error on I/O failure, and whatever the execution
///         provider raises.
///
/// @code
/// auto d = blake3pp::hash_file(path, pool.get_scheduler(),
///                              {.window_bytes = 16 * 1024 * 1024,
///                               .queue_depth = 8});
/// @endcode
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               Scheduler&& sched,
                               const hash_file_options& opts = {}) {
  hasher h = detail::make_hasher(opts, detail::resolve(opts.a));
  update_file<Budget>(h, path, std::forward<Scheduler>(sched), opts);
  return h.finalize();
}

/// One-shot digest of a file over the full pipeline, reporting failure
/// through ec instead of throwing.
/// @tparam Budget     What a window's part table may take; see
///                    stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param path   The file to hash.
/// @param sched  Where the subtree reductions run.
/// @param ec     Cleared on success; the error otherwise.
/// @param opts   The hasher's variant and mode, and the pipeline knobs.
/// @return The digest, or an all-zero digest when ec is set.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               Scheduler&& sched, std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept {
  return detail::with_error_code(ec, [&] {
    return hash_file<Budget>(path, std::forward<Scheduler>(sched), opts);
  });
}

namespace detail {

// One part of the window grid, in bytes: the alignment every window a
// stream ends early is rounded down to, which is what keeps such a
// window a run of whole, aligned parts.
template <stack_budget Budget>
[[nodiscard]] constexpr std::size_t stream_flush_quantum(
    std::size_t window_bytes) noexcept {
  return part_chunks<Budget>(window_bytes / chunk_size) * chunk_size;
}

// The geometry a stream runs on.
//
// A stream has one position, so exactly one window is being filled at
// any moment: the depth the device sees is one, and that is the depth
// the diagnosis reads the stream's own share against. The window count
// is what lets the pool and the reducer run ahead of the fill, not what
// a device's queue is given.
//
// Separate from update_stream() so a test can drive a scripted reader
// through the same geometry the shipped path uses.
[[nodiscard]] inline pipeline_plan plan_stream(std::uint64_t count,
                                               std::size_t window_bytes,
                                               unsigned windows) noexcept {
  const std::size_t window = rounded_window_bytes(window_bytes);
  return {.window_bytes = window,
          .reads_in_flight = 1,
          .windows = windows != 0 ? std::clamp(windows, 2u, max_queue_depth)
                                  : default_stream_windows,
          .head_bytes = first_window_bytes(count, window),
          .direct_io = false,
          .ends_message = true,
          .streaming = true};
}

}  // namespace detail

/// A stream handle: a file descriptor where the platform has those, a
/// HANDLE on Windows. `blake3pp::standard_input()` is this process's own.
using stream_handle = detail::native_stream;

/// This process's standard input as a stream handle.
[[nodiscard]] inline stream_handle standard_input() noexcept {
  return detail::standard_input();
}

/// What update_stream() takes.
struct stream_io_options {
  /// Bytes per window, 8 MiB by default; rounded down to a power-of-2
  /// multiple of chunk_size, minimum 64 KiB. A producer that keeps up
  /// fills every one of them.
  std::size_t window_bytes = detail::default_window_bytes;
  /// Window objects the run allocates, which is how many may be in
  /// flight -- one being filled, the rest on the pool or waiting to
  /// reduce. Zero takes the default.
  ///
  /// A stream has one position and fills one window at a time, so there
  /// is no device queue here for a depth to buy. blake3ppsum's --qd
  /// arrives here for that reason: the window count is the only thing a
  /// stream has for it to set.
  unsigned windows = 0;
  /// Ask the OS for this much stream buffer (Linux: F_SETPIPE_SZ). Zero
  /// leaves it alone. A larger buffer is what keeps an 8 MiB window from
  /// being a hundred hand-offs through a 64 KiB pipe; the request is
  /// clamped by the system's own ceiling and the figure in force comes
  /// back in stream_report.
  std::size_t stream_buffer_bytes = 1u << 20;
  /// Where to record per-window timing, or null for none; the same
  /// buffer file_io_options::trace takes.
  trace_buffer* trace = nullptr;
};

/// What one update_stream() run did.
struct stream_report {
  /// Bytes the stream delivered.
  std::uint64_t bytes = 0;
  /// Windows the run hashed.
  std::uint64_t windows = 0;
  /// Of those, the ones that ended early because the producer ran dry
  /// before filling them. Zero is a producer that kept up with the grid;
  /// anything else says the run was waiting on the producer.
  std::uint64_t short_flushes = 0;
  /// The window the run settled on, and how many it allocated: the
  /// figures diagnose_read() is read against.
  std::size_t window_bytes = 0;
  unsigned windows_planned = 0;
  /// The stream buffer in force, or zero where the platform has no such
  /// knob.
  std::size_t stream_buffer_bytes = 0;
};

/// Streams an unseekable source -- a pipe, a socket, standard input --
/// into a hasher with every window fanned out over a scheduler.
///
/// Same contract as update_file() in every respect the hasher can see: h
/// keeps its mode, stays open for more input and for every finalize
/// form, and the digest is the one the same bytes produce through any
/// other path. What differs is the geometry, because a stream has none
/// to plan from: its length is known only when it ends, and a window is
/// however much of it arrived.
///
/// The reader fills one window on this thread while the pool hashes the
/// ones before it, which is the overlap a push API cannot have. A
/// producer that keeps up fills every window and the run is bounded by
/// the hash or by the stream itself; a producer that does not has its
/// windows ended when the stream runs dry with half of one in hand, so
/// the pool is fed rather
/// than stalled and the producer is never left blocked on a full pipe.
/// Either way `--diag`'s device side is the stream: there is no other
/// device in the picture.
///
/// @pre The hasher sits on a window boundary: count() is a multiple of
///      the rounded window size, which every fresh hasher is. A hasher
///      part-way through one cannot be brought onto the grid from a
///      stream, because the window that would do it is the one window a
///      stream cannot promise to fill.
/// @tparam Budget     What a window's part table may take; see
///                    stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param h       The hasher to stream into.
/// @param handle  The stream to read; standard_input() for stdin.
/// @param sched   Where the subtree reductions run.
/// @param opts    The geometry.
/// @return What the run did; see stream_report.
/// @throws std::system_error on a read failure, std::logic_error for a
///         hasher off the window grid, and whatever the execution provider
///         raises. A producer that dies cannot be told from one that
///         closed its end: both read as the end of the stream.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
stream_report update_stream(hasher& h, stream_handle handle, Scheduler&& sched,
                            const stream_io_options& opts = {}) {
  const detail::pipeline_plan plan =
      detail::plan_stream(h.count(), opts.window_bytes, opts.windows);
  const std::size_t window = plan.window_bytes;
  detail::pipe_driver drv(
      detail::stream_reader{handle},
      {.flush_quantum = detail::stream_flush_quantum<Budget>(window),
       .widen_to = opts.stream_buffer_bytes});
  detail::pipe_driver::file file(drv);
  detail::run_window_pipeline<Budget>(
      h, drv, file, std::forward<Scheduler>(sched), plan,
      {.window_bytes = window,
       .trace = opts.trace,
       .stop_token = detail::blocking_stop_token()});
  return {.bytes = drv.bytes_read(),
          .windows = drv.windows_filled(),
          .short_flushes = drv.short_flushes(),
          .window_bytes = window,
          .windows_planned = plan.windows,
          .stream_buffer_bytes = drv.widened_to()};
}

/// update_stream() reporting failure through ec instead of throwing.
///
/// A read failure arrives as its own error code. Anything else reads as
/// not_enough_memory, as in every ec overload here: an allocation failure,
/// whatever the execution provider raises, and a hasher off the window
/// grid. A caller that needs to tell them apart takes the throwing form.
/// @param h       The hasher to stream into.
/// @param handle  The stream to read.
/// @param sched   Where the subtree reductions run.
/// @param ec      Cleared on success; the error otherwise.
/// @param opts    The geometry.
/// @return What the run did, or a default report when ec is set.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
stream_report update_stream(hasher& h, stream_handle handle, Scheduler&& sched,
                            std::error_code& ec,
                            const stream_io_options& opts = {}) noexcept {
  return detail::with_error_code(ec, [&] {
    return update_stream<Budget>(h, handle, std::forward<Scheduler>(sched),
                                 opts);
  });
}

/// update_file() over a scheduler for a foreign path type (see io.hpp).
/// @param h      The hasher to stream into.
/// @param path   The file to read.
/// @param sched  Where the subtree reductions run.
/// @param opts   The pipeline knobs.
template <stack_budget Budget = default_stack_budget, detail::foreign_path P,
          class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
void update_file(hasher& h, const P& path, Scheduler&& sched,
                 const file_io_options& opts = {}) {
  update_file<Budget>(h, std::filesystem::path(path.native()),
                      std::forward<Scheduler>(sched), opts);
}

/// update_file() over a scheduler for a foreign path type, reporting
/// through ec.
/// @param h      The hasher to stream into.
/// @param path   The file to read.
/// @param sched  Where the subtree reductions run.
/// @param ec     Cleared on success; the error otherwise.
/// @param opts   The pipeline knobs.
template <stack_budget Budget = default_stack_budget, detail::foreign_path P,
          class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
void update_file(hasher& h, const P& path, Scheduler&& sched,
                 std::error_code& ec, const file_io_options& opts = {}) noexcept {
  // The path conversion allocates, so it belongs inside the guard too.
  detail::with_error_code(ec, [&] {
    update_file<Budget>(h, std::filesystem::path(path.native()),
                        std::forward<Scheduler>(sched), opts);
  });
}

/// hash_file() over a scheduler for a foreign path type.
/// @param path   The file to hash.
/// @param sched  Where the subtree reductions run.
/// @param opts   The hasher's variant and mode, and the pipeline knobs.
template <stack_budget Budget = default_stack_budget, detail::foreign_path P,
          class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash_file(const P& path, Scheduler&& sched,
                               const hash_file_options& opts = {}) {
  return hash_file<Budget>(std::filesystem::path(path.native()),
                           std::forward<Scheduler>(sched), opts);
}

/// hash_file() over a scheduler for a foreign path type, reporting
/// through ec.
/// @param path   The file to hash.
/// @param sched  Where the subtree reductions run.
/// @param ec     Cleared on success; the error otherwise.
/// @param opts   The hasher's variant and mode, and the pipeline knobs.
/// @return The digest, or an all-zero digest when ec is set.
template <stack_budget Budget = default_stack_budget, detail::foreign_path P,
          class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash_file(const P& path, Scheduler&& sched,
                               std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept {
  return detail::with_error_code(ec, [&] {
    return hash_file<Budget>(std::filesystem::path(path.native()),
                             std::forward<Scheduler>(sched), opts);
  });
}

}  // namespace blake3pp
