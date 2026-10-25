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
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <span>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#endif

namespace {

// One iteration of a busy wait: tells the core it is spinning where the
// ISA has a way to say so.
//
// On AArch64 the hint is `isb`, an instruction synchronisation barrier
// used for its latency rather than its ordering: `yield` is the hint the
// architecture names for this, but it is a no-op on most implementations
// and spins at full rate, and `wfe` waits for an event the producer never
// sends and so could sleep until the next timer tick. `isb` drains the
// pipeline, which costs a few tens of cycles and nothing else, and is
// what Rust's std::hint::spin_loop emits on AArch64 for the same reason.
#if !defined(_WIN32)
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("isb" ::: "memory");
#else
  asm volatile("" ::: "memory");
#endif
}
#endif

}  // namespace

namespace blake3pp::detail {

#if defined(_WIN32)
native_stream standard_input() noexcept {
  return GetStdHandle(STD_INPUT_HANDLE);
}
#else
native_stream standard_input() noexcept { return STDIN_FILENO; }
#endif

#if defined(_WIN32)

std::size_t stream_reader::read_bound() const noexcept {
  return ask_bound_.bound();
}

// A blocking read is all this needs, and it is safe to make because an
// anonymous pipe is a byte-mode pipe: the read returns with whatever
// arrived rather than waiting for the count it asked for.
//
// What this no longer does is ask first. PeekNamedPipe reports the queue
// at an instant and a read capped at that count chases the producer
// instead of taking its whole write: measured at 4.1 to 4.8 syscalls per
// producer write, against 1.0 here, with one empty-pipe observation per
// write. The ask is bounded by stream_reader::read_bound() instead,
// which follows what earlier reads delivered (read_ask_bound) and is a
// bound on the range, not a reading of the queue.
//
// Nor does it watch the stream before the read, which is what the POSIX
// branch below does and what stream_watch exists for. On Windows the
// probe costs more than the sleep it avoids.
//
// PeekNamedPipe is 1.50 us per call on a Ryzen AI MAX+ 395, against
// 77-290 ns for a zero-timeout poll() on Linux, and the figure does not
// move with anything: 1501 ns on an empty pipe, 1506 ns with bytes
// queued, 1531 ns with eight other threads runnable, 1504 ns taken
// between the reads of a live pipeline. Win32 offers nothing cheaper on
// this handle. An anonymous pipe is not waitable, a CreatePipe handle
// has no overlapped form, and GetNamedPipeInfo answers 4096 against a
// buffer that holds a megabyte. NtQueryInformationFile with
// FilePipeLocalInformation answers the same question for 528 ns, and
// that is not enough either.
//
// The watch does avoid the sleeps, and still loses. Measured over 2 GiB
// through the stdin path at 4 KiB producer writes, it caught 259 079 of
// the 259 770 waits that found the stream empty and slept 691 times, for
// 45% more wall clock: catching took 2.16 probes per wait, and 561 351
// probes are 0.84 s of the 0.92 s it added. Six counterbalanced rounds
// at each of five producer write sizes and two more at --threads 2 lost
// all 42 pairs -- +45% wall at 4 KiB, +38% at 64 KiB, +22% at 256 KiB,
// +16% at 1 MiB, +2.4% at 4 MiB -- at higher total CPU across producer
// and consumer in six of the seven shapes.
//
// A zero budget does not rescue it, because the one probe that opens
// every wait is itself a loss: taken with the 528 ns call it costs 33% at
// 256 KiB producer writes. A Windows anonymous pipe holds about one
// producer write, measured as a high-water ReadDataAvailable of 8 KiB
// behind 4 KiB writes, 256 KiB behind 256 KiB writes and 1 MiB behind
// 1 MiB writes. The producer is therefore waiting on a full pipe for this
// read, and what the reader spends probing the producer spends stalled.
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
  ask_bound_.note(static_cast<std::size_t>(want),
                  static_cast<std::size_t>(moved));
  return {.bytes = static_cast<std::size_t>(moved), .eof = false};
}

