// End-to-end file hashing throughput: the number the whole project is
// about. Hashes a real file through the windowed io_uring pipeline,
// sequential and parallel, and reports the engaged backend.
//
//   blake3pp_bench_file <path> [--reps N] [--window MiB] [--qd N]
//                       [--no-direct] [--seq-only]
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

#if !defined(BLAKE3PP_HAS_STD_SENDERS)
#include <exec/static_thread_pool.hpp>
#endif

int main(int argc, char** argv) {
  std::string path;
  int reps = 3;
  blake3pp::hash_file_options opts;
  bool seq_only = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--reps" && i + 1 < argc) {
      reps = std::atoi(argv[++i]);
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

  {
    blake3pp::detail::file_reader probe(
        path.c_str(),
        {opts.window_bytes, opts.queue_depth, opts.direct_io, true});
    std::printf("file: %s (%.1f MiB), backend: %s, window %zu MiB, qd %u\n",
                path.c_str(),
                static_cast<double>(probe.file_size()) / (1024.0 * 1024.0),
                probe.backend(), opts.window_bytes >> 20, opts.queue_depth);
  }

  const auto run = [&](const char* label, auto&& fn) {
    blake3pp::digest d{};
    double best = 1e100;
    std::uint64_t bytes = 0;
    for (int r = 0; r < reps; ++r) {
      const auto t0 = std::chrono::steady_clock::now();
      d = fn();
      const auto t1 = std::chrono::steady_clock::now();
      const double s = std::chrono::duration<double>(t1 - t0).count();
      if (s < best) {
        best = s;
      }
    }
    blake3pp::detail::file_reader sz(path.c_str(), {});
    bytes = sz.file_size();
    std::printf("%-10s %8.2f GiB/s   (%s...)\n", label,
                static_cast<double>(bytes) / best / (1024.0 * 1024.0 * 1024.0),
                d.to_hex().substr(0, 16).c_str());
  };

  run("seq", [&] { return blake3pp::hash_file(path.c_str(), opts); });

#if !defined(BLAKE3PP_HAS_STD_SENDERS)
  if (!seq_only) {
    exec::static_thread_pool pool(std::thread::hardware_concurrency());
    auto sched = pool.get_scheduler();
    run("parallel",
        [&] { return blake3pp::hash_file(path.c_str(), sched, opts); });
  }
#endif
  return 0;
}
