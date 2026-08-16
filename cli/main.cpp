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
  std::size_t out_len = 32;
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
  return std::format(
      "blake3ppsum (blake3pp {})\n"
      "simd provider: {}\n"
      "execution provider: {}\n"
      "variants:{} (auto -> {})",
      blake3pp::version(), blake3pp::simd_provider(),
      blake3pp::execution_provider(), variants,
      blake3pp::to_string(blake3pp::best_available()));
}

std::string to_hex(std::span<const std::byte> bytes) {
  std::string s;
  for (const std::byte b : bytes) {
    std::format_to(std::back_inserter(s), "{:02x}",
                   std::to_integer<unsigned>(b));
  }
  return s;
}

// Accepts a file holding either exactly 32 raw bytes or 64 hex characters
// (trailing whitespace tolerated); "-" reads the key from stdin, in which
// case the data must come from files.
std::array<std::byte, 32> load_key(const std::string& source) {
  std::string content;
  if (source == "-") {
    std::array<char, 128> buf;
    const std::size_t n = std::fread(buf.data(), 1, buf.size(), stdin);
    content.assign(buf.data(), n);
  } else {
    std::ifstream in(source, std::ios::binary);
    if (!in) {
      throw std::system_error(errno, std::generic_category(), source);
    }
    content.assign(std::istreambuf_iterator<char>(in), {});
  }
  if (content.size() == 32) {
    std::array<std::byte, 32> key;
    for (std::size_t i = 0; i < 32; ++i) {
      key[i] = static_cast<std::byte>(static_cast<unsigned char>(content[i]));
    }
    return key;
  }
  while (!content.empty() &&
         (content.back() == '\n' || content.back() == '\r' ||
          content.back() == ' ')) {
    content.pop_back();
  }
  if (const auto parsed = blake3pp::digest::from_hex(content)) {
    return parsed.value().bytes;
  }
  throw std::runtime_error(
      "key must be exactly 32 raw bytes or 64 hex characters");
}

class engine {
 public:
  engine(const options& o) : opts_(o) {
    if (!o.derive_context.empty()) {
      proto_ = blake3pp::hasher::derive_key(o.derive_context, o.io.a);
    } else if (!o.key_file.empty()) {
      const auto key = load_key(o.key_file);
      proto_ = blake3pp::hasher::keyed(key, o.io.a);
    } else {
      proto_ = blake3pp::hasher{o.io.a};
    }
#if !defined(BLAKE3PP_HAS_STD_SENDERS)
    if (o.threads > 1) {
      pool_.emplace(o.threads);
    }
#endif
  }

  // Hashes stdin or a file into a fresh copy of the mode prototype. Every
  // mode gets async windowed reads, and multi-core when a pool exists.
  blake3pp::hasher hash_source(const std::string& path) {
    blake3pp::hasher h = proto_.value();  // hashers are cheap flat copies
    if (path == "-") {
      std::vector<std::byte> buf(1024 * 1024);
      std::size_t n = 0;
      while ((n = std::fread(buf.data(), 1, buf.size(), stdin)) > 0) {
        h.update(std::span{buf}.first(n));
      }
      if (std::ferror(stdin) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "reading standard input");
      }
      return h;
    }
    blake3pp::detail::file_reader reader(
        std::filesystem::path(path),
        {opts_.io.window_bytes, opts_.io.queue_depth, opts_.io.direct_io,
         true});
    const auto* const ops = blake3pp::detail::resolve(h.selected_arch());
    while (auto w = reader.next()) {
#if !defined(BLAKE3PP_HAS_STD_SENDERS)
      if (pool_.has_value() && !w->last) {
        auto sched = pool_.value().get_scheduler();
        blake3pp::detail::hash_window_parallel(
            ops, sched, h, w->data, w->bytes / blake3pp::chunk_size,
            w->offset / blake3pp::chunk_size);
        reader.release(w.value());
        continue;
      }
#endif
      h.update(std::span<const std::byte>{w->data, w->bytes});
      reader.release(w.value());
    }
    return h;
  }

  std::string hash_hex(const std::string& path, std::size_t out_len) {
    std::vector<std::byte> out(out_len);
    hash_source(path).finalize(out);
    return to_hex(out);
  }

 private:
  options opts_;
  std::optional<blake3pp::hasher> proto_;
#if !defined(BLAKE3PP_HAS_STD_SENDERS)
  std::optional<exec::static_thread_pool> pool_;
#endif
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
    if (hex_end == std::string_view::npos || hex.size() < 2 ||
        hex.size() % 2 != 0) {
      println(stderr, "blake3ppsum: {}:{}: malformed line", list_name,
              line_no);
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
  options o;
  std::size_t window_mib = 8;

  CLI::App app{
      "Print or check BLAKE3 checksums.\n"
      "With no FILE, or FILE of '-', read standard input."};
  app.set_version_flag("--version", version_text);

  app.add_flag("-c,--check", o.check,
               "read checksum lines from FILEs and verify them");
  auto* keyed =
      app.add_option("--keyed", o.key_file,
                     "keyed (MAC) mode; FILE holds the 32-byte key as raw "
                     "bytes or 64 hex chars ('-' reads it from stdin)");
  auto* derive = app.add_option("--derive-key", o.derive_context,
                                "key-derivation mode with this context "
                                "string");
  keyed->excludes(derive);
  derive->excludes(keyed);
  app.add_option("--length", o.out_len,
                 "output length in bytes (extended output)")
      ->check(CLI::Range(std::size_t{1}, std::size_t{1} << 20))
      ->capture_default_str();

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
  if (o.key_file == "-" &&
      (o.files.empty() ||
       std::find(o.files.begin(), o.files.end(), "-") != o.files.end())) {
    println(stderr,
            "blake3ppsum: with the key on stdin, data must come from files");
    return 2;
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
        println(stderr,
                "blake3ppsum: WARNING: {} computed checksum(s) did NOT match",
                failures);
      }
      return failures == 0 ? 0 : 1;
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
    return 2;
  }
  return failures == 0 ? 0 : 1;
}
