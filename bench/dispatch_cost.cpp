// The price of the fat binary at the call boundary, isolated: the same
// kernel body, compiled into this translation unit, called three ways.
//
//   dispatched  a pointer to the kernel_ops table loaded from memory and
//               an indirect call through it: what hasher does per batch
//   direct      a call through a noinline wrapper (a call boundary with no
//               indirection; the wrapper's own argument shuffle and tail
//               jump cost it 3-4 instructions per call, so compare it with
//               care)
//   inlined     the kernel entry called by name in the same TU: the
//               reference, a direct call to the same body
//
// The bench forbids two GCC optimisations that the library's separate
// kernel TUs make impossible and that otherwise turn the "dispatched"
// loop into something else (see bench/CMakeLists.txt): guessing the
// indirect call's target, and cloning the entry for constant arguments.
//
// Measured on messages of one block (one call per 64 bytes, the worst
// case), one chunk (hash_many over 2 x simd_degree chunks per call, the
// library's leaf batch) and 64 chunks per call, pinned to one core,
// with hardware counters where Linux offers them: cycles, instructions,
// branches and branch-misses per message, next to the wall clock. The
// instruction delta between dispatched and direct is the exact size of
// the dispatch mechanism; the branch-miss column shows whether the
// indirect call is predicted.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ratio>
#include <string>
#include <vector>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// The kernel, instantiated here for the variant the build chose
// (BLAKE3PP_ARCH_NS and the matching -m flags come from CMake).
#include "kernel/kernel.cpp"

// The kernel TU reads the transpose16 dial and nothing else from the
// library; this bench links no library (the library's own copy of this
// kernel would collide with the one compiled here), so the dial is
// defined here at its measured-best default, as src/dispatch/
// transpose16.cpp defines it.
namespace blake3pp::kern {
std::atomic<transpose16_mode> transpose16_active{transpose16_mode::quartered};
}

#define BLAKE3PP_DC_STR_(x) #x
#define BLAKE3PP_DC_STR(x) BLAKE3PP_DC_STR_(x)

