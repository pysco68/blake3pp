// The platform boundary the pipe instruments share: standard input and
// output as raw handles, one read and one complete write on them, and the
// busy-wait hint. Everything else in bench/pipe/ is standard C++.
//
// Raw handles rather than stdio, because stdio buffers and, on Windows,
// translates line endings unless told otherwise: an instrument that
// measures a pipe must make exactly the calls it reports.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if defined(_M_ARM64)
#include <intrin.h>  // __isb
#endif
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#endif

namespace pipebench {

#if defined(_WIN32)
using handle = HANDLE;
inline handle std_in() noexcept { return GetStdHandle(STD_INPUT_HANDLE); }
inline handle std_out() noexcept { return GetStdHandle(STD_OUTPUT_HANDLE); }
#else
using handle = int;
inline handle std_in() noexcept { return STDIN_FILENO; }
inline handle std_out() noexcept { return STDOUT_FILENO; }
#endif

// What one read did.
struct read_result {
  std::size_t bytes = 0;
  bool eof = false;    // the writer closed its end
  bool error = false;  // anything else
  // How the read ended when it returned no bytes: Windows' GetLastError()
  // (ERROR_BROKEN_PIPE is 109) or 0 for a successful zero-byte read;
  // POSIX errno, or 0 for end of file.
  unsigned long code = 0;
};

// Closes this process's standard input, so a writer into it meets a pipe
// with no reader.
inline void close_std_in() noexcept {
#if defined(_WIN32)
  CloseHandle(GetStdHandle(STD_INPUT_HANDLE));
  SetStdHandle(STD_INPUT_HANDLE, nullptr);
#else
  ::close(STDIN_FILENO);
#endif
}

// One blocking read: returns as soon as anything has arrived.
inline read_result read_some(handle h, void* buf, std::size_t n) noexcept {
#if defined(_WIN32)
  const DWORD want = n > 0xFFFF'FFFFu ? 0xFFFF'FFFFu : static_cast<DWORD>(n);
  DWORD got = 0;
  if (ReadFile(h, buf, want, &got, nullptr) == 0) {
    const DWORD e = GetLastError();
    if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) {
      return {.bytes = 0, .eof = true, .code = e};
    }
    return {.bytes = 0, .error = true, .code = e};
  }
  if (got == 0) {
    return {.bytes = 0, .eof = true};
  }
  return {.bytes = got};
#else
  for (;;) {
    const ssize_t r = ::read(h, buf, n);
    if (r > 0) {
      return {.bytes = static_cast<std::size_t>(r)};
    }
    if (r == 0) {
      return {.bytes = 0, .eof = true};
    }
    if (errno != EINTR) {
      return {.bytes = 0, .error = true, .code = static_cast<unsigned long>(errno)};
    }
  }
#endif
}

// Writes all of `n`, in as many calls as the platform needs.
inline bool write_all(handle h, const void* buf, std::size_t n) noexcept {
  const auto* p = static_cast<const unsigned char*>(buf);
  while (n > 0) {
#if defined(_WIN32)
    const DWORD want = n > 0xFFFF'FFFFu ? 0xFFFF'FFFFu : static_cast<DWORD>(n);
    DWORD put = 0;
    if (WriteFile(h, p, want, &put, nullptr) == 0) {
      return false;
    }
    const std::size_t done = put;
#else
    const ssize_t r = ::write(h, p, n);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    const std::size_t done = static_cast<std::size_t>(r);
#endif
    p += done;
    n -= done;
  }
  return true;
}

// Ends this process abnormally: no exit handlers, no flush, no clean
// close -- the descriptors are closed by the kernel as it reaps the
// process. SIGKILL rather than abort(), which would write a core.
[[noreturn]] inline void die() noexcept {
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(), 9);
  for (;;) Sleep(INFINITE);
#else
  ::kill(::getpid(), SIGKILL);
  for (;;) ::pause();
#endif
}

// One iteration of a busy wait. On AArch64 `isb` rather than `yield`,
// which most implementations execute as a no-op (the library's
// stream_reader.cpp has the same choice and its reasoning).
inline void relax() noexcept {
#if defined(_M_ARM64)
  __isb(_ARM64_BARRIER_SY);
#elif defined(_WIN32)
  YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("isb" ::: "memory");
#else
  asm volatile("" ::: "memory");
#endif
}

}  // namespace pipebench
