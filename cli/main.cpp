// blake3ppsum: a sha1sum-style checksum utility over the blake3pp engine.
//
//   blake3ppsum [OPTIONS] [FILE...]         print "<hex>  <file>" per file
//   blake3ppsum --check [OPTIONS] [LIST...] verify previously printed lines
//
// FILE of "-" (or no files) reads stdin. Options expose the interesting
// internals: --arch pins a SIMD variant, --threads sizes the compute pool
// (0 = sequential), and --window/--qd/--no-direct tune the I/O pipeline.

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include <blake3pp/io.hpp>

#if !defined(BLAKE3PP_HAS_STD_SENDERS)
#include <exec/static_thread_pool.hpp>
#endif

namespace {

struct options {
  blake3pp::hash_file_options io;
  unsigned threads = std::thread::hardware_concurrency();
  bool check = false;
  std::vector<std::string> files;
};

std::string version_text() {
  std::string out = "blake3ppsum (blake3pp " BLAKE3PP_VERSION ")\n";
#if defined(BLAKE3PP_HAS_STD_SIMD)
  out += "simd provider: std::simd\n";
#elif defined(BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD)
  out += "simd provider: std::experimental::simd\n";
#else
  out += "simd provider: xsimd\n";
#endif
#if defined(BLAKE3PP_HAS_STD_SENDERS)
  out += "execution provider: std::execution\n";
#else
  out += "execution provider: stdexec\n";
#endif
  out += "variants:";
  for (const auto a : {blake3pp::arch::scalar, blake3pp::arch::sse42,
                       blake3pp::arch::avx2, blake3pp::arch::avx512,
                       blake3pp::arch::neon}) {
    if (blake3pp::is_available(a)) {
      out += ' ';
      out += blake3pp::to_string(a);
    }
  }
  out += " (auto -> ";
  out += blake3pp::to_string(blake3pp::best_available());
  out += ')';
  return out;
}

blake3pp::digest hash_stdin(blake3pp::arch a) {
  blake3pp::hasher h{a};
  std::vector<std::byte> buf(1024 * 1024);
  std::size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), stdin)) > 0) {
    h.update(std::span<const std::byte>{buf.data(), n});
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

  blake3pp::digest hash(const std::string& path) {
    if (path == "-") {
      return hash_stdin(opts_.io.a);
    }
#if !defined(BLAKE3PP_HAS_STD_SENDERS)
    if (pool_) {
      return blake3pp::hash_file(std::filesystem::path(path),
                                 pool_->get_scheduler(), opts_.io);
    }
#endif
    return blake3pp::hash_file(std::filesystem::path(path), opts_.io);
  }

 private:
  options opts_;
#if !defined(BLAKE3PP_HAS_STD_SENDERS)
  std::optional<exec::static_thread_pool> pool_;
#endif
};

// Verifies "<64 hex>  <name>" lines (also accepts the '*' binary marker).
int run_check(engine& eng, std::istream& in, const std::string& list_name) {
  int failures = 0;
  int line_no = 0;
  std::string line;
  while (std::getline(in, line)) {
    ++line_no;
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const auto parsed = line.size() >= 66
                            ? blake3pp::digest::from_hex(line.substr(0, 64))
                            : std::nullopt;
    if (!parsed) {
      std::fprintf(stderr, "blake3ppsum: %s:%d: malformed line\n",
                   list_name.c_str(), line_no);
      failures++;
      continue;
    }
    std::size_t pos = 64;
    while (pos < line.size() && line[pos] == ' ') {
      ++pos;
    }
    if (pos < line.size() && line[pos] == '*') {
      ++pos;
    }
    const std::string name = line.substr(pos);
    try {
      if (eng.hash(name) == *parsed) {
        std::printf("%s: OK\n", name.c_str());
      } else {
        std::printf("%s: FAILED\n", name.c_str());
        failures++;
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "blake3ppsum: %s: %s\n", name.c_str(), e.what());
      std::printf("%s: FAILED open or read\n", name.c_str());
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

  const std::map<std::string, blake3pp::arch> arch_names{
      {"auto", blake3pp::arch::auto_detect},
      {"scalar", blake3pp::arch::scalar},
      {"sse42", blake3pp::arch::sse42},
      {"avx2", blake3pp::arch::avx2},
      {"avx512", blake3pp::arch::avx512},
      {"neon", blake3pp::arch::neon}};
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
    std::fprintf(stderr,
                 "blake3ppsum: arch '%s' not available on this machine "
                 "(auto -> %s)\n",
                 blake3pp::to_string(o.io.a),
                 blake3pp::to_string(blake3pp::best_available()));
    return 2;
  }

  engine eng(o);
  int failures = 0;

  if (o.check) {
    if (o.files.empty()) {
      o.files.emplace_back("-");
    }
    for (const auto& f : o.files) {
      if (f == "-") {
        failures += run_check(eng, std::cin, "-");
        continue;
      }
      std::ifstream in(f);
      if (!in) {
        std::fprintf(stderr, "blake3ppsum: %s: cannot open\n", f.c_str());
        failures++;
        continue;
      }
      failures += run_check(eng, in, f);
    }
    if (failures > 0) {
      std::fprintf(stderr,
                   "blake3ppsum: WARNING: %d computed checksum(s) did NOT "
                   "match\n",
                   failures);
    }
    return failures == 0 ? 0 : 1;
  }

  if (o.files.empty()) {
    o.files.emplace_back("-");
  }
  for (const auto& f : o.files) {
    try {
      std::printf("%s  %s\n", eng.hash(f).to_hex().c_str(), f.c_str());
    } catch (const std::exception& e) {
      std::fprintf(stderr, "blake3ppsum: %s: %s\n", f.c_str(), e.what());
      failures++;
    }
  }
  return failures == 0 ? 0 : 1;
}
