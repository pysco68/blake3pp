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
#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/io.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/trace.hpp>

namespace blake3pp {

namespace detail {

// The window loop, over anything shaped like file_reader: next() hands
// windows out in file order, release() recycles them, and that is the
// whole vocabulary it needs. update_file() drives it with a real
// file_reader; the bench drives it with a source that performs no I/O,
// which is how the pipeline's hash-bound ceiling is measured with no
// device in the way. The tracing lives here, so both see the same
// records.
template <stack_budget Budget = default_stack_budget, class Reader,
          class Scheduler>
void update_from_reader(hasher& h, Reader& reader, Scheduler&& sched,
                        const file_io_options& opts) {
  const kern::kernel_ops* const ops = detail::resolve(h.selected_arch());
  const std::uint64_t base = h.count();
  const bool on_chunk_boundary = base % chunk_size == 0;
  trace_buffer* const trace = opts.trace;
  std::uint64_t index = 0;
  for (;;) {
    // The wait is timed before the record is claimed: the last next()
    // returns nothing, and that iteration gets no record.
    const std::int64_t t_wait_begin = trace ? trace->now() : 0;
    auto w = reader.next();
    if (!w) {
      break;
    }
    window_record* const rec = trace ? trace->claim_window() : nullptr;
    if (rec) {
      rec->index = index;
      rec->bytes = w->bytes;
      rec->flags = w->last ? window_record::flag_last : 0;
      rec->t_wait_begin = t_wait_begin;
      rec->t_ready = trace->now();
    }
    const std::uint64_t chunks = w->bytes / chunk_size;
    const std::uint64_t counter = base / chunk_size + w->offset / chunk_size;
    if (!w->last && on_chunk_boundary && counter % chunks == 0) {
      detail::hash_window_parallel<Budget>(ops, sched, h, w->data, chunks,
                                           counter, trace, rec);
    } else {
      h.update(std::span<const std::byte>{w->data, w->bytes});
      if (rec) {
        rec->t_joined = rec->t_absorbed = trace->now();
      }
    }
    reader.release(*w);
    if (rec) {
      rec->t_released = trace->now();
    }
    ++index;
  }
}

}  // namespace detail

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
///         provider raises.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
void update_file(hasher& h, const std::filesystem::path& path,
                 Scheduler&& sched, const file_io_options& opts = {}) {
  detail::file_reader reader(
      path, {opts.window_bytes, opts.queue_depth, opts.direct_io, true,
             opts.offload_submit});
  detail::update_from_reader<Budget>(h, reader, std::forward<Scheduler>(sched),
                                     opts);
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
