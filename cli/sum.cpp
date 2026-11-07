// blake3ppsum: a sha1sum-style checksum utility over the blake3pp engine.
//
//   blake3ppsum [OPTIONS] [FILE...]         print "<hex>  <file>" per file
//   blake3ppsum --check [OPTIONS] [LIST...] verify previously printed lines
//
// FILE of "-" (or no files) reads stdin. All three BLAKE3 modes are
// available (--keyed, --derive-key), plus extended output (--length), a
// pinned SIMD variant (--arch), compute threads (--threads) and the I/O
// pipeline knobs (--window/--qd/--no-direct).

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include <CLI/CLI.hpp>
#include <blake3pp/blake3pp.hpp>

#include "diagnosis_report.hpp"
#include "tool_common.hpp"
#include "trace_json.hpp"

namespace {

using b3tool::print;
using b3tool::println;
using b3tool::undiagnosable;
using b3tool::reading_label;
using b3tool::service_label;

// Trace windows allotted to an input whose size is not known up front: a
// pipe on standard input, and each checksum list under --check, whose
// files are named only as it is read. 64 GiB at the default window;
// records past it are counted as dropped.
constexpr std::size_t unsized_trace_windows = 16384;

// The size of standard input if it is a regular file (a redirect), or
// nothing for a pipe, a terminal or a device.
std::optional<std::uint64_t> stdin_file_size() noexcept {
#if defined(_WIN32)
  struct _stat64 st {};
  if (::_fstat64(0, &st) != 0 || (st.st_mode & _S_IFMT) != _S_IFREG) {
    return std::nullopt;
  }
#else
  struct stat st {};
  if (::fstat(0, &st) != 0 || !S_ISREG(st.st_mode)) {
    return std::nullopt;
  }
#endif
  return static_cast<std::uint64_t>(st.st_size);
}

struct options {
  blake3pp::hash_file_options io;
  unsigned threads = b3tool::default_threads();
  bool check = false;
  std::size_t out_len = blake3pp::digest_size;
  std::string key_file;        // --keyed: path to 32 raw bytes or 64 hex
  std::string derive_context;  // --derive-key
  std::string trace_path;      // --trace: Chrome trace JSON out
  bool trace_agents = false;   // --trace-agents
  bool diag = false;           // --diag: what limited the run
  std::vector<std::string> files;
};

std::string version_text() {
  std::string variants;
  for (const auto a : blake3pp::compiled_arches()) {
    std::format_to(std::back_inserter(variants), " {}{}",
                   blake3pp::to_string(a),
                   blake3pp::is_available(a) ? "" : "[no cpu support]");
  }
  // Variants the CPU supports that this binary does not carry (e.g.
  // xthead on T-Head silicon in a build whose compiler could not express
  // it). The line is printed only when there are any.
  std::string uncompiled;
  for (const auto a : blake3pp::all_arches()) {
    if (a != blake3pp::arch::auto_detect && blake3pp::cpu_supports(a) &&
        !blake3pp::is_available(a)) {
      std::format_to(std::back_inserter(uncompiled), " {}",
                     blake3pp::to_string(a));
    }
  }
  // The I/O engines in the same form: those the build carries, which of
  // them this kernel grants, and what a fresh open engages. Whether a
  // given FILE keeps the engine and bypasses the cache needs an open;
  // probe_file_io() answers that.
  std::string engines;
  for (const auto e : blake3pp::compiled_io_engines()) {
    std::format_to(std::back_inserter(engines), " {}{}",
                   blake3pp::to_string(e),
                   blake3pp::is_available(e) ? "" : "[no os support]");
  }
  std::string text = std::format(
      "blake3ppsum (blake3pp {})\n"
      "simd provider: {}\n"
      "execution provider: {}\n"
      "variants:{} (auto -> {})",
      blake3pp::version(), blake3pp::simd_provider(),
      blake3pp::execution_provider(), variants,
      blake3pp::to_string(blake3pp::best_available()));
  if (!uncompiled.empty()) {
    std::format_to(std::back_inserter(text),
                   "\ncpu also supports:{} (not compiled in)", uncompiled);
  }
  std::format_to(std::back_inserter(text), "\nio engines:{} (auto -> {})",
                 engines, blake3pp::to_string(blake3pp::best_io_engine()));
  return text;
}

// Accepts a file holding either exactly 32 raw bytes or 64 hex characters
// (trailing whitespace tolerated); "-" reads the key from stdin, in which
// case the data must come from files.
std::array<std::byte, blake3pp::key_size> load_key(const std::string& source) {
  std::string content;
  if (source == "-") {
    // Room for the hex form plus a line ending; anything longer is not a
    // key and fails the checks below.
    std::array<char, 2 * blake3pp::key_size + 4> buf;
    const std::size_t n = std::fread(buf.data(), 1, buf.size(), stdin);
    content.assign(buf.data(), n);
  } else {
    std::ifstream in(source, std::ios::binary);
    if (!in) {
      const int e = errno;  // before the throw allocates the exception
      throw std::system_error(e, std::generic_category(), source);
    }
    content.assign(std::istreambuf_iterator<char>(in), {});
  }
  if (content.size() == blake3pp::key_size) {
    std::array<std::byte, blake3pp::key_size> key;
    for (std::size_t i = 0; i < key.size(); ++i) {
      key[i] = static_cast<std::byte>(static_cast<unsigned char>(content[i]));
    }
    return key;
  }
  while (!content.empty() && (content.back() == '\n' || content.back() == '\r' || content.back() == ' ')) {
    content.pop_back();
  }
  if (const auto parsed = blake3pp::digest::from_hex(content)) {
    return parsed.value().bytes;
  }
  throw std::runtime_error("key must be exactly 32 raw bytes or 64 hex characters");
}

// One file's outcome: a digest, or why there is none.
struct result {
  std::string hex;
  std::string error;
};

// A name as a digest line writes it, in the GNU sha256sum and b3sum form:
// a name holding a backslash, a newline or a carriage return has them
// written as "\\", "\n" and "\r", and the line starts with a backslash
// (prefix). Any other name is written unchanged with an empty prefix.
struct line_name {
  std::string_view prefix;
  std::string text;
};

line_name escape_name(std::string_view name) {
  if (name.find_first_of("\\\n\r") == std::string_view::npos) {
    return {"", std::string{name}};
  }
  line_name out{"\\", {}};
  for (const char c : name) {
    switch (c) {
      case '\\': out.text += "\\\\"; break;
      case '\n': out.text += "\\n"; break;
      case '\r': out.text += "\\r"; break;
      default: out.text += c; break;
    }
  }
  return out;
}

// Whether a name converts to a std::filesystem::path. On Windows the
// conversion decodes the name in the process code page, UTF-8 under the
// tools' manifest, and throws on bytes that are not valid in it, as a
// checksum list written in another encoding may hold. Elsewhere a path is
// the name's bytes.
bool is_path(const std::string& name) noexcept {
#if defined(_WIN32)
  try {
    static_cast<void>(std::filesystem::path(name));
    return true;
  } catch (...) {
    return false;
  }
#else
  (void)name;
  return true;
#endif
}

// What --check found, summed over every list.
struct check_counts {
  int malformed = 0;    // lines not in the checksum-line format
  int unreadable = 0;   // listed files that could not be read
  int mismatched = 0;   // files whose digest differs from the listed one
  int bad_lists = 0;    // lists that could not be read or hold no
                        // well-formed line, each reported on its own

