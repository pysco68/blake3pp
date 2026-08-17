// End-to-end file hashing throughput: the number the whole project is
// about. First measures the RAW device read speed through the identical
// windowed pipeline (same backend, window, queue depth; data delivered
// and discarded, no hashing), then hashes for real, sequential and
// parallel. The utilization column relates the two: 100%% means the
// drive, not the hash, is the limit.
//
//   blake3pp_bench_file <path> [--reps N] [--window MiB] [--qd N]
//                       [--no-direct] [--seq-only] [--cooldown s]
//   blake3pp_bench_file --make <MiB>   # create a test file and use it
//
// Note: with direct I/O the page cache is bypassed, so repetitions measure
// the device (or the host-side cache of a virtualized disk), not RAM.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <blake3pp/io.hpp>



int main(int argc, char** argv) {
  std::string path;
  int reps = 3;
  double cooldown_s = 5.0;
  blake3pp::hash_file_options opts;
  bool seq_only = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--reps" && i + 1 < argc) {
      reps = std::atoi(argv[++i]);
    } else if (arg == "--cooldown" && i + 1 < argc) {
      cooldown_s = std::strtod(argv[++i], nullptr);
    } else if (arg == "--window" && i + 1 < argc) {
      opts.window_bytes =
          static_cast<std::size_t>(std::atoi(argv[++i])) * 1024 * 1024;
    } else if (arg == "--qd" && i + 1 < argc) {
      opts.queue_depth = static_cast<unsigned>(std::atoi(argv[++i]));
    } else if (arg == "--no-direct") {
      opts.direct_io = false;
    } else if (arg == "--seq-only") {
      seq_only = true;
    } else if (arg == "--make" && i + 1 < argc) {
      const std::size_t mib =
          static_cast<std::size_t>(std::atoi(argv[++i]));
      path = "blake3pp_bench_file.dat";
      std::ofstream out(path, std::ios::binary);
      std::vector<char> block(1024 * 1024);
      for (std::size_t k = 0; k < block.size(); ++k) {
        block[k] = static_cast<char>(k % 251);
      }
      for (std::size_t m = 0; m < mib; ++m) {
        out.write(block.data(), static_cast<std::streamsize>(block.size()));
      }
      std::printf("created %s (%zu MiB)\n", path.c_str(), mib);
    } else {
      path = arg;
    }
  }
  if (path.empty()) {
    std::fprintf(stderr,
                 "usage: %s <path> | --make <MiB>  [--reps N] [--window MiB] "
                 "[--qd N] [--no-direct] [--seq-only]\n",
                 argv[0]);
    return 2;
  }

  std::uint64_t bytes = 0;
  {
    blake3pp::detail::file_reader probe(
        path.c_str(),
        {opts.window_bytes, opts.queue_depth, opts.direct_io, true});
    bytes = probe.file_size();
    std::printf("file: %s (%.1f MiB), backend: %s, window %zu MiB, qd %u\n",
                path.c_str(), static_cast<double>(bytes) / (1024.0 * 1024.0),
                probe.backend(), opts.window_bytes >> 20, opts.queue_depth);
  }

  const auto time_best = [&](auto&& fn) {
    double best = 1e100;
    for (int r = 0; r < reps; ++r) {
      const auto t0 = std::chrono::steady_clock::now();
      fn();
      const auto t1 = std::chrono::steady_clock::now();
      const double s = std::chrono::duration<double>(t1 - t0).count();
      if (s < best) {
        best = s;
      }
    }
    return best;
  };
  const auto gibs = [&](double secs) {
    return static_cast<double>(bytes) / secs / (1024.0 * 1024.0 * 1024.0);
  };

  // The control group: the identical pipeline delivering windows that are
  // simply released unread. This is the device ceiling as seen through
  // this backend/window/qd; every hash row below is a fraction of it.
  const double raw_s = time_best([&] {
    blake3pp::detail::file_reader r(
        path.c_str(),
        {opts.window_bytes, opts.queue_depth, opts.direct_io, true});
    auto w = r.next();
    while (w.has_value()) {
      r.release(w.value());
      w = r.next();
    }
  });
  std::printf("%-10s %8.2f GiB/s   [device ceiling, no hashing]\n", "raw io",
              gibs(raw_s));

  const auto run = [&](const char* label, auto&& fn) {
    // Laptops throttle: don't let this measurement inherit the previous
    // one's heat (the raw-io pass above ran unconditionally already).
    if (cooldown_s > 0) {
      std::fflush(stdout);
      std::this_thread::sleep_for(std::chrono::duration<double>(cooldown_s));
    }
    blake3pp::digest d{};
    const double best = time_best([&] { d = fn(); });
    std::printf("%-10s %8.2f GiB/s   (%s...)  [%3.0f%% of raw]\n", label,
                gibs(best), d.to_hex().substr(0, 16).c_str(),
                100.0 * raw_s / best);
  };

  run("seq", [&] { return blake3pp::hash_file(path.c_str(), opts); });

  if (!seq_only) {
    auto sched = blake3pp::get_parallel_scheduler();
    run("parallel",
        [&] { return blake3pp::hash_file(path.c_str(), sched, opts); });
  }
  return 0;
}
