// drain: a consumer for a pipe that does nothing with the bytes, so what
// it measures is the pipe and the producer.
//
//   drain [--buf <KiB>] [--widen <KiB>] [--threads <N>]
//   drain [--buf <KiB>] [--widen <KiB>] --watch <probe> [--budget <us>] [--interval <ns>]
//   drain [--buf <KiB>] [--widen <KiB>] --close-after <N> [--mark <path>]
//
// The summary goes to stderr, one line:
//   drain: bytes=<n> in <s> s = <MiB/s> MiB/s | reads=<n> bytes_per_read=<n> [| probes=<n> caught=<n> slept=<n>] | pipe=<n> | end=<how>
// with the watch's counts only under --watch.
//
// Three shapes of reader, because "the pipe's ceiling" depends on its
// reader:
//
//   * one blocking reader (the default): the `cat > /dev/null` end of a
//     pipe. Between two of its reads there is no read posted, so a
//     producer that fills the pipe waits for it to come back.
//   * --threads N, N >= 2: N blocking readers on the one handle, so a read
//     is always posted and the reader is never what the pipe waits for.
//     That is the never-sleeping drain: the floor any consumer is measured
//     against. Byte order does not matter to a drain, which is what makes
//     concurrent reads on one handle legitimate here.
//   * --watch: one reader that probes an empty pipe before it reads, by
//     the rule the library's stream_watch (detail/pipe_driver.hpp)
//     follows -- a 16 us budget that grows on a catch and halves on a
//     time-out, one probe per microsecond. There is no readiness wait on
//     a Windows anonymous pipe, so the "sleep" at the end of a watch is
//     the blocking read itself, on every platform alike. The rule is
//     transcribed, not linked, so this builds where the library's bench
//     targets do not.
//
// --widen <KiB> asks the OS for that much pipe buffer first, the way
// update_stream() does (1024 KiB by default, update_stream's own
// default); 0 leaves the pipe alone. Only Linux has the knob
// (F_SETPIPE_SZ). A drain that does not widen reads a different pipe
// from a blake3ppsum that does, and is no ceiling for it. The summary's
// `pipe=` is the buffer in force, or `na` where it cannot be asked.
//
// --close-after N reads N bytes, closes standard input and exits 0. The
// writer is left with a pipe that has no reader, which it meets as a
// broken pipe: EPIPE or SIGPIPE on POSIX, ERROR_NO_DATA on Windows.
// N = 0 closes before reading anything. --mark PATH creates PATH once the
// end is closed, so another process can wait for that (pump --wait-for)
// instead of for a delay.
//
// The summary's `end=` says how the stream ended: `eof` for a zero-byte
// read, `code-<n>` with the platform's code for a read that failed, or
// `closed-by-reader` under --close-after. On Windows a writer's close
// shows up as ERROR_BROKEN_PIPE, `code-109`, which is how a test tells
// whether a terminated writer looks any different from a clean close.
//
// --buf is the ask, and on Windows the ask is priced: a ReadFile costs
// about 2.6 us per MiB of range it does not fill (`pipeprobe price`). A
// drain that asks for far more than the pipe holds is measuring that, not
// the pipe; match --buf to the producer's write when a ceiling is wanted.
//
// Probes by platform:
//   poll      POSIX: poll() with a zero timeout
//   fionread  POSIX: ioctl(FIONREAD)
//   peek      Windows: PeekNamedPipe
//   ntq       Windows: NtQueryInformationFile, FilePipeLocalInformation

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>

#include "pipe_os.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#if defined(__sun)
#include <sys/filio.h>
#endif
#endif

namespace {

using clock_type = std::chrono::steady_clock;

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             clock_type::now().time_since_epoch())
      .count();
}

#if defined(_WIN32)
// The ntdll entry point, looked up at run time so nothing links ntdll.lib.
struct pipe_local_info {
  ULONG type, configuration, max_instances, cur_instances, inbound_quota,
      read_data_available, outbound_quota, write_quota_available, state, end;
};
struct io_status {
  void* status_or_pointer;
  ULONG_PTR information;
};
using ntqif_fn = LONG(WINAPI*)(HANDLE, io_status*, void*, ULONG, ULONG);
constexpr ULONG file_pipe_local_information = 24;

