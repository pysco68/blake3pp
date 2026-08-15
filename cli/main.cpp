// blake3ppsum: a sha1sum-style checksum utility over the blake3pp engine.
//
//   blake3ppsum [OPTIONS] [FILE...]         print "<hex>  <file>" per file
//   blake3ppsum --check [OPTIONS] [LIST...] verify previously printed lines
//
// FILE of "-" (or no files) reads stdin. Options expose the interesting
// internals: --arch pins a SIMD variant, --threads sizes the compute pool
// (0 = sequential), and --window/--qd/--no-direct tune the I/O pipeline.

#include <cerrno>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <version>

#if defined(__cpp_lib_print)
#include <print>
#endif

#include <CLI/CLI.hpp>
#include <blake3pp/blake3pp.hpp>

#if !defined(BLAKE3PP_HAS_STD_SENDERS)
#include <exec/static_thread_pool.hpp>
#endif

namespace {

// The project's provider pattern in miniature: std::print where the
// standard library ships it (C++23), a std::format shim on the C++20
// toolchains; same source either way.
#if defined(__cpp_lib_print)
using std::print;
using std::println;
#else
template <class... Args>
void print(std::FILE* stream, std::format_string<Args...> fmt,
           Args&&... args) {
  std::fputs(std::format(fmt, std::forward<Args>(args)...).c_str(), stream);
}

template <class... Args>
void println(std::FILE* stream, std::format_string<Args...> fmt,
             Args&&... args) {
  print(stream, fmt, std::forward<Args>(args)...);
  std::fputc('\n', stream);
}
#endif

struct options {
  blake3pp::hash_file_options io;
  unsigned threads = std::thread::hardware_concurrency();
  bool check = false;
  std::vector<std::string> files;
};

std::string version_text() {
  std::string variants;
  for (const auto a : blake3pp::compiled_arches()) {
    std::format_to(std::back_inserter(variants), " {}{}",
                   blake3pp::to_string(a),
                   blake3pp::is_available(a) ? "" : "[no cpu support]");
  }
  return std::format(
      "blake3ppsum (blake3pp {})\n"
      "simd provider: {}\n"
      "execution provider: {}\n"
      "variants:{} (auto -> {})",
      blake3pp::version(), blake3pp::simd_provider(),
      blake3pp::execution_provider(), variants,
      blake3pp::to_string(blake3pp::best_available()));
}

blake3pp::digest hash_stdin(blake3pp::arch a) {
  blake3pp::hasher h{a};
  std::vector<std::byte> buf(1024 * 1024);
  std::size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), stdin)) > 0) {
    h.update(std::span{buf}.first(n));
  }
  if (std::ferror(stdin) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "reading standard input");
  }
  return h.finalize();
}

class engine {
 public:
  explicit engine(const options& o) : opts_(o) {
#if !defined(BLAKE3PP_HAS_STD_SENDERS)
    if (o.threads > 1) {
      pool_.emplace(o.threads);
    }
#endif
  }

  blake3pp::digest hash(const std::filesystem::path& path) {
    if (path == "-") {
      return hash_stdin(opts_.io.a);
    }
#if !defined(BLAKE3PP_HAS_STD_SENDERS)
    if (pool_.has_value()) {
      return blake3pp::hash_file(path, pool_.value().get_scheduler(),
                                 opts_.io);
    }
#endif
    return blake3pp::hash_file(path, opts_.io);
  }

 private:
  options opts_;
#if !defined(BLAKE3PP_HAS_STD_SENDERS)
  std::optional<exec::static_thread_pool> pool_;
#endif
};

// Verifies "<64 hex>  <name>" lines (also accepts the '*' binary marker).
int run_check(engine& eng, std::istream& in, std::string_view list_name) {
  int failures = 0;
  std::string line;
  for (int line_no = 1; std::getline(in, line); ++line_no) {
    const std::string_view sv{line};
    if (sv.empty() || sv.starts_with('#')) {
      continue;
    }
    const auto expected = sv.size() >= 66
                              ? blake3pp::digest::from_hex(sv.substr(0, 64))
                              : std::nullopt;
    if (!expected.has_value()) {
      println(stderr, "blake3ppsum: {}:{}: malformed line", list_name,
              line_no);
      failures++;
      continue;
    }
    auto rest = sv.substr(64);
    rest.remove_prefix(std::min(rest.find_first_not_of(' '), rest.size()));
    if (rest.starts_with('*')) {
      rest.remove_prefix(1);
    }
    const std::string name{rest};
    try {
      if (expected == eng.hash(name)) {  // optional's heterogeneous ==
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
  options o;
  std::size_t window_mib = 8;

  CLI::App app{
      "Print or check BLAKE3 (256-bit) checksums.\n"
      "With no FILE, or FILE of '-', read standard input."};
  app.set_version_flag("--version", version_text);

  app.add_flag("-c,--check", o.check,
               "read checksum lines from FILEs and verify them");

  // The name<->enum mapping comes from the library's canonical list; the
  // CLI never re-enumerates the arch enum.
  std::map<std::string, blake3pp::arch> arch_names;
  for (const auto a : blake3pp::all_arches()) {
    arch_names.emplace(blake3pp::to_string(a), a);
  }
  app.add_option("--arch", o.io.a, "pin a SIMD variant")
      ->transform(CLI::CheckedTransformer(arch_names, CLI::ignore_case))
      ->default_str("auto");

  app.add_option("--threads", o.threads,
                 "compute threads (0 = sequential; default: all)");
  app.add_option("--window", window_mib, "I/O window size in MiB")
      ->check(CLI::PositiveNumber)
      ->capture_default_str();
  app.add_option("--qd", o.io.queue_depth, "I/O queue depth")
      ->check(CLI::Range(2u, 32u))
      ->capture_default_str();
  app.add_flag("!--no-direct", o.io.direct_io,
               "keep the OS page cache (no O_DIRECT)");
  app.add_option("files", o.files, "files to hash (or checksum lists)");

  CLI11_PARSE(app, argc, argv);

  o.io.window_bytes = window_mib * 1024 * 1024;
  if (o.io.a != blake3pp::arch::auto_detect &&
      !blake3pp::is_available(o.io.a)) {
    println(stderr,
            "blake3ppsum: arch '{}' not available on this machine "
            "(auto -> {})",
            blake3pp::to_string(o.io.a),
            blake3pp::to_string(blake3pp::best_available()));
    return 2;
  }

  engine eng(o);
  if (o.files.empty()) {
    o.files.emplace_back("-");
  }
  int failures = 0;

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
      println(stderr,
              "blake3ppsum: WARNING: {} computed checksum(s) did NOT match",
              failures);
    }
    return failures == 0 ? 0 : 1;
  }

  for (const auto& f : o.files) {
    try {
      println(stdout, "{}  {}", eng.hash(f), f);  // digest is formattable
    } catch (const std::exception& e) {
      println(stderr, "blake3ppsum: {}: {}", f, e.what());
      failures++;
    }
  }
  return failures == 0 ? 0 : 1;
}
