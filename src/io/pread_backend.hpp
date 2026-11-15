#pragma once

// The synchronous POSIX backend: plain positional pread/pwrite on the
// deferred pump's lanes, with the O_DIRECT reopen where the platform
// offers it. This is the compile-time choice for POSIX systems other than
// Linux, Darwin and FreeBSD; the runtime floor of those three lives inside
// their own backends. Internal to src/io/, never installed.

#if defined(__unix__) || defined(__APPLE__)

#include <cassert>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <span>

#include "io/fd.hpp"
#include "io/io_context.hpp"
#include "io/options.hpp"
#include "io/sync_context.hpp"
#include "io/posix_file.hpp"

namespace blake3pp::detail::io_impl {

// The source half of the synchronous contract (sync_context in
// backend.hpp supplies the lanes, the poll loop and the wake).
class pread_source {
 public:
  // The engine this source is, for sync_context's rung declaration; the
  // word name() below spells, from the one table that spells it.
  static constexpr io_engine rung_engine = io_engine::posix_sync;
  // pread and pwrite carry their own offset and leave the descriptor's
  // alone, so two of them may run at once on one file.
  static constexpr bool positional = true;

  pread_source(const std::filesystem::path& path,
               const file_open_options& how)
      : write_(how.write) {
    const int base = write_ ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
    f_.open(path.c_str(), base | O_CLOEXEC, 0644);
    size_ = write_ ? 0 : f_.stat_size();
    // No preallocation: fallocate is Linux and F_PREALLOCATE is Darwin,
    // each living in its own backend, and synchronous writes do not
    // suffer the extending-write serialization that makes it matter.
    if (how.direct_io) {
      f_.try_odirect(path.c_str(), (write_ ? O_WRONLY : O_RDONLY) | O_CLOEXEC);
    }
  }

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::string_view name() const noexcept {
    if (write_) {
      return f_.direct() ? "pwrite+direct" : "pwrite";
    }
    return f_.direct() ? "pread+direct" : "pread";
  }

  void read_at(std::uint64_t off, std::span<std::byte> buf) {
    f_.pread_all(f_.sync_fd(buf.size()), buf, off);
  }

  void write_at(std::uint64_t off, std::span<const std::byte> buf) {
    f_.pwrite_all(f_.sync_fd(buf.size()), buf, off);
  }

  void set_size(std::uint64_t bytes) {
    if (::ftruncate(f_.fd_plain(), static_cast<off_t>(bytes)) != 0) {
      throw_errno("ftruncate");
    }
  }

 private:
  posix_file f_;
  std::uint64_t size_ = 0;
  bool write_ = false;
};

using pread_context = sync_context<pread_source>;

// Definition-site conformance check (see uring_backend.hpp).
static_assert(io_context<pread_context>);

}  // namespace blake3pp::detail::io_impl

#endif  // __unix__ || __APPLE__
