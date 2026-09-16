// End-to-end file hashing throughput: the number the whole project is
// about. First measures the RAW device read speed through the identical
// windowed pipeline (same backend, window, queue depth; data delivered
// and discarded, no hashing), then hashes for real, sequential and
// parallel. The utilization column relates the two: 100% means the drive,
// not the hash, is the limit.
//
//   blake3pp_bench_file <FILE> [--reps N] [--window MiB] [--qd N]
//                       [--no-direct] [--seq-only] [--cooldown S]
//   blake3pp_bench_file <FILE> --io-sweep   # window x qd pacing matrix
//   blake3pp_bench_file --make <MiB>   # create a test file and use it
//
// Note: with direct I/O the page cache is bypassed, so repetitions measure
// the device (or the host-side cache of a virtualized disk), not RAM.
//
// --io-sweep exists because the right window/qd is a property of the
// DEVICE (bandwidth x latency), not the CPU: the 8 MiB x qd4 defaults
// were tuned on low-latency NVMe and measured 23-25% slow on GCP pd-class
// volumes, where 32x16 recovered it. The sweep is
// the stopwatch that settles it per machine.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <ios>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <unistd.h>  // sync(): flushes dirty pages before a cache drop
#endif

#include <CLI/CLI.hpp>
#include <blake3pp/io.hpp>
#include <blake3pp/parallel_io.hpp>
#if defined(BLAKE3PP_HAS_SIZED_SCHEDULER)
#include <blake3pp/parallel_backend.hpp>
#endif

#include "tool_common.hpp"

namespace {

using b3tool::println;

// Writes a deterministic file of `mib` MiB and returns its path.
std::string make_test_file(std::size_t mib) {
  const std::string path = "blake3pp_bench_file.dat";
  std::ofstream out(path, std::ios::binary);
  std::vector<char> block(1024 * 1024);
  for (std::size_t k = 0; k < block.size(); ++k) {
    block[k] = static_cast<char>(k % 251);
  }
  for (std::size_t m = 0; m < mib; ++m) {
    out.write(block.data(), static_cast<std::streamsize>(block.size()));
  }
  return path;
}

// Best-effort cache drop before every timed rep, so each rep reads the
// device: vm.drop_caches=3 on Linux, purge(8) on macOS, and on Windows
// the sequence RAMMap uses (empty the working sets, flush the modified
// list, purge the standby list). Wants root, or an elevated token on
// Windows. With direct I/O a failed drop only leaves metadata and
// readahead state warm; with --no-direct it leaves the data itself warm.
bool drop_caches() {
#if defined(__linux__)
  ::sync();
  std::ofstream f("/proc/sys/vm/drop_caches");
  if (!f.is_open()) {
    return false;
  }
  f << "3" << std::flush;
  return f.good();
#elif defined(__APPLE__)
  return std::system("/usr/sbin/purge 2>/dev/null") == 0;
#elif defined(_WIN32)
#pragma comment(lib, "advapi32.lib")
  // The memory-list calls need SeProfileSingleProcessPrivilege, and
  // AdjustTokenPrivileges reports a token without it through
  // ERROR_NOT_ALL_ASSIGNED rather than by failing.
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    return false;
  }
  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  const bool enabled =
      ::LookupPrivilegeValueW(nullptr, L"SeProfileSingleProcessPrivilege",
                              &tp.Privileges[0].Luid) &&
      ::AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) &&
      ::GetLastError() == ERROR_SUCCESS;
  ::CloseHandle(token);
  using set_information_fn = LONG(NTAPI*)(ULONG, PVOID, ULONG);
  const auto set_information = reinterpret_cast<set_information_fn>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtSetSystemInformation"));
  if (!enabled || set_information == nullptr) {
    return false;
  }
  constexpr ULONG system_memory_list_information = 80;
  // MemoryEmptyWorkingSets, MemoryFlushModifiedList, MemoryPurgeStandbyList
  for (ULONG command : {2UL, 3UL, 4UL}) {
    if (set_information(system_memory_list_information, &command, sizeof command) < 0) {
      return false;
    }
  }
  return true;
#else
  return false;
#endif
}

// best_seconds with the cache dropped before every rep, outside the
// timed region; without the per-rep drop, a --no-direct run's first rep
// warms the page cache and every later rep reports RAM speed.
template <class F>
double best_cold_seconds(int reps, bool cold, F&& fn) {
  double best = 1e100;
  for (int r = 0; r < reps; ++r) {
    if (cold) {
      drop_caches();
    }
    best = std::min(best, b3tool::best_seconds(1, /*warmup=*/false, fn));
  }
  return best;
}