  [[nodiscard]] bool failed() const noexcept {
    return malformed + unreadable + mismatched + bad_lists > 0;
  }
};

#if defined(_MSC_VER)
// The engine's members carry an alignment, so MSVC pads the class and says
// so at its closing brace (C4324). The padding is intended.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
class engine {
 public:
  explicit engine(const options& o) : opts_(o), pool_(o.threads) {
    // The mode goes into the options, which every library entry point
    // here takes; the parser makes --keyed and --derive-key exclusive. The
    // mode copies the key's 32 bytes. The context string is a view into
    // opts_, this engine's own copy, so it lives as long as the engine.
    if (!opts_.key_file.empty()) {
      opts_.io.mode = blake3pp::hash_mode::keyed(load_key(opts_.key_file));
    } else if (!opts_.derive_context.empty()) {
      opts_.io.mode =
          blake3pp::hash_mode::derive_key(std::string_view{opts_.derive_context});
    }
  }

  // Hex of the requested output length over stdin or a file. Both go
  // through the library's pipeline when a pool exists -- a file through
  // the platform's async reader, stdin through the stream driver, which
  // fills one window on this thread while the pool hashes the ones
  // before it. Without a pool each is read into the plain hasher. Either
  // way the mode comes from the options.
  std::string hash_hex(const std::string& path, std::size_t out_len) {
    std::vector<std::byte> out(out_len);
    const auto finish = [&](const auto& h) {
      h.finalize(out);
      return blake3pp::to_hex(out);
    };
    if (path == "-") {
      blake3pp::hasher h = blake3pp::make_hasher(opts_.io);
      const auto t0 = std::chrono::steady_clock::now();
      if (pool_.parallel()) {
        // --qd reaches a stream as its window count: a stream has one
        // position and no device queue for a depth to set. Both defaults
        // are 4, so the default run is unchanged.
        stream_ = blake3pp::update_stream(
            h, blake3pp::standard_input(), pool_.scheduler(),
            {.window_bytes = opts_.io.window_bytes,
             .windows = opts_.io.queue_depth,
             .trace = opts_.io.trace});
        pipelined_ = true;
      } else {
        read_stdin_into(h);
        pipelined_ = false;
      }
      wall_ns_ = std::chrono::duration<double, std::nano>(
                     std::chrono::steady_clock::now() - t0)
                     .count();
      return finish(h);
    }
    blake3pp::hasher h = blake3pp::make_hasher(opts_.io);
    // Asking for the scheduler is what builds the pool, so a file too small
    // to fan out does not ask. Below min_pipeline_bytes the library skips
    // the pipeline and hashes sequentially anyway, and the threads would
    // be started, parked and joined for nothing. Measured on 16 threads, best
    // of eleven, a whole run took 7.6 ms with the pool and 4.8 ms without
    // on a 64 KiB file, and 6.7 and 3.3 ms on an empty one. A size the
    // filesystem will not give is treated as large: the pipeline is the
    // better guess for a file of unknown size (a block device, a procfs
    // entry).
    std::error_code size_ec;
    const std::uintmax_t size = std::filesystem::file_size(path, size_ec);
    const bool worth_a_pool =
        size_ec || size >= blake3pp::detail::min_pipeline_bytes;
    const auto t0 = std::chrono::steady_clock::now();
    if (pool_.parallel() && worth_a_pool) {
      blake3pp::update_file(h, path, pool_.scheduler(), opts_.io);
      pipelined_ = true;
    } else {
      blake3pp::update_file(h, path, opts_.io);
      pipelined_ = false;
    }
    wall_ns_ = std::chrono::duration<double, std::nano>(
                   std::chrono::steady_clock::now() - t0)
                   .count();
    return finish(h);
  }

