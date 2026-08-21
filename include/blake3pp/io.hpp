#pragma once

/// @file
/// File hashing at storage speed: the windowed pipeline that joins the
/// file_reader (io_uring + O_DIRECT, IOCP + NO_BUFFERING, or GCD +
/// F_NOCACHE, whatever the platform allows) to the compute engine. While
/// window i is being hashed, windows i+1..i+depth-1 are already streaming
/// in; the queue-depth buffer ring is the backpressure mechanism, so the
/// pipeline never allocates past setup and never lets the device idle
/// waiting for compute (or vice versa).
///
/// Every full window is a power-of-2, subtree-aligned run of chunks, so its
/// chaining values drop into the hasher through the same push_subtree_cv
/// seam the in-memory parallel engine uses. The final window (which may be
/// partial and contains the message end) goes through hasher::update to
/// keep ROOT finalization correct.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include <blake3pp/core.hpp>

namespace blake3pp {

/// hash_file()'s knobs: the SIMD variant and optional key of the hasher it
/// constructs, and the pipeline's window, queue depth and direct-I/O
/// choice.
struct hash_file_options {
  /// The SIMD variant of the hasher.
  arch a = arch::auto_detect;
  /// Bytes per window; rounded down to a power-of-2 multiple of chunk_size,
  /// minimum 64 KiB.
  std::size_t window_bytes = 8 * 1024 * 1024;
  /// Windows in flight at once; clamped to [2, 32].
  unsigned queue_depth = 4;
  /// Bypass the page cache where the platform supports it.
  bool direct_io = true;
  /// Keyed (MAC/PRF) mode when set, e.g. for authenticated file manifests.
  std::optional<std::array<std::byte, 32>> key = std::nullopt;
};

namespace detail {
inline hasher make_hasher(const hash_file_options& opts,
                          const kern::kernel_ops* ops) noexcept {
  if (opts.key.has_value()) {
    return hasher::keyed(std::span<const std::byte, 32>{opts.key.value()},
                         ops);
  }
  return hasher{ops};
}
}  // namespace detail

/// One-shot digest of a file: a hasher shaped by opts (SIMD variant,
/// optional key), the file streamed through it, finalized.
/// @param path  The file to hash.
/// @param opts  The hasher's variant and key, and the pipeline knobs.
/// @throws std::system_error on I/O failure.
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               const hash_file_options& opts = {});
/// One-shot digest of a file, reporting failure through ec instead of
/// throwing.
/// @param path  The file to hash.
/// @param ec    Cleared on success; the I/O error otherwise.
/// @param opts  The hasher's variant and key, and the pipeline knobs.
/// @return The digest, or an all-zero digest when ec is set.
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept;

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

/// hash_file() for a foreign path type.
/// @param path  The file to hash.
/// @param opts  The hasher's variant and key, and the pipeline knobs.
template <detail::foreign_path P>
[[nodiscard]] digest hash_file(const P& path,
                               const hash_file_options& opts = {}) {
  return hash_file(std::filesystem::path(path.native()), opts);
}

/// hash_file() for a foreign path type, reporting through ec.
/// @param path  The file to hash.
/// @param ec    Cleared on success; the error otherwise.
/// @param opts  The hasher's variant and key, and the pipeline knobs.
/// @return The digest, or an all-zero digest when ec is set.
template <detail::foreign_path P>
[[nodiscard]] digest hash_file(const P& path, std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept {
  return hash_file(std::filesystem::path(path.native()), ec, opts);
}

}  // namespace blake3pp
