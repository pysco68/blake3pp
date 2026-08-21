// End-to-end file hashing throughput: the number the whole project is
// about. First measures the RAW device read speed through the identical
// windowed pipeline (same backend, window, queue depth; data delivered
// and discarded, no hashing), then hashes for real, sequential and
// parallel. The utilization column relates the two: 100% means the drive,
// not the hash, is the limit.
//
//   blake3pp_bench_file <FILE> [--reps N] [--window MiB] [--qd N]
//                       [--no-direct] [--seq-only] [--cooldown S]
//   blake3pp_bench_file --make <MiB>   # create a test file and use it
//
// Note: with direct I/O the page cache is bypassed, so repetitions measure
// the device (or the host-side cache of a virtualized disk), not RAM.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

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

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  int reps = 3;
  double cooldown_s = 5.0;
  std::size_t window_mib = 8;
  std::size_t make_mib = 0;
  bool seq_only = false;
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
  app.add_option("--window", window_mib, "I/O window size in MiB")
      ->check(CLI::PositiveNumber)
      ->capture_default_str();
  app.add_option("--qd", opts.queue_depth, "I/O queue depth")
      ->check(CLI::Range(2u, 32u))
      ->capture_default_str();
  app.add_flag("!--no-direct", opts.direct_io,
               "keep the OS page cache (no O_DIRECT)");
  app.add_flag("--seq-only", seq_only, "skip the parallel measurement");
  CLI11_PARSE(app, argc, argv);

  opts.window_bytes = window_mib * 1024 * 1024;
  if (make_mib > 0) {
    path = make_test_file(make_mib);
    println(stdout, "created {} ({} MiB)", path, make_mib);
  }
  if (path.empty()) {
    println(stderr, "blake3pp_bench_file: give a FILE, or --make <MiB>");
    return 2;
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
    auto sched = blake3pp::get_parallel_scheduler();
    run("parallel",
        [&] { return blake3pp::hash_file(path.c_str(), sched, opts); });
  }
  return 0;
}