ntqif_fn load_ntqif() {
  HMODULE nt = GetModuleHandleW(L"ntdll.dll");
  if (nt == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<ntqif_fn>(
      reinterpret_cast<void*>(GetProcAddress(nt, "NtQueryInformationFile")));
}
#endif

// Whether a read would return now. A probe that fails says yes: the read
// is what reports an end or an error, and it must be allowed to.
class prober {
 public:
  bool set(const std::string& name) {
#if defined(_WIN32)
    if (name == "peek") {
      kind_ = kind::peek;
      return true;
    }
    if (name == "ntq") {
      ntq_ = load_ntqif();
      kind_ = kind::ntq;
      return ntq_ != nullptr;
    }
#else
    if (name == "poll") {
      kind_ = kind::poll;
      return true;
    }
    if (name == "fionread") {
      kind_ = kind::fionread;
      return true;
    }
#endif
    return false;
  }

  bool ready(pipebench::handle h) const {
    switch (kind_) {
#if defined(_WIN32)
      case kind::peek: {
        DWORD avail = 0;
        if (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) == 0) {
          return true;
        }
        return avail != 0;
      }
      case kind::ntq: {
        io_status iosb{};
        pipe_local_info info{};
        if (ntq_(h, &iosb, &info, sizeof info, file_pipe_local_information) != 0) {
          return true;
        }
        return info.read_data_available != 0;
      }
#else
      case kind::poll: {
        pollfd p{.fd = h, .events = POLLIN, .revents = 0};
        return ::poll(&p, 1, 0) != 0;
      }
      case kind::fionread: {
        int avail = 0;
        if (::ioctl(h, FIONREAD, &avail) != 0) {
          return true;
        }
        return avail != 0;
      }
#endif
      default:
        return true;
    }
  }

 private:
  enum class kind { none, poll, fionread, peek, ntq };
  kind kind_ = kind::none;
#if defined(_WIN32)
  ntqif_fn ntq_ = nullptr;
#endif
};

struct tally {
  std::uint64_t bytes = 0, reads = 0, probes = 0, caught = 0, slept = 0;
  bool error = false;
  bool eof = false;
  unsigned long end_code = 0;
};

// Records how the stream ended.
void finish(tally& t, const pipebench::read_result& r) {
  t.error = r.error;
  t.eof = r.eof;
  t.end_code = r.code;
}

// One reader, reading until the end.
void plain_reader(pipebench::handle h, std::size_t buf_bytes, tally& t) {
  std::vector<unsigned char> buf(buf_bytes, 1);  // resident before the clock
  for (;;) {
    const pipebench::read_result r = pipebench::read_some(h, buf.data(), buf.size());
    if (r.eof || r.error) {
      finish(t, r);
      return;
    }
    t.bytes += r.bytes;
    ++t.reads;
  }
}

// Reads exactly `limit` bytes (fewer if the stream ends first), then
// closes standard input under the writer.
void closing_reader(pipebench::handle h, std::size_t buf_bytes, std::uint64_t limit,
                    const std::string& mark, tally& t) {
  std::vector<unsigned char> buf(buf_bytes, 1);
  while (t.bytes < limit) {
    const std::uint64_t left = limit - t.bytes;
    const std::size_t ask = left < buf.size() ? static_cast<std::size_t>(left) : buf.size();
    const pipebench::read_result r = pipebench::read_some(h, buf.data(), ask);
    if (r.eof || r.error) {
      finish(t, r);
      return;
    }
    t.bytes += r.bytes;
    ++t.reads;
  }
  pipebench::close_std_in();
  if (!mark.empty()) {
    if (std::FILE* f = pipebench::open_file(mark.c_str(), "wb")) {
      std::fclose(f);
    }
  }
}