// --threads resolved to a scheduler: an owned pool of exactly that size
// under stdexec, the process-wide scheduler elsewhere. The option exists
// as a DIAGNOSTIC for the sync-tier oversubscription question (the pread
// fallback's reader is an implicit extra thread the pool doesn't know
// about, i.e. 11 runnable threads on a 10-core phone): sweep N-1/N/N+1 to
// separate reader starvation from other shortfalls.
class engine_threads {
 public:
  explicit engine_threads(unsigned threads)
      : threads_(threads != 0 ? threads : std::thread::hardware_concurrency())
#if defined(BLAKE3PP_EXECUTION_STDEXEC) && !defined(BLAKE3PP_HAS_SIZED_SCHEDULER)
        ,
        pool_(threads_)
#endif
  {
#if defined(BLAKE3PP_HAS_SIZED_SCHEDULER)
    // The process scheduler, sized for every provider alike.
    blake3pp::size_parallel_scheduler(threads_);
#endif
  }

  [[nodiscard]] unsigned count() const noexcept { return threads_; }

  [[nodiscard]] blake3pp::parallel_scheduler_t scheduler() {
#if defined(BLAKE3PP_HAS_SIZED_SCHEDULER)
    return blake3pp::get_parallel_scheduler();
#elif defined(BLAKE3PP_EXECUTION_STDEXEC)
    return pool_.get_scheduler();
#else
    return blake3pp::get_parallel_scheduler();
#endif
  }

 private:
  unsigned threads_;
#if defined(BLAKE3PP_EXECUTION_STDEXEC) && !defined(BLAKE3PP_HAS_SIZED_SCHEDULER)
  exec::static_thread_pool pool_;
#endif
};

// The window x qd pacing matrix: same measurement as the main rows, swept.
void io_sweep(const std::string& path, std::uint64_t bytes,
              blake3pp::hash_file_options opts, int reps, double cooldown_s,
              bool seq_only, unsigned pool_threads) {
  constexpr std::size_t windows[] = {8, 16, 32, 64};
  constexpr unsigned depths[] = {4, 8, 16, 32};

  const bool cold = drop_caches();
  println(stdout,
          "window x qd sweep: {} hash of {:.0f} MiB, best of {}, {}\n"
          "cache drops {} ({})\n",
          seq_only ? "sequential" : "parallel",
          static_cast<double>(bytes) / (1024.0 * 1024.0), reps,
          opts.direct_io ? "direct I/O" : "--no-direct",
          cold ? "active" : "unavailable",
          cold ? "before every rep"
               : "needs root or elevation; direct I/O bypasses the page "
                 "cache anyway, but metadata/readahead state stays warm");

  std::printf("  %10s", "window\\qd");
  for (const unsigned qd : depths) {
    std::printf("  qd=%-2u        ", qd);
  }
  std::printf("\n");

  b3tool::cooldown cooldown(cooldown_s);
  engine_threads engine(pool_threads);
  auto sched = engine.scheduler();
  double best_gibs = 0.0;
  double default_gibs = 0.0;
  std::size_t best_w = 0;
  unsigned best_qd = 0;

  for (const std::size_t w : windows) {
    std::printf("  %6zu MiB", w);
    for (const unsigned qd : depths) {
      cooldown();
      opts.window_bytes = w * 1024 * 1024;
      opts.queue_depth = qd;
      const double secs = best_cold_seconds(reps, cold, [&] {
        if (seq_only) {
          (void)blake3pp::hash_file(path.c_str(), opts);
        } else {
          (void)blake3pp::hash_file(path.c_str(), sched, opts);
        }
      });
      const double gibs =
          b3tool::gib_per_s(static_cast<std::size_t>(bytes), secs);
      std::printf("  %s",
                  b3tool::rate(static_cast<std::size_t>(bytes), secs).c_str());
      std::fflush(stdout);
      if (gibs > best_gibs) {
        best_gibs = gibs;
        best_w = w;
        best_qd = qd;
      }
      if (w == 8 && qd == 4) {
        default_gibs = gibs;
      }
    }
    std::printf("\n");
  }

  println(stdout,
          "\n  best: window {} MiB, qd {} ({:.2f} GiB/s), {:+.0f}% vs the "
          "8 MiB x qd4 default ({:.2f} GiB/s)",
          best_w, best_qd, best_gibs,
          default_gibs > 0.0 ? 100.0 * (best_gibs / default_gibs - 1.0) : 0.0,
          default_gibs);
  println(stdout,
          "  (a basin, not a slope: oversized windows fight the hash for "
          "cache, over-deep queues add latency)");
}

}  // namespace

