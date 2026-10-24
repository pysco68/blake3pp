// End-to-end file hashing throughput, the library's headline number.
// First measures the RAW device read speed through the identical
// windowed pipeline (same backend, window, queue depth; data delivered
// and discarded, no hashing), then hashes for real, sequential and
// parallel. The utilization column relates the two: 100% means the drive,
// not the hash, is the limit.
//
//   blake3pp_bench_file <FILE> [--reps N] [--window MiB] [--qd N]
//                       [--no-direct] [--no-async] [--threads N]
//                       [--seq-only] [--cooldown S]
//                       [--trace PATH [--trace-agents]]
//   blake3pp_bench_file <FILE> --io-sweep   # window x qd pacing matrix
//   blake3pp_bench_file --null-source <GiB> # read pipeline, no device
//   blake3pp_bench_file --null-sink <GiB>   # write pipeline, no device
//                       [--null-rate GiB/s [--null-knee QD]]  # modelled device
//   blake3pp_bench_file --make <MiB>   # create a test file and use it
//
// Note: with direct I/O the page cache is bypassed, so repetitions measure
// the device (or the host-side cache of a virtualized disk), not RAM.
//
// --trace PATH records where each window's time goes through the
// library's trace_buffer: waiting for the read, hashing, absorbing the
// chaining values, handing the buffer back. It prints a per-row summary
// from the last rep and writes that rep as Chrome trace JSON to PATH
// (open in chrome://tracing or Perfetto; with both rows, PATH gets a
// .seq/.parallel suffix). --trace-agents adds one record per pool agent
// per window: the parts it took, its busy time, the CPU it ran on. The
// summary turns those into a per-CPU effective rate, where slow cores,
// preemption and cache-cold parts show up. bench/README.md has the
// details and the pairing with perf record.
//
// --io-sweep exists because the right window/qd is a property of the
// DEVICE (bandwidth x latency), not the CPU: the 8 MiB x qd4 defaults,
// tuned on low-latency NVMe, measure 23-25% slow on GCP pd-class volumes,
// where 32x16 recovers it. The sweep settles it per machine.

#include <algorithm>
#include <array>
#include <new>
#include <numeric>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
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

#include <blake3pp/detail/file_pipeline.hpp>
#include <blake3pp/detail/memory_driver.hpp>
#include <blake3pp/detail/write_pipeline.hpp>


#include "tool_common.hpp"
#include "trace_json.hpp"

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
// timed region. Without the per-rep drop, a --no-direct run's first rep
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

// Whole-machine counters around one rep, so the kernel's share of the
// machine can be told from the pool's. /proc/stat's first line is printed
// as cores over the rep's wall time. /proc/interrupts is read per CPU, so
// a CPU that services the device can be told from a slow one. Linux only,
// and diagnostic only: nothing passes or fails on them.
struct kernel_counters {
  static constexpr const char* names[] = {"user",   "nice", "sys",     "idle",
                                          "iowait", "irq",  "softirq", "steal"};
  std::array<std::uint64_t, 8> stat{};
  std::vector<std::uint64_t> irqs;  // per-CPU interrupt totals
  std::chrono::steady_clock::time_point at{};
  bool valid = false;
};

kernel_counters read_kernel_counters() {
  kernel_counters k;
  k.at = std::chrono::steady_clock::now();
#if defined(__linux__)
  {
    std::ifstream stat("/proc/stat");
    std::string tag;
    if (stat >> tag && tag == "cpu") {
      for (auto& v : k.stat) {
        stat >> v;
      }
      k.valid = !stat.fail();
    }
  }
  {
    // The header names the CPUs; every line below it is one source with
    // one count per CPU, except the totals at the end (ERR, MIS) with a
    // single number, which the per-CPU read rejects.
    std::ifstream irq("/proc/interrupts");
    std::string line;
    std::size_t ncpu = 0;
    if (std::getline(irq, line)) {
      std::istringstream header(line);
      std::string tok;
      while (header >> tok) {
        ncpu += tok.starts_with("CPU") ? 1 : 0;
      }
    }
    k.irqs.assign(ncpu, 0);
    while (std::getline(irq, line)) {
      std::istringstream row(line);
      std::string source;
      row >> source;
      std::vector<std::uint64_t> counts;
      std::uint64_t v = 0;
      while (counts.size() < ncpu && row >> v) {
        counts.push_back(v);
      }
      if (counts.size() != ncpu) {
        continue;
      }
      for (std::size_t c = 0; c < ncpu; ++c) {
        k.irqs[c] += counts[c];
      }
    }
  }
#endif
  return k;
}

struct kernel_deltas {
  std::array<double, 8> cores{};    // each /proc/stat field over the wall
  std::vector<std::uint64_t> irqs;  // per-CPU interrupts during the rep
  bool valid = false;
};

