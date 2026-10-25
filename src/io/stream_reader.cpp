// The platform's read of a stream, for detail/pipe_driver.hpp.
//
// Everything platform-shaped about reading a pipe lives here, so the
// driver that windows one stays a standard-library header the pipeline
// can include.
//
// The contract is "wait for the producer's next write, then return what
// has arrived". The driver reads in a loop until the window is full, and
// a read that comes back short of its ask is how it learns the stream
// ran dry: the queue is empty, so there is no moment at which a full
// pipe is left undrained and no call whose only answer is "nothing".

#include <blake3pp/detail/pipe_driver.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
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

// A blocking read is all this needs, and it is safe to make because an
// anonymous pipe is a byte-mode pipe: the read returns with whatever
// arrived rather than waiting for the count it asked for.
//
// What this no longer does is ask first. PeekNamedPipe reports the queue
// at an instant and a read capped at that count chases the producer
// instead of taking its whole write: measured at 4.1 to 4.8 syscalls per
// producer write, against 1.0 here, with one empty-pipe observation per
// write. The ask is bounded by stream_reader::read_bound() instead,
// which is a bound on the range and not on the queue.
stream_reader::result stream_reader::read_some(std::span<std::byte> out) {
  if (out.empty()) {
    return {};
  }
  const DWORD want =
      static_cast<DWORD>(std::min<std::size_t>(out.size(), 0xFFFF'FFFFu));
  DWORD moved = 0;
  if (ReadFile(h_, out.data(), want, &moved, nullptr) == 0) {
    const DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
      return {.bytes = 0, .eof = true};
    }
    throw std::system_error(static_cast<int>(err), std::system_category(),
                            "reading a stream");
  }
  if (moved == 0) {
    return {.bytes = 0, .eof = true};
  }
  return {.bytes = static_cast<std::size_t>(moved), .eof = false};
}

std::size_t stream_reader::widen(std::size_t) noexcept {
  // Windows sets a pipe's buffer when it is created, and stdin's was
  // created by whoever launched the process.
  return 0;
}

#else

stream_reader::result stream_reader::read_some(std::span<std::byte> out) {
  if (out.empty()) {
    return {};
  }
  for (;;) {
    const auto n = ::read(h_, out.data(), out.size());
    if (n > 0) {
      return {.bytes = static_cast<std::size_t>(n), .eof = false};
    }
    if (n == 0) {
      return {.bytes = 0, .eof = true};
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // A descriptor the caller's shell handed over non-blocking: wait
      // for readiness instead of spinning on it.
      pollfd pfd{.fd = h_, .events = POLLIN, .revents = 0};
      if (::poll(&pfd, 1, -1) < 0 && errno != EINTR) {
        throw std::system_error(errno, std::generic_category(),
                                "waiting on a stream");
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
