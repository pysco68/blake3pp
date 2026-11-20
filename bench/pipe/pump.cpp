// pump: a producer for a pipe, with nothing in it but the writes.
//
//   pump --gen <GiB> [--chunk <KiB>] [--gap-us <us>]
//   pump --file <path> [--chunk <KiB>] [--gap-us <us>]
//
// --gen writes a generated pattern from memory, so no device is in the
// path and the figure is the pipe's and the consumer's. --file forwards a
// file in --chunk reads, as `cat` does with a buffer of that size. Every
// write is exactly one --chunk (the last may be shorter), so the
// producer's write size is a stated quantity.
//
// --gap-us paces the writes: one chunk, then a gap measured on the
// monotonic clock from the start of that write. A gap of 2 ms or more
// sleeps; a shorter one spins, because the platform's sleep is far
// coarser than that (a Windows sleep rounds up to the 15.6 ms timer tick
// unless the process raises the timer resolution, which this does not).
// A pacing that cannot be met is reported, not hidden.
//
// The pattern repeats every chunk, so the digest of a --gen stream
// depends on --chunk as well as on --gen.
//
// Faults, each at an exact byte count, so a test can compute what the
// consumer must have received:
//   --close-after N   write N bytes, then close and exit 0: a short stream
//   --die-after N     write N bytes, then terminate abnormally with no
//                     clean exit: SIGKILL on POSIX, TerminateProcess on
//                     Windows. The bytes already written stay in the pipe.
//   --stall-ms T --after N
//                     after N bytes, pause T ms once, then carry on
//   --wait-for PATH --after N
//                     after N bytes, wait until PATH exists, then carry on.
//                     A handshake with another process (drain --mark),
//                     for a test whose ordering must not rest on a delay.
// A write is split where a threshold falls inside it.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>

#include <blake3pp/detail/cpu_relax.hpp>

#include "pipe_os.hpp"

namespace {

using clock_type = std::chrono::steady_clock;

// Holds the pace: returns once `gap` has passed since `from`.
void wait_until(clock_type::time_point from, std::chrono::microseconds gap) {
  const auto until = from + gap;
  if (gap >= std::chrono::milliseconds(2)) {
    std::this_thread::sleep_until(until);
    return;
  }
  while (clock_type::now() < until) {
    blake3pp::detail::cpu_relax();
  }
}

}  // namespace