  // The last file's wall clock, and whether it took the pipeline. --diag
  // reads both: a run on the sequential path has no driver and no
  // counters to read.
  [[nodiscard]] double wall_ns() const noexcept { return wall_ns_; }
  [[nodiscard]] bool pipelined() const noexcept { return pipelined_; }
  // What the last stdin run did, for the geometry --diag reads its
  // figures against. Zeroed unless a stream was hashed.
  [[nodiscard]] const blake3pp::stream_report& stream() const noexcept {
    return stream_;
  }

  // Whether hash_many() would do anything the per-file loop does not.
  // Standard input has no file to hand to a context, and one file needs
  // no arena shared with anything. A batch takes every name as a path up
  // front, so a name that is not one leaves the files to the per-file
  // loop, which reports that name alone as unreadable.
  [[nodiscard]] bool can_batch(const std::vector<std::string>& files) const {
    return pool_.parallel() && files.size() > 1 &&
           std::none_of(files.begin(), files.end(),
                        [](const std::string& f) { return f == "-"; }) &&
           std::all_of(files.begin(), files.end(), is_path);
  }

  // Several files over one context, a bounded number in flight at a
  // time, each outcome reported as its file completes and in input order.
  //
  // The gain over calling hash_hex() per file is the arena. The blocking
  // path allocates windows * window_bytes per call, takes the page faults
  // of its first pass over it, and lets no two files overlap. Here one
  // allocation serves every file, faults in once, and several files are
  // in flight at once.
  //
  // A file that fails carries its message instead of a digest, so one bad
  // file does not end the run. report(index, result) says whether that
  // file passed; the count of the ones that did not is returned. A write
  // to stdout that throws ends the run instead: it is stopped, counted as
  // one failure, and left to main()'s check of stdout to report.
  template <class Report>
  int run_batch(const std::vector<std::string>& files,
                const std::vector<std::size_t>& out_lens, Report report) {
    blake3pp::async::io_context ctx({.window_bytes = opts_.io.window_bytes,
                                     .queue_depth = opts_.io.queue_depth,
                                     .arena_bytes = 128u << 20,
                                     .direct_io = opts_.io.direct_io,
                                     .offload_submit = opts_.io.offload_submit,
                                     .ioring = opts_.io.ioring,
                                     .registered_buffers =
                                         opts_.io.registered_buffers});
    const std::vector<std::filesystem::path> paths(files.begin(), files.end());
    std::vector<std::optional<result>> out(files.size());
    std::size_t next = 0;  // the next outcome to report
    int failures = 0;
    bool write_failed = false;
    // The tracer is a type the sender carries, so the two spellings are
    // two instantiations of the same call.
    const auto run = [&](auto tracer) {
      auto work = blake3pp::async::async_update_files(
          ctx, paths, pool_.scheduler(), {.io = opts_.io},
          [this](std::size_t) { return blake3pp::make_hasher(opts_.io); },
          [&](std::size_t i, blake3pp::hasher& h,
              const blake3pp::async::file_outcome& o) noexcept {
            out[i] = describe(o, h, out_lens[i]);
            if (write_failed) {
              return;
            }
            try {
              for (; next < out.size() && out[next].has_value(); ++next) {
                if (!report(next, *out[next])) {
                  failures++;
                }
              }
            } catch (const std::system_error&) {
              write_failed = true;
              ctx.request_stop();
            }
          },
          tracer);
      // A stopped run returns an empty optional; only the write failure
      // stops one.
      blake3pp::async::sync_wait(ctx, std::move(work));
    };
    if (opts_.io.trace != nullptr) {
      run(blake3pp::buffer_tracer{opts_.io.trace});
    } else {
      run(blake3pp::null_tracer{});
    }
    return failures + (write_failed ? 1 : 0);
  }

