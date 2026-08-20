// Single-thread hashing throughput per architecture variant.
//
//   blake3pp_bench [--size <MiB>] [--reps <N>] [--cooldown <s>] [arch ...]
//
// With no arch arguments, measures every variant available on this machine.
// Reports the best of N repetitions, the interesting number for a
// throughput ceiling; interference only ever slows a run down. Deliberately
// framework-free: pair it with hyperfine when process-level statistics are
// wanted.

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <blake3pp/blake3pp.hpp>
#include <blake3pp/parallel.hpp>

#if defined(BLAKE3PP_EXECUTION_STDEXEC) && defined(__APPLE__)
#include <exec/libdispatch_queue.hpp>
#endif

#if defined(BLAKE3PP_BENCH_UPSTREAM)
#include <blake3.h>
#endif

#if defined(BLAKE3PP_BENCH_UPSTREAM) && \
    (defined(__x86_64__) || defined(__aarch64__))
#include "kernel/kernel.hpp"

// Upstream's hand-tuned wide kernel, already present in the linked baseline
// library: hand-scheduled AVX2 assembly on x86-64, NEON intrinsics on
// aarch64 (upstream ships no ARM assembly). Wrapping it as a kernel_ops
// table plugs it into OUR entire pipeline (subtree batching, CV stack,
// parallel engine) through the same seam every portable variant uses. Only
// the flag argument widths differ (upstream narrows them to uint8_t).
#if defined(__x86_64__)
extern "C" void blake3_hash_many_avx2(
    const std::uint8_t* const* inputs, std::size_t num_inputs,
    std::size_t blocks, const std::uint32_t key[8], std::uint64_t counter,
    bool increment_counter, std::uint8_t flags, std::uint8_t flags_start,
    std::uint8_t flags_end, std::uint8_t* out);
#else
extern "C" void blake3_hash_many_neon(
    const std::uint8_t* const* inputs, std::size_t num_inputs,
    std::size_t blocks, const std::uint32_t key[8], std::uint64_t counter,
    bool increment_counter, std::uint8_t flags, std::uint8_t flags_start,
    std::uint8_t flags_end, std::uint8_t* out);
#endif

namespace {
#if defined(__x86_64__)
constexpr auto upstream_hash_many = &blake3_hash_many_avx2;
#else
constexpr auto upstream_hash_many = &blake3_hash_many_neon;
#endif

void asm_hash_many(const std::uint8_t* const* inputs, std::size_t num_inputs,
                   std::size_t blocks, const std::uint32_t key[8],
                   std::uint64_t counter, bool increment_counter,
                   std::uint32_t flags, std::uint32_t flags_start,
                   std::uint32_t flags_end, std::uint8_t* out) noexcept {
  upstream_hash_many(inputs, num_inputs, blocks, key, counter,
                     increment_counter, static_cast<std::uint8_t>(flags),
                     static_cast<std::uint8_t>(flags_start),
                     static_cast<std::uint8_t>(flags_end), out);
}
}  // namespace
#endif

namespace {

blake3pp::arch parse_arch(const std::string& name) {
  if (const auto a = blake3pp::arch_from_string(name); a.has_value()) {
    return a.value();
  }
  std::fprintf(stderr, "unknown arch '%s'\n", name.c_str());
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t mib = 512;
  int reps = 5;
  double cooldown_s = 5.0;
  std::vector<blake3pp::arch> arches;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--size" && i + 1 < argc) {
      mib = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (arg == "--reps" && i + 1 < argc) {
      reps = std::atoi(argv[++i]);
    } else if (arg == "--cooldown" && i + 1 < argc) {
      cooldown_s = std::strtod(argv[++i], nullptr);
    } else {
      arches.push_back(parse_arch(arg));
    }
  }
  if (arches.empty()) {
    // Available variants, printed worst-to-best so the table reads as an
    // ascending progression.
    const auto avail = blake3pp::available_arches();
    arches.assign(avail.rbegin(), avail.rend());
  }

