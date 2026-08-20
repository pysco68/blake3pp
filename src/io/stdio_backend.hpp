#pragma once

// The portable floor: buffered, fully synchronous stdio, for platforms
// with none of the native backends (e.g. wasm). Internal to src/io/,
// never installed.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <system_error>

#include "io/backend.hpp"

namespace blake3pp::detail::io_impl {

class stdio_reader {
 public:
  stdio_reader(const std::filesystem::path& path, const file_reader_options&,
               unsigned) {
    stream_ = std::fopen(path.string().c_str(), "rb");
    if (stream_ == nullptr) {
      throw_errno("fopen");
    }
    std::fseek(stream_, 0, SEEK_END);
    size_ = static_cast<std::uint64_t>(std::ftell(stream_));
  }
  ~stdio_reader() {
    if (stream_ != nullptr) {
      std::fclose(stream_);
    }
  }
  stdio_reader(const stdio_reader&) = delete;
  stdio_reader& operator=(const stdio_reader&) = delete;

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] const char* name() const noexcept { return "stdio"; }
  [[nodiscard]] bool wants_async(std::uint64_t, std::size_t) const noexcept {
    return false;
  }

  void start(unsigned, std::uint64_t, std::span<std::byte>) {
    assert(false && "stdio backend has no async path");
  }
  void wait(unsigned) { assert(false && "stdio backend has no async path"); }

  void read_sync(std::uint64_t off, std::span<std::byte> buf) {
    if (std::fseek(stream_, static_cast<long>(off), SEEK_SET) != 0) {
      throw_errno("fseek");
    }
    if (std::fread(buf.data(), 1, buf.size(), stream_) != buf.size()) {
      throw std::system_error(EIO, std::generic_category(), "fread");
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

  [[nodiscard]] const char* name() const noexcept { return "stdio"; }
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