  // The batch behind the printing path: one line per file.
  int hash_many(const std::vector<std::string>& files, std::size_t out_len) {
    return run_batch(files, std::vector<std::size_t>(files.size(), out_len),
                     [&](std::size_t i, const result& r) {
                       if (!r.error.empty()) {
                         println(stderr, "blake3ppsum: {}: {}", files[i],
                                 r.error);
                         return false;
                       }
                       const line_name n = escape_name(files[i]);
                       println(stdout, "{}{}  {}", n.prefix, r.hex, n.text);
                       return true;
                     });
  }

  // The batch behind --check: each file's digest taken to the length of
  // the hex it is compared against. Failures go into counts.
  void verify_many(const std::vector<std::string>& files,
                   const std::vector<std::string>& expected,
                   check_counts& counts) {
    std::vector<std::size_t> out_lens(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
      out_lens[i] = expected[i].size() / 2;
    }
    run_batch(files, out_lens,
              [&](std::size_t i, const result& r) {
                const line_name n = escape_name(files[i]);
                if (!r.error.empty()) {
                  println(stderr, "blake3ppsum: {}: {}", files[i], r.error);
                  println(stdout, "{}{}: FAILED open or read", n.prefix,
                          n.text);
                  counts.unreadable++;
                  return false;
                }
                const bool ok = r.hex == expected[i];
                println(stdout, "{}{}: {}", n.prefix, n.text,
                        ok ? "OK" : "FAILED");
                if (!ok) {
                  counts.mismatched++;
                }
                return ok;
              });
  }

 private:
  static result describe(const blake3pp::async::file_outcome& o,
                         blake3pp::hasher& h, std::size_t out_len) noexcept {
    result r;
    try {
      if (o.ok()) {
        std::vector<std::byte> digest(out_len);
        h.finalize(digest);
        r.hex = blake3pp::to_hex(digest);
      } else if (o.error) {
        r.error = o.error.message();
      } else if (o.stopped) {
        r.error = "cancelled";
      } else {
        std::rethrow_exception(o.exception);
      }
    } catch (const std::exception& e) {
      r.error = e.what();
    } catch (...) {
      r.error = "unknown failure";
    }
    return r;
  }

  // The sequential path's read of stdin, through the library's stream
  // reader. It retries a read interrupted by a signal (EINTR), waits out
  // a descriptor handed over non-blocking (EAGAIN), and on Windows bounds
  // its ask by what the pipe delivers. stdio's fread reports EINTR and
  // EAGAIN as errors, which would end the run with "Interrupted system
  // call" or "Resource temporarily unavailable".
  template <class Sink>
  static void read_stdin_into(Sink& sink) {
    blake3pp::detail::stream_reader reader{blake3pp::standard_input()};
    std::vector<std::byte> buf(b3tool::stream_buffer_bytes);
    for (;;) {
      const std::size_t bound = reader.read_bound();
      const std::size_t ask = bound == 0 ? buf.size() : std::min(buf.size(), bound);
      const blake3pp::detail::stream_reader::result r =
          reader.read_some(std::span{buf}.first(ask));
      if (r.eof) {
        return;
      }
      sink.update(std::span{buf}.first(r.bytes));
    }
  }

