// The platform's read of a stream, for detail/pipe_driver.hpp.
//
// Everything platform-shaped about reading a pipe lives here, so the
// driver that windows one stays a standard-library header the pipeline
// can include.
//
// The contract is "return as soon as anything arrives, and never past
// the deadline". That is a wait with a timeout followed by a read, in
// that order, because a read that blocked would own the thread past the
// deadline and the deadline is what keeps a pool fed by a slow producer
// from stalling.

#include <blake3pp/detail/pipe_driver.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace blake3pp::detail {

#if defined(_WIN32)
native_stream standard_input() noexcept {
  return GetStdHandle(STD_INPUT_HANDLE);
}
#else
native_stream standard_input() noexcept { return STDIN_FILENO; }
#endif

#if defined(_WIN32)

// Windows has no readiness wait for an anonymous pipe, so the wait is
// PeekNamedPipe and a sleep: the peek says how much is queued without
// consuming it, and a pipe whose writers have all closed fails it with
// ERROR_BROKEN_PIPE, which is this platform's end of stream.
stream_reader::result stream_reader::read_some(std::span<std::byte> out,
                                               std::int64_t deadline_ns) {
  if (out.empty()) {
    return {};
  }
  for (;;) {
    DWORD avail = 0;
    if (PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr) == 0) {
      const DWORD err = GetLastError();
      if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
        return {.bytes = 0, .eof = true, .timed_out = false};
      }
      // Not a pipe (a redirected file, a console): read it directly and
      // let the read report the end.
      if (err == ERROR_INVALID_FUNCTION || err == ERROR_INVALID_HANDLE) {
        avail = static_cast<DWORD>(
            std::min<std::size_t>(out.size(), 0xFFFF'FFFFu));
      } else {
        throw std::system_error(static_cast<int>(err), std::system_category(),
                                "reading a stream");
      }
    }
    if (avail != 0) {
      DWORD moved = 0;
      const DWORD want = static_cast<DWORD>(
          std::min<std::size_t>(out.size(), static_cast<std::size_t>(avail)));
      if (ReadFile(h_, out.data(), want, &moved, nullptr) == 0) {
        const DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
          return {.bytes = 0, .eof = true, .timed_out = false};
        }
        throw std::system_error(static_cast<int>(err), std::system_category(),
                                "reading a stream");
      }
      if (moved == 0) {
        return {.bytes = 0, .eof = true, .timed_out = false};
      }
      return {.bytes = static_cast<std::size_t>(moved),
              .eof = false,
              .timed_out = false};
    }
    if (deadline_ns != no_deadline && stream_now_ns() >= deadline_ns) {
      return {.bytes = 0, .eof = false, .timed_out = true};
    }
    // Nothing queued and time left: the only wait a pipe offers.
    Sleep(1);
  }
}

std::size_t stream_reader::widen(std::size_t) noexcept {
  // Windows sets a pipe's buffer when it is created, and stdin's was
  // created by whoever launched the process.
  return 0;
}

#else

stream_reader::result stream_reader::read_some(std::span<std::byte> out,
                                               std::int64_t deadline_ns) {
  if (out.empty()) {
    return {};
  }
  for (;;) {
    if (deadline_ns != no_deadline) {
      // Readiness first, so the read below cannot own the thread past
      // the deadline.
      const std::int64_t left = deadline_ns - stream_now_ns();
      pollfd pfd{.fd = h_, .events = POLLIN, .revents = 0};
      const int ms = left <= 0
                         ? 0
                         : static_cast<int>(std::min<std::int64_t>(
                               (left + 999'999) / 1'000'000, 3600'000));
      const int ready = ::poll(&pfd, 1, ms);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::system_error(errno, std::generic_category(),
                                "waiting on a stream");
      }
      if (ready == 0) {
        return {.bytes = 0, .eof = false, .timed_out = true};
      }
    }
    const auto n = ::read(h_, out.data(), out.size());
    if (n > 0) {
      return {.bytes = static_cast<std::size_t>(n),
              .eof = false,
              .timed_out = false};
    }
    if (n == 0) {
      return {.bytes = 0, .eof = true, .timed_out = false};
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // A non-blocking descriptor whose poll() lost the race, or one
      // handed over non-blocking by the caller's shell.
      if (deadline_ns != no_deadline && stream_now_ns() >= deadline_ns) {
        return {.bytes = 0, .eof = false, .timed_out = true};
      }
      continue;
    }
    throw std::system_error(errno, std::generic_category(),
                            "reading a stream");
  }
}

std::size_t stream_reader::widen(std::size_t bytes) noexcept {
#if defined(F_SETPIPE_SZ) && defined(F_GETPIPE_SZ)
  // Best effort in both directions: the request is clamped by
  // /proc/sys/fs/pipe-max-size for an unprivileged process, and F_SETPIPE_SZ
  // fails outright on anything that is not a pipe. What matters is the
  // buffer in force afterwards, which is what this reports.
  if (bytes > 0) {
    (void)::fcntl(h_, F_SETPIPE_SZ, static_cast<int>(bytes));
  }
  const int have = ::fcntl(h_, F_GETPIPE_SZ);
  return have > 0 ? static_cast<std::size_t>(have) : 0;
#else
  (void)bytes;
  return 0;
#endif
}

#endif

}  // namespace blake3pp::detail
