#pragma once

// Shared POSIX file plumbing for the I/O backends: the buffered fd that
// always exists, the optional O_DIRECT reopen next to it (a per-open flag,
// hence a second fd; Darwin's F_NOCACHE backend instead flips `direct` on
// the one fd), and the EINTR-looping positional read/write primitives.
// Internal to src/io/, never installed.

#if defined(__unix__) || defined(__APPLE__)

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <system_error>

#include "io/backend.hpp"

namespace blake3pp::detail::io_impl {

struct posix_file {
  int fd_plain = -1;  // always-buffered fd (unaligned tails, fallback)
  int fd = -1;        // == fd_plain unless the O_DIRECT reopen engaged
  bool direct = false;

  posix_file() = default;
  posix_file(const posix_file&) = delete;
  posix_file& operator=(const posix_file&) = delete;
  ~posix_file() {
    if (fd != fd_plain && fd >= 0) {
      ::close(fd);
    }
    if (fd_plain >= 0) {
      ::close(fd_plain);
    }
  }

  void open(const char* path, int flags, ::mode_t mode = 0) {
    fd_plain = ::open(path, flags, mode);
    if (fd_plain < 0) {
      throw_errno("open");
    }
    fd = fd_plain;
  }

  [[nodiscard]] std::uint64_t stat_size() const {
    struct stat st;
    if (::fstat(fd_plain, &st) != 0) {
      throw_errno("fstat");
    }
    return static_cast<std::uint64_t>(st.st_size);
  }

  // O_DIRECT is a per-open flag: engage by reopening, keeping the plain fd
  // for unaligned lengths. Silently declines where the platform (Darwin)
  // lacks the flag or the filesystem refuses it.
  void try_odirect(const char* path, int flags) noexcept {
#if defined(O_DIRECT)
    const int dfd = ::open(path, flags | O_DIRECT);
    if (dfd >= 0) {
      fd = dfd;
      direct = true;
    }
#else
    (void)path;
    (void)flags;
#endif
  }

  // Picks the fd for a synchronous positional transfer: direct only when
  // engaged AND the length keeps O_DIRECT's alignment contract.
  [[nodiscard]] int sync_fd(std::size_t len) const noexcept {
    return direct && len % direct_align == 0 ? fd : fd_plain;
  }

  void pread_all(int use_fd, std::byte* dst, std::size_t len,
                 std::uint64_t off) const {
    std::size_t got = 0;
    while (got < len) {
      const ssize_t n = ::pread(use_fd, dst + got, len - got,
                                static_cast<off_t>(off + got));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw_errno("pread");
      }
      if (n == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF");
      }
      got += static_cast<std::size_t>(n);
    }
  }

  void pwrite_all(int use_fd, const std::byte* src, std::size_t len,
                  std::uint64_t off) const {
    std::size_t put = 0;
    while (put < len) {
      const ssize_t n = ::pwrite(use_fd, src + put, len - put,
                                 static_cast<off_t>(off + put));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw_errno("pwrite");
      }
      put += static_cast<std::size_t>(n);
    }
  }
};

}  // namespace blake3pp::detail::io_impl

#endif  // __unix__ || __APPLE__