  options opts_;
  b3tool::compute_pool pool_;
  blake3pp::stream_report stream_{};
  double wall_ns_ = 0.0;
  bool pipelined_ = false;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// --diag: what the run that just happened was limited by, from the
// counters it kept. One line of reading, four of figures, and, where the
// arena was the constraint, the default plan's arithmetic at this
// machine's thread count. The last line states what a plan would
// allocate; it predicts no rate.
void report_read_diagnosis(const blake3pp::diagnosis& r, unsigned threads,
                           unsigned hw_threads) {
  if (const auto why = undiagnosable(r, "blake3ppsum", "reads",
                                     "nothing to read -- the file went down the sequential "
                   "path, which has no driver to report");
      why) {
    println(stderr, "{}", *why);
    return;
  }
  println(stderr, "diagnosis: {}", reading_label(r));
  println(stderr, "  pool busy      {:5.1f}% of {} threads",
          100.0 * r.pool_busy, threads);
  println(stderr,
          "  arena          {} of {} windows in flight, {} each ({}), {} the "
          "queue depth of {}",
          r.windows_used, r.windows_planned,
          b3tool::bytes_label(r.window_bytes),
          b3tool::bytes_label(r.windows_planned * r.window_bytes),
          r.arena_is_depth ? "no wider than" : "wider than", r.depth);
  // The bytes outstanding at the device, as depth times window. A
  // device's rate depends on them: one four-drive PCIe 5 stripe reads
  // 38.8 GiB/s through the pipeline at the default 32 MiB outstanding
  // and 46.9 at 1 GiB. A device reading needs this figure beside it.
  println(stderr, "  in flight      {} x {} = {} outstanding at the device",
          r.device_depth, b3tool::bytes_label(r.window_bytes),
          b3tool::bytes_label(r.bytes_in_flight));
  // Where the engine holds fewer reads than the pipeline queued, the
  // device answered the engine's count, not the queue depth.
  if (r.device_depth < r.depth) {
    println(stderr,
            "                 (the queue depth is {}; this engine holds {} at "
            "a time, and each read waited {} for a slot)",
            r.depth, r.device_depth, service_label(r.mean_lane_wait_ns));
  }
  println(stderr,
          "  device         {} per window over {} reads (min {}, {:.1f} of {} "
          "queued)",
          service_label(r.mean_service_ns), r.transfers,
          service_label(static_cast<double>(r.min_service_ns)),
          r.device_queued * static_cast<double>(r.device_depth),
          r.device_depth);
  switch (r.label) {
    case blake3pp::bottleneck::hash:
      println(stderr,
              "  the pool was saturated: the hash is the constraint and I/O "
              "geometry will not move it.");
      break;
    case blake3pp::bottleneck::device:
      // Named at the geometry: a full queue is the device's answer at
      // the bytes this run kept outstanding, which is not its ceiling
      // (see the in-flight line above).
      println(stderr,
              "  the pool had a window whenever it wanted one and still did "
              "not saturate, and the device's queue was full at {} "
              "outstanding: it is the wall at this geometry.",
              b3tool::bytes_label(r.bytes_in_flight));
      break;
    case blake3pp::bottleneck::arena:
      println(stderr,
              "  the pool idled waiting for a window while the device was not "
              "queued.");
      break;
    case blake3pp::bottleneck::in_flight:
      println(stderr,
              "  the pool had windows to spare and the device served every "
              "read as if it were alone: what the run never had more of is "
              "the {} outstanding, which is the depth times the window.",
              b3tool::bytes_label(r.bytes_in_flight));
      break;
    case blake3pp::bottleneck::generator:
    case blake3pp::bottleneck::undecided:
      break;
  }
  if (r.label == blake3pp::bottleneck::arena ||
      r.second == blake3pp::bottleneck::arena) {
    // The default budget rule at this machine's thread count:
    // update_file() over a scheduler resolves it from
    // hardware_concurrency(), not from --threads. Printed only where it
    // would allocate more windows than this run had; otherwise the arena
    // line already says it.
    const std::size_t budget =
        blake3pp::detail::read_plan_budget_bytes(hw_threads, r.window_bytes);
    const std::size_t windows = budget / r.window_bytes;
    if (windows > r.windows_planned) {
      println(stderr,
              "  at {} hardware threads a {} window plans {} windows ({}).",
              hw_threads, b3tool::bytes_label(r.window_bytes), windows,
              b3tool::bytes_label(budget));
    }
  }
}

// One checksum line in the GNU sha256sum and b3sum format: an optional
// leading backslash, the hex digest (all hex digits, even length), one
// space, then a space (text mode) or '*' (binary mode), then the name to
// the end of the line. On a line with the leading backslash the name is
// unescaped: "\\" is a backslash, "\n" a newline and "\r" a carriage
// return; any other escape is malformed. Nothing when the line is not in
// this format.
struct check_line {
  std::string hex;  // lowercase
  std::string name;
};

std::optional<check_line> parse_check_line(std::string_view sv) {
  const bool escaped = sv.starts_with('\\');
  if (escaped) {
    sv.remove_prefix(1);
  }
  const std::size_t hex_end = sv.find(' ');
  if (hex_end == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view hex = sv.substr(0, hex_end);
  if (hex.size() < 2 || hex.size() % 2 != 0 ||
      !std::all_of(hex.begin(), hex.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
      })) {
    return std::nullopt;
  }
  std::string_view rest = sv.substr(hex_end + 1);
  if (rest.size() < 2 || (rest.front() != ' ' && rest.front() != '*')) {
    return std::nullopt;
  }
  rest.remove_prefix(1);
  check_line out;
  out.hex.reserve(hex.size());
  for (const char c : hex) {
    out.hex += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (!escaped) {
    out.name = rest;
    return out;
  }
  for (std::size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] != '\\') {
      out.name += rest[i];
      continue;
    }
    if (++i == rest.size()) {
      return std::nullopt;
    }
    switch (rest[i]) {
      case '\\': out.name += '\\'; break;
      case 'n': out.name += '\n'; break;
      case 'r': out.name += '\r'; break;
      default: return std::nullopt;
    }
  }
  return out;
}

// Verifies the checksum lines of one list (see parse_check_line) of any
// digest length. Empty lines and lines starting with '#' are skipped, and
// one trailing carriage return is dropped from every line.
//
// The list is read first, then the files are hashed: with a compute pool
// and more than one file they go through one context together, as on the
// printing path. Malformed lines are reported as the list is read, so
// they print before the results. A read error on the list, and a list
// without one well-formed line, are reported and counted in bad_lists.
void run_check(engine& eng, std::istream& in, std::string_view list_name,
               check_counts& counts) {
  std::vector<std::string> names;
  std::vector<std::string> expected;
  std::string line;
  for (int line_no = 1; std::getline(in, line); ++line_no) {
    std::string_view sv{line};
    if (sv.ends_with('\r')) {
      sv.remove_suffix(1);
    }
    if (sv.empty() || sv.starts_with('#')) {
      continue;
    }
    std::optional<check_line> parsed = parse_check_line(sv);
    if (!parsed) {
      println(stderr, "blake3ppsum: {}:{}: malformed line", list_name, line_no);
      counts.malformed++;
      continue;
    }
    names.push_back(std::move(parsed->name));
    expected.push_back(std::move(parsed->hex));
  }
  if (in.bad() || !in.eof()) {
    println(stderr, "blake3ppsum: {}: read error", list_name);
    counts.bad_lists++;
  } else if (names.empty()) {
    println(stderr, "blake3ppsum: {}: no properly formatted checksum lines found",
            list_name);
    counts.bad_lists++;
  }

  if (eng.can_batch(names)) {
    eng.verify_many(names, expected, counts);
    return;
  }

  for (std::size_t i = 0; i < names.size(); ++i) {
    const std::string& name = names[i];
    const line_name n = escape_name(name);
    try {
      if (eng.hash_hex(name, expected[i].size() / 2) == expected[i]) {
        println(stdout, "{}{}: OK", n.prefix, n.text);
      } else {
        println(stdout, "{}{}: FAILED", n.prefix, n.text);
        counts.mismatched++;
      }
    } catch (const std::exception& e) {
      println(stderr, "blake3ppsum: {}: {}", name, e.what());
      println(stdout, "{}{}: FAILED open or read", n.prefix, n.text);
      counts.unreadable++;
    }
  }
}

}  // namespace

