// blake3ppgen: a deterministic, SEEKABLE stream generator built on
// BLAKE3's extended output.
//
//   blake3ppgen --seed hello --length 1G > testdata.bin
//   blake3ppgen --seed hello --seek 10G --length 1M > slice.bin
//
// The same seed always produces the same infinite stream, and --seek is
// O(1), so a slice at offset 10 GB costs the same as one at offset 0. That
// makes it a reproducible test-data source at hashing speed (dd for
// deterministic bytes).
//
//   * --derive-key domain-separates streams sharing seed material.
//   * --hex prints hex instead of raw bytes.
//   * --output FILE writes through the platform's direct async writer
//     where available (io_uring, IoRing or IOCP, GCD, POSIX AIO). The
//     generator fills the writer's aligned buffers in place, so bytes go
//     from the XOF kernel to the device with no page cache and no
//     intermediate copy.
//   * --seed-file streams the seed through the library's file pipeline
//     (direct async reads, multi-core windows when --threads allows)
//     without first copying it into memory.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <CLI/CLI.hpp>
#include <blake3pp/parallel_io.hpp>

#include "diagnosis.hpp"
#include "tool_common.hpp"
#include "trace_json.hpp"

namespace {

using b3tool::println;

// Parses "123", "16K", "8M", "2G", "1T" (binary units); "inf" is nullopt.
std::optional<std::uint64_t> parse_size(const std::string& text) {
  if (text == "inf") {
    return std::nullopt;
  }
  std::size_t consumed = 0;
  const std::uint64_t base = std::stoull(text, &consumed);
  std::uint64_t multiplier = 1;
  if (consumed + 1 == text.size()) {
    switch (text[consumed]) {
      case 'K': multiplier = std::uint64_t{1} << 10; break;
      case 'M': multiplier = std::uint64_t{1} << 20; break;
      case 'G': multiplier = std::uint64_t{1} << 30; break;
      case 'T': multiplier = std::uint64_t{1} << 40; break;
      default: throw CLI::ValidationError("size", "unknown unit suffix");
    }
  } else if (consumed != text.size()) {
    throw CLI::ValidationError("size", "malformed size");
  }
  return base * multiplier;
}

// The bytes still to emit: bounded by --length, or unbounded for "inf".
class budget {
 public:
  explicit budget(std::optional<std::uint64_t> total) : left_(total) {}

  [[nodiscard]] bool exhausted() const noexcept { return left_ == 0; }
  [[nodiscard]] std::optional<std::uint64_t> total() const noexcept {
    return left_;
  }

  // How much of a buffer of `capacity` bytes to fill next.
  [[nodiscard]] std::size_t take(std::size_t capacity) const noexcept {
    return left_ ? static_cast<std::size_t>(std::min<std::uint64_t>(
                       *left_, capacity))
                 : capacity;
  }
  void consume(std::size_t n) noexcept {
    if (left_) {
      *left_ -= n;
    }
  }