std::size_t stream_reader::widen(std::size_t) noexcept {
  // Windows sets a pipe's buffer when it is created, and stdin's was
  // created by whoever launched the process.
  return 0;
}

#else

namespace {

// poll() with a timeout, answering whether a read would now return.
// POLLHUP and POLLERR count: the read is what reports the end or the
// error, and it must be allowed to.
[[nodiscard]] bool readable(int fd, int timeout_ms) {
  pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
  for (;;) {
    const int r = ::poll(&pfd, 1, timeout_ms);
    if (r > 0) {
      return true;
    }
    if (r == 0) {
      return false;
    }
    if (errno != EINTR) {
      throw std::system_error(errno, std::generic_category(),
                              "waiting on a stream");
    }
  }
}

[[nodiscard]] std::int64_t now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

// The platform under stream_watch: the probe is poll() with a zero
// timeout rather than a non-blocking read, because it answers from the
// queue's state and leaves the descriptor's flags alone. That matters:
// standard input is an open file description shared with the parent
// shell and every sibling process, and O_NONBLOCK set here would be set
// for all of them.
//
// A probe is a system call, so stream_watch paces them by the clock, one
// per microsecond, rather than by a count of pause instructions, whose
// cost differs from core to core. Measured on Linux, the watch lowered
// total CPU across producer and consumer (13% at 512-byte producer
// writes, 19% at 4 KiB): the sleeps it removes were themselves system
// time.
//
// One probe precedes every read, and what it costs is a property of the
// platform, not of the watch. On Linux a zero-timeout poll() of a pipe
// keeps the calling thread on its core, so for a producer that keeps up
// it is one call per read of a megabyte or more, and for one that does
// not, the read was going to wait anyway. On XNU the same call costs the
// caller an involuntary context switch whenever anything else is
// runnable, data present or not: measured on an M2 at 512-byte producer
// writes, 1.06 million probes of which 3 593 entered the watch, 645 000
// involuntary switches against 10 500 without the probe, and 31% more
// wall clock. The Darwin branch below therefore skips wait_readable()
// entirely rather than setting the budget to zero.
//
// ioctl(FIONREAD) does not take that switch on XNU: a watch built on it
// made 4.8 million probes for 11 500 switches. It was 10% faster at `cat`
// and 5% at 4 KiB producer writes, level at 512 bytes and 64 KiB, and
// cost 11-48% more consumer CPU in every shape, so Darwin keeps the
// blocking read.
namespace {

struct posix_watch_platform {
  int fd;
  [[nodiscard]] std::int64_t now() const noexcept { return now_ns(); }
  [[nodiscard]] bool probe() const { return readable(fd, 0); }
  void sleep() const { (void)readable(fd, -1); }
  void relax() const noexcept { cpu_relax(); }
};

}  // namespace

// Nothing to bound: a read() copies min(asked, queued) and leaves the
// rest of the range untouched, so an ask costs only what it returns.
std::size_t stream_reader::read_bound() const noexcept { return 0; }

void stream_reader::wait_readable() {
#if defined(__APPLE__) || defined(__EMSCRIPTEN__)
  // No probe and no watch on Darwin, where the read blocks (measured at
  // parity with a reader that never probed, at 512-byte and 4 KiB
  // producer writes). Under Emscripten poll() on standard input throws a
  // TypeError from the runtime's syscall layer rather than returning an
  // error.
#else
  posix_watch_platform p{h_};
  watch_.wait(p);
#endif
}

stream_reader::result stream_reader::read_some(std::span<std::byte> out) {
  if (out.empty()) {
    return {};
  }
  wait_readable();
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
      // A descriptor the caller's shell handed over non-blocking, and a
      // queue another reader emptied between the probe and the read.
      (void)readable(h_, -1);
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
