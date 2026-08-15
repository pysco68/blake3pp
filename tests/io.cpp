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
    CHECK(blake3pp::hash_file(f.path.c_str()) == expected);
    CHECK(blake3pp::hash_file(f.path.c_str(), small_windows) == expected);
    CHECK(blake3pp::hash_file(
              f.path.c_str(),
              {.direct_io = false}) == expected);
  }
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
    CHECK(blake3pp::hash_file(f.path.c_str(), sched) == expected);
    CHECK(blake3pp::hash_file(f.path.c_str(), sched,
                              {.window_bytes = 1024 * 1024,
                               .queue_depth = 3}) == expected);
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
  blake3pp::detail::file_reader r(f.path.c_str(), {});
  CHECK(r.file_size() == content.size());
  MESSAGE("file_reader backend: " << r.backend());
  CHECK(std::string(r.backend()).size() > 0);
}

}  // TEST_SUITE

}  // namespace
