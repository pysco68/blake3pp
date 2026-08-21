#pragma once

/// @file
/// File hashing driven by a scheduler: the intersection of io.hpp (which
/// windows a file at storage speed) and parallel.hpp (which hashes subtrees
/// across threads). It is its own header because that intersection is the
/// only part of the file API that needs an execution provider: io.hpp on
/// its own compiles against the standard library alone, and parallel.hpp on
/// its own knows nothing about files.
///
/// Each entry point mirrors io.hpp's dual-overload idiom: the plain form
/// throws std::system_error on I/O failure, the std::error_code& form
/// reports through ec instead.

#include <filesystem>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/io.hpp>
#include <blake3pp/parallel.hpp>

namespace blake3pp {

/// One-shot digest of a file over the full pipeline: a hasher shaped by
/// opts (SIMD variant, optional key), the file streamed through it
/// multi-core, finalized.
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
template <class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               Scheduler&& sched,
                               const hash_file_options& opts = {}) {
  detail::file_reader reader(
      path, {opts.window_bytes, opts.queue_depth, opts.direct_io, true});
  const kern::kernel_ops* const ops = detail::resolve(opts.a);
  hasher h = detail::make_hasher(opts, ops);
  while (auto w = reader.next()) {
    if (!w->last) {
      detail::hash_window_parallel(ops, sched, h, w->data,
                                   w->bytes / chunk_size, w->offset /
                                       chunk_size);
    } else {
      h.update(std::span<const std::byte>{w->data, w->bytes});
    }
    reader.release(*w);
  }
  return h.finalize();
}

/// One-shot digest of a file over the full pipeline, reporting failure
/// through ec instead of throwing.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param path   The file to hash.
/// @param sched  Where the subtree reductions run.
/// @param ec     Cleared on success; the error otherwise.
/// @param opts   The hasher's variant and key, and the pipeline knobs.
/// @return The digest, or an all-zero digest when ec is set.
template <class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               Scheduler&& sched, std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept {
  try {
    ec.clear();
    return hash_file(path, std::forward<Scheduler>(sched), opts);
  } catch (const std::system_error& e) {
    ec = e.code();
  } catch (...) {
    ec = std::make_error_code(std::errc::not_enough_memory);
  }
  return digest{};
}

/// hash_file() over a scheduler for a foreign path type.
/// @param path   The file to hash.
/// @param sched  Where the subtree reductions run.
/// @param opts   The hasher's variant and key, and the pipeline knobs.
template <detail::foreign_path P, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash_file(const P& path, Scheduler&& sched,
                               const hash_file_options& opts = {}) {
  return hash_file(std::filesystem::path(path.native()),
                   std::forward<Scheduler>(sched), opts);
}

/// hash_file() over a scheduler for a foreign path type, reporting
/// through ec.
/// @param path   The file to hash.
/// @param sched  Where the subtree reductions run.
/// @param ec     Cleared on success; the error otherwise.
/// @param opts   The hasher's variant and key, and the pipeline knobs.
/// @return The digest, or an all-zero digest when ec is set.
template <detail::foreign_path P, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash_file(const P& path, Scheduler&& sched,
                               std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept {
  return hash_file(std::filesystem::path(path.native()),
                   std::forward<Scheduler>(sched), ec, opts);
}

}  // namespace blake3pp