namespace {

using namespace blake3pp;
using kern::block_len;
using kern::chunk_len;
namespace k = blake3pp::kern::BLAKE3PP_ARCH_NS;

// Hides a value from the optimizer: after this the compiler knows
// nothing about p, so a call through it is a real indirect call and a
// load through it a real load.
template <class T>
T launder(T p) {
  asm volatile("" : "+r"(p));
  return p;
}

// --- the three call shapes ---------------------------------------------

struct table_holder {
  const kern::kernel_ops* ops;   // hasher::ops_, in shape
};

[[gnu::noinline]] void direct_compress(std::uint32_t cv[8], const std::uint8_t* block,
                                       std::uint32_t len, std::uint64_t counter,
                                       std::uint32_t flags) noexcept {
  k::compress_in_place(cv, block, len, counter, flags);
}
[[gnu::noinline]] void direct_hash_many(const std::uint8_t* const* inputs, std::size_t n,
                                        std::size_t blocks, const std::uint32_t key[8],
                                        std::uint64_t counter, bool inc, std::uint32_t flags,
                                        std::uint32_t fs, std::uint32_t fe,
                                        std::uint8_t* out) noexcept {
  k::hash_many(inputs, n, blocks, key, counter, inc, flags, fs, fe, out);
}

// --- counters ------------------------------------------------------------

struct counters {
  double ns = 0;
  std::array<std::uint64_t, 4> hw{};   // cycles, instructions, branches, misses
  bool have_hw = false;
};

#if defined(__linux__)
class perf_group {
 public:
  perf_group() {
    const std::uint64_t cfg[4] = {PERF_COUNT_HW_CPU_CYCLES, PERF_COUNT_HW_INSTRUCTIONS,
                                  PERF_COUNT_HW_BRANCH_INSTRUCTIONS, PERF_COUNT_HW_BRANCH_MISSES};
    for (int i = 0; i < 4; ++i) {
      perf_event_attr a{};
      a.type = PERF_TYPE_HARDWARE;
      a.size = sizeof a;
      a.config = cfg[i];
      a.disabled = 1;
      a.exclude_kernel = 1;
      a.exclude_hv = 1;
      fd_[i] = static_cast<int>(syscall(__NR_perf_event_open, &a, 0, -1, -1, 0));
    }
  }
  ~perf_group() {
    for (int fd : fd_) if (fd >= 0) close(fd);
  }
  bool ok() const { return std::all_of(fd_.begin(), fd_.end(), [](int fd) { return fd >= 0; }); }
  void start() {
    for (int fd : fd_) { ioctl(fd, PERF_EVENT_IOC_RESET, 0); ioctl(fd, PERF_EVENT_IOC_ENABLE, 0); }
  }
  void stop(counters& c) {
    for (int i = 0; i < 4; ++i) {
      ioctl(fd_[i], PERF_EVENT_IOC_DISABLE, 0);
      std::uint64_t v = 0;
      if (read(fd_[i], &v, sizeof v) == static_cast<ssize_t>(sizeof v)) c.hw[i] = v;
    }
    c.have_hw = true;
  }
 private:
  std::array<int, 4> fd_{-1, -1, -1, -1};
};

void pin_to_current_cpu() {
  const int cpu = sched_getcpu();
  if (cpu < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof set, &set);
}
#else
struct perf_group {
  bool ok() const { return false; }
  void start() {}
  void stop(counters&) {}
};
void pin_to_current_cpu() {}
#endif

template <class F>
counters measure(F&& body) {
  perf_group pg;
  counters c;
  const auto t0 = std::chrono::steady_clock::now();
  if (pg.ok()) pg.start();
  body();
  if (pg.ok()) pg.stop(c);
  c.ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
  return c;
}

// --- workloads ------------------------------------------------------------

constexpr std::uint32_t key[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                                  0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};

struct row {
  const char* size;
  const char* shape;
  std::size_t messages;   // per rep
  counters c[3];          // dispatched, direct, inlined: the median rep of each
};

// The three variants run round-robin, one rep each per round, so a drift
// in clock or thermal state lands on all three alike; each variant's
// figure is the median over its reps (per counter), immune to one slow
// round.
int g_only_variant = -1;   // -1: all three, round-robin

template <class Run>
void rounds(row& r, std::size_t reps, Run&& run_variant) {
  std::vector<counters> samples[3];
  for (std::size_t rep = 0; rep < reps; ++rep) {
    for (int v = 0; v < 3; ++v) {
      if (g_only_variant < 0 || v == g_only_variant) samples[v].push_back(run_variant(v));
    }
  }
  for (int v = 0; v < 3; ++v) {
    auto& sm = samples[v];
    if (sm.empty()) continue;
    auto median = [&](auto get) {
      std::vector<double> xs;
      for (const counters& c : sm) xs.push_back(get(c));
      std::sort(xs.begin(), xs.end());
      return xs[xs.size() / 2];
    };
    r.c[v].ns = median([](const counters& c) { return c.ns; });
    r.c[v].have_hw = sm.front().have_hw;
    for (int i = 0; i < 4; ++i) r.c[v].hw[i] = static_cast<std::uint64_t>(median([i](const counters& c) { return double(c.hw[i]); }));
  }
}

// One block per call: compress_in_place on 64-byte messages.
void run_blocks(row& r, const std::vector<std::uint8_t>& data, std::size_t reps, const kern::kernel_ops* t) {
  const std::size_t n = data.size() / block_len;
  std::uint32_t cv[8];
  auto body = [&](auto&& call) {
    for (std::size_t i = 0; i < n; ++i) {
      std::memcpy(cv, key, sizeof cv);
      call(cv, data.data() + i * block_len, block_len, static_cast<std::uint64_t>(i), 0u);
    }
    launder(cv[0]);
  };
  r.messages = n;
  rounds(r, reps, [&](int v) {
    switch (v) {
      case 0: return measure([&] { body([&](auto... a) { t->compress_in_place(a...); }); });
      case 1: return measure([&] { body([&](auto... a) { direct_compress(a...); }); });
      default: return measure([&] { body([&](auto... a) { k::compress_in_place(a...); }); });
    }
  });
}

// chunks_per_call chunks of 16 blocks per hash_many call.
void run_chunks(row& r, const std::vector<std::uint8_t>& data, std::size_t reps, std::size_t chunks_per_call,
                const kern::kernel_ops* t) {
  const std::size_t chunks = data.size() / chunk_len;
  std::vector<const std::uint8_t*> ptrs(chunks);
  for (std::size_t i = 0; i < chunks; ++i) ptrs[i] = data.data() + i * chunk_len;
  std::vector<std::uint8_t> out(chunks * 32);
  auto body = [&](auto&& call) {
    for (std::size_t i = 0; i + chunks_per_call <= chunks; i += chunks_per_call) {
      call(ptrs.data() + i, chunks_per_call, std::size_t{16}, key, static_cast<std::uint64_t>(i), true,
           0u, 1u /*CHUNK_START*/, 2u /*CHUNK_END*/, out.data() + i * 32);
    }
    launder(out[0]);
  };
  r.messages = chunks;
  rounds(r, reps, [&](int v) {
    switch (v) {
      case 0: return measure([&] { body([&](auto... a) { t->hash_many(a...); }); });
      case 1: return measure([&] { body([&](auto... a) { direct_hash_many(a...); }); });
      default: return measure([&] { body([&](auto... a) { k::hash_many(a...); }); });
    }
  });
}

void print(const row& r, std::size_t reps) {
  const char* names[3] = {"dispatched", "direct", "inlined"};
  std::printf("\n%s, %s (median of %zu interleaved reps)\n", r.size, r.shape, reps);
  std::printf("  %-11s %9s %9s %9s %9s %10s\n", "", "ns/msg", "cyc/msg", "ins/msg", "br/msg", "miss/msg");
  const double per = static_cast<double>(r.messages);
  for (int v = 0; v < 3; ++v) {
    const counters& c = r.c[v];
    if (g_only_variant >= 0 && v != g_only_variant) continue;
    if (c.have_hw) {
      std::printf("  %-11s %9.2f %9.1f %9.1f %9.2f %10.4f\n", names[v], c.ns / per, c.hw[0] / per, c.hw[1] / per,
                  c.hw[2] / per, c.hw[3] / per);
    } else {
      std::printf("  %-11s %9.2f %9s\n", names[v], c.ns / per, "(no hw counters)");
    }
  }
  if (g_only_variant >= 0) return;
  const counters& d = r.c[0];
  const counters& e = r.c[1];
  const counters& f = r.c[2];
  std::printf("  dispatched - direct: %+.2f ns", (d.ns - e.ns) / per);
  if (d.have_hw) std::printf(", %+.1f cycles, %+.1f instructions", (double(d.hw[0]) - double(e.hw[0])) / per,
                             (double(d.hw[1]) - double(e.hw[1])) / per);
  std::printf(" per message (%+.1f%%)\n", 100.0 * (d.ns - e.ns) / e.ns);
  std::printf("  direct - inlined:    %+.2f ns", (e.ns - f.ns) / per);
  if (e.have_hw) std::printf(", %+.1f cycles, %+.1f instructions", (double(e.hw[0]) - double(f.hw[0])) / per,
                             (double(e.hw[1]) - double(f.hw[1])) / per);
  std::printf(" per message (%+.1f%%)\n", 100.0 * (e.ns - f.ns) / f.ns);
}

}  // namespace

