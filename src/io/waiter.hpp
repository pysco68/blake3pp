#pragma once

// The wake primitive behind the poll(block) of the uring and synchronous
// contexts, and of an aio context without a kqueue. wake() is
// the only member of a context another thread may call: the caller's
// other threads use it to get a blocked poll() back, and the lanes use it
// to report a finished transfer. A wake issued while nobody is blocked is
// remembered, and the next sleep returns at once, so a wake that races a
// poll is not lost.
//
// Linux gets an eventfd because io_uring can poll it: the uring context
// arms an IORING_OP_POLL_ADD on fd() so one enter() waits for reads and
// wakes alike. Elsewhere a condition variable says the same thing.
// Internal to src/io/, never installed.

#include <cstdint>
#if defined(__linux__)
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

#include "io/fd.hpp"
#else
#include <condition_variable>
#include <mutex>
#include <utility>
#endif

namespace blake3pp::detail::io_impl {

class poll_waiter {
 public:
#if defined(__linux__)
  poll_waiter() noexcept : fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}
  poll_waiter(const poll_waiter&) = delete;
  poll_waiter& operator=(const poll_waiter&) = delete;

  // Pollable by anything that takes a descriptor; -1 if the eventfd
  // could not be created, and sleep() then throws.
  [[nodiscard]] int fd() const noexcept { return fd_.get(); }

  void wake() noexcept {
    const std::uint64_t one = 1;
    // EAGAIN is the counter already saturated, and a wake is pending
    // either way. The result is bound to a variable because a cast to
    // void does not silence warn_unused_result on write().
    const ssize_t n = ::write(fd_.get(), &one, sizeof one);
    (void)n;
  }

  [[nodiscard]] bool take() noexcept {
    std::uint64_t v = 0;
    return ::read(fd_.get(), &v, sizeof v) == static_cast<ssize_t>(sizeof v);
  }

  void sleep() {
    if (!fd_) {
      // poll() ignores a negative descriptor and would wait on nothing,
      // forever. A context whose eventfd could not be created throws
      // instead.
      throw std::system_error(EBADF, std::generic_category(),
                              "blake3pp: no wake descriptor to sleep on");
    }
    ::pollfd p{fd_.get(), POLLIN, 0};
    while (::poll(&p, 1, -1) < 0) {
      if (errno != EINTR) {
        throw_errno("poll(wake)");
      }
    }
  }

 private:
  unique_fd fd_;
#else
  void wake() noexcept {
    {
      const std::lock_guard<std::mutex> lock(m_);
      signalled_ = true;
    }
    cv_.notify_one();
  }

  [[nodiscard]] bool take() noexcept {
    const std::lock_guard<std::mutex> lock(m_);
    return std::exchange(signalled_, false);
  }

  void sleep() {
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait(lock, [this] { return signalled_; });
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  bool signalled_ = false;
#endif
};

}  // namespace blake3pp::detail::io_impl