  std::vector<std::byte> input(mib * 1024 * 1024);
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<std::byte>(i % 251);
  }

  std::printf(
      "blake3pp single-thread throughput, %zu MiB, best of %d, "
      "%.0fs cooldown\n",
      mib, reps, cooldown_s);
  std::printf("auto resolves to: %s\n\n",
              blake3pp::to_string(blake3pp::best_available()));

  // Laptops throttle: run variants back to back and each one measures the
  // previous one's heat (observed: ~20% swing on identical code). Idle
  // between measurements; the first one starts immediately, --cooldown 0
  // disables.
  bool first_measurement = true;
  const auto cooldown = [&] {
    if (first_measurement) {
      first_measurement = false;
      return;
    }
    if (cooldown_s > 0) {
      std::fflush(stdout);
      std::this_thread::sleep_for(std::chrono::duration<double>(cooldown_s));
    }
  };

  for (const auto a : arches) {
    if (!blake3pp::is_available(a)) {
      std::printf("%-8s unavailable on this machine\n",
                  blake3pp::to_string(a));
      continue;
    }
    cooldown();
    blake3pp::digest d{};
    double best_s = 1e100;
    for (int r = 0; r < reps + 1; ++r) {  // rep 0 is warmup
      blake3pp::hasher h{a};
      const auto t0 = std::chrono::steady_clock::now();
      h.update(input);
      d = h.finalize();
      const auto t1 = std::chrono::steady_clock::now();
      const double s = std::chrono::duration<double>(t1 - t0).count();
      if (r > 0 && s < best_s) {
        best_s = s;
      }
    }
    const double gib_s =
        static_cast<double>(input.size()) / best_s / (1024.0 * 1024.0 * 1024.0);
    std::printf("%-8s %8.2f GiB/s   (%s...)\n", blake3pp::to_string(a), gib_s,
                d.to_hex().substr(0, 16).c_str());
  }

  // The AVX-512 transpose strategies, raced in-process (the dial is
  // runtime; no per-strategy binaries needed), plus what the tuner picks.
  if (blake3pp::is_available(blake3pp::arch::avx512)) {
    const auto saved = blake3pp::active_transpose16();
    for (const auto strat :
         {blake3pp::transpose16::staging, blake3pp::transpose16::tree,
          blake3pp::transpose16::quartered}) {
      blake3pp::set_transpose16(strat);
      cooldown();
      blake3pp::digest d{};
      double best_s = 1e100;
      for (int r = 0; r < reps + 1; ++r) {
        blake3pp::hasher h{blake3pp::arch::avx512};
        const auto t0 = std::chrono::steady_clock::now();
        h.update(input);
        d = h.finalize();
        const auto t1 = std::chrono::steady_clock::now();
        const double s = std::chrono::duration<double>(t1 - t0).count();
        if (r > 0 && s < best_s) {
          best_s = s;
        }
      }
      const double gib_s = static_cast<double>(input.size()) / best_s /
                           (1024.0 * 1024.0 * 1024.0);
      std::printf("  t16-%-9s %6.2f GiB/s   (%s...)\n",
                  std::string(blake3pp::to_string(strat)).c_str(), gib_s,
                  d.to_hex().substr(0, 16).c_str());
    }
    blake3pp::set_transpose16(saved);
    cooldown();
    const auto picked = blake3pp::tune_transpose16();
    std::printf("  t16 tuner picks: %s\n",
                std::string(blake3pp::to_string(picked)).c_str());
  }

#if defined(BLAKE3PP_BENCH_UPSTREAM) && \
    (defined(__x86_64__) || defined(__aarch64__))
  // Upstream's wide kernel driven by OUR tree and parallel machinery.
#if defined(__x86_64__)
  constexpr auto asm_arch = blake3pp::arch::avx2;
  constexpr const char* asm_row = "asm-avx2";
  constexpr const char* asm_what = "upstream asm";
#else
  constexpr auto asm_arch = blake3pp::arch::neon;
  constexpr const char* asm_row = "asm-neon";
  constexpr const char* asm_what = "upstream neon";
#endif
  if (blake3pp::is_available(asm_arch)) {
    blake3pp::kern::kernel_ops asm_ops = *blake3pp::detail::resolve(asm_arch);
    asm_ops.hash_many = &asm_hash_many;  // compress_in_place stays portable

    {
      cooldown();
      blake3pp::digest d{};
      double best_s = 1e100;
      for (int r = 0; r < reps + 1; ++r) {
        blake3pp::hasher h{&asm_ops};
        const auto t0 = std::chrono::steady_clock::now();
        h.update(std::span<const std::byte>{input});
        d = h.finalize();
        const auto t1 = std::chrono::steady_clock::now();
        const double s = std::chrono::duration<double>(t1 - t0).count();
        if (r > 0 && s < best_s) {
          best_s = s;
        }
      }
      const double gib_s = static_cast<double>(input.size()) / best_s /
                           (1024.0 * 1024.0 * 1024.0);
      std::printf("%-8s %8.2f GiB/s   (%s...)  [%s in our tree]\n", asm_row,
                  gib_s, d.to_hex().substr(0, 16).c_str(), asm_what);
    }

    {
      cooldown();
      auto sched = blake3pp::get_parallel_scheduler();
      blake3pp::digest d{};
      double best_s = 1e100;
      for (int r = 0; r < reps + 1; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        d = blake3pp::hash(std::span<const std::byte>{input}, sched, &asm_ops);
        const auto t1 = std::chrono::steady_clock::now();
        const double s = std::chrono::duration<double>(t1 - t0).count();
        if (r > 0 && s < best_s) {
          best_s = s;
        }
      }
      const double gib_s = static_cast<double>(input.size()) / best_s /
                           (1024.0 * 1024.0 * 1024.0);
      std::printf("%-8s %8.2f GiB/s   (%s...)  [%s, parallel]\n", "asm-par",
                  gib_s, d.to_hex().substr(0, 16).c_str(), asm_what);
    }
  }
