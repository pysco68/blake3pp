#pragma once

// The portable floor: buffered, fully synchronous stdio, for platforms
// with none of the native backends (e.g. wasm). Internal to src/io/,
// never installed.

#include <cassert>
#include <cerrno>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <system_error>

#include "io/backend.hpp"

namespace blake3pp::detail::io_impl {

// std::fseek/std::ftell take and return `long`, which is 32 bits on
// Windows (LLP64) and on wasm32, precisely the platforms this fallback
// exists to serve. Truncating a file offset there does not fail: it seeks
// somewhere else and returns the wrong bytes, which for a hash function
// means a silently wrong digest. These wrappers keep the full 64-bit range
// where the platform offers it, and where it does not they REFUSE the
// offset rather than truncate it.
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

class stdio_reader {
 public:
  stdio_reader(const std::filesystem::path& path, const file_reader_options&,
               unsigned) {
    stream_ = std::fopen(path.string().c_str(), "rb");
    if (stream_ == nullptr) {
      throw_errno("fopen");
    }
    // Unchecked, these silently produce a nonsense size: a failed seek
    // leaves ftell returning -1, which as an unsigned size is ~18 EiB, and
    // the engine then computes a huge window count that fails obscurely.
    if (std::fseek(stream_, 0, SEEK_END) != 0) {
      throw_errno("fseek(end)");
    }
    const std::int64_t end = tell64(stream_);
    if (end < 0) {
      throw_errno("ftell");
    }
    size_ = static_cast<std::uint64_t>(end);
  }
  ~stdio_reader() {
    if (stream_ != nullptr) {
      std::fclose(stream_);
    }
  }
  stdio_reader(const stdio_reader&) = delete;
  stdio_reader& operator=(const stdio_reader&) = delete;

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::string_view name() const noexcept { return "stdio"; }
  [[nodiscard]] bool wants_async(std::uint64_t, std::size_t) const noexcept {
    return false;
  }

  void start(unsigned, std::uint64_t, std::span<std::byte>) {
    assert(false && "stdio backend has no async path");
  }
  void wait(unsigned) { assert(false && "stdio backend has no async path"); }

  void read_sync(std::uint64_t off, std::span<std::byte> buf) {
    if (seek64(stream_, off) != 0) {
      throw_errno("fseek");
    }
    if (std::fread(buf.data(), 1, buf.size(), stream_) != buf.size()) {
      // Distinguish a real read error from a short read at EOF; the engine
      // never asks for more than the file holds, so EOF here means the file
      // was truncated underneath us.
      throw std::system_error(std::ferror(stream_) != 0 ? errno : EIO,
                              std::generic_category(), "fread");
    }
  }

 private:
  std::FILE* stream_ = nullptr;
  std::uint64_t size_ = 0;
};

class stdio_writer {
 public:
  stdio_writer(const std::filesystem::path& path, const file_writer_options&,
               unsigned) {
    stream_ = std::fopen(path.string().c_str(), "wb");
    if (stream_ == nullptr) {
      throw_errno("fopen");
    }
  }
  ~stdio_writer() {
    if (stream_ != nullptr) {
      std::fclose(stream_);
    }
  }
  stdio_writer(const stdio_writer&) = delete;
  stdio_writer& operator=(const stdio_writer&) = delete;

  [[nodiscard]] std::string_view name() const noexcept { return "stdio"; }
  [[nodiscard]] bool wants_async(std::size_t) const noexcept { return false; }

  void start_write(unsigned, std::uint64_t, std::span<const std::byte>) {
    assert(false && "stdio backend has no async path");
  }
  void wait_slot(unsigned) {}  // nothing is ever in flight

  // Writes are strictly sequential (the engine's offset only grows), so
  // the stream position is already `off` and no seek is needed.
  void write_sync(std::uint64_t, std::span<const std::byte> buf) {
    if (std::fwrite(buf.data(), 1, buf.size(), stream_) != buf.size()) {
      throw std::system_error(EIO, std::generic_category(), "fwrite");
    }
  }

  void finish(std::uint64_t) {
    if (std::fflush(stream_) != 0) {
      throw_errno("fflush");
    }
  }

 private:
  std::FILE* stream_ = nullptr;
};

// Definition-site conformance check (see uring_backend.hpp).
static_assert(reader_backend<stdio_reader>);
static_assert(writer_backend<stdio_writer>);

}  // namespace blake3pp::detail::io_impl
