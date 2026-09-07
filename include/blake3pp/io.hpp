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
/// The primitive is update_file(): hasher::update() with a file as the
/// source. It streams into a caller-owned hasher and returns, so the
/// hasher's mode (plain, keyed, derive_key) and every finalize form
/// (digest, extended output, the seekable reader) compose with file input
/// without this header knowing about them. hash_file() is the one-shot
/// convenience on top: construct, update_file, finalize.
///
/// This header is free of any execution-provider dependency: core.hpp,
/// dispatch.hpp and io.hpp compile against the standard library alone.
/// Reads still overlap hashing here (the reader is asynchronous); what is
/// sequential is the compute. The scheduler-taking overloads, which fan
/// each window out over cores, live in parallel_io.hpp, the one public
/// header that needs stdexec/beman/std::execution.
///
/// std::filesystem::path is the path currency throughout (string literals
/// and std::string convert implicitly). Each entry point follows the
/// standard library's dual-overload idiom: the plain form throws
/// std::system_error on I/O failure, the std::error_code& form reports
/// through ec instead.

#include <array>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include <blake3pp/core.hpp>

namespace blake3pp {

/// The pipeline's knobs: what update_file() takes. The hasher it streams
/// into already carries the SIMD variant and the mode.
struct file_io_options {
  /// Bytes per window; rounded down to a power-of-2 multiple of chunk_size,
  /// minimum 64 KiB.
  std::size_t window_bytes = 8 * 1024 * 1024;
  /// Windows in flight at once; clamped to [2, 32].
  unsigned queue_depth = 4;
  /// Bypass the page cache where the platform supports it; degrades to
  /// buffered reads where it does not.
  bool direct_io = true;
};

/// hash_file()'s knobs: the pipeline's, plus what shapes the hasher it
/// constructs internally.
///
/// Both structs spell the shared fields the same way so designated
/// initializers read alike, and this one converts to file_io_options so a
/// single options object can drive both entry points (a tool's
/// --window/--qd/--no-direct flags land in one place).
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
  /// derive_key and extended output have no shortcut here: build the
  /// hasher yourself and use update_file().
  std::optional<std::array<std::byte, key_size>> key = std::nullopt;

  /// The pipeline knobs alone, so one options object drives update_file()
  /// too.
  constexpr operator file_io_options() const noexcept {
    return {window_bytes, queue_depth, direct_io};
  }
};

namespace detail {

inline hasher make_hasher(const hash_file_options& opts,
                          const kern::kernel_ops* ops) noexcept {
  if (opts.key.has_value()) {
    return hasher::keyed(std::span<const std::byte, key_size>{opts.key.value()},
                         ops);
  }
  return hasher{ops};
}

// The body of every std::error_code overload: ec is cleared, then set
// from the std::system_error the throwing form raises. Anything else
// escaping the pipeline is an allocation failure at setup (the buffer
// ring, the queue), reported as not_enough_memory. Value-returning
// callers get a default-constructed result on failure.
template <class F>
auto with_error_code(std::error_code& ec, F&& fn) noexcept
    -> std::invoke_result_t<F> {
  using result = std::invoke_result_t<F>;
  try {
    ec.clear();
    return std::forward<F>(fn)();
  } catch (const std::system_error& e) {
    ec = e.code();
  } catch (...) {
    ec = std::make_error_code(std::errc::not_enough_memory);
  }
  if constexpr (!std::is_void_v<result>) {
    return result{};
  }
}

}  // namespace detail

/// Streams a file's bytes into a hasher, as h.update() would, and returns
/// with h open for more input or any finalize form.
///
/// Files hash in sequence: after update_file(h, a); update_file(h, b);
/// h holds the hash of a's bytes followed by b's. The hasher's mode
/// (plain, keyed, derive_key) applies unchanged.
/// @param h     The hasher to stream into.
/// @param path  The file to read.
/// @param opts  The pipeline knobs.
/// @throws std::system_error on I/O failure; h is then in an unspecified
///         but valid state (reset() or discard it).
///
/// @code
/// blake3pp::hasher h = blake3pp::hasher::derive_key("fixture v3 2026-09");
/// blake3pp::update_file(h, "seed.bin");
/// auto stream = h.finalize_xof();
/// @endcode
void update_file(hasher& h, const std::filesystem::path& path,
                 const file_io_options& opts = {});
/// Streams a file's bytes into a hasher, reporting failure through ec
/// instead of throwing.
/// @param h     The hasher to stream into.
/// @param path  The file to read.
/// @param ec    Cleared on success; the I/O error otherwise (allocation
///              failure at setup reads as not_enough_memory).
/// @param opts  The pipeline knobs.
void update_file(hasher& h, const std::filesystem::path& path,
                 std::error_code& ec,
                 const file_io_options& opts = {}) noexcept;

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

/// update_file() for a path type from another filesystem library (e.g.
/// boost::filesystem::path): anything with a native() the standard path
/// accepts, bridged without transcoding.
/// @param h     The hasher to stream into.
/// @param path  The file to read.
/// @param opts  The pipeline knobs.
template <detail::foreign_path P>
void update_file(hasher& h, const P& path, const file_io_options& opts = {}) {
  update_file(h, std::filesystem::path(path.native()), opts);
}

/// update_file() for a foreign path type, reporting through ec.
/// @param h     The hasher to stream into.
/// @param path  The file to read.
/// @param ec    Cleared on success; the error otherwise.
/// @param opts  The pipeline knobs.
template <detail::foreign_path P>
void update_file(hasher& h, const P& path, std::error_code& ec,
                 const file_io_options& opts = {}) noexcept {
  // The path conversion allocates, so it belongs inside the guard too.
  detail::with_error_code(ec, [&] {
    update_file(h, std::filesystem::path(path.native()), opts);
  });
}

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
  return detail::with_error_code(ec, [&] {
    return hash_file(std::filesystem::path(path.native()), opts);
  });
}

}  // namespace blake3pp