kernel_deltas kernel_delta(const kernel_counters& before,
                           const kernel_counters& after) {
  kernel_deltas d;
  if (!before.valid || !after.valid) {
    return d;
  }
  const double wall = std::chrono::duration<double>(after.at - before.at).count();
#if defined(__linux__)
  const long hz = ::sysconf(_SC_CLK_TCK);
#else
  const long hz = 0;
#endif
  if (wall <= 0.0 || hz <= 0) {
    return d;
  }
  for (std::size_t i = 0; i < d.cores.size(); ++i) {
    d.cores[i] = static_cast<double>(after.stat[i] - before.stat[i]) /
                 static_cast<double>(hz) / wall;
  }
  const std::size_t n = std::min(before.irqs.size(), after.irqs.size());
  d.irqs.resize(n);
  for (std::size_t c = 0; c < n; ++c) {
    d.irqs[c] = after.irqs[c] - before.irqs[c];
  }
  d.valid = true;
  return d;
}

// A device to put under the null driver, so a control law can be shown
// a knee that was chosen rather than measured.
//
// One FIFO server with a fixed service time per window plus a fixed
// propagation delay: a transfer starts when the server is free, holds
// it for `service_ns`, and lands `latency_ns` later. At depth d the
// steady-state rate is min(d x window / (service + latency), window /
// service), so throughput rises linearly to a knee at
// 1 + latency / service and is flat above it. `knee` names that depth
// and the latency follows from it.
struct null_device {
  double rate_gib_s = 0.0;  // 0: complete immediately, no device at all
  unsigned knee = 1;
  std::int64_t service_ns = 0;
  std::int64_t latency_ns = 0;

  [[nodiscard]] bool active() const noexcept { return rate_gib_s > 0.0; }

  void settle(std::size_t window_bytes) noexcept {
    if (!active()) {
      return;
    }
    const double bytes_per_ns = rate_gib_s * 1024.0 * 1024.0 * 1024.0 / 1e9;
    service_ns = static_cast<std::int64_t>(
        std::max(1.0, static_cast<double>(window_bytes) / bytes_per_ns));
    latency_ns = static_cast<std::int64_t>(knee > 1 ? knee - 1 : 0) *
                 service_ns;
  }
};

// The pipeline with no device under it, and no bytes either.
//
// The file is VIRTUAL: it reports whatever size the run asks for, while
// the driver owns only windows x window_bytes of buffer and every window
// reuses its slot's slice. Reads complete with whatever is already there,
// so what this measures is the engine, the fan-out and the fold, with
// the device removed -- the ceiling a real one is measured against.
//
// That is NOT the same benchmark as hashing a real span:
// the bytes here are a handful of resident windows, not a working set.
// See bench/README.md.
//
// The completion half is memory_driver's, because a source with nothing
// to read has exactly one job -- report from poll() and not before.
// Given a null_device it paces those reports instead, so the synthetic
// arms can be run against a device whose knee was chosen rather than
// measured.
class null_driver {
 public:
  // The pool is sized and faulted in at construction, before the caller
  // starts its clock. Leaving it to reserve() would put a quarter of a
  // gigabyte of first-touch page faults inside the measurement at 64 MiB
  // windows, which is exactly the shape of a pipeline regression that is
  // not one.
  null_driver(const blake3pp::detail::io_driver_options&, unsigned,
              std::uint64_t size, std::byte filler, std::size_t pool_bytes,
              null_device dev = {})
      : size_(size), dev_(dev), pool_(pool_bytes, filler) {}
  null_driver(const null_driver&) = delete;
  null_driver& operator=(const null_driver&) = delete;

  class file {
   public:
    explicit file(null_driver& drv) noexcept : drv_(&drv) {}
    [[nodiscard]] std::uint64_t size() const noexcept { return drv_->size_; }
    [[nodiscard]] std::string_view name() const noexcept { return "null"; }
    // The sink has no length to trim: the write scope calls this at the
    // ends of a run and the answer is the same either way.
    void set_size(std::uint64_t) noexcept {}

   private:
    null_driver* drv_;
  };

  void reserve(unsigned windows, std::size_t window_bytes) {
    const std::size_t want = window_bytes * windows;
    if (pool_.size() < want) {
      pool_.assign(want, filler_);
    }
    window_stride_ = window_bytes;
  }

  // Each slot owns one slice, which every window through that slot
  // reuses: the source has no distinct bytes, and separate slots keep
  // the reads from overlapping.
  [[nodiscard]] blake3pp::detail::window_storage window_storage_for(
      file&, unsigned slot, std::uint64_t, std::size_t bytes) noexcept {
    const std::span<std::byte> slice{pool_.data() + slot * window_stride_,
                                     bytes};
    return {slice, std::span<const std::byte>(slice)};
  }