#endif

  // The sender-based parallel engine over the process-wide parallel
  // scheduler: the number that matters for feeding modern storage.
  {
    cooldown();
    const unsigned nthreads = std::thread::hardware_concurrency();
    auto sched = blake3pp::get_parallel_scheduler();
    blake3pp::digest d{};
    double best_s = 1e100;
    for (int r = 0; r < reps + 1; ++r) {
      const auto t0 = std::chrono::steady_clock::now();
      d = blake3pp::hash(std::span<const std::byte>{input}, sched);
      const auto t1 = std::chrono::steady_clock::now();
      const double s = std::chrono::duration<double>(t1 - t0).count();
      if (r > 0 && s < best_s) {
        best_s = s;
      }
    }
    const double gib_s =
        static_cast<double>(input.size()) / best_s / (1024.0 * 1024.0 * 1024.0);
    std::printf("%-8s %8.2f GiB/s   (%s...)  [%s, %u threads]\n", "parallel",
                gib_s, d.to_hex().substr(0, 16).c_str(),
                blake3pp::execution_provider().data(), nthreads);
  }

#if defined(BLAKE3PP_EXECUTION_STDEXEC) && defined(__APPLE__)
  // The identical sender pipeline on Apple's platform runtime: stdexec's
  // libdispatch scheduler submits the bulk work to GCD's global pool
  // (QoS-placed across P/E cores by the OS) instead of a process-owned
  // thread pool. Same engine, different scheduler argument; compare with
  // the parallel row above.
  {
    cooldown();
    exec::libdispatch_queue queue;
    auto sched = queue.get_scheduler();
    blake3pp::digest d{};
    double best_s = 1e100;
    for (int r = 0; r < reps + 1; ++r) {
      const auto t0 = std::chrono::steady_clock::now();
      d = blake3pp::hash(std::span<const std::byte>{input}, sched);
      const auto t1 = std::chrono::steady_clock::now();
      const double s = std::chrono::duration<double>(t1 - t0).count();
      if (r > 0 && s < best_s) {
        best_s = s;
      }
    }
    const double gib_s =
        static_cast<double>(input.size()) / best_s / (1024.0 * 1024.0 * 1024.0);
    std::printf("%-8s %8.2f GiB/s   (%s...)  [stdexec on GCD]\n", "gcd-par",
                gib_s, d.to_hex().substr(0, 16).c_str());
  }
#endif

#if defined(BLAKE3PP_BENCH_UPSTREAM)
  // Baseline: the official C library with its hand-written assembly kernels,
  // own runtime dispatch, same input. The digest must match ours.
  {
    cooldown();
    std::uint8_t out[BLAKE3_OUT_LEN];
    double best_s = 1e100;
    for (int r = 0; r < reps + 1; ++r) {
      blake3_hasher h;
      blake3_hasher_init(&h);
      const auto t0 = std::chrono::steady_clock::now();
      blake3_hasher_update(&h, input.data(), input.size());
      blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
      const auto t1 = std::chrono::steady_clock::now();
      const double s = std::chrono::duration<double>(t1 - t0).count();
      if (r > 0 && s < best_s) {
        best_s = s;
      }
    }
    const double gib_s =
        static_cast<double>(input.size()) / best_s / (1024.0 * 1024.0 * 1024.0);
    std::printf("%-8s %8.2f GiB/s   (", "upstream", gib_s);
    for (int i = 0; i < 8; ++i) {
      std::printf("%02x", out[i]);
    }
    std::printf("...)  [official C/asm %s]\n", BLAKE3_VERSION_STRING);
  }
#endif
  return 0;
}
