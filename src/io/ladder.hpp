#pragma once

// The degradation ladder, declared so that code can walk it.
//
// Every backend in this directory degrades: io_uring to pread, the I/O
// ring to the completion port to ReadFile, libdispatch to pread, POSIX
// AIO to pread. That ladder used to exist only as prose in the backend
// headers, as `if` branches in their constructors and as a suffix on a
// file's name(). Nothing could walk it, so blake3pp::compiled_io_engines()
// was given a hand-written list of what each platform compiles instead,
// and the list was wrong for Windows the day it was written. This
// header is the vocabulary that replaces it.
//
// Each context declares two compile-time facts and answers one runtime
// question:
//
//   static constexpr io_engine rung_engine = io_engine::io_uring;
//   using rung_below = posix_sync_rung;        // void at the floor
//   [[nodiscard]] rung_verdict engaged_rung() const noexcept;
//
// The chain of rung_below from native_io_context is the set of engines
// this binary carries, in preference order. A build without
// <ioringapi.h> cannot report an I/O ring: windows_engine declares its
// top rung behind the same macro the ring itself is behind. The verdict
// a constructed context hands back is the rung it landed on, which is
// the engine a fresh open uses.
//
// This is the DECLARATION form. The composition form -- every rung a
// real object, the way windows_engine already holds a real
// ioring_engine or a real iocp_engine -- is a separate question. The
// synchronous rung a POSIX context degrades to is served inline from
// its deferred_ list, which is where the contract's hardest rule lives
// (a drain must not read a transfer). Declaring a rung moves no code;
// giving it an object does.
//
// Internal to src/io/, never installed.

#include <blake3pp/io.hpp>
#include <blake3pp/trace.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace blake3pp::detail::io_impl {

// Which rung a constructed context landed on, and whether that answer
// can change.
//
// A context that could not get io_uring because the descriptor table
// was full says nothing about the kernel: with the descriptors
// returned, a real open engages the ring microseconds later. The
// FreeBSD AIO queue limit is a sysctl an administrator can raise while
// the process runs, and the Windows ring's completion event is a handle
// that can simply fail to be created. So a refusal says whether it can
// be taken back, and only a settled verdict may be remembered. The
// first version of this code remembered all of them, arguing from the
// CPU probe that a machine cannot grow a feature it lacks -- true of
// AVX-512, false of every refusal here.
struct rung_verdict {
  io_engine engine;  // the rung this context actually engaged
  bool settled;      // false while the same question may answer otherwise
};

// Whether a refused facility might be granted on the next attempt.
//
// The resource errors can lift: a descriptor table or a memory limit
// belongs to this moment and this process, not to the kernel. ENOSYS
// and EPERM cannot -- a kernel without io_uring does not grow one, and
// a seccomp policy forbidding it is not lifted -- and they are the
// common refusals, so classifying keeps the probe free on the machines
// that refuse. An errno nobody recorded (0) counts as able to lift;
// not knowing why is no reason to remember the answer.
[[nodiscard]] constexpr bool refusal_may_lift(int err) noexcept {
  switch (err) {
    case EPERM:
    case ENOSYS:
#ifdef EOPNOTSUPP
    case EOPNOTSUPP:
#endif
#if defined(ENOTSUP) && (!defined(EOPNOTSUPP) || ENOTSUP != EOPNOTSUPP)
    case ENOTSUP:
#endif
    case EINVAL:
      return false;
    default:
      // EMFILE, ENFILE, ENOMEM, EAGAIN, EINTR, ENOBUFS and 0.
      return true;
  }
}

// The rungs that are inlined code rather than objects.
//
// A POSIX context that cannot have its async engine serves every
// transfer from its own deferred_ list, through posix_file's pread and
// pwrite; a Windows one calls ReadFile on the calling thread. There is
// no class to ask, so these declare for them: the engine's name, and
// that nothing is below it. Nothing constructs one.
//
// They are the floor in the strong sense too. A kernel can refuse
// io_uring; none refuses pread(). That is what makes
// available_io_engines() non-empty and best_io_engine() always
// answerable.
struct posix_sync_rung {
  static constexpr io_engine rung_engine = io_engine::posix_sync;
  using rung_below = void;
};

struct win_sync_rung {
  static constexpr io_engine rung_engine = io_engine::win_sync;
  using rung_below = void;
};

// A rung's name is what driver_stats::read_backend records, so every
// engine bounds the read names it can produce and asserts the bound at
// compile time against max_read_backend_length. A refusal reason may
// print a number; these are the widest each kind prints.
inline constexpr std::size_t int_text_max = 11;  // -2147483648
inline constexpr std::size_t u32_text_max = 10;  // 4294967295

// The longest of a set of names.
[[nodiscard]] constexpr std::size_t longest_of(
    std::initializer_list<std::string_view> names) noexcept {
  std::size_t n = 0;
  for (const std::string_view s : names) {
    n = std::max(n, s.size());
  }
  return n;
}

}  // namespace blake3pp::detail::io_impl