int main(int argc, char** argv) {
  double gib = 0;
  std::string path;
  std::size_t chunk_kib = 256;
  std::int64_t gap_us = 0;
  std::uint64_t close_after = 0, die_after = 0, stall_after = 0;
  std::int64_t stall_ms = 0;
  std::string wait_for;

  CLI::App app{"pump: write a generated or file-backed stream to stdout in fixed-size writes"};
  auto* gen = app.add_option("--gen", gib, "GiB to generate in memory");
  auto* file = app.add_option("--file", path, "file to forward");
  gen->excludes(file);
  app.add_option("--chunk", chunk_kib, "bytes per write, in KiB")
      ->check(CLI::PositiveNumber);
  app.add_option("--gap-us", gap_us, "microseconds from one write's start to the next")
      ->check(CLI::NonNegativeNumber);
  auto* close_opt = app.add_option("--close-after", close_after, "write this many bytes, then exit cleanly");
  auto* die_opt = app.add_option("--die-after", die_after, "write this many bytes, then terminate abnormally");
  close_opt->excludes(die_opt);
  auto* stall_opt = app.add_option("--stall-ms", stall_ms, "pause this long once, after --after bytes")
                        ->check(CLI::NonNegativeNumber);
  auto* wait_opt = app.add_option("--wait-for", wait_for, "after --after bytes, wait until this file exists");
  auto* after_opt = app.add_option("--after", stall_after, "where --stall-ms or --wait-for pauses, in bytes");
  stall_opt->needs(after_opt);
  wait_opt->needs(after_opt);
  CLI11_PARSE(app, argc, argv);
  if (gen->count() == 0 && file->count() == 0) {
    std::fprintf(stderr, "pump: one of --gen or --file is required\n");
    return 2;
  }

  const std::size_t chunk = chunk_kib * 1024;
  std::vector<unsigned char> buf(chunk);
  for (std::size_t i = 0; i < chunk; ++i) {
    buf[i] = static_cast<unsigned char>(i * 167 + i / 251);
  }
  const pipebench::handle out = pipebench::std_out();
  const std::chrono::microseconds gap(gap_us);
  std::uint64_t late = 0;  // writes that started after their slot
  std::uint64_t writes = 0;

  std::uint64_t written = 0;
  bool stalled = stall_ms == 0 && wait_for.empty();
  // The pause at --after: a fixed delay, a wait for a file, or both.
  auto pause_here = [&] {
    if (stall_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
    }
    if (!wait_for.empty()) {
      std::error_code ec;
      while (!std::filesystem::exists(wait_for, ec)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    stalled = true;
  };
  const bool closing = close_opt->count() > 0;
  const bool dying = die_opt->count() > 0;

  // One write of the stream, with the pacing; false when the reader went
  // away.
  auto paced_write = [&](const unsigned char* p, std::size_t n) {
    const auto start = clock_type::now();
    if (!pipebench::write_all(out, p, n)) {
      return false;
    }
    ++writes;
    if (gap_us > 0) {
      if (clock_type::now() - start > gap) {
        ++late;
      }
      wait_until(start, gap);
    }
    return true;
  };

  // A chunk of the stream, cut where a fault's byte count falls inside
  // it. Returns 0 to carry on, or the exit code to leave with.
  auto emit = [&](const unsigned char* p, std::size_t n) -> int {
    while (n > 0) {
      std::uint64_t limit = n;
      auto cut_at = [&](std::uint64_t mark) {
        if (mark > written && mark - written < limit) limit = mark - written;
      };
      if (closing) cut_at(close_after);
      if (dying) cut_at(die_after);
      if (!stalled) cut_at(stall_after);
      if (!stalled && written >= stall_after) pause_here();
      if (closing && written >= close_after) return -1;  // done, cleanly
      if (dying && written >= die_after) pipebench::die();
      const std::size_t take = static_cast<std::size_t>(limit);
      if (!paced_write(p, take)) return 3;  // the reader went away
      p += take;
      n -= take;
      written += take;
    }
    if (!stalled && written >= stall_after) pause_here();
    if (closing && written >= close_after) return -1;
    if (dying && written >= die_after) pipebench::die();
    return 0;
  };

  if (!path.empty()) {
    std::FILE* f = pipebench::open_file(path.c_str(), "rb");
    if (f == nullptr) {
      std::fprintf(stderr, "pump: cannot open %s\n", path.c_str());
      return 1;
    }
    for (;;) {
      const std::size_t got = std::fread(buf.data(), 1, chunk, f);
      if (got == 0) {
        break;
      }
      const int rc = emit(buf.data(), got);
      if (rc != 0) {
        std::fclose(f);
        return rc < 0 ? 0 : rc;
      }
    }
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
      std::fprintf(stderr, "pump: read error on %s\n", path.c_str());
      return 1;
    }
  } else {
    auto total = static_cast<std::uint64_t>(gib * 1073741824.0);
    while (total > 0) {
      const std::size_t n = total < chunk ? static_cast<std::size_t>(total) : chunk;
      const int rc = emit(buf.data(), n);
      if (rc != 0) {
        return rc < 0 ? 0 : rc;
      }
      total -= n;
    }
  }
  if (dying) {
    pipebench::die();  // the stream was shorter than --die-after
  }
  if (late > 0) {
    std::fprintf(stderr,
                 "pump: %llu of %llu writes took longer than the %lld us gap; "
                 "the pacing was not met\n",
                 static_cast<unsigned long long>(late),
                 static_cast<unsigned long long>(writes),
                 static_cast<long long>(gap_us));
  }
  return 0;
}
