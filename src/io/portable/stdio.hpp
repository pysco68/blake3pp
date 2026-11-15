#pragma once

// The portable floor: buffered, synchronous stdio on one lane, for a
// platform that matches none of the native backends in
// native.hpp. Internal to src/io/, never installed.

#include <cassert>
#include <cerrno>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <system_error>

#include "io/fd.hpp"
#include "io/io_context.hpp"
#include "io/options.hpp"
#include "io/sync_context.hpp"

namespace blake3pp::detail::io_impl {

// std::fseek/std::ftell take and return `long`, which is 32 bits on
// Windows (LLP64) and on wasm32. Truncating a file offset there does not
// fail: it seeks somewhere else and returns the wrong bytes, which for a
// hash function means a silently wrong digest. These wrappers use the
// 64-bit call each platform offers.
inline int seek64(std::FILE* f, std::uint64_t off) noexcept {
#if defined(_MSC_VER)
  return _fseeki64(f, static_cast<__int64>(off), SEEK_SET);
#elif defined(_WIN32)
  return fseeko64(f, static_cast<off64_t>(off), SEEK_SET);
#else
  // POSIX, including macOS and Emscripten (musl's off_t is always 64-bit).
  return ::fseeko(f, static_cast<::off_t>(off), SEEK_SET);
#endif
}

inline std::int64_t tell64(std::FILE* f) noexcept {
#if defined(_MSC_VER)
  return _ftelli64(f);
#elif defined(_WIN32)
  return ftello64(f);
#else
  return ::ftello(f);
#endif
}

// The source half of the synchronous contract; see pread_source.
class stdio_source {
 public:
  // See pread_source: the engine this source is.
  static constexpr io_engine rung_engine = io_engine::stdio;
  // Seek then read is two calls over one cursor, so transfers here may
  // not overlap: this source gets a single lane.
  static constexpr bool positional = false;

  stdio_source(const std::filesystem::path& path,
               const file_open_options& how) {
    // The Microsoft CRT deprecates std::fopen in favour of fopen_s, which
    // opens without sharing; std::fopen is the call this source means.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    // The path's native form: on Windows the narrow one is the ANSI code
    // page's, which cannot name every file.
#if defined(_WIN32)
    stream_.reset(::_wfopen(path.c_str(), how.write ? L"wb" : L"rb"));
#else
    stream_.reset(std::fopen(path.c_str(), how.write ? "wb" : "rb"));
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (!stream_) {
      throw_errno("fopen");
    }
    if (how.write) {
      return;  // created empty; there is no size to learn
    }
    if (std::fseek(stream_.get(), 0, SEEK_END) != 0) {
      throw_errno("fseek(end)");
    }
    const std::int64_t end = tell64(stream_.get());
    if (end < 0) {
      throw_errno("ftell");
    }
    size_ = static_cast<std::uint64_t>(end);
  }
  stdio_source(const stdio_source&) = delete;
  stdio_source& operator=(const stdio_source&) = delete;

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::string_view name() const noexcept { return "stdio"; }

  void read_at(std::uint64_t off, std::span<std::byte> buf) {
    if (seek64(stream_.get(), off) != 0) {
      throw_errno("fseek");
    }
    if (std::fread(buf.data(), 1, buf.size(), stream_.get()) != buf.size()) {
      throw std::system_error(EIO, std::generic_category(), "fread");
    }
  }

  void write_at(std::uint64_t off, std::span<const std::byte> buf) {
    if (seek64(stream_.get(), off) != 0) {
      throw_errno("fseek");
    }
    if (std::fwrite(buf.data(), 1, buf.size(), stream_.get()) != buf.size()) {
      throw std::system_error(EIO, std::generic_category(), "fwrite");
    }
  }

  // stdio has no truncate; flushing is the most it can promise, and the
  // writer above it only ever asks for a size it has actually written.
  void set_size(std::uint64_t) {
    if (std::fflush(stream_.get()) != 0) {
      throw_errno("fflush");
    }
  }

 private:
  struct closer {
    void operator()(std::FILE* f) const noexcept { std::fclose(f); }
  };
  std::unique_ptr<std::FILE, closer> stream_;
  std::uint64_t size_ = 0;
};

using stdio_context = sync_context<stdio_source>;

// Definition-site conformance check (see linux/io_uring.hpp).
static_assert(io_context<stdio_context>);

}  // namespace blake3pp::detail::io_impl