// dispatch_cost [MiB] [reps] [variant] [row]: the last two restrict a
// run to one call shape (0 dispatched, 1 direct, 2 in-TU) and one row
// (0 blocks, 1 leaf batch, 2 64-chunk batch), for running under an
// instruction-counting emulator one process at a time, where the
// difference between two processes' totals is the mechanism's exact
// instruction count.
int main(int argc, char** argv) {
  const std::size_t total = argc > 1 ? std::stoull(argv[1]) : 64;   // MiB per rep
  const std::size_t reps = argc > 2 ? std::stoull(argv[2]) : 9;
  const int only_variant = argc > 3 ? std::stoi(argv[3]) : -1;
  const int only_row = argc > 4 ? std::stoi(argv[4]) : -1;
  pin_to_current_cpu();

  std::vector<std::uint8_t> data(total << 20);
  std::uint64_t x = 0x9E3779B97F4A7C15ull;
  for (std::size_t i = 0; i < data.size(); i += 8) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    std::memcpy(data.data() + i, &x, std::min<std::size_t>(8, data.size() - i));
  }

  // The table pointer, laundered: the compiler cannot see that it is
  // &k::ops, so calls through it are what hasher makes.
  table_holder holder{launder(&k::ops)};
  const kern::kernel_ops* t = holder.ops;

  std::printf("dispatch cost, kernel %s (simd_degree %zu), %zu MiB x %zu reps, one core\n",
              BLAKE3PP_DC_STR(BLAKE3PP_ARCH_NS), k::ops.simd_degree, total, reps);
  {
    perf_group probe;
    std::printf("hardware counters: %s\n", probe.ok() ? "cycles, instructions, branches, branch-misses (user only)"
                                                       : "unavailable; wall clock only");
  }

  // The library's leaf batch is 2 * simd_degree chunks per hash_many call
  // (src/core/subtree.hpp), so the dispatched calls per byte halve with
  // every doubling of the vector width; the 64-chunk row shows the
  // remaining per-call share at a batch no variant reaches.
  row rows[3] = {{"64 B messages", "one compress_in_place per message", 0, {}},
                 {"1 KiB chunks", "hash_many, 2 x simd_degree chunks per call (the library's leaf batch)", 0, {}},
                 {"1 KiB chunks", "hash_many, 64 chunks per call", 0, {}}};
  g_only_variant = only_variant;
  if (only_row < 0) {
    // Warm up: touch the data and the code once through each path.
    run_blocks(rows[0], data, 1, t);
  }
  if (only_row < 0 || only_row == 0) run_blocks(rows[0], data, reps, t);
  if (only_row < 0 || only_row == 1) run_chunks(rows[1], data, reps, 2 * k::ops.simd_degree, t);
  if (only_row < 0 || only_row == 2) run_chunks(rows[2], data, reps, 64, t);
  for (int i = 0; i < 3; ++i) {
    if (only_row < 0 || only_row == i) print(rows[i], reps);
  }
  return 0;
}