int main(int argc, char** argv) {
  b3tool::tool_startup();
  std::string path;
  int reps = 3;
  double cooldown_s = 5.0;
  std::size_t make_mib = 0;
  unsigned pool_threads = b3tool::default_threads();
  bool seq_only = false;
  bool inline_submit = false;
  bool no_direct = false;
  blake3pp::hash_file_options opts;

  CLI::App app{
      "End-to-end file hashing throughput against the device ceiling.\n"
      "Measures the same windowed pipeline with and without hashing."};
  app.add_option("file", path, "file to hash");
  app.add_option("--make", make_mib,
                 "create a test file of this many MiB and use it");
  app.add_option("--reps", reps, "timed repetitions (best wins)")
      ->check(CLI::PositiveNumber)
      ->capture_default_str();
  app.add_option("--cooldown", cooldown_s,
                 "idle seconds between measurements (0 disables)")
      ->capture_default_str();
  app.add_option("--window", opts.window_bytes, "I/O window size in MiB")
      ->transform(b3tool::mib_to_bytes)
      ->default_str(std::to_string(opts.window_bytes >> 20));
  app.add_option("--qd", opts.queue_depth,
                 "I/O queue depth (the reader clamps it to its range)")
      ->capture_default_str();
  app.add_flag("--no-direct", no_direct,
               "keep the OS page cache (no O_DIRECT)");
  app.add_flag("--inline-submit", inline_submit,
               "issue each read inline in the submitting thread instead of "
               "on io_uring's workers (IOSQE_ASYNC off)");
  app.add_option("--threads", pool_threads,
                 "parallel-engine threads (1 = a one-thread pool; default: "
                 "all); diagnostic: sweep N-1/N/N+1 to test whether the "
                 "sync-tier reader is starving as the N+1th runnable thread")
      ->check(b3tool::at_least_one_thread)
      ->capture_default_str();
  app.add_flag("--seq-only", seq_only, "skip the parallel measurement");
  bool sweep = false;
  app.add_flag("--io-sweep", sweep,
               "sweep the window x qd pacing matrix (8-64 MiB x 4-32) "
               "instead of the standard rows; the optimum is a device "
               "property, run this per machine");
  CLI11_PARSE(app, argc, argv);

  opts.direct_io = !no_direct;
  opts.offload_submit = !inline_submit;
  if (make_mib > 0) {
    path = make_test_file(make_mib);
    println(stdout, "created {} ({} MiB)", path, make_mib);
  }
  if (path.empty()) {
    println(stderr, "blake3pp_bench_file: give a FILE, or --make <MiB>");
    return b3tool::exit_usage;
  }

  std::uint64_t bytes = 0;
  {
    blake3pp::detail::file_reader probe(
        path.c_str(),
        {opts.window_bytes, opts.queue_depth, opts.direct_io, true,
         opts.offload_submit});
    bytes = probe.file_size();
    println(stdout, "file: {} ({:.1f} MiB), backend: {}, window {} MiB, qd {}",
            path, static_cast<double>(bytes) / (1024.0 * 1024.0),
            probe.backend(), opts.window_bytes >> 20, opts.queue_depth);
  }
  if (sweep) {
    io_sweep(path, bytes, opts, reps, cooldown_s, seq_only, pool_threads);
    return 0;
  }

  const bool cold = drop_caches();
  println(stdout, "cache: {}",
          cold ? "dropped before every rep"
          : opts.direct_io
              ? "not dropped (needs root or elevation); direct I/O bypasses "
                "it, metadata stays warm"
              : "NOT DROPPED (needs root or elevation): with --no-direct, "
                "every rep after the first reads RAM, not the device");

  // The raw-io pass below runs unconditionally and heats the machine, so
  // the first hashing measurement must cool down too.
  b3tool::cooldown cooldown(cooldown_s, /*skip_first=*/false);

  // The control group: the identical pipeline delivering windows that are
  // simply released unread. This is the device ceiling as seen through
  // this backend/window/qd; every hash row below is a fraction of it.
  const double raw_s = best_cold_seconds(reps, cold, [&] {
    blake3pp::detail::file_reader r(
        path.c_str(),
        {opts.window_bytes, opts.queue_depth, opts.direct_io, true,
         opts.offload_submit});
    auto w = r.next();
    while (w.has_value()) {
      r.release(w.value());
      w = r.next();
    }
  });
  println(stdout, "{:<10} {}   [device ceiling, no hashing]",
          "raw io", b3tool::rate(static_cast<std::size_t>(bytes), raw_s));

  const auto run = [&](const char* label, auto&& fn) {
    cooldown();
    blake3pp::digest d{};
    const double best = best_cold_seconds(reps, cold, [&] { d = fn(); });
    println(stdout, "{:<10} {}   ({}...)  [{:3.0f}% of raw]", label,
            b3tool::rate(static_cast<std::size_t>(bytes), best),
            d.to_hex().substr(0, 16), 100.0 * raw_s / best);
  };

  run("seq", [&] { return blake3pp::hash_file(path.c_str(), opts); });

  if (!seq_only) {
    engine_threads engine(pool_threads);
    auto sched = engine.scheduler();
    println(stdout, "parallel:   {} threads, affinity mask: {}",
            engine.count(),
            b3tool::affinity_cpu_count() > 0
                ? std::format("{} cpus", b3tool::affinity_cpu_count())
                : std::string{"unknown"});
    run("parallel",
        [&] { return blake3pp::hash_file(path.c_str(), sched, opts); });
  }
  return 0;
}