  void submit_read(file& f, std::uint64_t, std::span<std::byte> buf,
                   blake3pp::detail::io_op& op) noexcept {
    submit(f, buf.size(), op);
  }
  // The sink half: the bytes are already where they were generated, so
  // the only work is reporting from poll() and not before.
  void submit_write(file& f, std::uint64_t, std::span<const std::byte> buf,
                    blake3pp::detail::io_op& op) noexcept {
    submit(f, buf.size(), op);
  }
  void flush() noexcept {}

  std::size_t poll(bool block) {
    if (!dev_.active()) {
      return reads_.poll(block);
    }
    release_ready();
    if (block && reads_.in_flight() == 0 && !timed_.empty()) {
      wait_until(timed_.front().ready);
      release_ready();
    }
    // Only ever park on the waiter with nothing of the device's own
    // still to land: the waiter is woken by the pool, not by the clock.
    return reads_.poll(block && timed_.empty());
  }

  void wake() noexcept { reads_.wake(); }
  void cancel(blake3pp::detail::io_op& op) noexcept {
    for (auto it = timed_.begin(); it != timed_.end(); ++it) {
      if (it->op == &op) {
        reads_.push(op, it->owner);
        timed_.erase(it);
        break;
      }
    }
    reads_.cancel(op);
  }
  [[nodiscard]] std::size_t in_flight() const noexcept {
    return reads_.in_flight() + timed_.size();
  }
  void drain() noexcept {
    release_all();
    reads_.drain();
  }
  // Nothing here is in the OS's hands; one file's share is its own queued
  // operations, as on the memory driver.
  void drain(file& f) noexcept {
    release_all();
    reads_.drain(&f);
  }

 private:
  struct timed_op {
    blake3pp::detail::io_op* op;
    const void* owner;
    std::int64_t ready;
  };

  [[nodiscard]] static std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  void submit(file& f, std::size_t bytes, blake3pp::detail::io_op& op) noexcept {
    if (!dev_.active()) {
      reads_.push(op, &f);
      return;
    }
    // One server, FIFO, so the ready times this produces are already in
    // order and the queue is a plain deque.
    const double share = dev_.service_ns == 0 || window_stride_ == 0
                             ? 1.0
                             : static_cast<double>(bytes) /
                                   static_cast<double>(window_stride_);
    const auto service =
        static_cast<std::int64_t>(static_cast<double>(dev_.service_ns) * share);
    const std::int64_t start = std::max(now_ns(), server_free_);
    server_free_ = start + service;
    timed_.push_back({&op, &f, server_free_ + dev_.latency_ns});
    op.live = true;
  }

  void release_ready() noexcept {
    const std::int64_t t = now_ns();
    while (!timed_.empty() && timed_.front().ready <= t) {
      reads_.push(*timed_.front().op, timed_.front().owner);
      timed_.pop_front();
    }
  }

  void release_all() noexcept {
    while (!timed_.empty()) {
      reads_.push(*timed_.front().op, timed_.front().owner);
      timed_.pop_front();
    }
  }

  // Waits for one completion, in slices no longer than a service time:
  // sleeping the whole way would park the driver thread through the
  // rounds that admit the next windows, which caps the concurrency at
  // whatever one sleep allows. Spins for the last microsecond, which is
  // what keeps a service time of a few hundred microseconds from being
  // quantised by the scheduler's timer slack.
  void wait_until(std::int64_t ready) const noexcept {
    const std::int64_t slice = std::max<std::int64_t>(dev_.service_ns, 1000);
    const std::int64_t left = ready - now_ns();
    if (left <= 0) {
      return;
    }
    if (left > 2000) {
      std::this_thread::sleep_for(
          std::chrono::nanoseconds(std::min(left - 1000, slice)));
    } else {
      std::this_thread::yield();
    }
  }

  std::uint64_t size_;
  std::byte filler_{};
  std::size_t window_stride_ = 0;
  null_device dev_{};
  std::int64_t server_free_ = 0;
  std::deque<timed_op> timed_;
  std::vector<std::byte> pool_;
  blake3pp::detail::queued_reads reads_;
};

static_assert(blake3pp::detail::file_driver<null_driver>);


// The share of the wall clock during which at least one agent was
// busy, from merged intervals, since agents overlap.
[[nodiscard]] double idle_covered(std::span<const blake3pp::agent_record> agents,
                                  double wall_ns) {
  if (agents.empty() || wall_ns <= 0) {
    return 0.0;
  }
  std::vector<std::pair<std::int64_t, std::int64_t>> iv;
  iv.reserve(agents.size());
  for (const auto& a : agents) {
    iv.emplace_back(a.t_begin, a.t_begin + static_cast<std::int64_t>(a.busy_ns));
  }
  std::sort(iv.begin(), iv.end());
  double covered = 0;
  std::int64_t cur_begin = iv.front().first;
  std::int64_t cur_end = iv.front().second;
  for (const auto& [b, e] : iv) {
    if (b > cur_end) {
      covered += static_cast<double>(cur_end - cur_begin);
      cur_begin = b;
      cur_end = e;
    } else if (e > cur_end) {
      cur_end = e;
    }
  }
  covered += static_cast<double>(cur_end - cur_begin);
  return covered / wall_ns;
}

