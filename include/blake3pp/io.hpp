#pragma once

// File hashing at storage speed: the windowed pipeline that joins the
// file_reader (io_uring + O_DIRECT where the platform allows) to the
// compute engine. While window i is being hashed, windows i+1..i+depth-1
// are already streaming in; the queue-depth buffer ring is the
// backpressure mechanism, so the pipeline never allocates past setup and
// never lets the device idle waiting for compute (or vice versa).
//
// Every full window is a power-of-2, subtree-aligned run of chunks, so its
// chaining values drop into the hasher through the same push_subtree_cv
// seam the in-memory parallel engine uses. The final window (which may be
// partial and contains the message end) goes through hasher::update to
// keep ROOT finalization correct.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <system_error>
#include <utility>

#include <blake3pp/blake3pp.hpp>
#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/parallel.hpp>

namespace blake3pp {

struct hash_file_options {
  arch a = arch::auto_detect;
  std::size_t window_bytes = 8 * 1024 * 1024;
  unsigned queue_depth = 4;
  bool direct_io = true;  // bypass the page cache where supported
};

// Sequential compute over the async reader: reads still overlap hashing.
// std::filesystem::path is the path currency throughout (string literals
// and std::string convert implicitly). Each entry point follows the
// standard library's dual-overload idiom: the plain form throws
// std::system_error on I/O failure, the std::error_code& form reports
// through ec instead (returning a zero digest on failure).
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               const hash_file_options& opts = {});
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept;

namespace detail {

// Fans one full window (num_chunks: power of two, counter-aligned) out
// over the scheduler and absorbs the part CVs in file order.
template <class Scheduler>
void hash_window_parallel(const kern::kernel_ops* ops, Scheduler& sched,
                          hasher& h, const std::byte* data,
                          std::size_t num_chunks,
                          std::uint64_t chunk_counter) {
  constexpr std::size_t max_parts = 256;
  const std::size_t part = std::bit_floor(
      std::max<std::size_t>(num_chunks / max_parts + 1, 16));
  if (part >= num_chunks) {
    // Window too small to fan out; hash it inline.
    h.update(std::span<const std::byte>{data, num_chunks * chunk_size});
    return;
  }
  const std::size_t n_parts = num_chunks / part;

  struct alignas(64) padded_cv {
    std::uint32_t words[8];
  };
  padded_cv cvs[2 * max_parts];

  auto work = ex::schedule(sched) |
              ex::bulk(ex::par, n_parts, [&](std::size_t i) noexcept {
                compress_subtree_cv(ops, data + i * part * chunk_size, part,
                                    chunk_counter + i * part, cvs[i].words);
              });
  ex::sync_wait(std::move(work));
  for (std::size_t i = 0; i < n_parts; ++i) {
    h.push_subtree_cv(cvs[i].words, part);
  }
}

}  // namespace detail

// The full pipeline: async reads + parallel subtree hashing per window.
// Throws std::system_error on I/O failure. (The scheduler concept
// constraint keeps this template from hijacking the options-only
// sequential overload above.)
template <class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               Scheduler&& sched,
                               const hash_file_options& opts = {}) {
  detail::file_reader reader(
      path, {opts.window_bytes, opts.queue_depth, opts.direct_io, true});
  const kern::kernel_ops* const ops = detail::resolve(opts.a);
  hasher h{ops};
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

// error_code form of the parallel pipeline.
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

namespace detail {

// Path types from other filesystem libraries (boost::filesystem::path is
// the motivating case): anything exposing a native() character sequence
// that std::filesystem::path accepts as a Source. Bridging through
// native() preserves the platform encoding exactly (no lossy transcoding,
// unlike .string() on Windows). Structural, so no third-party dependency
// or naming enters this library.
template <class P>
concept foreign_path =
    !std::same_as<std::remove_cvref_t<P>, std::filesystem::path> &&
    requires(const P& p) { std::filesystem::path(p.native()); };

}  // namespace detail

// Transparent forwarding for foreign path types (e.g. boost::filesystem).
template <detail::foreign_path P>
[[nodiscard]] digest hash_file(const P& path,
                               const hash_file_options& opts = {}) {
  return hash_file(std::filesystem::path(path.native()), opts);
}

template <detail::foreign_path P>
[[nodiscard]] digest hash_file(const P& path, std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept {
  return hash_file(std::filesystem::path(path.native()), ec, opts);
}

template <detail::foreign_path P, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash_file(const P& path, Scheduler&& sched,
                               const hash_file_options& opts = {}) {
  return hash_file(std::filesystem::path(path.native()),
                   std::forward<Scheduler>(sched), opts);
}

template <detail::foreign_path P, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash_file(const P& path, Scheduler&& sched,
                               std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept {
  return hash_file(std::filesystem::path(path.native()),
                   std::forward<Scheduler>(sched), ec, opts);
}

}  // namespace blake3pp
