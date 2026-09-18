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
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <CLI/CLI.hpp>
#include <blake3pp/blake3pp.hpp>

#include "tool_common.hpp"

namespace {

using b3tool::print;
using b3tool::println;

struct options {
  blake3pp::hash_file_options io;
  unsigned threads = b3tool::default_threads();
  bool check = false;
  std::size_t out_len = blake3pp::digest_size;
  std::string key_file;        // --keyed: path to 32 raw bytes or 64 hex
  std::string derive_context;  // --derive-key
  std::vector<std::string> files;
};

std::string version_text() {
  std::string variants;
  for (const auto a : blake3pp::compiled_arches()) {
    std::format_to(std::back_inserter(variants), " {}{}",
                   blake3pp::to_string(a),
                   blake3pp::is_available(a) ? "" : "[no cpu support]");
  }
  // The confession line: variants the CPU could run but this binary does
  // not carry (e.g. xthead on T-Head silicon in a build whose compiler
  // could not express it). Silence when there is nothing to confess.
  std::string uncompiled;
  for (const auto a : blake3pp::all_arches()) {
    if (a != blake3pp::arch::auto_detect && blake3pp::cpu_supports(a) &&
        !blake3pp::is_available(a)) {
      std::format_to(std::back_inserter(uncompiled), " {}",
                     blake3pp::to_string(a));
    }
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
      throw std::system_error(errno, std::generic_category(), source);
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

class engine {
 public:
  explicit engine(const options& o) : opts_(o), pool_(o.threads) {
    if (!o.key_file.empty()) {
      key_ = load_key(o.key_file);
    }
  }

  // Hex of the requested output length over stdin or a file. Files stream
  // through the library's pipeline (async windowed reads, multi-core when
  // a pool exists); stdin has no file to hand over, so it is read in
  // buffers into a parallel_hasher when a pool exists, the plain hasher
  // otherwise. Either way the mode comes from the options.
  std::string hash_hex(const std::string& path, std::size_t out_len) {
    std::vector<std::byte> out(out_len);
    const auto finish = [&](const auto& h) {
      h.finalize(out);
      return blake3pp::to_hex(out);
    };
    if (path == "-") {
      if (pool_.parallel()) {
        auto ph = make_parallel_hasher();
        read_stdin_into(ph);
        return finish(ph);
      }
      blake3pp::hasher h = make_hasher();
      read_stdin_into(h);
      return finish(h);
    }
    blake3pp::hasher h = make_hasher();
    if (pool_.parallel()) {
      blake3pp::update_file(h, path, pool_.scheduler(), opts_.io);
    } else {
      blake3pp::update_file(h, path, opts_.io);
    }
    return finish(h);
  }

 private:
  blake3pp::hasher make_hasher() const {
    if (!opts_.derive_context.empty()) {
      return blake3pp::hasher::derive_key(opts_.derive_context, opts_.io.a);
    }
    if (key_) {
      return blake3pp::hasher::keyed(*key_, opts_.io.a);
    }
    return blake3pp::hasher{opts_.io.a};
  }

  blake3pp::parallel_hasher<blake3pp::parallel_scheduler_t>
  make_parallel_hasher() {
    const blake3pp::parallel_hasher_options popts{.a = opts_.io.a};
    if (!opts_.derive_context.empty()) {
      return blake3pp::parallel_hasher{pool_.scheduler(),
                                       std::string_view{opts_.derive_context},
                                       popts};
    }
    if (key_) {
      return blake3pp::parallel_hasher{
          pool_.scheduler(), std::span<const std::byte, blake3pp::key_size>{*key_},
          popts};
    }
    return blake3pp::parallel_hasher{pool_.scheduler(), popts};
  }

  template <class Sink>
  static void read_stdin_into(Sink& sink) {
    std::vector<std::byte> buf(b3tool::stream_buffer_bytes);
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), stdin)) > 0) {
      sink.update(std::span{buf}.first(n));
    }
    if (std::ferror(stdin) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "reading standard input");
    }
  }

  options opts_;
  std::optional<std::array<std::byte, blake3pp::key_size>> key_;
  b3tool::compute_pool pool_;
};

