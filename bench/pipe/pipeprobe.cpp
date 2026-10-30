// pipeprobe: re-check the facts about pipes that the stream reader's
// comments rely on (src/io/stream_reader.cpp, detail/pipe_driver.hpp).
//
//   pipeprobe depth [--ms <n>]        as the consumer of a pipe: how much
//                                     the pipe holds with nothing reading
//   pipeprobe cost  [--spin <n>]      what one readiness probe, one clock
//                                     read and one busy-wait hint cost
//   pipeprobe price [--trials <n>]    whether a read is priced by the
//                                     range it asks for or by the bytes
//                                     it returns
//
// depth reads nothing for --ms milliseconds while it polls the queued
// count, then drains. Run it behind a producer of known write size:
//   pump --gen 1 --chunk 256 | pipeprobe depth
// The claim it checks: a Windows anonymous pipe has a 4096-byte quota,
// a write past it does not return until a reader drains it, and the
// readable depth is about one producer write. On Linux the buffer is
// what F_GETPIPE_SZ reports.
//
// cost times each call over many repetitions on a pipe of its own, empty
// and with bytes queued. --spin keeps that many threads busy meanwhile,
// so that other runnable threads are part of the measurement. The
// figures it checks: 77-290 ns for poll() on Linux, which the stream
// watch's pacing assumes, and the cost of PeekNamedPipe and
// NtQueryInformationFile on Windows, where the stream is not watched.
//
// price makes a fresh pipe per trial, queues exactly N bytes in it and
// times one read that asks for N, 2N, ... up to 8 MiB; every arm returns
// the same N bytes. The asks rotate per trial and the rotation reverses
// on odd trials. A fresh pipe each time because a second write into a
// Windows quota-0 pipe that nothing has drained blocks for ever. The
// claim it checks: a Windows ReadFile costs about 2.6 us per MiB of range
// it does not fill, and a POSIX read() costs nothing for it. A pipe that
// cannot hold N bytes (a POSIX pipe's capacity) skips that N and says so.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ratio>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>

#include "pipe_os.hpp"

#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

using clock_type = std::chrono::steady_clock;

double ns_since(clock_type::time_point t0) {
  return std::chrono::duration<double, std::nano>(clock_type::now() - t0).count();
}

// ---- the platform: an owned pipe, and how much is queued in it --------

#if defined(_WIN32)
struct pipe_local_info {
  ULONG type, configuration, max_instances, cur_instances, inbound_quota,
      read_data_available, outbound_quota, write_quota_available, state, end;
};
struct io_status {
  void* status_or_pointer;
  ULONG_PTR information;
};
using ntqif_fn = LONG(WINAPI*)(HANDLE, io_status*, void*, ULONG, ULONG);

ntqif_fn ntqif() {
  static const ntqif_fn fn = [] {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt == nullptr ? ntqif_fn{nullptr}
                         : reinterpret_cast<ntqif_fn>(reinterpret_cast<void*>(
                               GetProcAddress(nt, "NtQueryInformationFile")));
  }();
  return fn;
}
#endif

struct owned_pipe {
#if defined(_WIN32)
  pipebench::handle rd = nullptr, wr = nullptr;
#else
  pipebench::handle rd = -1, wr = -1;
#endif
  bool open() {
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    return CreatePipe(&rd, &wr, &sa, 0) != 0;
#else
    int fds[2];
    if (::pipe(fds) != 0) {
      return false;
    }
    rd = fds[0];
    wr = fds[1];
    return true;
#endif
  }
  void close_write() {
#if defined(_WIN32)
    if (wr != nullptr) CloseHandle(wr);
    wr = nullptr;
#else
    if (wr >= 0) ::close(wr);
    wr = -1;
#endif
  }
  void close_read() {
#if defined(_WIN32)
    if (rd != nullptr) CloseHandle(rd);
    rd = nullptr;
#else
    if (rd >= 0) ::close(rd);
    rd = -1;
#endif
  }
  ~owned_pipe() {
    close_write();
    close_read();
  }
};

// Bytes queued and readable now, or -1 where it cannot be asked.
long long queued(pipebench::handle h) {
#if defined(_WIN32)
  DWORD avail = 0;
  if (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) == 0) {
    return -1;
  }
  return static_cast<long long>(avail);
