// readshim: count a process's read() calls without changing it, and
// optionally inject a read fault.
//
//   LD_PRELOAD=/path/libpipe_readshim.so blake3ppsum -  < in      (Linux, FreeBSD)
//   DYLD_INSERT_LIBRARIES=/path/libpipe_readshim.dylib blake3ppsum - < in   (macOS)
//
// At exit it prints, to stderr, the calls and bytes on standard input and
// on all descriptors together, then the interrupted calls and the
// injected errors:
//
//   readshim: fd0 reads=<n> bytes=<n> | all reads=<n> bytes=<n> | eintr=<n> poll_eintr=<n> injected_eio=<n>
//
// It counts calls to the C library's read() made through the dynamic
// linker. What that does not see, by platform:
//
//   * Windows: nothing to load it into. There is no preload mechanism
//     short of a Detours-style hook that rewrites the target's import
//     table or code, which is outside what a bench tool should do. It is
//     not built there.
//   * macOS: interposition goes through the interpose table (declared in
//     __DATA,__interpose; the linker places it in __DATA_CONST) and
//     catches calls that bind to read() and poll(). The C library's own
//     fread() reads through an internal, non-interposable symbol, so a
//     program that reads stdin with stdio shows zero reads. glibc's stdio
//     does the same on Linux: fread() is invisible to it there too.
//   * Everywhere: a statically linked program, or one that issues the
//     system call directly (Go, and some Rust paths), bypasses it. A count
//     of zero on a program that clearly read its input means this, not
//     that it made no reads.
//
// Two faults it can inject, set by environment variable because a
// preloaded library has no command line:
//
//   READSHIM_EINTR=1      install a SIGUSR1 handler WITHOUT SA_RESTART and
//                         unblock SIGUSR1, so a signal that arrives during
//                         a blocking read makes it fail with EINTR. Send
//                         the signals with `rux --signal-storm`. The
//                         report counts the EINTRs read() and poll()
//                         actually returned -- a reader that waits in
//                         poll() before it reads takes them there.
//   READSHIM_FAIL_AFTER=N once N bytes have been read from standard
//                         input, every further read of it fails with EIO.
//                         The read that crosses N is shortened to end at
//                         N, so exactly N bytes arrive before the error.
//

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#if !defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace {

std::atomic<unsigned long long> fd0_calls{0}, fd0_bytes{0}, all_calls{0}, all_bytes{0},
    eintrs{0}, poll_eintrs{0}, injected{0};

// READSHIM_FAIL_AFTER, or -1 when unset.
long long fail_after() noexcept {
  static const long long n = [] {
    const char* v = std::getenv("READSHIM_FAIL_AFTER");
    return v == nullptr ? -1LL : std::atoll(v);
  }();
  return n;
}

void on_signal(int) noexcept {}

struct installer {
  installer() {
    const char* v = std::getenv("READSHIM_EINTR");
    if (v == nullptr || v[0] != '1') return;
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: an interrupted read fails with EINTR
    sigaction(SIGUSR1, &sa, nullptr);
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGUSR1);
    sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
  }
} install_at_load;

// The shortened ask, or 0 when the injected error is due now.
size_t before_read(int fd, size_t n, bool& fail) noexcept {
  fail = false;
  const long long limit = fail_after();
  if (fd != 0 || limit < 0) return n;
  const unsigned long long done = fd0_bytes.load(std::memory_order_relaxed);
  if (done >= static_cast<unsigned long long>(limit)) {
    fail = true;
    return 0;
  }
  const unsigned long long left = static_cast<unsigned long long>(limit) - done;
  return left < n ? static_cast<size_t>(left) : n;
}

void count(int fd, ssize_t r) noexcept {
  if (r < 0 && errno == EINTR) eintrs.fetch_add(1, std::memory_order_relaxed);
  all_calls.fetch_add(1, std::memory_order_relaxed);
  if (r > 0) all_bytes.fetch_add(static_cast<unsigned long long>(r), std::memory_order_relaxed);
  if (fd == 0) {
    fd0_calls.fetch_add(1, std::memory_order_relaxed);
    if (r > 0) fd0_bytes.fetch_add(static_cast<unsigned long long>(r), std::memory_order_relaxed);
  }
}

struct reporter {
  ~reporter() {
    std::fprintf(stderr,
                 "readshim: fd0 reads=%llu bytes=%llu | all reads=%llu bytes=%llu | eintr=%llu "
                 "poll_eintr=%llu injected_eio=%llu\n",
                 fd0_calls.load(), fd0_bytes.load(), all_calls.load(), all_bytes.load(),
                 eintrs.load(), poll_eintrs.load(), injected.load());
  }
} report_at_exit;

}  // namespace

#if defined(__APPLE__)

// dyld replaces every binding to read() with shim_read; inside the shim,
// read() still binds to the real one.
extern "C" ssize_t shim_read(int fd, void* buf, size_t n) {
  bool fail = false;
  const size_t ask = before_read(fd, n, fail);
  if (fail && n > 0) {
    injected.fetch_add(1, std::memory_order_relaxed);
    errno = EIO;
    return -1;
  }
  const ssize_t r = read(fd, buf, ask);
  count(fd, r);
  return r;
}

extern "C" int shim_poll(pollfd* fds, nfds_t n, int timeout) {
  const int r = poll(fds, n, timeout);
  if (r < 0 && errno == EINTR) poll_eintrs.fetch_add(1, std::memory_order_relaxed);
  return r;
}

extern "C" {
__attribute__((used)) static const struct {
  const void* replacement;
  const void* replacee;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
    {reinterpret_cast<const void*>(&shim_read), reinterpret_cast<const void*>(&read)},
    {reinterpret_cast<const void*>(&shim_poll), reinterpret_cast<const void*>(&poll)}};
}

#else

// The preloaded definition of read(); the real one is the next in the
// lookup order.
extern "C" ssize_t read(int fd, void* buf, size_t n) {
  using read_fn = ssize_t (*)(int, void*, size_t);
  static const read_fn real = reinterpret_cast<read_fn>(dlsym(RTLD_NEXT, "read"));
  bool fail = false;
  const size_t ask = before_read(fd, n, fail);
  if (fail && n > 0) {
    injected.fetch_add(1, std::memory_order_relaxed);
    errno = EIO;
    return -1;
  }
  const ssize_t r = real(fd, buf, ask);
  count(fd, r);
  return r;
}

extern "C" int poll(pollfd* fds, nfds_t n, int timeout) {
  using poll_fn = int (*)(pollfd*, nfds_t, int);
  static const poll_fn real = reinterpret_cast<poll_fn>(dlsym(RTLD_NEXT, "poll"));
  const int r = real(fds, n, timeout);
  if (r < 0 && errno == EINTR) poll_eintrs.fetch_add(1, std::memory_order_relaxed);
  return r;
}

#endif