// Verifies "<hex>  <name>" lines of any (even) digest length; also accepts
// the '*' binary marker.
int run_check(engine& eng, std::istream& in, std::string_view list_name) {
  int failures = 0;
  std::string line;
  for (int line_no = 1; std::getline(in, line); ++line_no) {
    const std::string_view sv{line};
    if (sv.empty() || sv.starts_with('#')) {
      continue;
    }
    const std::size_t hex_end = sv.find(' ');
    const std::string_view hex = sv.substr(0, hex_end);
    if (hex_end == std::string_view::npos || hex.size() < 2 || hex.size() % 2 != 0) {
      println(stderr, "blake3ppsum: {}:{}: malformed line", list_name, line_no);
      failures++;
      continue;
    }
    auto rest = sv.substr(hex_end);
    rest.remove_prefix(std::min(rest.find_first_not_of(' '), rest.size()));
    if (rest.starts_with('*')) {
      rest.remove_prefix(1);
    }
    const std::string name{rest};
    try {
      std::string expected{hex};
      for (char& c : expected) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (eng.hash_hex(name, hex.size() / 2) == expected) {
        println(stdout, "{}: OK", name);
      } else {
        println(stdout, "{}: FAILED", name);
        failures++;
      }
    } catch (const std::exception& e) {
      println(stderr, "blake3ppsum: {}: {}", name, e.what());
      println(stdout, "{}: FAILED open or read", name);
      failures++;
    }
  }
  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  b3tool::tool_startup();
  options o;
  constexpr std::size_t mib = std::size_t{1} << 20;
  bool no_direct = false;
  bool inline_submit = false;

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
      ->capture_default_str();

  app.add_option("--window", o.io.window_bytes, "I/O window size in MiB")
      ->transform(b3tool::mib_to_bytes)
      ->default_str(std::to_string(o.io.window_bytes / mib));

  app.add_option("--qd", o.io.queue_depth, "I/O queue depth (the reader clamps it to its range)")
      ->capture_default_str();

  app.add_flag("--no-direct", no_direct, "keep the OS page cache (no O_DIRECT)");
  app.add_flag("--inline-submit", inline_submit, "issue reads inline in the submitting thread, not on io_uring's workers");
  app.add_option("files", o.files, "files to hash (or checksum lists)");

  CLI11_PARSE(app, argc, argv);

  o.io.direct_io = !no_direct;
  o.io.offload_submit = !inline_submit;
  if (!blake3pp::is_available(o.io.a)) {
    println(stderr, "blake3ppsum: arch '{}' not available on this machine (auto -> {})", blake3pp::to_string(o.io.a), blake3pp::to_string(blake3pp::best_available()));
    return b3tool::exit_usage;
  }

  if (o.key_file == "-" && (o.files.empty() || std::find(o.files.begin(), o.files.end(), "-") != o.files.end())) {
    println(stderr, "blake3ppsum: with the key on stdin, data must come from files");
    return b3tool::exit_usage;
  }

  int failures = 0;
  try {
    engine eng(o);
    if (o.files.empty()) {
      o.files.emplace_back("-");
    }

    if (o.check) {
      for (const auto& f : o.files) {
        if (f == "-") {
          failures += run_check(eng, std::cin, "-");
          continue;
        }
        std::ifstream in(f);
        if (!in) {
          println(stderr, "blake3ppsum: {}: cannot open", f);
          failures++;
          continue;
        }
        failures += run_check(eng, in, f);
      }
      if (failures > 0) {
        println(stderr, "blake3ppsum: WARNING: {} computed checksum(s) did NOT match", failures);
      }
      return failures == 0 ? 0 : b3tool::exit_failure;
    }

    for (const auto& f : o.files) {
      try {
        println(stdout, "{}  {}", eng.hash_hex(f, o.out_len), f);
      } catch (const std::exception& e) {
        println(stderr, "blake3ppsum: {}: {}", f, e.what());
        failures++;
      }
    }
  } catch (const std::exception& e) {
    println(stderr, "blake3ppsum: {}", e.what());
    return b3tool::exit_usage;
  }

  return failures == 0 ? 0 : b3tool::exit_failure;
}
