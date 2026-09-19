// End-to-end file hashing throughput: the number the whole project is
// about. First measures the RAW device read speed through the identical
// windowed pipeline (same backend, window, queue depth; data delivered
// and discarded, no hashing), then hashes for real, sequential and
// parallel. The utilization column relates the two: 100% means the drive,
// not the hash, is the limit.
//
//   blake3pp_bench_file <FILE> [--reps N] [--window MiB] [--qd N]
//                       [--no-direct] [--seq-only] [--cooldown S]
//                       [--trace PATH [--trace-agents]]
//   blake3pp_bench_file <FILE> --io-sweep   # window x qd pacing matrix
//   blake3pp_bench_file --make <MiB>   # create a test file and use it
//
// Note: with direct I/O the page cache is bypassed, so repetitions measure
// the device (or the host-side cache of a virtualized disk), not RAM.
//
// --trace PATH records where each window's time goes (waiting for the
// read, hashing, absorbing the chaining values, handing the buffer back)
// through the library's trace_buffer, prints a per-row summary from the
// last rep, and writes that rep as Chrome trace JSON to PATH (open in
// chrome://tracing or Perfetto; with both rows, PATH gets a .seq/.parallel
// suffix). --trace-agents adds one record per pool agent per window (the
// parts it took, its busy time, the CPU it ran on), which the summary
// turns into a per-CPU effective rate: slow cores, preemption and
// cache-cold parts show up there. bench/README.md has the details and
// the pairing with perf record.
//
// --io-sweep exists because the right window/qd is a property of the
// DEVICE (bandwidth x latency), not the CPU: the 8 MiB x qd4 defaults
// were tuned on low-latency NVMe and measured 23-25% slow on GCP pd-class
// volumes, where 32x16 recovered it. The sweep is
// the stopwatch that settles it per machine.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <unistd.h>  // sync(): flushes dirty pages before a cache drop;
                     // sysconf(_SC_CLK_TCK) for the io-wq CPU time
#endif