 private:
  std::optional<std::uint64_t> left_;
};

// --diag: which side of the write pipeline was the constraint, from
// the counters the run kept.
//
// The directions are not symmetrical. Reading, the pool consumes
// windows the device produces and the arena between them is a third
// number. Writing, the pool PRODUCES windows the device drains, and one
// window is one fill task, so the windows in flight are the generator's
// parallelism and the device's queue at once -- one number, and no
// arena reading separate from it. So the two candidates share the same
// windows and the reading is which of them held them longer.
//
// starved_ns is not printed on either side. Reading, it counts "no free
// window", which a run whose pool is the constraint spends most of its
// wall clock in, so it is not the pool-starvation measure it reads like.
// Writing, it is only counted where the depth is at least the
// generator's parallelism, which is the configuration a
// generator-limited run is NOT in.
void report_write_diagnosis(const b3tool::diagnosis& r, unsigned threads) {
  const auto service = [](double ns) {
    return ns >= 1e6 ? std::format("{:.3f} ms", ns / 1e6)
                     : std::format("{:.1f} us", ns / 1e3);
  };
  if (!r.measured()) {
    println(stderr,
            "blake3ppgen: diagnosis: nothing was written through the "
            "pipeline, so there is no run to describe");
    return;
  }
  if (!r.long_enough()) {
    println(stderr,
            "blake3ppgen: diagnosis: {} writes at a depth of {} is this run's "
            "own ramp and drain, not a measurement of either end",
            r.transfers, r.depth);
    return;
  }
  println(stderr, "diagnosis: {} (write side)",
          r.label == b3tool::reading::undecided
              ? std::format("{} or {}, this run does not separate them",
                            b3tool::to_string(r.first),
                            b3tool::to_string(r.second))
              : b3tool::to_string(r.label));
  println(stderr,
          "  generator      {:5.1f}% of the {} windows in flight "
          "({:.1f}% of {} threads)",
          100.0 * r.fill_busy, r.depth, 100.0 * r.pool_busy, threads);
  println(stderr,
          "  windows        {} in flight, {} each ({}); the arena and the "
          "depth are one number here",
          r.depth, b3tool::bytes_label(r.window_bytes),
          b3tool::bytes_label(r.depth * r.window_bytes));
  println(stderr,
          "  device         {} per window over {} writes (min {}), holding "
          "{:5.1f}% of the windows",
          service(r.mean_service_ns), r.transfers,
          service(static_cast<double>(r.min_service_ns)),
          100.0 * r.device_busy);
  switch (r.label) {
    case b3tool::reading::generator_limited:
      println(stderr,
              "  the windows spent more of the run being filled than being "
              "written; one window is one fill task, so the windows in "
              "flight are the generator's parallelism.");
      println(stderr,
              "  at {} threads a {} window plans depth {} ({} in flight).",
              threads, b3tool::bytes_label(r.window_bytes),
              blake3pp::detail::resolve_write_queue_depth(0, r.window_bytes,
                                                          threads),
              b3tool::bytes_label(
                  blake3pp::detail::resolve_write_queue_depth(
                      0, r.window_bytes, threads) *
                  r.window_bytes));
      break;
    case b3tool::reading::device_limited:
      println(stderr,
              "  the device held a full queue for the run: it is the wall.");
      break;
    case b3tool::reading::hash_limited:
    case b3tool::reading::arena_limited:
    case b3tool::reading::undecided:
      break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  b3tool::tool_startup();
  std::string seed;
  std::string seed_file;
  std::string context;
  std::string length_text = "inf";
  std::string seek_text = "0";
  std::string output = "-";
  bool hex = false;
  bool no_direct = false;
  bool no_async = false;
  bool inline_submit = false;
  bool no_ioring = false;
  bool waive_zero_fill = false;
  std::string trace_path;
  bool trace_agents = false;
  bool diag = false;
  std::size_t window_mib = 0;
  unsigned qd = 0;
  bool verbose = false;
  unsigned threads = b3tool::default_threads();

  CLI::App app{
      "Deterministic seekable byte stream from BLAKE3 extended output.\n"
      "The same seed always yields the same stream; --seek is O(1)."};

  // seed
  auto* seed_opt = app.add_option("--seed", seed, "seed string");
  auto* seed_file_opt = app.add_option("--seed-file", seed_file, "read seed bytes from FILE");
  seed_opt->excludes(seed_file_opt);
  seed_file_opt->excludes(seed_opt);

  app.add_option("--derive-key", context, "domain-separate the stream with this context string");
  app.add_option("--length", length_text, "bytes to emit: N, NK/NM/NG/NT, or 'inf'")->capture_default_str();
  app.add_option("--seek", seek_text, "starting offset in the stream")->capture_default_str();

  auto* hex_flag = app.add_flag("--hex", hex, "emit lowercase hex instead of raw bytes");
  auto* output_opt = app.add_option("--output", output, "write to FILE via direct async I/O (io_uring or IOCP where available) instead of stdout");
  hex_flag->excludes(output_opt);
  output_opt->excludes(hex_flag);

  app.add_flag("--no-direct", no_direct, "with --output: no direct I/O (write through the page cache)")->needs(output_opt);
  app.add_flag("--no-async", no_async, std::format("with --output: write synchronously ({})", b3tool::no_async_note(blake3pp::io_direction::write)))->needs(output_opt);
  app.add_flag("--inline-submit", inline_submit, "with --output: issue writes inline in the submitting thread, not on io_uring's workers")->needs(output_opt);
  app.add_flag("--no-ioring", no_ioring, "with --output, Windows: write through the completion port instead of the I/O ring")->needs(output_opt);
  app.add_flag("--waive-zero-fill", waive_zero_fill, "with --output, Windows: set the valid data length up front instead of writing a sparse file (needs the volume-maintenance privilege; exposes stale disk blocks until written)")->needs(output_opt);
  auto* trace_opt = app.add_option("--trace", trace_path, "with --output and --length, on the pipelined path: record per-window timing and write it as Chrome trace JSON to PATH (open in ui.perfetto.dev)")->needs(output_opt);
  app.add_flag("--trace-agents", trace_agents, "with --trace: also record which CPU filled each window, and for how long")->needs(trace_opt);
  app.add_flag("--diag", diag, "report on stderr which side limited the run (the generator or the device); needs --output and --length on the pipelined path")->needs(output_opt);
  app.add_option("--window", window_mib, "with --output: write buffer size in MiB (default: 4 MiB per generator thread, so every fill fans out fully)")->needs(output_opt);
  app.add_option("--qd", qd, "with --output: write buffers in flight (default: one per generator thread, or 2 with --threads 1; the writer clamps it to its range)")->needs(output_opt);
  app.add_flag("-v,--verbose", verbose, "report the engaged write backend on stderr");
  app.add_option("--threads", threads, "generator threads (1 = sequential; default: all)")->check(b3tool::at_least_one_thread)->capture_default_str();
  CLI11_PARSE(app, argc, argv);

  std::optional<std::uint64_t> length;
  std::uint64_t seek = 0;
  try {
    length = parse_size(length_text);
    const auto seek_to = parse_size(seek_text);
    if (!seek_to) {
      throw CLI::ValidationError("--seek", "must be finite");
    }
    seek = *seek_to;
  } catch (const std::exception& e) {
    println(stderr, "blake3ppgen: {}", e.what());
    return b3tool::exit_usage;
  }
  budget remaining(length);

  b3tool::compute_pool pool(threads);

  // Each parallel fill task takes one segment of this size. The pipelined
  // file sink makes each window one segment; the sequential file sink and
  // stdout buffer one segment per thread so every fill fans out fully, the
  // file sink capped at max_buffer. --window overrides the file sinks.
  constexpr std::size_t segment = blake3pp::default_fill_segment_bytes;
  constexpr std::size_t max_buffer = 64 * 1024 * 1024;

  // Seed ingestion: the hasher carries the mode, and a seed file streams
  // into it on the same pool that will generate the output.
  blake3pp::hasher h = context.empty()
                           ? blake3pp::hasher{}
                           : blake3pp::hasher::derive_key(context);
  if (!seed_file.empty()) {
    std::error_code ec;
    if (pool.parallel()) {
      blake3pp::update_file(h, seed_file, pool.scheduler(), ec);
    } else {
      blake3pp::update_file(h, seed_file, ec);
    }
    if (ec) {
      println(stderr, "blake3ppgen: {}: {}", seed_file, ec.message());
      return b3tool::exit_usage;
    }
  } else {
    h.update(seed);
  }

  blake3pp::output_reader stream = h.finalize_xof();
  stream.seek(seek);

  // Fills `out` from the stream's current position and advances it,
  // multi-core over the pool when one is running and there is more than
  // one segment to split. blake3pp::fill() does one segment or less
  // sequentially anyway, and asking for a scheduler is what starts the
  // pool, so a run emitting a few hundred bytes starts no workers.
  const auto fill = [&](std::span<std::byte> out) {
    if (pool.parallel() && out.size() > segment) {
      blake3pp::fill(stream, out, pool.scheduler(), segment);
    } else {
      blake3pp::fill(stream, out);
    }
  };

  if (output != "-" && pool.parallel() && remaining.total()) {
    // File sink, pipelined: windows are filled on the pool and written
    // by the driver, overlapping generation with the device instead of
    // alternating between them.
    //
    // The generator has to be addressable by offset, which an extended
    // output already is: seeking it is O(1), so each window copies the
    // reader and seeks its own position. The fill of one window is
    // sequential. The parallelism is the windows in flight, and a fan-out
    // nested inside a pool agent would deadlock a one-thread pool.
    try {
      blake3pp::file_write_options wopts;
      wopts.direct_io = !no_direct;
      wopts.async = !no_async;
      wopts.offload_submit = !inline_submit;
      wopts.ioring = !no_ioring;
      wopts.waive_zero_fill = waive_zero_fill;
      // One window per thread, each one segment, so every thread has a
      // window to generate into. The depth is this pool's size: the
      // library's automatic depth counts the machine's threads.
      wopts.window_bytes = window_mib > 0 ? window_mib * 1024 * 1024 : segment;
      wopts.queue_depth = qd > 0 ? qd : threads;
      const std::uint64_t total = *remaining.total();
      // One record per window, and with --trace-agents one agent record
      // per window, since a fill is one agent's work.
      std::vector<blake3pp::window_record> window_records;
      std::vector<blake3pp::agent_record> agent_records;
      std::optional<blake3pp::trace_buffer> trace;
      // --diag reads driver_stats only, which a buffer with no window
      // storage still collects: two clock reads and one relaxed add per
      // window, and no allocation.
      if (diag && trace_path.empty()) {
        trace.emplace(std::span<blake3pp::window_record>{});
        wopts.trace = &*trace;
      }
      if (!trace_path.empty()) {
        const std::size_t window =
            blake3pp::detail::rounded_window_bytes(wopts.window_bytes);
        const std::size_t windows =
            static_cast<std::size_t>((total + window - 1) / window) + 1;
        window_records.resize(windows);
        if (trace_agents) {
          agent_records.resize(windows);
        }
        trace.emplace(std::span{window_records}, std::span{agent_records});
        wopts.trace = &*trace;
      }
      const auto t0 = std::chrono::steady_clock::now();
      const blake3pp::write_report report = blake3pp::write_file(
          output, total,
          [&stream, seek](std::span<std::byte> out,
                          std::uint64_t off) noexcept {
            blake3pp::output_reader r = stream;
            r.seek(seek + off);
            blake3pp::fill(r, out);
          },
          pool.scheduler(), wopts);
      const double wall_ns = std::chrono::duration<double, std::nano>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
      if (verbose) {
        println(stderr, "blake3ppgen: write backend: {} (pipeline)",
                report.backend);
      }
      if (diag) {
        report_write_diagnosis(
            b3tool::diagnose_write(
                trace->driver(), wall_ns, threads,
                blake3pp::detail::rounded_window_bytes(wopts.window_bytes)),
            threads);
      }
      if (trace && !trace_path.empty()) {
        if (b3tool::write_chrome_trace(trace_path, *trace, "blake3ppgen",
                                       threads, /*write_side=*/true)) {
          println(stderr, "blake3ppgen: trace: {} window records{}, JSON to {}",
                  trace->windows().size(),
                  trace_agents ? std::format(" and {} agent records",
                                             trace->agents().size())
                               : std::string{},
                  trace_path);
        } else {
          println(stderr, "blake3ppgen: could not write {}", trace_path);
          return b3tool::exit_failure;
        }
      }
    } catch (const std::exception& e) {
      println(stderr, "blake3ppgen: {}", e.what());
      return b3tool::exit_failure;
    }
    return 0;
  }

  if (output != "-") {
    if (!trace_path.empty()) {
      println(stderr,
              "blake3ppgen: --trace records the pipelined path only (a known "
              "--length and more than one thread); nothing written");
    }
    if (diag) {
      println(stderr,
              "blake3ppgen: --diag describes the pipelined path only (a known "
              "--length and more than one thread); nothing reported");
    }
    // File sink, sequential: the generator fills one of the writer's
    // O_DIRECT-aligned buffers at a time and the next fills while the
    // device drains this one. This is the path for a one-thread pool, and
    // the only one for a length the generator decides (--length omitted),
    // which the pipelined form cannot preallocate.
    try {
      blake3pp::file_write_options wopts;
      wopts.direct_io = !no_direct;
      wopts.async = !no_async;
      wopts.offload_submit = !inline_submit;
      wopts.ioring = !no_ioring;
      wopts.waive_zero_fill = waive_zero_fill;
      if (window_mib > 0) {
        wopts.window_bytes = window_mib * 1024 * 1024;
      } else if (pool.parallel()) {
        // One segment per thread so every fill fans out, capped at
        // max_buffer: on a 32-thread machine writing to a striped NVMe
        // volume, the uncapped default put half a gigabyte in flight and
        // cost 17%.
        wopts.window_bytes = std::min<std::size_t>(threads * segment, max_buffer);
      }
      if (qd > 0) {
        wopts.queue_depth = qd;
      }
      // The generator walks the stream in file order, so it takes the
      // bytes the remaining budget allows and ends the file by filling
      // less than a whole buffer.
      const auto produce = [&](std::span<std::byte> out,
                               std::uint64_t) -> std::size_t {
        if (remaining.exhausted()) {
          return 0;
        }
        const std::size_t take = remaining.take(out.size());
        fill(out.first(take));
        remaining.consume(take);
        return take;
      };
      const blake3pp::write_report wrote =
          remaining.total()
              ? blake3pp::write_file(output, *remaining.total(),
                                    [&](std::span<std::byte> out,
                                        std::uint64_t off) { produce(out, off); },
                                    wopts)
              : blake3pp::write_file(output, produce, wopts);
      if (verbose) {
        println(stderr, "blake3ppgen: write backend: {}", wrote.backend);
      }
    } catch (const std::exception& e) {
      println(stderr, "blake3ppgen: {}", e.what());
      return b3tool::exit_failure;
    }
    return 0;
  }

  std::vector<std::byte> buf(pool.parallel() ? threads * segment
                                             : b3tool::stream_buffer_bytes);
  while (!remaining.exhausted()) {
    const std::size_t take = remaining.take(buf.size());
    fill(std::span{buf}.first(take));
    if (hex) {
      const std::string line = blake3pp::to_hex(std::span{buf}.first(take));
      if (std::fwrite(line.data(), 1, line.size(), stdout) != line.size()) {
        break;  // downstream closed (e.g. head); not an error
      }
    } else if (std::fwrite(buf.data(), 1, take, stdout) != take) {
      break;
    }
    remaining.consume(take);
  }
  if (hex) {
    std::fputc('\n', stdout);
  }
  return 0;
}