// The per-row summary of one traced rep: where the wall time went, how
// busy the pool was, and with agent records what each CPU delivered.
void print_trace_summary(const blake3pp::trace_buffer& trace,
                         unsigned threads, double iowq_cpu_s,
                         double achieved_gib_s = 0.0,
                         const kernel_deltas* kernel = nullptr) {
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
  // With several windows in flight these no longer sum to the wall
  // clock: they are per-window shares of a pipeline, not a timeline.
  println(stdout,
          "    per window: wait {:5.1f}%  hash {:5.1f}%  absorb {:5.1f}%  "
          "release {:5.1f}%",
          pct(wait), pct(hash), pct(absorb), pct(release));

  // The agent slices split by kind, as a line of text: a fold is three
  // microseconds beside a compress of three hundred and seventy, so at
  // any zoom where the compress slices are legible the folds are
  // sub-pixel and the viewer does not draw them. The trace carries one
  // fold record per window; their share is printed here.
  if (const auto agents = trace.agents(); !agents.empty()) {
    double compress_ns = 0;
    double fold_ns = 0;
    std::size_t compress_n = 0;
    std::size_t fold_n = 0;
    for (const auto& a : agents) {
      if (a.parts == 0) {
        fold_ns += static_cast<double>(a.busy_ns);
        ++fold_n;
      } else {
        compress_ns += static_cast<double>(a.busy_ns);
        ++compress_n;
      }
    }
    const double agent_ns = compress_ns + fold_ns;
    const auto share = [&](double ns) {
      return agent_ns > 0 ? 100.0 * ns / agent_ns : 0.0;
    };
    println(stdout,
            "    agent slices: compress {} ({:.1f} ms, {:4.1f}%)  fold {} "
            "({:.1f} ms, {:4.1f}%)",
            compress_n, compress_ns / 1e6, share(compress_ns), fold_n,
            fold_ns / 1e6, share(fold_ns));
    // And what those slices look like in the viewer, said as a number
    // so nobody has to infer it from how empty the lanes are: the pool
    // is busy for a fraction of the run whenever the device cannot feed
    // it, which is most runs. A low occupancy here is the reader
    // starving the pool, not the trace losing records.
    if (threads > 0 && wall_ns > 0) {
      println(stdout,
              "    pool occupancy: {:4.1f}% of {} threads x wall; no agent "
              "busy at all for {:4.1f}% of it",
              100.0 * agent_ns / (wall_ns * threads), threads,
              100.0 * (1.0 - idle_covered(agents, wall_ns)));
    }
  }

  // What the driver thread itself did, which is the number the compress
  // stage is judged against: a driver parked most of the time has room
  // for more work, one that is busy is the ceiling.
  const auto& d = trace.driver();
  if (d.iterations > 0) {
    const double driver_ns = static_cast<double>(d.busy_ns + d.parked_ns);
    if (const std::string_view rung = blake3pp::read_backend(d);
        !rung.empty()) {
      println(stdout, "    read backend: {}", rung);
    }
    println(stdout,
            "    driver busy {:5.1f}% of its own {:.3f} s, {} iterations, {} "
            "polls ({} blocking, {} woken), {} completions, {} admission "
            "stalls, reducer held at most {}, at most {} reads and {} "
            "windows in flight",
            100.0 * static_cast<double>(d.busy_ns) / driver_ns,
            driver_ns / 1e9, d.iterations, d.polls, d.blocking_polls,
            d.wakes, d.queue_runs, d.admission_stalls, d.max_pending,
            d.max_reads_in_flight, d.max_windows_in_flight);
    if (d.transfers_landed > 0) {
      println(stdout,
              "    queue depth: settled at {} (peak {}); device {:.3f} ms per "
              "window over {} transfers, starved {:.3f} s",
              d.queue_depth_final, d.queue_depth_peak,
              static_cast<double>(d.device_wait_ns) /
                  static_cast<double>(d.transfers_landed) / 1e6,
              d.transfers_landed, static_cast<double>(d.starved_ns) / 1e9);
    }
  }

  // Per-window latencies, as medians: with windows overlapping, an
  // average over stages says less than where the middle window sat.
  {
    std::vector<std::int64_t> to_read;
    std::vector<std::int64_t> to_cv;
    std::vector<std::int64_t> to_reduced;
    to_read.reserve(windows.size());
    to_cv.reserve(windows.size());
    to_reduced.reserve(windows.size());
    for (const auto& w : windows) {
      to_read.push_back(w.t_ready - w.t_wait_begin);
      to_cv.push_back(w.t_joined - w.t_ready);
      to_reduced.push_back(w.t_absorbed - w.t_joined);
    }
    const auto median = [](std::vector<std::int64_t>& v) {
      std::ranges::nth_element(v, v.begin() + v.size() / 2);
      return static_cast<double>(v[v.size() / 2]) / 1000.0;
    };
    println(stdout,
            "    median per window: submit->read {:8.1f} us  read->cv {:8.1f} "
            "us  cv->reduced {:8.1f} us",
            median(to_read), median(to_cv), median(to_reduced));
  }
  if (fanned > 0) {
    println(stdout,
            "    pool busy {:5.1f}% of {} threads, {:.1f} agents active per "
            "fanned window",
            100.0 * busy / (static_cast<double>(threads) * wall_ns), threads,
            active / static_cast<double>(fanned));
  }
  std::string machine;
  if (kernel != nullptr && kernel->valid) {
    machine = "; machine (/proc/stat):";
    for (std::size_t i = 0; i < kernel->cores.size(); ++i) {
      machine += std::format(" {} {:.2f}", kernel_counters::names[i],
                             kernel->cores[i]);
    }
    machine += " cores";
  }
  println(stdout, "    io-wq cpu = {:.2f} cores (worker threads' cpu / wall){}",
          iowq_cpu_s / (wall_ns / 1e9), machine);

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
  const bool irqs = kernel != nullptr && kernel->valid && !kernel->irqs.empty();
  println(stdout, "      {:>5} {:>8} {:>8} {:>10} {:>14}{}", "cpu", "records",
          "parts", "busy ms", "effective", irqs ? "      irqs" : "");
  for (const auto& [cpu, c] : per_cpu) {
    const double secs = static_cast<double>(c.busy_ns) / 1e9;
    rates.push_back(b3tool::gib_per_s(c.bytes, secs));
    std::string irq_col;
    if (irqs) {
      irq_col = cpu < kernel->irqs.size()
                    ? std::format("  {:>8}", kernel->irqs[cpu])
                    : std::string{"         -"};
    }
    println(stdout, "      {:>5} {:>8} {:>8} {:>10.1f} {}{}",
            cpu == ~std::uint32_t{0} ? std::string{"?"} : std::to_string(cpu),
            c.records, c.parts, static_cast<double>(c.busy_ns) / 1e6,
            b3tool::rate(c.bytes, secs), irq_col);
  }
  if (!rates.empty()) {
    std::ranges::sort(rates);
    println(stdout,
            "      per-cpu GiB/s: min {:.2f}, median {:.2f}, max {:.2f}; "
            "worst max/min part on one agent: {:.1f}x",
            rates.front(), rates[rates.size() / 2], rates.back(),
            worst_spread);
    // What the cpus managed while compressing, added up, against what
    // the run actually delivered: the gap is everything that is not
    // compression -- reading, waiting, and the parts of the file no cpu
    // was working on at that moment.
    const double sum = std::accumulate(rates.begin(), rates.end(), 0.0);
    if (achieved_gib_s > 0.0 && sum > 0.0) {
      println(stdout,
              "      sum of per-cpu rates {:.2f} GiB/s; the run delivered "
              "{:.2f} GiB/s, {:.1f}% of it",
              sum, achieved_gib_s, 100.0 * achieved_gib_s / sum);
    }
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

// The pipeline with nothing behind it: the reader hands back buffers that
// were filled once and never touched again, so what is left in the
// measurement is the engine, the window fan-out and the fold. This is the
// hash-bound ceiling the device rows are read against, and the only
// configuration in which the tracer's own cost is visible.
//
// The filler is one byte repeated: BLAKE3's rate is data-independent,
// and every slot holds the same window anyway, so a pseudo-random pattern
// would detect no more than a constant does. It is not zero, so that a
// zeroed buffer the engine never filled hashes differently from one it
// did; the digest check below is what proves the loop absorbed what it
// claims to have read.
constexpr std::byte null_filler{0xa5};

// --threads resolved to a scheduler (b3tool::compute_pool, one_thread::pooled:
// a pool at exactly that size, one included, since a count of one is a
// measurement here rather than the sequential path).
//
// The option exists as a DIAGNOSTIC for the sync-tier oversubscription
// question. The pread fallback's reader is an implicit extra thread the
// pool does not know about, so 11 runnable threads on a 10-core phone.
// Sweep N-1/N/N+1 to separate reader starvation from other shortfalls.
[[nodiscard]] inline b3tool::compute_pool engine_threads(unsigned threads) {
  return b3tool::compute_pool{threads, b3tool::one_thread::pooled};
}

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
  auto engine = engine_threads(pool_threads);
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
  double null_gib = 0;
  double null_sink_gib = 0;
  bool inline_submit = false;
  bool no_async = false;
  bool no_direct = false;
  bool no_ioring = false;
  bool registered_buffers = false;
  double null_rate = 0;
  unsigned null_knee = 1;
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
  app.add_option("--null-rate", null_rate,
                 "give the synthetic source or sink a device of this many "
                 "GiB/s instead of completing immediately")
      ->check(CLI::PositiveNumber);
  app.add_option("--null-knee", null_knee,
                 "the queue depth at which that device saturates: its "
                 "propagation delay is set to (knee - 1) service times, so "
                 "throughput rises to this depth and is flat above it")
      ->check(CLI::Range(1u, 1024u));
  app.add_flag("--no-direct", no_direct,
               "keep the OS page cache (no O_DIRECT)");
  app.add_flag("--no-async", no_async,
               std::format("read synchronously ({}): the comparison arm "
                           "against the platform's async engine",
                           b3tool::no_async_note()));
  app.add_flag("--inline-submit", inline_submit,
               "issue each read inline in the submitting thread instead of "
               "on io_uring's workers (IOSQE_ASYNC off)");
  app.add_flag("--no-ioring", no_ioring,
               "Windows: read through the completion port instead of the "
               "I/O ring (the comparison arm)");
  app.add_flag("--registered-buffers", registered_buffers,
               "Windows I/O ring: register the window arena and the file "
               "handle with the ring instead of naming a pointer and a "
               "HANDLE per transfer");
  app.add_option("--threads", pool_threads,
                 "parallel-engine threads (1 = a one-thread pool; default: "
                 "all); diagnostic: sweep N-1/N/N+1 to test whether the "
                 "sync-tier reader is starving as the N+1th runnable thread")
      ->check(b3tool::at_least_one_thread)
      ->capture_default_str();
  app.add_flag("--seq-only", seq_only, "skip the parallel measurement");
  app.add_option("--null-source", null_gib,
                 "hash this many GiB from a source that performs no I/O: the "
                 "pipeline's hash-bound ceiling, with no file and no device")
      ->check(CLI::PositiveNumber);
  app.add_option("--null-sink", null_sink_gib,
                 "write N GiB through the write pipeline into a sink that "
                 "completes immediately (the device-free ceiling)")
      ->check(CLI::PositiveNumber);
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
  opts.async = !no_async;
  opts.offload_submit = !inline_submit;
  opts.ioring = !no_ioring;
  opts.registered_buffers = registered_buffers;
  if (null_gib > 0) {
        const auto total =
        static_cast<std::uint64_t>(null_gib * 1024.0 * 1024.0 * 1024.0);
    const std::size_t win =
        blake3pp::detail::rounded_window_bytes(opts.window_bytes);
    const unsigned threads =
        pool_threads != 0 ? pool_threads : std::thread::hardware_concurrency();
    std::vector<blake3pp::window_record> window_records;
    std::vector<blake3pp::agent_record> agent_records;
    std::optional<blake3pp::trace_buffer> trace;
    blake3pp::file_io_options io_opts{
        .window_bytes = opts.window_bytes,
        .queue_depth = opts.queue_depth,
        .direct_io = opts.direct_io,
        .offload_submit = opts.offload_submit,
        .ioring = opts.ioring,
        .registered_buffers = opts.registered_buffers};
    null_device dev{.rate_gib_s = null_rate, .knee = null_knee};
    dev.settle(win);
    if (!trace_path.empty()) {
      const std::size_t n = static_cast<std::size_t>((total + win - 1) / win) + 1;
      window_records.resize(n);
      if (trace_agents) {
        agent_records.resize(n * threads);
      }
      trace.emplace(std::span{window_records}, std::span{agent_records});
      io_opts.trace = &*trace;
    }
    const std::vector<std::byte> pattern(win, null_filler);
    println(stdout,
            "null source: {:.2f} GiB, window {} MiB, qd {}, {} threads",
            null_gib, win >> 20, opts.queue_depth, threads);
    if (dev.active()) {
      println(stdout,
              "  device: {:.1f} GiB/s, knee at qd {} ({:.3f} ms service, "
              "{:.3f} ms latency)",
              dev.rate_gib_s, dev.knee,
              static_cast<double>(dev.service_ns) / 1e6,
              static_cast<double>(dev.latency_ns) / 1e6);
    }
    // The arena the pool draws its work from, resolved the way every
    // read entry point resolves it: from this machine's thread count.
    const blake3pp::detail::pipeline_plan plan =
        blake3pp::detail::plan_pipeline(
            0, opts.window_bytes, opts.queue_depth, false, total,
            blake3pp::detail::read_plan_budget_bytes(threads,
                                                     opts.window_bytes));

    // The digest this must produce: the same bytes, hashed in memory.
    blake3pp::hasher expect;
    for (std::uint64_t done = 0; done < total;) {
      const auto n = static_cast<std::size_t>(
          std::min<std::uint64_t>(win, total - done));
      expect.update(std::span<const std::byte>{pattern.data(), n});
      done += n;
    }
    const blake3pp::digest want = expect.finalize();

    auto engine = engine_threads(threads);
    auto sched = engine.scheduler();
    b3tool::cooldown cooldown(cooldown_s, /*skip_first=*/true);
    double best = 1e30;
    blake3pp::digest got{};
    kernel_deltas kernel;
    for (int r = 0; r < reps; ++r) {
      cooldown();
      if (trace) {
        trace->clear();
      }
      const kernel_counters kernel_before = read_kernel_counters();
      null_driver drv({.async = true,
                       .offload_submit = opts.offload_submit,
                       .ioring = opts.ioring,
                       .registered_buffers = opts.registered_buffers},
                      plan.reads_in_flight, total, null_filler,
                      plan.window_bytes * plan.windows, dev);
      null_driver::file nf(drv);
      blake3pp::hasher h;
      const auto t0 = std::chrono::steady_clock::now();
      blake3pp::detail::run_window_pipeline<blake3pp::default_stack_budget>(
          h, drv, nf, sched, plan,
          {.window_bytes = opts.window_bytes,
           .queue_depth = opts.queue_depth,
           .trace = io_opts.trace});
      got = h.finalize();
      const auto t1 = std::chrono::steady_clock::now();
      kernel = kernel_delta(kernel_before, read_kernel_counters());
      best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
    }
    println(stdout, "{:<10} {}   ({}...)  [{}]", "null",
            b3tool::rate(static_cast<std::size_t>(total), best),
            got.to_hex().substr(0, 16),
            got == want ? "digest matches the in-memory hash"
                        : "DIGEST MISMATCH");
    if (trace) {
      print_trace_summary(*trace, threads, 0.0,
                          b3tool::gib_per_s(static_cast<std::size_t>(total),
                                            best),
                          &kernel);
      const std::string file = trace_file_name(trace_path, "null", false);
      if (b3tool::write_chrome_trace(file, *trace, "blake3pp_bench_file null",
                             threads)) {
        println(stdout, "    trace written to {}", file);
      }
    }
    return got == want ? 0 : 1;
  }

  // The write pipeline with the device removed: windows are generated on
  // the pool and handed to a sink that completes them from poll(). What
  // is left is the generator, the chain and the loop -- the ceiling a
  // real drive is measured against, the mirror of --null-source.
  if (null_sink_gib > 0) {
    const auto total =
        static_cast<std::uint64_t>(null_sink_gib * 1024.0 * 1024.0 * 1024.0);
    const std::size_t win =
        blake3pp::detail::rounded_window_bytes(opts.window_bytes);
    const unsigned threads =
        pool_threads != 0 ? pool_threads : std::thread::hardware_concurrency();
    null_device dev{.rate_gib_s = null_rate, .knee = null_knee};
    dev.settle(win);
    const unsigned start_depth = blake3pp::detail::resolve_write_queue_depth(
        opts.queue_depth, opts.window_bytes, threads);
    println(stdout,
            "null sink: {:.2f} GiB, window {} MiB, qd {}, {} threads",
            null_sink_gib, win >> 20, start_depth, threads);
    if (dev.active()) {
      println(stdout,
              "  device: {:.1f} GiB/s, knee at qd {} ({:.3f} ms service, "
              "{:.3f} ms latency)",
              dev.rate_gib_s, dev.knee,
              static_cast<double>(dev.service_ns) / 1e6,
              static_cast<double>(dev.latency_ns) / 1e6);
    }
    blake3pp::trace_buffer sink_trace{std::span<blake3pp::window_record>{}};

    // The same generator blake3ppgen uses, so the fill costs what it
    // costs there: an extended output seeks in O(1), so each window
    // copies the reader and seeks its own offset.
    blake3pp::hasher seed;
    seed.update(std::span<const std::byte>{&null_filler, 1});
    const blake3pp::output_reader stream = seed.finalize_xof();

    auto engine = engine_threads(threads);
    auto sched = engine.scheduler();
    b3tool::cooldown cooldown(cooldown_s, /*skip_first=*/true);
    double best = 1e30;
    for (int r = 0; r < reps; ++r) {
      cooldown();
      // Size the pool exactly as the write scope will ask for it, so its
      // reserve() is a no-op and no first-touch page fault lands inside
      // the clock.
      const unsigned windows = static_cast<unsigned>(std::min<std::uint64_t>(
          (total + win - 1) / win, start_depth));
      null_driver drv({.async = true,
                       .offload_submit = opts.offload_submit,
                       .ioring = opts.ioring,
                       .registered_buffers = opts.registered_buffers},
                      windows, total, null_filler, win * windows, dev);
      null_driver::file nf(drv);
      const auto t0 = std::chrono::steady_clock::now();
      blake3pp::detail::run_write_pipeline(
          drv, nf, sched,
          [&stream](std::span<std::byte> out, std::uint64_t off) noexcept {
            blake3pp::output_reader rd = stream;
            rd.seek(off);
            blake3pp::fill(rd, out);
          },
          total,
          {.window_bytes = opts.window_bytes,
           .queue_depth = start_depth,
           .trace = &sink_trace,
           .generator_threads = threads});
      const auto t1 = std::chrono::steady_clock::now();
      best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
    }
    println(stdout, "{:<10} {}", "null sink",
            b3tool::rate(static_cast<std::size_t>(total), best));
    {
      const auto& d = sink_trace.driver();
      println(stdout,
              "  device: {:.3f} ms per window over {} writes, generator "
              "starved {:.3f} s",
              d.transfers_landed > 0
                  ? static_cast<double>(d.device_wait_ns) /
                        static_cast<double>(d.transfers_landed) / 1e6
                  : 0.0,
              d.transfers_landed, static_cast<double>(d.starved_ns) / 1e9);
    }
    return 0;
  }

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
        {.window_bytes = opts.window_bytes,
         .queue_depth = opts.queue_depth,
         .direct_io = opts.direct_io,
         .async = opts.async,
         .offload_submit = opts.offload_submit,
         .ioring = opts.ioring,
         .registered_buffers = opts.registered_buffers});
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
        {.window_bytes = opts.window_bytes,
         .queue_depth = opts.queue_depth,
         .direct_io = opts.direct_io,
         .async = opts.async,
         .offload_submit = opts.offload_submit,
         .ioring = opts.ioring,
         .registered_buffers = opts.registered_buffers});
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
  // Every row hashes the same file, so their digests are one answer: the
  // first row's is what the rest are held to, and a disagreement makes the
  // run fail rather than print two hexes and a rate. Without it a wrong
  // parallel digest reads as a successful benchmark.
  blake3pp::digest expect{};
  bool have_expect = false;
  bool digests_agree = true;
  const auto run = [&](const char* label, auto&& fn) {
    cooldown();
    blake3pp::digest d{};
    std::map<std::string, std::uint64_t> iowq_before;
    double iowq_cpu_s = 0.0;
    kernel_counters kernel_before;
    kernel_deltas kernel;
    const double best = best_cold_seconds(
        reps, cold, [&] { d = fn(); },
        [&] {
          if (trace) {
            trace->clear();
            iowq_before = iowq_cpu_ticks();
            kernel_before = read_kernel_counters();
          }
        },
        [&] {
          if (trace) {
            iowq_cpu_s = iowq_cpu_seconds(iowq_before, iowq_cpu_ticks());
            kernel = kernel_delta(kernel_before, read_kernel_counters());
          }
        });
    println(stdout, "{:<10} {}   ({}...)  [{:3.0f}% of raw]", label,
            b3tool::rate(static_cast<std::size_t>(bytes), best),
            d.to_hex().substr(0, 16), 100.0 * raw_s / best);
    if (!have_expect) {
      expect = d;
      have_expect = true;
    } else if (d != expect) {
      digests_agree = false;
      println(stdout,
              "  DIGEST MISMATCH: {} hashed the same file as the row above "
              "and got {} instead of {}",
              label, d.to_hex(), expect.to_hex());
    }
    if (trace) {
      print_trace_summary(
          *trace, threads_for_trace, iowq_cpu_s,
          b3tool::gib_per_s(static_cast<std::size_t>(bytes), best), &kernel);
      const std::string file = trace_file_name(trace_path, label, !seq_only);
      if (b3tool::write_chrome_trace(file, *trace,
                             std::format("blake3pp_bench_file {}", label),
                             threads_for_trace)) {
        println(stdout, "    trace written to {}", file);
      } else {
        println(stderr, "    could not write {}", file);
      }
    }
  };

  run("seq", [&] { return blake3pp::hash_file(path.c_str(), opts); });

  if (!seq_only) {
    auto engine = engine_threads(pool_threads);
    auto sched = engine.scheduler();
    println(stdout, "parallel:   {} threads, affinity mask: {}",
            engine.count(),
            b3tool::affinity_cpu_count() > 0
                ? std::format("{} cpus", b3tool::affinity_cpu_count())
                : std::string{"unknown"});
    run("parallel",
        [&] { return blake3pp::hash_file(path.c_str(), sched, opts); });
  }
  return digests_agree ? 0 : 1;
}
