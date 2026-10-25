// The platform's read of a stream, for detail/pipe_driver.hpp.
//
// Everything platform-shaped about reading a pipe lives here, so the
// driver that windows one stays a standard-library header the pipeline
// can include.
//
// The contract is "return what has arrived". A blocking call waits for
// the first byte and wakes on the producer's own write; a non-blocking
// one takes what is already queued and reports an empty stream as zero
// bytes and no end. The driver drains with the second form and hands the
// window over when the stream runs dry, so there is no clock here and no
// moment at which a full pipe is left undrained.

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

// PeekNamedPipe says how much is queued without consuming it, which is
// how a non-blocking read is expressed on a handle that has no such
// mode; a pipe whose writers have all closed fails it with
// ERROR_BROKEN_PIPE, which is this platform's end of stream.
//
// A blocking read is safe to make because an anonymous pipe is a
// byte-mode pipe: the read returns with whatever arrived rather than
// waiting for the count it asked for. The count still has to be bounded
// -- see stream_reader::read_bound_ -- because the range a read asks for
// is locked whether the bytes come or not.
stream_reader::result stream_reader::read_some(std::span<std::byte> out,
                                               bool blocking) {
  if (out.empty()) {
    return {};
  }
  const auto want_all = [&] {
    return static_cast<DWORD>(std::min<std::size_t>(out.size(), 0xFFFF'FFFFu));
  };
  DWORD want = want_all();
  DWORD avail = 0;
  if (PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr) == 0) {
    const DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
      return {.bytes = 0, .eof = true};
    }
    // Not a pipe (a redirected file, a console): nothing to peek, and a
    // read of one does not leave a producer blocked. Read it directly
    // and let the read report the end.
    if (err != ERROR_INVALID_FUNCTION && err != ERROR_INVALID_HANDLE) {
      throw std::system_error(static_cast<int>(err), std::system_category(),
                              "reading a stream");
    }
  } else if (avail == 0) {
    if (!blocking) {
      return {.bytes = 0, .eof = false};
    }
    // Nothing queued, so there is no count to ask for: bound the read
    // by what the pipe can hold, because the read returns with whatever
    // arrives and the range it asked for is locked either way.
    if (!bound_known_) {
      DWORD in = 0;
      DWORD out_buf = 0;
      read_bound_ =
          GetNamedPipeInfo(h_, nullptr, &out_buf, &in, nullptr) != 0
              ? static_cast<std::size_t>(in != 0 ? in : out_buf)
              : 0;
      bound_known_ = true;
    }
    if (read_bound_ != 0) {
      want = static_cast<DWORD>(
          std::min<std::size_t>(out.size(), read_bound_));
    }
  } else {
    want = static_cast<DWORD>(
        std::min<std::size_t>(out.size(), static_cast<std::size_t>(avail)));
  }
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

stream_reader::result stream_reader::read_some(std::span<std::byte> out,
                                               bool blocking) {
  if (out.empty()) {
    return {};
  }
  for (;;) {
    if (!blocking) {
      // The descriptor may well be blocking -- stdin usually is -- so
      // readiness is asked before the read rather than inferred from
      // EAGAIN afterwards.
      pollfd pfd{.fd = h_, .events = POLLIN, .revents = 0};
      const int ready = ::poll(&pfd, 1, 0);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::system_error(errno, std::generic_category(),
                                "waiting on a stream");
      }
      if (ready == 0) {
        return {.bytes = 0, .eof = false};
      }
    }
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
      if (!blocking) {
        // A readiness poll that lost its race: the stream is dry again.
        return {.bytes = 0, .eof = false};
      }
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