#else
  int avail = 0;
  if (::ioctl(h, FIONREAD, &avail) != 0) {
    return -1;
  }
  return avail;
#endif
}

// ---- depth -------------------------------------------------------------

int depth(int ms) {
  const pipebench::handle in = pipebench::std_in();
#if defined(_WIN32)
  DWORD flags = 0, outbuf = 0, inbuf = 0, instances = 0;
  if (GetNamedPipeInfo(in, &flags, &outbuf, &inbuf, &instances) != 0) {
    std::printf("reported buffer: in=%lu out=%lu (GetNamedPipeInfo)\n", inbuf, outbuf);
  } else {
    std::printf("reported buffer: not a pipe (GetNamedPipeInfo failed)\n");
  }
#elif defined(F_GETPIPE_SZ)
  std::printf("reported buffer: %d (F_GETPIPE_SZ)\n", ::fcntl(in, F_GETPIPE_SZ));
#else
  std::printf("reported buffer: not reported on this platform\n");
#endif
  long long high = -1;
  for (int waited = 0; waited < ms; waited += 25) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    high = std::max(high, queued(in));
  }
  if (high < 0) {
    std::printf("queued high-water: cannot be asked on this handle\n");
  } else {
    std::printf("queued high-water: %lld bytes (%.1f KiB) with nothing reading\n",
                high, static_cast<double>(high) / 1024.0);
  }
  std::vector<unsigned char> buf(1 << 20);
  unsigned long long total = 0;
  for (;;) {
    const pipebench::read_result r = pipebench::read_some(in, buf.data(), buf.size());
    if (r.eof || r.error) break;
    total += r.bytes;
  }
  std::printf("drained %llu bytes\n", total);
  std::fflush(stdout);
  return 0;
}

// ---- cost --------------------------------------------------------------

template <class F>
double per_call_ns(int n, F f) {
  const auto t0 = clock_type::now();
  for (int i = 0; i < n; ++i) f();
  return ns_since(t0) / n;
}

int cost(unsigned spin) {
  std::atomic<bool> stop{false};
  std::vector<std::thread> load;
  for (unsigned i = 0; i < spin; ++i) {
    load.emplace_back([&stop] {
      while (!stop.load(std::memory_order_relaxed)) pipebench::relax();
    });
  }
  owned_pipe p;
  if (!p.open()) {
    std::fprintf(stderr, "pipeprobe: cannot make a pipe\n");
    return 1;
  }
  const int n = 200000;
  std::printf("busy threads: %u\n", spin);
  for (int queued_bytes : {0, 64}) {
    if (queued_bytes > 0) {
      unsigned char b[64] = {};
      pipebench::write_all(p.wr, b, sizeof b);
    }
    const char* state = queued_bytes == 0 ? "empty" : "64 B queued";
#if defined(_WIN32)
    std::printf("%-36s: %8.1f ns\n", (std::string("PeekNamedPipe, ") + state).c_str(), per_call_ns(n, [&] {
                  DWORD a = 0;
                  PeekNamedPipe(p.rd, nullptr, 0, nullptr, &a, nullptr);
                }));
    if (ntqif_fn fn = ntqif()) {
      std::printf("%-36s: %8.1f ns\n", (std::string("NtQueryInformationFile, ") + state).c_str(), per_call_ns(n, [&] {
                    io_status s{};
                    pipe_local_info info{};
                    fn(p.rd, &s, &info, sizeof info, 24);
                  }));
    }
#else
    std::printf("%-36s: %8.1f ns\n", (std::string("poll(0), ") + state).c_str(), per_call_ns(n, [&] {
                  pollfd q{.fd = p.rd, .events = POLLIN, .revents = 0};
                  ::poll(&q, 1, 0);
                }));
    std::printf("%-36s: %8.1f ns\n", (std::string("ioctl(FIONREAD), ") + state).c_str(), per_call_ns(n, [&] {
                  int a = 0;
                  ::ioctl(p.rd, FIONREAD, &a);
                }));
#endif
  }
  std::printf("%-36s: %8.1f ns\n", "steady_clock::now", per_call_ns(n, [] {
                const auto t = clock_type::now();
                std::atomic_signal_fence(std::memory_order_seq_cst);
                (void)t;
              }));
  std::printf("%-36s: %8.2f ns\n", "busy-wait hint",
              per_call_ns(n * 10, [] { pipebench::relax(); }));
  stop = true;
  for (auto& t : load) t.join();
  return 0;
}

