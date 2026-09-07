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

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <unistd.h>  // sync(): flushes dirty pages before a cache drop
#endif

#include <CLI/CLI.hpp>
#include <blake3pp/io.hpp>
#include <blake3pp/parallel_io.hpp>

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

// Best-effort page-cache drop (vm.drop_caches=3). Wants root; with the
// default direct I/O the cache is bypassed anyway, so a failed drop only
// leaves metadata/readahead state warm; reported once, not fatal.
bool drop_caches() {
#if defined(__linux__)
  ::sync();
  std::ofstream f("/proc/sys/vm/drop_caches");
  if (!f.is_open()) {
    return false;
  }
  f << "3" << std::flush;
  return f.good();
#else
  return false;
#endif
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
#if defined(BLAKE3PP_EXECUTION_STDEXEC)
        ,
        pool_(threads_)
#endif
  {
  }

  [[nodiscard]] unsigned count() const noexcept { return threads_; }

  [[nodiscard]] blake3pp::parallel_scheduler_t scheduler() {
#if defined(BLAKE3PP_EXECUTION_STDEXEC)
    return pool_.get_scheduler();
#else
    return blake3pp::get_parallel_scheduler();
#endif
  }

 private:
  unsigned threads_;
#if defined(BLAKE3PP_EXECUTION_STDEXEC)
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
          cold ? "vm.drop_caches=3 before every combo"
               : "not root; direct I/O bypasses the page cache anyway, but "
                 "metadata/readahead state stays warm");

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
      if (cold) {
        drop_caches();
      }
      opts.window_bytes = w * 1024 * 1024;
      opts.queue_depth = qd;
      const double secs = b3tool::best_seconds(reps, /*warmup=*/false, [&] {
        if (seq_only) {
          (void)blake3pp::hash_file(path.c_str(), opts);
        } else {
          (void)blake3pp::hash_file(path.c_str(), sched, opts);
        }
      });
      const double gibs =
          b3tool::gib_per_s(static_cast<std::size_t>(bytes), secs);
      std::printf("  %6.2f GiB/s", gibs);
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
        {opts.window_bytes, opts.queue_depth, opts.direct_io, true});
    bytes = probe.file_size();
    println(stdout, "file: {} ({:.1f} MiB), backend: {}, window {} MiB, qd {}",
            path, static_cast<double>(bytes) / (1024.0 * 1024.0),
            probe.backend(), opts.window_bytes >> 20, opts.queue_depth);
  }
  if (sweep) {
    io_sweep(path, bytes, opts, reps, cooldown_s, seq_only, pool_threads);
    return 0;
  }

  const auto gibs = [&](double secs) {
    return b3tool::gib_per_s(static_cast<std::size_t>(bytes), secs);
  };

  // The raw-io pass below runs unconditionally and heats the machine, so
  // the first hashing measurement must cool down too.
  b3tool::cooldown cooldown(cooldown_s, /*skip_first=*/false);

  // The control group: the identical pipeline delivering windows that are
  // simply released unread. This is the device ceiling as seen through
  // this backend/window/qd; every hash row below is a fraction of it.
  const double raw_s = b3tool::best_seconds(reps, /*warmup=*/false, [&] {
    blake3pp::detail::file_reader r(
        path.c_str(),
        {opts.window_bytes, opts.queue_depth, opts.direct_io, true});
    auto w = r.next();
    while (w.has_value()) {
      r.release(w.value());
      w = r.next();
    }
  });
  println(stdout, "{:<10} {:8.2f} GiB/s   [device ceiling, no hashing]",
          "raw io", gibs(raw_s));

  const auto run = [&](const char* label, auto&& fn) {
    cooldown();
    blake3pp::digest d{};
    const double best =
        b3tool::best_seconds(reps, /*warmup=*/false, [&] { d = fn(); });
    println(stdout, "{:<10} {:8.2f} GiB/s   ({}...)  [{:3.0f}% of raw]", label,
            gibs(best), d.to_hex().substr(0, 16), 100.0 * raw_s / best);
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