#include <CLI/CLI.hpp>
#include <blake3pp/io.hpp>
#include <blake3pp/parallel_io.hpp>
#include <blake3pp/trace.hpp>
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
  const auto set_information = std::bit_cast<set_information_fn>(
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
// warms the page cache and every later rep reports RAM speed. before and
// after run around each rep, outside the timed region too.
template <class F, class Before = void (*)(), class After = void (*)()>
double best_cold_seconds(
    int reps, bool cold, F&& fn, Before&& before = [] {},
    After&& after = [] {}) {
  double best = 1e100;
  for (int r = 0; r < reps; ++r) {
    if (cold) {
      drop_caches();
    }
    before();
    best = std::min(best, b3tool::best_seconds(1, /*warmup=*/false, fn));
    after();
  }
  return best;
}

// CPU ticks (user + system) of the kernel's io_uring worker threads,
// which on Linux appear as threads of this process named iou-wrk-<tgid>,
// keyed by thread id. The threads come and go with the load (a worker
// exits after idling), so a scan before and after a rep can only credit
// workers alive at both ends; one that vanished between the scans
// counts as zero. Empty where there are no such threads.
std::map<std::string, std::uint64_t> iowq_cpu_ticks() {
  std::map<std::string, std::uint64_t> ticks;
#if defined(__linux__)
  std::error_code ec;
  for (const auto& task :
       std::filesystem::directory_iterator("/proc/self/task", ec)) {
    std::ifstream comm(task.path() / "comm");
    std::string name;
    std::getline(comm, name);
    if (!name.starts_with("iou-wrk")) {
      continue;
    }
    std::ifstream stat_file(task.path() / "stat");
    std::string stat;
    std::getline(stat_file, stat);
    // The comm field is in parentheses and may contain spaces, so the
    // numeric fields are counted from the last ')': utime and stime are
    // fields 14 and 15 of the line, the 12th and 13th after it.
    const auto close = stat.rfind(')');
    if (close == std::string::npos) {
      continue;
    }
    std::uint64_t utime = 0;
    std::uint64_t stime = 0;
    if (std::sscanf(stat.c_str() + close + 1,
                    " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
                    &utime, &stime) == 2) {
      ticks[task.path().filename().string()] = utime + stime;
    }
  }
#endif
  return ticks;
}

double iowq_cpu_seconds(const std::map<std::string, std::uint64_t>& before,
                        const std::map<std::string, std::uint64_t>& after) {
  std::uint64_t delta = 0;
  for (const auto& [tid, ticks] : after) {
    const auto it = before.find(tid);
    delta += ticks - (it == before.end() ? 0 : it->second);
  }
#if defined(__linux__)
  const long hz = ::sysconf(_SC_CLK_TCK);
  return hz > 0 ? static_cast<double>(delta) / static_cast<double>(hz) : 0.0;
#else
  (void)delta;
  return 0.0;
#endif
}

// The record kinds as Chrome trace JSON, one complete ("X") event per
// phase per window on tid 1 and, with agent records, one "compress" event
// per record on a tid per CPU (100 + cpu; 99 where the CPU is unknown),
// so the viewer shows the pipeline against the cores. ts and dur are in
// microseconds as the format wants; epoch_ns at the top level is the
// records' zero as absolute CLOCK_MONOTONIC nanoseconds, for joining with
// timestamps other tools take on the same clock.
bool write_chrome_trace(const std::string& path,
                        const blake3pp::trace_buffer& trace,
                        std::string_view process_name) {
  std::ofstream out(path);
  if (!out.is_open()) {
    return false;
  }
  const auto us = [](std::int64_t ns) {
    return static_cast<double>(ns) / 1000.0;
  };
  out << std::format("{{\"epoch_ns\":{},\"displayTimeUnit\":\"ns\","
                     "\"traceEvents\":[\n",
                     trace.epoch_ns());
  out << std::format("{{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,"
                     "\"args\":{{\"name\":\"{}\"}}}},\n",
                     process_name);
  out << "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":1,"
         "\"args\":{\"name\":\"pipeline\"}}";
  for (const auto& w : trace.windows()) {
    const std::pair<const char*, std::pair<std::int64_t, std::int64_t>>
        phases[] = {{"wait", {w.t_wait_begin, w.t_ready}},
                    {"hash", {w.t_ready, w.t_joined}},
                    {"absorb", {w.t_joined, w.t_absorbed}},
                    {"release", {w.t_absorbed, w.t_released}}};
    for (const auto& [name, span] : phases) {
      out << std::format(",\n{{\"name\":\"{}\",\"ph\":\"X\",\"pid\":1,"
                         "\"tid\":1,\"ts\":{:.3f},\"dur\":{:.3f},"
                         "\"args\":{{\"window\":{}}}}}",
                         name, us(span.first), us(span.second - span.first),
                         w.index);
    }
  }
  std::map<std::uint32_t, bool> cpus_named;
  for (const auto& a : trace.agents()) {
    const std::uint32_t tid = a.cpu == ~std::uint32_t{0} ? 99 : 100 + a.cpu;
    if (!cpus_named[tid]) {
      cpus_named[tid] = true;
      out << std::format(",\n{{\"name\":\"thread_name\",\"ph\":\"M\","
                         "\"pid\":1,\"tid\":{},\"args\":{{\"name\":\"{}\"}}}}",
                         tid,
                         tid == 99 ? std::string{"cpu ?"}
                                   : std::format("cpu {}", a.cpu));
    }
    out << std::format(",\n{{\"name\":\"compress\",\"ph\":\"X\",\"pid\":1,"
                       "\"tid\":{},\"ts\":{:.3f},\"dur\":{:.3f},"
                       "\"args\":{{\"window\":{},\"parts\":{},"
                       "\"min_part_ns\":{},\"max_part_ns\":{}}}}}",
                       tid, us(a.t_begin),
                       us(static_cast<std::int64_t>(a.busy_ns)), a.window,
                       a.parts, a.min_part_ns, a.max_part_ns);
  }
  out << "\n]}\n";
  return out.good();
}

// The per-row summary of one traced rep: where the wall time went, how
// busy the pool was, and with agent records what each CPU delivered.
void print_trace_summary(const blake3pp::trace_buffer& trace,
                         unsigned threads, double iowq_cpu_s) {
  const auto windows = trace.windows();
  if (windows.empty()) {
    println(stdout, "  trace: no window records");
    return;
  }
  const double wall_ns =
      static_cast<double>(windows.back().t_released - windows.front().t_wait_begin);
  double wait = 0;
  double hash = 0;
  double absorb = 0;
  double release = 0;
  double busy = 0;
  double active = 0;
  std::size_t fanned = 0;
  for (const auto& w : windows) {
    wait += static_cast<double>(w.t_ready - w.t_wait_begin);
    hash += static_cast<double>(w.t_joined - w.t_ready);
    absorb += static_cast<double>(w.t_absorbed - w.t_joined);
    release += static_cast<double>(w.t_released - w.t_absorbed);
    busy += static_cast<double>(w.agent_busy_ns);
    if (w.flags & blake3pp::window_record::flag_parallel) {
      ++fanned;
      active += w.agents_active;
    }
  }
  const auto pct = [&](double ns) { return 100.0 * ns / wall_ns; };
  println(stdout,
          "  trace (last rep): {} windows ({} fanned out, {} dropped), wall "
          "{:.3f} s",
          windows.size(), fanned, trace.dropped_windows(), wall_ns / 1e9);
  println(stdout,
          "    wait {:5.1f}%  hash {:5.1f}%  absorb {:5.1f}%  release {:5.1f}%",
          pct(wait), pct(hash), pct(absorb), pct(release));
  if (fanned > 0) {
    println(stdout,
            "    pool busy {:5.1f}% of {} threads, {:.1f} agents active per "
            "fanned window",
            100.0 * busy / (static_cast<double>(threads) * wall_ns), threads,
            active / static_cast<double>(fanned));
  }
  println(stdout, "    io-wq cpu = {:.2f} cores (worker threads' cpu / wall)",
          iowq_cpu_s / (wall_ns / 1e9));

  const auto agents = trace.agents();
  if (agents.empty() && trace.dropped_agents() == 0) {
    return;
  }
  // Per CPU: everything its agents compressed over the time they spent.
  struct cpu_sum {
    std::uint64_t records = 0;
    std::uint64_t parts = 0;
    std::uint64_t bytes = 0;
    std::uint64_t busy_ns = 0;
  };
  std::map<std::uint32_t, cpu_sum> per_cpu;
  double worst_spread = 0.0;
  for (const auto& a : agents) {
    auto& c = per_cpu[a.cpu];
    ++c.records;
    c.parts += a.parts;
    c.bytes += static_cast<std::uint64_t>(a.parts) * a.part_chunks *
               blake3pp::chunk_size;
    c.busy_ns += a.busy_ns;
    if (a.min_part_ns > 0) {
      worst_spread = std::max(worst_spread,
                              static_cast<double>(a.max_part_ns) /
                                  static_cast<double>(a.min_part_ns));
    }
  }
  std::vector<double> rates;
  println(stdout, "    {} agent records ({} dropped) on {} cpus:",
          agents.size(), trace.dropped_agents(), per_cpu.size());
  println(stdout, "      {:>5} {:>8} {:>8} {:>10} {:>14}", "cpu", "records",
          "parts", "busy ms", "effective");
  for (const auto& [cpu, c] : per_cpu) {
    const double secs = static_cast<double>(c.busy_ns) / 1e9;
    rates.push_back(b3tool::gib_per_s(c.bytes, secs));
    println(stdout, "      {:>5} {:>8} {:>8} {:>10.1f} {}",
            cpu == ~std::uint32_t{0} ? std::string{"?"} : std::to_string(cpu),
            c.records, c.parts, static_cast<double>(c.busy_ns) / 1e6,
            b3tool::rate(c.bytes, secs));
  }
  if (!rates.empty()) {
    std::ranges::sort(rates);
    println(stdout,
            "      per-cpu GiB/s: min {:.2f}, median {:.2f}, max {:.2f}; "
            "worst max/min part on one agent: {:.1f}x",
            rates.front(), rates[rates.size() / 2], rates.back(),
            worst_spread);
  }
}

// PATH with the row's label before the extension when both rows run.
std::string trace_file_name(const std::string& path, const char* label,
                            bool both_rows) {
  if (!both_rows) {
    return path;
  }
  const std::filesystem::path p(path);
  return (p.parent_path() /
          (p.stem().string() + "." + label + p.extension().string()))
      .string();
}

// --threads resolved to a scheduler: an owned pool of exactly that size
// under stdexec, the process-wide scheduler elsewhere.
//
// The option exists as a DIAGNOSTIC for the sync-tier oversubscription
// question. The pread fallback's reader is an implicit extra thread the
// pool does not know about, so 11 runnable threads on a 10-core phone.
// Sweep N-1/N/N+1 to separate reader starvation from other shortfalls.
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
  auto* sweep_opt =
      app.add_flag("--io-sweep", sweep,
                   "sweep the window x qd pacing matrix (8-64 MiB x 4-32) "
                   "instead of the standard rows; the optimum is a device "
                   "property, run this per machine");
  std::string trace_path;
  bool trace_agents = false;
  auto* trace_opt =
      app.add_option("--trace", trace_path,
                     "record per-window timing; summarize the last rep of "
                     "each row and write it as Chrome trace JSON to PATH")
          ->excludes(sweep_opt);
  app.add_flag("--trace-agents", trace_agents,
               "with --trace: also record each pool agent's part of every "
               "window (parts, busy time, cpu)")
      ->needs(trace_opt);
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

  // Trace storage sized for the file: one record per window as the
  // reader rounds it, plus one, and with agents one record per pool
  // thread per window, since a bulk invocation is at most one per thread.
  const std::size_t effective_window =
      blake3pp::detail::rounded_window_bytes(opts.window_bytes);
  const std::size_t n_windows =
      static_cast<std::size_t>((bytes + effective_window - 1) / effective_window) + 1;
  std::vector<blake3pp::window_record> window_records;
  std::vector<blake3pp::agent_record> agent_records;
  std::optional<blake3pp::trace_buffer> trace;
  const unsigned threads_for_trace =
      pool_threads != 0 ? pool_threads : std::thread::hardware_concurrency();
  if (!trace_path.empty()) {
    window_records.resize(n_windows);
    if (trace_agents) {
      agent_records.resize(n_windows * threads_for_trace);
    }
    trace.emplace(std::span{window_records}, std::span{agent_records});
    opts.trace = &*trace;
    println(stdout, "trace: {} window records{}, JSON to {}",
            window_records.size(),
            trace_agents
                ? std::format(" and {} agent records", agent_records.size())
                : std::string{},
            trace_path);
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
  // released unread. This is the device ceiling as seen through this
  // backend, window and qd, and every hash row below is a fraction of it.
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

  // Each row: best of reps. When tracing, every rep starts from an empty
  // buffer and the io-wq workers are read before and after it, so what is
  // summarized and exported is the last rep alone.
  const auto run = [&](const char* label, auto&& fn) {
    cooldown();
    blake3pp::digest d{};
    std::map<std::string, std::uint64_t> iowq_before;
    double iowq_cpu_s = 0.0;
    const double best = best_cold_seconds(
        reps, cold, [&] { d = fn(); },
        [&] {
          if (trace) {
            trace->clear();
            iowq_before = iowq_cpu_ticks();
          }
        },
        [&] {
          if (trace) {
            iowq_cpu_s = iowq_cpu_seconds(iowq_before, iowq_cpu_ticks());
          }
        });
    println(stdout, "{:<10} {}   ({}...)  [{:3.0f}% of raw]", label,
            b3tool::rate(static_cast<std::size_t>(bytes), best),
            d.to_hex().substr(0, 16), 100.0 * raw_s / best);
    if (trace) {
      print_trace_summary(*trace, threads_for_trace, iowq_cpu_s);
      const std::string file = trace_file_name(trace_path, label, !seq_only);
      if (write_chrome_trace(file, *trace,
                             std::format("blake3pp_bench_file {}", label))) {
        println(stdout, "    trace written to {}", file);
      } else {
        println(stderr, "    could not write {}", file);
      }
    }
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
