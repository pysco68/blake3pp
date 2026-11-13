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

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#define BLAKE3PP_IO_MSAN 1
#endif
#endif

namespace blake3pp::detail::io_impl {

// MemorySanitizer cannot see a read the kernel completes on its own: the
// bytes land without any libc call MSan intercepts, so they stay
// "uninitialized" in its shadow. A backend whose reads complete that way
// (io_uring, AIO) unpoisons the range each one filled, stating a fact
// MSan has no other way to learn. A no-op in every other build.
inline void msan_unpoison([[maybe_unused]] const void* p,
                          [[maybe_unused]] std::size_t n) noexcept {
#if defined(BLAKE3PP_IO_MSAN)
  __msan_unpoison(p, n);
#endif
}

struct posix_file {
  posix_file() = default;
  posix_file(const posix_file&) = delete;
  posix_file& operator=(const posix_file&) = delete;

  // The always-buffered descriptor: unaligned tails and the fallback.
  [[nodiscard]] int fd_plain() const noexcept { return plain_.get(); }
  // The fast one: the O_DIRECT reopen when it engaged, else the plain.
  [[nodiscard]] int fd() const noexcept {
    return direct_ ? direct_.get() : plain_.get();
  }
  // Whether the fast descriptor bypasses the page cache: a second
  // O_DIRECT descriptor, or the plain one with caching switched off.
  [[nodiscard]] bool direct() const noexcept {
    return bool(direct_) || nocache_;
  }
  void set_direct() noexcept { nocache_ = true; }

  void open(const char* path, int flags, ::mode_t mode = 0) {
    plain_.reset(::open(path, flags, mode));
    if (!plain_) {
      throw_errno("open");
    }
  }

  [[nodiscard]] std::uint64_t stat_size() const {
    struct stat st;
    if (::fstat(fd_plain(), &st) != 0) {
      throw_errno("fstat");
    }
    return static_cast<std::uint64_t>(st.st_size);
  }

  // O_DIRECT is a per-open flag: engage by reopening, keeping the plain fd
  // for unaligned lengths. Silently declines where the platform (Darwin)
  // lacks the flag or the filesystem refuses it.
  void try_odirect(const char* path, int flags) noexcept {
#if defined(O_DIRECT)
    direct_.reset(::open(path, flags | O_DIRECT));
#else
    (void)path;
    (void)flags;
#endif
  }

  // Gives up the O_DIRECT reopen, leaving the plain descriptor as the
  // only one: for a backend that PROBES the descriptor at open and finds
  // the filesystem will not serve it (FreeBSD's ZFS answers EINVAL).
  void drop_direct() noexcept { direct_.reset(); }

  // Picks the fd for a synchronous positional transfer: direct only when
  // engaged AND the length keeps O_DIRECT's alignment contract.
  [[nodiscard]] int sync_fd(std::size_t len) const noexcept {
    return direct() && len % direct_align == 0 ? fd() : fd_plain();
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
      if (n == 0) {
        // No progress and no error: retrying would spin for ever.
        throw std::system_error(EIO, std::generic_category(),
                                "pwrite wrote nothing");
      }
      put += static_cast<std::size_t>(n);
    }
  }

 private:
  unique_fd plain_;
  unique_fd direct_;
  bool nocache_ = false;
};

}  // namespace blake3pp::detail::io_impl

#endif  // __unix__ || __APPLE__