// One reader with the watch in front of each read.
void watching_reader(pipebench::handle h, std::size_t buf_bytes,
                     const prober& probe, std::int64_t ceiling_ns,
                     std::int64_t interval_ns, tally& t) {
  std::vector<unsigned char> buf(buf_bytes, 1);
  const std::int64_t step_ns = std::max<std::int64_t>(1, ceiling_ns / 8);
  const std::int64_t floor_ns = interval_ns;
  std::int64_t budget = ceiling_ns;
  for (;;) {
    ++t.probes;
    if (!probe.ready(h)) {
      const std::int64_t t0 = now_ns();
      const std::int64_t b = budget;
      std::int64_t last = t0;
      bool hit = false;
      for (;;) {
        std::int64_t now = now_ns();
        while (now - last < interval_ns) {
          pipebench::relax();
          now = now_ns();
        }
        last = now;
        ++t.probes;
        if (probe.ready(h)) {
          budget = std::min(ceiling_ns, b + step_ns);
          ++t.caught;
          hit = true;
          break;
        }
        if (now - t0 >= b) {
          break;
        }
      }
      if (!hit) {
        budget = std::max(floor_ns, b / 2);
        ++t.slept;
        const pipebench::read_result r = pipebench::read_some(h, buf.data(), buf.size());
        if (r.eof || r.error) {
          finish(t, r);
          return;
        }
        t.bytes += r.bytes;
        ++t.reads;
        if (now_ns() - t0 < ceiling_ns) {
          budget = std::min(ceiling_ns, b + step_ns);
        }
        continue;
      }
    }
    const pipebench::read_result r = pipebench::read_some(h, buf.data(), buf.size());
    if (r.eof || r.error) {
      finish(t, r);
      return;
    }
    t.bytes += r.bytes;
    ++t.reads;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t buf_kib = 1024;
  unsigned threads = 1;
  std::string watch;
  std::int64_t budget_us = 16;
  std::int64_t interval_ns = 1000;
  std::uint64_t close_after = 0;
  std::size_t widen_kib = 1024;
  std::string mark;

  CLI::App app{"drain: read stdin to the end and report what the reads did"};
  app.add_option("--buf", buf_kib, "bytes asked for per read, in KiB")
      ->check(CLI::PositiveNumber);
  auto* thr = app.add_option("--threads", threads,
                             "concurrent blocking readers; 2 or more never leave the pipe without a read posted")
                  ->check(CLI::Range(1u, 64u));
#if defined(_WIN32)
  const char* probes = "peek|ntq";
#else
  const char* probes = "poll|fionread";
#endif
  auto* wat = app.add_option("--watch", watch,
                             std::string("probe an empty pipe before each read: ") + probes);
  wat->excludes(thr);
  app.add_option("--budget", budget_us, "watch ceiling in microseconds")
      ->check(CLI::NonNegativeNumber);
  app.add_option("--interval", interval_ns, "nanoseconds between probes")
      ->check(CLI::PositiveNumber);
  app.add_option("--widen", widen_kib, "pipe buffer to ask the OS for, in KiB (Linux); 0 leaves it");
  auto* close_opt = app.add_option("--close-after", close_after,
                                   "read this many bytes, then close stdin under the writer and exit 0");
  close_opt->excludes(thr)->excludes(wat);
  app.add_option("--mark", mark, "with --close-after: create this file once stdin is closed")
      ->needs(close_opt);
  CLI11_PARSE(app, argc, argv);

  prober probe;
  if (!watch.empty() && !probe.set(watch)) {
    std::fprintf(stderr, "drain: probe '%s' is not available here (%s)\n",
                 watch.c_str(), probes);
    return 2;
  }

  const pipebench::handle in = pipebench::std_in();
  const std::size_t buf_bytes = buf_kib * 1024;
  long pipe_bytes = -1;  // the buffer in force, where it can be asked
#if defined(F_SETPIPE_SZ) && defined(F_GETPIPE_SZ)
  if (widen_kib > 0) {
    ::fcntl(in, F_SETPIPE_SZ, static_cast<int>(widen_kib * 1024));
  }
  pipe_bytes = ::fcntl(in, F_GETPIPE_SZ);
#else
  (void)widen_kib;
#endif
  std::vector<tally> tallies(threads);
  const auto start = clock_type::now();
  if (close_opt->count() > 0) {
    closing_reader(in, buf_bytes, close_after, mark, tallies[0]);
  } else if (!watch.empty()) {
    watching_reader(in, buf_bytes, probe, budget_us * 1000, interval_ns, tallies[0]);
  } else if (threads == 1) {
    plain_reader(in, buf_bytes, tallies[0]);
  } else {
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i) {
      pool.emplace_back(plain_reader, in, buf_bytes, std::ref(tallies[i]));
    }
    for (auto& t : pool) {
      t.join();
    }
  }
  const double s = std::chrono::duration<double>(clock_type::now() - start).count();

  tally sum;
  for (const tally& t : tallies) {
    sum.bytes += t.bytes;
    sum.reads += t.reads;
    sum.probes += t.probes;
    sum.caught += t.caught;
    sum.slept += t.slept;
    sum.error = sum.error || t.error;
    if (t.eof || t.error) sum.end_code = t.end_code;
  }
  const double mib = static_cast<double>(sum.bytes) / 1048576.0;
  std::fprintf(stderr,
               "drain: bytes=%llu in %.3f s = %.1f MiB/s | reads=%llu bytes_per_read=%.0f",
               static_cast<unsigned long long>(sum.bytes), s, s > 0 ? mib / s : 0.0,
               static_cast<unsigned long long>(sum.reads),
               sum.reads != 0 ? static_cast<double>(sum.bytes) / static_cast<double>(sum.reads) : 0.0);
  if (!watch.empty()) {
    std::fprintf(stderr, " | probes=%llu caught=%llu slept=%llu",
                 static_cast<unsigned long long>(sum.probes),
                 static_cast<unsigned long long>(sum.caught),
                 static_cast<unsigned long long>(sum.slept));
  }
  if (pipe_bytes > 0) {
    std::fprintf(stderr, " | pipe=%ld", pipe_bytes);
  } else {
    std::fprintf(stderr, " | pipe=na");
  }
  if (close_opt->count() > 0) {
    std::fprintf(stderr, " | end=closed-by-reader");
  } else if (sum.end_code == 0) {
    std::fprintf(stderr, " | end=eof");
  } else {
    std::fprintf(stderr, " | end=code-%lu", sum.end_code);
  }
  std::fprintf(stderr, "\n");
  if (sum.error) {
    std::fprintf(stderr, "drain: a read failed\n");
    return 1;
  }
  return 0;
}
