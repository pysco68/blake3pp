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
/// every byte of `out`; windows are filled and written concurrently, in
/// no particular order, so a generator must depend on its offset and
/// nothing else. It must also be `noexcept`, and say so: the fill runs on
/// a pool thread inside a window already counted in flight, where an
/// exception has nowhere to go, so a generator that is not declared
/// noexcept does not compile. A generator that can fail should record the
/// failure and fill the window, leaving the caller to check afterwards.
/// The sequential forms in <blake3pp/io.hpp> take a throwing generator,
/// because there the fill runs on the calling thread.
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
  detail::io_driver drv({opts.async, opts.offload_submit},
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
template <class Scheduler, class Fill>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
void write_file(const std::filesystem::path& path, std::uint64_t size,
                Fill&& fill, Scheduler&& sched, std::error_code& ec,
                const file_write_options& opts = {}) noexcept {
  detail::with_error_code(ec, [&] {
    write_file(path, size, std::forward<Fill>(fill),
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
  const detail::pipeline_plan plan = detail::plan_pipeline(
      h.count(), opts.window_bytes, opts.queue_depth, opts.direct_io,
      size_ec ? 0 : static_cast<std::uint64_t>(size));
  detail::io_driver drv({opts.async, opts.offload_submit},
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
