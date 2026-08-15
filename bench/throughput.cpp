// Single-thread hashing throughput per architecture variant.
//
//   blake3pp_bench [--size <MiB>] [--reps <N>] [arch ...]
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
#include <vector>

#include <blake3pp/blake3pp.hpp>

#if defined(BLAKE3PP_BENCH_UPSTREAM)
#include <blake3.h>
#endif

namespace {

constexpr blake3pp::arch all_arches[] = {
    blake3pp::arch::scalar, blake3pp::arch::sse42, blake3pp::arch::avx2,
    blake3pp::arch::avx512, blake3pp::arch::neon,
};

blake3pp::arch parse_arch(const std::string& name) {
  for (const auto a : all_arches) {
    if (name == blake3pp::to_string(a)) {
      return a;
    }
  }
  std::fprintf(stderr, "unknown arch '%s'\n", name.c_str());
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t mib = 512;
  int reps = 5;
  std::vector<blake3pp::arch> arches;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--size" && i + 1 < argc) {
      mib = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (arg == "--reps" && i + 1 < argc) {
      reps = std::atoi(argv[++i]);
    } else {
      arches.push_back(parse_arch(arg));
    }
  }
  if (arches.empty()) {
    for (const auto a : all_arches) {
      if (blake3pp::is_available(a)) {
        arches.push_back(a);
      }
    }
  }

  std::vector<std::byte> input(mib * 1024 * 1024);
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<std::byte>(i % 251);
  }

  std::printf("blake3pp single-thread throughput, %zu MiB, best of %d\n",
              mib, reps);
  std::printf("auto resolves to: %s\n\n",
              blake3pp::to_string(blake3pp::best_available()));

  for (const auto a : arches) {
    if (!blake3pp::is_available(a)) {
      std::printf("%-8s unavailable on this machine\n",
                  blake3pp::to_string(a));
      continue;
    }
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

#if defined(BLAKE3PP_BENCH_UPSTREAM)
  // Baseline: the official C library with its hand-written assembly kernels,
  // own runtime dispatch, same input. The digest must match ours.
  {
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
