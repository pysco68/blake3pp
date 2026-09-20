#pragma once

// The synchronous POSIX backend: plain positional pread/pwrite, with the
// O_DIRECT reopen where the platform offers it. This is the compile-time
// choice for POSIX systems with neither io_uring nor GCD; the equivalent
// RUNTIME floor on Linux lives inside uring_backend.hpp. Internal to
// src/io/, never installed.

#if defined(__unix__) || defined(__APPLE__)

#include <cassert>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <span>

#include "io/backend.hpp"
#include "io/posix_file.hpp"

namespace blake3pp::detail::io_impl {

// The source half of the synchronous contract (sync_context in
// backend.hpp supplies the deferred list, the poll loop and the wake).
class pread_source {
 public:
  pread_source(const std::filesystem::path& path, bool direct_io) {
    f_.open(path.c_str(), O_RDONLY | O_CLOEXEC);
    size_ = f_.stat_size();
    if (direct_io) {
      f_.try_odirect(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
  }

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::string_view name() const noexcept {
    return f_.direct ? "pread+direct" : "pread";
  }

  void read_at(std::uint64_t off, std::span<std::byte> buf) {
    f_.pread_all(f_.sync_fd(buf.size()), buf.data(), buf.size(), off);
  }

 private:
  posix_file f_;
  std::uint64_t size_ = 0;
};

using pread_context = sync_context<pread_source>;

class pread_writer {
 public:
  pread_writer(const std::filesystem::path& path,
               const file_writer_options& opts, unsigned) {
    f_.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    // No preallocation here: fallocate is Linux, F_PREALLOCATE is Darwin,
    // and each lives in its platform's backend. Synchronous writes don't
    // suffer the extending-write serialization anyway.
    (void)opts;
    if (opts.direct_io) {
      f_.try_odirect(path.c_str(), O_WRONLY | O_CLOEXEC);
    }
  }

  [[nodiscard]] std::string_view name() const noexcept {
    return f_.direct ? "pwrite+direct" : "pwrite";
  }
  [[nodiscard]] bool wants_async(std::size_t) const noexcept { return false; }

  void start_write(unsigned, std::uint64_t, std::span<const std::byte>) {
    assert(false && "pread backend has no async path");
  }
  void wait_slot(unsigned) {}  // nothing is ever in flight

  void write_sync(std::uint64_t off, std::span<const std::byte> buf) {
    f_.pwrite_all(f_.sync_fd(buf.size()), buf.data(), buf.size(), off);
  }

  void finish(std::uint64_t) {}  // no queue to drain, no preallocation

 private:
  posix_file f_;
};

// Definition-site conformance check (see uring_backend.hpp).
static_assert(reader_context<pread_context>);
static_assert(writer_backend<pread_writer>);

}  // namespace blake3pp::detail::io_impl

#endif  // __unix__ || __APPLE__