int run_main(int argc, char** argv) {
  b3tool::tool_startup();
  options o;
  constexpr std::size_t mib = std::size_t{1} << 20;
  bool no_direct = false;
  bool no_async = false;
  bool inline_submit = false;
  bool no_ioring = false;

  CLI::App app{
      "Print or check BLAKE3 checksums.\n"
      "With no FILE, or FILE of '-', read standard input."};
  app.set_version_flag("--version", version_text);

  app.add_flag("-c,--check", o.check, "read checksum lines from FILEs and verify them");
  auto* keyed = app.add_option("--keyed", o.key_file, "keyed (MAC) mode; FILE holds the 32-byte key as raw bytes or 64 hex chars ('-' reads it from stdin)");
  auto* derive = app.add_option("--derive-key", o.derive_context, "key-derivation mode with this context string");
  keyed->excludes(derive);
  derive->excludes(keyed);

  app.add_option("--length", o.out_len, "output length in bytes (extended output)")
      ->check(CLI::Range(std::size_t{1}, mib))
      ->capture_default_str();

  app.add_option("--arch", o.io.a, "pin a SIMD variant")
      ->transform(b3tool::arch_transformer())
      ->default_str("auto");

  app.add_option("--threads", o.threads, "compute threads (1 = sequential; default: all)")
      ->check(b3tool::at_least_one_thread)
      ->default_str(b3tool::threads_default_str());

  app.add_option("--window", o.io.window_bytes, "I/O window size in MiB")
      ->transform(b3tool::mib_to_bytes)
      ->default_str(std::to_string(o.io.window_bytes / mib));

  app.add_option("--qd", o.io.queue_depth, "I/O queue depth (the reader clamps it to its range)")
      ->capture_default_str();

  app.add_flag("--no-direct", no_direct, "keep the OS page cache (no O_DIRECT)");
  app.add_flag("--no-async", no_async, std::format("read synchronously ({})", b3tool::no_async_note()));
  app.add_flag("--inline-submit", inline_submit, "issue reads inline in the submitting thread, not on io_uring's workers");
  app.add_flag("--no-ioring", no_ioring, "Windows: read through the completion port instead of the I/O ring");
  auto* trace_opt = app.add_option("--trace", o.trace_path, "record per-window timing and write it as Chrome trace JSON to PATH (open in ui.perfetto.dev)");
  app.add_flag("--trace-agents", o.trace_agents, "with --trace: also record each pool agent's part of every window")->needs(trace_opt);
  app.add_flag("--diag", o.diag, "report on stderr which side limited the run (the hash, the device, or the window arena between them); one FILE or '-', on the pipelined path");
  app.add_option("files", o.files, "files to hash (or checksum lists)");

  CLI11_PARSE(app, argc, argv);

  o.io.direct_io = !no_direct;
  o.io.async = !no_async;
  o.io.offload_submit = !inline_submit;
  o.io.ioring = !no_ioring;
  if (!blake3pp::is_available(o.io.a)) {
    println(stderr, "blake3ppsum: arch '{}' not available on this machine (auto -> {})", blake3pp::to_string(o.io.a), blake3pp::to_string(blake3pp::best_available()));
    return b3tool::exit_usage;
  }

  if (o.key_file == "-" && (o.files.empty() || std::find(o.files.begin(), o.files.end(), "-") != o.files.end())) {
    println(stderr, "blake3ppsum: with the key on stdin, data must come from files");
    return b3tool::exit_usage;
  }

  // --diag reads the counters of one run. A batch or a checksum list
  // would sum several runs into one record, so the combination is a
  // usage error.
  if (o.diag && (o.check || o.files.size() > 1)) {
    println(stderr,
            "blake3ppsum: --diag describes one run: give at most one FILE, "
            "and not --check");
    return b3tool::exit_usage;
  }

  int failures = 0;
  try {
    // Trace storage is sized up front: one record per window of each
    // input, with a spare for the short first window. Standard input
    // redirected from a file is sized by fstat; a pipe, and every list
    // under --check, gets unsized_trace_windows. Records past the storage
    // are counted as dropped. The buffer outlives the engine, whose
    // operations record into it.
    std::vector<blake3pp::window_record> window_records;
    std::vector<blake3pp::agent_record> agent_records;
    std::optional<blake3pp::trace_buffer> trace;
    // --diag needs no per-window storage: every figure it reads is in
    // driver_stats, which a buffer with an empty window span still
    // collects, at a few clock reads per window and two per agent and no
    // allocation.
    if (o.diag && o.trace_path.empty()) {
      trace.emplace(std::span<blake3pp::window_record>{});
      o.io.trace = &*trace;
    }
    if (o.files.empty()) {
      o.files.emplace_back("-");
    }
    if (!o.trace_path.empty()) {
      const std::size_t window =
          blake3pp::detail::rounded_window_bytes(o.io.window_bytes);
      std::size_t windows = 0;
      for (const auto& f : o.files) {
        std::optional<std::uint64_t> size;
        if (o.check) {
          // The list's files are named only as it is read.
        } else if (f == "-") {
          size = stdin_file_size();
        } else {
          // A FIFO or device named on the command line has no size;
          // a path that is not there reads nothing.
          std::error_code ec;
          const std::uintmax_t n = std::filesystem::file_size(f, ec);
          if (!ec) {
            size = n;
          } else if (!std::filesystem::exists(f, ec)) {
            size = 0;
          }
        }
        windows += size ? static_cast<std::size_t>(*size / window) + 2
                        : unsized_trace_windows;
      }
      window_records.resize(windows);
      if (o.trace_agents) {
        agent_records.resize(windows * o.threads);
      }
      trace.emplace(std::span{window_records}, std::span{agent_records});
      o.io.trace = &*trace;
    }
    engine eng(o);

    // Written on every way out of the run, --check included.
    const auto write_trace = [&] {
      if (trace && !o.trace_path.empty()) {
        if (b3tool::write_chrome_trace(o.trace_path, *trace, "blake3ppsum",
                                       o.threads)) {
          println(stderr,
                  "blake3ppsum: trace: {} window records{}{}, JSON to {}",
                  trace->windows().size(),
                  o.trace_agents
                      ? std::format(" and {} agent records",
                                    trace->agents().size())
                      : std::string{},
                  trace->dropped_windows() > 0
                      ? std::format(" ({} dropped)", trace->dropped_windows())
                      : std::string{},
                  o.trace_path);
        } else {
          println(stderr, "blake3ppsum: could not write {}", o.trace_path);
          failures++;
        }
      }
    };

    if (o.check) {
      check_counts counts;
      for (const auto& f : o.files) {
        if (f == "-") {
          run_check(eng, std::cin, "-", counts);
          continue;
        }
        // Binary, as standard input is: the parser drops the carriage
        // return of a CRLF line itself, on every platform.
        std::ifstream in(f, std::ios::binary);
        if (!in) {
          println(stderr, "blake3ppsum: {}: cannot open", f);
          counts.bad_lists++;
          continue;
        }
        run_check(eng, in, f, counts);
      }
      if (counts.malformed > 0) {
        println(stderr, "blake3ppsum: WARNING: {} {} improperly formatted",
                counts.malformed,
                counts.malformed == 1 ? "line is" : "lines are");
      }
      if (counts.unreadable > 0) {
        println(stderr, "blake3ppsum: WARNING: {} listed {} could not be read",
                counts.unreadable, counts.unreadable == 1 ? "file" : "files");
      }
      if (counts.mismatched > 0) {
        println(stderr, "blake3ppsum: WARNING: {} computed {} did NOT match",
                counts.mismatched,
                counts.mismatched == 1 ? "checksum" : "checksums");
      }
      if (counts.failed()) {
        failures++;
      }
      write_trace();
      return failures == 0 ? 0 : b3tool::exit_failure;
    }

    if (eng.can_batch(o.files)) {
      // Several files over one context: one arena, several in flight,
      // lines printed as files complete, in input order.
      failures += eng.hash_many(o.files, o.out_len);
    } else {
      for (const auto& f : o.files) {
        std::string hex;
        try {
          hex = eng.hash_hex(f, o.out_len);
        } catch (const std::exception& e) {
          println(stderr, "blake3ppsum: {}: {}", f, e.what());
          failures++;
          continue;
        }
        // A failed write is stdout's, not the file's: main() reports it.
        try {
          const line_name n = escape_name(f);
          println(stdout, "{}{}  {}", n.prefix, hex, n.text);
        } catch (const std::system_error&) {
          failures++;
          break;
        }
      }
    }
    if (o.diag) {
      const unsigned hw_threads = b3tool::default_threads();
      std::size_t window = 0;
      unsigned windows = 0;
      if (o.files.front() == "-") {
        // The stream's geometry as the run settled it. A stream fills one
        // window at a time, so the device side of the diagnosis is the
        // stream itself.
        window = eng.stream().window_bytes;
        windows = eng.stream().windows_planned;
      } else {
        // The plan update_file() made, from the same budget rule: the
        // arena figure is read against its window count.
        window = blake3pp::detail::rounded_window_bytes(o.io.window_bytes);
        std::error_code size_ec;
        const std::uintmax_t size =
            std::filesystem::file_size(o.files.front(), size_ec);
        windows = blake3pp::detail::plan_pipeline(
                      0, o.io.window_bytes, o.io.queue_depth, o.io.direct_io,
                      size_ec ? 0 : static_cast<std::uint64_t>(size),
                      blake3pp::detail::read_plan_budget_bytes(
                          hw_threads, o.io.window_bytes))
                      .windows;
      }
      report_read_diagnosis(
          eng.pipelined()
              ? blake3pp::diagnose_read(trace->driver(), eng.wall_ns(), o.threads,
                                      windows, window)
              : blake3pp::diagnosis{},
          o.threads, hw_threads);
      if (o.files.front() == "-" && eng.pipelined()) {
        // What the diagnosis cannot show about a stream run: how the
        // windows ended, the pipe buffer the OS granted, and what the
        // reader did before each read.
        for (const auto& line : b3tool::stream_lines(
                 eng.stream(),
                 [](std::uint64_t n) { return b3tool::bytes_label(n); })) {
          println(stderr, "{}", line);
        }
      }
    }
    write_trace();
  } catch (const std::exception& e) {
    println(stderr, "blake3ppsum: {}", e.what());
    return b3tool::exit_usage;
  }

  return failures == 0 ? 0 : b3tool::exit_failure;
}

// The digest lines go out through a buffered stdout, so a write into a
// pipe whose reader has gone (with SIGPIPE ignored, and on Windows, which
// has no SIGPIPE) fails at a flush, and the flush at exit is silent.
// Flushing here and checking the stream's error state turns that into a
// failed run on every path.
int main(int argc, char** argv) {
  const int rc = run_main(argc, argv);
  errno = 0;
  if (std::fflush(stdout) != 0 || std::ferror(stdout) != 0) {
    const int e = errno;
    println(stderr, "blake3ppsum: write error on standard output{}{}",
            e != 0 ? ": " : "", e != 0 ? std::generic_category().message(e) : "");
    return rc == 0 ? b3tool::exit_failure : rc;
  }
  return rc;
}
