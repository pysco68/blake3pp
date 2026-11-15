#pragma once

// errno as an exception, and a POSIX file descriptor that closes itself:
// what every backend that makes system calls throws and holds.
// Internal to src/io/, never installed.

#include <cerrno>
#include <system_error>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace blake3pp::detail::io_impl {

[[noreturn]] inline void throw_errno(const char* what) {
  // Read before the throw: the exception object is allocated first, and
  // errno must be the failed call's, not the allocator's.
  const int e = errno;
  throw std::system_error(e, std::generic_category(), what);
}

#if defined(__unix__) || defined(__APPLE__)
// A file descriptor that closes itself. Moves, does not copy; -1 is
// empty. The borrowed int goes where a system call takes one.
class unique_fd {
 public:
  unique_fd() noexcept = default;
  explicit unique_fd(int fd) noexcept : fd_(fd) {}
  unique_fd(unique_fd&& o) noexcept : fd_(o.release()) {}
  unique_fd& operator=(unique_fd&& o) noexcept {
    if (this != &o) {
      reset(o.release());
    }
    return *this;
  }
  unique_fd(const unique_fd&) = delete;
  unique_fd& operator=(const unique_fd&) = delete;
  ~unique_fd() { reset(); }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }
  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};
#endif  // __unix__ || __APPLE__

}  // namespace blake3pp::detail::io_impl
