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

#include <cstdint>
#include <filesystem>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/file_pipeline.hpp>
#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/detail/write_pipeline.hpp>
#include <blake3pp/io.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/trace.hpp>

namespace blake3pp {

namespace detail {
#if defined(BLAKE3PP_TEST_PIPELINE_STOP)
// The stop the blocking entry points hand the pipeline.
//
// Phase 7 gives callers a stop of their own; until then the stop path
// has no way in from the public API, so the test executable compiles
// this header with a seam that lets one be installed. The shipped
// library has neither the pointer nor the branch: the other definition
// returns a token with no state, which the scope tests once per run.
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

/// Knobs for write_file(): the geometry of a generated file.
///
/// The defaults are blake3ppgen's measured shape, not the reader's:
/// 64 MiB windows, two in flight. On four PCIe 5 drives in RAID 0 with
/// 32 threads that measured 24.2 GiB/s, against 20.7 at 128 MiB x 4 and
/// 14.8 at 256 MiB x 8 -- past a few hundred MiB in flight a striped
/// volume slows down.
struct file_write_options {
  /// Bytes per window; rounded to a power of two in the reader's range.
  std::size_t window_bytes = detail::default_write_window_bytes;
  /// Windows in flight, clamped to [2, 32].
  unsigned queue_depth = detail::default_write_queue_depth;
  /// Bypass the page cache where the platform can.
  bool direct_io = true;
  /// io_uring: issue on io-wq rather than inline in the submit call.
  bool offload_submit = true;
};

/// Generates a file of exactly `size` bytes, filling each window over a
/// scheduler and writing it through the platform's fastest path.
///
/// `fill` is called as `fill(std::span<std::byte> out, std::uint64_t
/// offset)` on the scheduler's agents, once per window, and must fill
/// every byte of `out`; windows are filled and written concurrently, in
/// no particular order, so a generator must depend on its offset and
/// nothing else.
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
/// @throws std::system_error on I/O failure, and whatever the execution
///         provider raises.
template <class Scheduler, class Fill>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
void write_file(const std::filesystem::path& path, std::uint64_t size,
                Fill&& fill, Scheduler&& sched,
                const file_write_options& opts = {}) {
  detail::io_driver drv({/*async=*/true, opts.offload_submit},
                        detail::clamp_queue_depth(opts.queue_depth));
  detail::io_driver::file file(
      drv, path,
      detail::io_driver::open_options{.write = true,
                                      .direct_io = opts.direct_io,
                                      .preallocate_bytes = size});
  detail::run_write_pipeline(drv, file, std::forward<Scheduler>(sched),
                             std::forward<Fill>(fill), size,
                             {.window_bytes = opts.window_bytes,
                              .queue_depth = opts.queue_depth});
}

/// Streams a file into a hasher with every complete window fanned out
/// over a scheduler; the final window is absorbed by h itself.
///
/// Same contract as io.hpp's update_file(): h keeps its mode, stays open
/// for more input and for every finalize form, and files hash in
/// sequence. A window is offloaded as a subtree only when h sits on a
/// boundary aligned to it: always the case for a fresh hasher, and after
/// files whose sizes are multiples of the window. Elsewhere the window
/// goes through `h.update()` instead, so the digest is the same either way
/// and only the parallelism varies.
/// @tparam Budget     The stack a window's part table may take; see
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
  // Every file goes through the pipeline. A hasher already part-way
  // through a message is brought back onto a window boundary by one
  // short first window rather than by hashing the whole file
  // sequentially, so there is no second path left to choose. The plan
  // is made once here and is the only geometry the driver, the file and
  // the scope see; its direct_io is the caller's wish only where the
  // grid is aligned, since an unaligned head misaligns every window
  // after it and would degrade each read to the synchronous path.
  // The size is read before the open because the plan decides the open
  // mode and sizes the window objects to the file; a size the filesystem
  // cannot give leaves the count to the budget, and the open reports why.
  std::error_code size_ec;
  const std::uintmax_t size = std::filesystem::file_size(path, size_ec);
  const detail::pipeline_plan plan = detail::plan_pipeline(
      h.count(), opts.window_bytes, opts.queue_depth, opts.direct_io,
      size_ec ? 0 : static_cast<std::uint64_t>(size));
  detail::io_driver drv({/*async=*/true, opts.offload_submit},
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
/// @tparam Budget     The stack a window's part table may take; see
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
/// opts (SIMD variant, optional key), the file streamed through it
/// multi-core, finalized.
/// @tparam Budget     The stack a window's part table may take; see
///                    stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param path   The file to hash.
/// @param sched  Where the subtree reductions run.
/// @param opts   The hasher's variant and key, and the pipeline knobs.
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
/// @tparam Budget     The stack a window's part table may take; see
///                    stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param path   The file to hash.
/// @param sched  Where the subtree reductions run.
/// @param ec     Cleared on success; the error otherwise.
/// @param opts   The hasher's variant and key, and the pipeline knobs.
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
/// @param opts   The hasher's variant and key, and the pipeline knobs.
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
/// @param opts   The hasher's variant and key, and the pipeline knobs.
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
