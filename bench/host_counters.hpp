#pragma once

// What blake3pp_bench_file reads of the machine around a timed rep: the
// cache drop that makes a rep read the device, the io_uring workers' CPU
// time, and the whole-machine counters a rep is compared against. Each is
// best effort and says so through its result; nothing passes or fails on
// them.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(__linux__)
#include <unistd.h>  // sync(): flushes dirty pages before a cache drop;
                     // sysconf(_SC_CLK_TCK) for the io-wq CPU time
#endif

#include "tool_common.hpp"  // <windows.h>, set up the way the tools take it

namespace b3bench {

// Best-effort cache drop before every timed rep, so each rep reads the
// device: vm.drop_caches=3 on Linux, purge(8) on macOS, and on Windows
// the sequence RAMMap uses (empty the working sets, flush the modified
// list, purge the standby list). Wants root, or an elevated token on
// Windows. With direct I/O a failed drop only leaves metadata and
// readahead state warm; with --no-direct it leaves the data itself warm.
inline bool drop_caches() {
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
inline std::map<std::string, std::uint64_t> iowq_cpu_ticks() {
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

inline double iowq_cpu_seconds(
    const std::map<std::string, std::uint64_t>& before,
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

inline kernel_counters read_kernel_counters() {
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
        if (tok.starts_with("CPU")) {
          ++ncpu;
        }
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

inline kernel_deltas kernel_delta(const kernel_counters& before,
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

}  // namespace b3bench
