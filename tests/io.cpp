#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <blake3pp/io.hpp>
#include <doctest/doctest.h>

#if !defined(BLAKE3PP_HAS_STD_SENDERS)
#include <exec/static_thread_pool.hpp>
#endif

namespace {

namespace fs = std::filesystem;

std::vector<std::byte> make_input(std::size_t len) {
  std::vector<std::byte> v(len);
  for (std::size_t i = 0; i < len; ++i) {
    v[i] = static_cast<std::byte>(i % 251);
  }
  return v;
}

struct temp_file {
  fs::path path;
  explicit temp_file(const std::vector<std::byte>& content) {
    static const unsigned run_id = std::random_device{}();
    path = fs::temp_directory_path() /
           ("blake3pp_io_test_" + std::to_string(run_id) + "_" +
            std::to_string(counter++));
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(content.data()),
              static_cast<std::streamsize>(content.size()));
  }
  ~temp_file() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  static inline int counter = 0;
};

TEST_SUITE("io") {

// Every size class: empty, sub-chunk, exact windows, unaligned tails,
// multi-window. Small windows + low queue depth force buffer recycling.
TEST_CASE("hash_file matches in-memory hash across size classes") {
  const blake3pp::hash_file_options small_windows{
      .window_bytes = 64 * 1024, .queue_depth = 3};

  for (const std::size_t len :
       {std::size_t{0}, std::size_t{1}, std::size_t{1024},
        std::size_t{4096}, std::size_t{64 * 1024},
        std::size_t{64 * 1024 + 1}, std::size_t{192 * 1024},
        std::size_t{1024 * 1024 + 4097}, std::size_t{3 * 1024 * 1024 + 17}}) {
    CAPTURE(len);
    const auto content = make_input(len);
    const temp_file f(content);
    const auto expected = blake3pp::hash(content);
    CHECK(blake3pp::hash_file(f.path) == expected);  // fs::path overload
    CHECK(blake3pp::hash_file(f.path, small_windows) == expected);
    CHECK(blake3pp::hash_file(f.path, {.direct_io = false}) == expected);
  }
}

TEST_CASE("error_code overload reports instead of throwing") {
  std::error_code ec;
  const auto d =
      blake3pp::hash_file("/nonexistent/blake3pp/no/such/file", ec);
  CHECK(ec);
  CHECK(ec == std::errc::no_such_file_or_directory);
  CHECK(d == blake3pp::digest{});

  const auto content = make_input(4096);
  const temp_file f(content);
  const auto ok = blake3pp::hash_file(f.path, ec);
  CHECK(!ec);
  CHECK(ok == blake3pp::hash(content));
}

#if !defined(BLAKE3PP_HAS_STD_SENDERS)
TEST_CASE("parallel hash_file matches, across window boundaries") {
  exec::static_thread_pool pool(std::thread::hardware_concurrency());
  auto sched = pool.get_scheduler();

  for (const std::size_t len :
       {std::size_t{1024 * 1024}, std::size_t{4 * 1024 * 1024},
        std::size_t{9 * 1024 * 1024 + 12345}}) {
    CAPTURE(len);
    const auto content = make_input(len);
    const temp_file f(content);
    const auto expected = blake3pp::hash(content);
    CHECK(blake3pp::hash_file(f.path, sched) == expected);
    CHECK(blake3pp::hash_file(f.path, sched,
                              {.window_bytes = 1024 * 1024,
                               .queue_depth = 3}) == expected);
    std::error_code ec;
    CHECK(blake3pp::hash_file(f.path, sched, ec) == expected);
    CHECK(!ec);
  }
}
#endif

TEST_CASE("missing file throws system_error") {
  CHECK_THROWS_AS(
      (void)blake3pp::hash_file("/nonexistent/blake3pp/no/such/file"),
      std::system_error);
}

TEST_CASE("reader reports a backend") {
  const auto content = make_input(256 * 1024);
  const temp_file f(content);
  blake3pp::detail::file_reader r(f.path, {});
  CHECK(r.file_size() == content.size());
  MESSAGE("file_reader backend: " << r.backend());
  CHECK(std::string(r.backend()).size() > 0);
}

// Stand-in for boost::filesystem::path: satisfies the foreign_path concept
// structurally, which is exactly how a real boost path would enter.
struct fake_boost_path {
  std::string s;
  const std::string& native() const { return s; }
};

TEST_CASE("foreign path-like types (boost::filesystem shape) forward") {
  const auto content = make_input(128 * 1024 + 7);
  const temp_file f(content);
  const fake_boost_path bp{f.path.string()};
  const auto expected = blake3pp::hash(content);
  CHECK(blake3pp::hash_file(bp) == expected);
  std::error_code ec;
  CHECK(blake3pp::hash_file(bp, ec) == expected);
  CHECK(!ec);
#if !defined(BLAKE3PP_HAS_STD_SENDERS)
  exec::static_thread_pool pool(2);
  CHECK(blake3pp::hash_file(bp, pool.get_scheduler()) == expected);
#endif
}

TEST_CASE("digest hex round trip and std::format") {
  const auto d = blake3pp::hash(std::string_view{"round trip"});
  const auto parsed = blake3pp::digest::from_hex(d.to_hex());
  REQUIRE(parsed.has_value());
  CHECK(*parsed == d);

  std::string upper = d.to_hex();
  for (char& c : upper) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  CHECK(blake3pp::digest::from_hex(upper) == d);

  CHECK(!blake3pp::digest::from_hex("abc"));
  CHECK(!blake3pp::digest::from_hex(std::string(64, 'g')));

#if defined(__cpp_lib_format)
  CHECK(std::format("{}", d) == d.to_hex());
#endif
}

}  // TEST_SUITE

}  // namespace