// ---- price -------------------------------------------------------------

int price(int trials) {
  constexpr std::size_t K = 1024, M = 1024 * 1024;
  const std::vector<std::size_t> holds = {64 * K, 256 * K, 1 * M};
  std::vector<unsigned char> dest(16 * M, 1);  // resident, like the driver's arena
  std::printf("hold_bytes,ask_bytes,got_bytes,n,min_ns,median_ns,p90_ns\n");
  for (const std::size_t hold : holds) {
    std::vector<std::size_t> asks;
    for (std::size_t a = hold; a <= 8 * M; a *= 2) asks.push_back(a);
    std::vector<unsigned char> src(hold, 7);
    std::vector<std::vector<double>> samples(asks.size());
    bool fits = true;
    for (int t = 0; t < trials && fits; ++t) {
      for (std::size_t s = 0; s < asks.size() && fits; ++s) {
        const std::size_t r = (s + static_cast<std::size_t>(t)) % asks.size();
        const std::size_t k = t % 2 == 0 ? r : asks.size() - 1 - r;
        owned_pipe p;
        if (!p.open()) return 1;
#if defined(F_SETPIPE_SZ)
        ::fcntl(p.wr, F_SETPIPE_SZ, static_cast<int>(hold));
#endif
        // The writer may block until the read below drains it (a write
        // past a Windows pipe's quota does), so it has its own thread and
        // the trial waits on the queue, not on the write.
        std::thread writer([&] { pipebench::write_all(p.wr, src.data(), src.size()); });
        const auto t0 = clock_type::now();
        long long q = 0;
        while ((q = queued(p.rd)) < static_cast<long long>(hold)) {
          if (ns_since(t0) > 2e9) {
            fits = false;
            break;
          }
          pipebench::relax();
        }
        if (!fits) {
          std::printf("%zu,skipped: the pipe never queued more than %lld bytes\n", hold, q);
          p.close_read();  // fails the blocked write, so the writer returns
          writer.join();
          break;
        }
        const auto a = clock_type::now();
        const pipebench::read_result got = pipebench::read_some(p.rd, dest.data(), asks[k]);
        const double ns = ns_since(a);
        p.close_read();
        writer.join();
        if (got.bytes == hold) samples[k].push_back(ns);
      }
    }
    if (!fits) continue;
    for (std::size_t k = 0; k < asks.size(); ++k) {
      std::vector<double>& v = samples[k];
      if (v.empty()) continue;
      std::sort(v.begin(), v.end());
      std::printf("%zu,%zu,%zu,%zu,%.0f,%.0f,%.0f\n", hold, asks[k], hold, v.size(),
                  v.front(), v[v.size() / 2], v[v.size() * 9 / 10]);
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"pipeprobe: re-check the pipe facts the stream reader relies on"};
  app.require_subcommand(1);
  int ms = 1000;
  unsigned spin = 0;
  int trials = 100;
  auto* d = app.add_subcommand("depth", "as a pipe's consumer: how much it holds with nothing reading");
  d->add_option("--ms", ms, "how long to watch before draining")->check(CLI::PositiveNumber);
  auto* c = app.add_subcommand("cost", "per-call cost of a readiness probe, a clock read and a busy-wait hint");
  c->add_option("--spin", spin, "threads kept busy meanwhile")->check(CLI::Range(0u, 64u));
  auto* pr = app.add_subcommand("price", "read cost against the range asked, on a pipe holding a fixed amount");
  pr->add_option("--trials", trials, "trials per ask")->check(CLI::PositiveNumber);
  CLI11_PARSE(app, argc, argv);
#if !defined(_WIN32)
  // price ends a write that cannot complete by closing the read end; the
  // writer must see EPIPE, not die of SIGPIPE.
  std::signal(SIGPIPE, SIG_IGN);
#endif
  if (d->parsed()) return depth(ms);
  if (c->parsed()) return cost(spin);
  return price(trials);
}
