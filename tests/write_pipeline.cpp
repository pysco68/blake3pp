// The write pipeline: windows generated on a pool, written by the driver.
//
// The check that matters is end to end: the file a generator describes
// is the file that ends up on disk, whatever order the windows were
// filled and written in. Hashing it is the cheapest way to say so, and
// the digest of a generator's output is computable without the pipeline.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <blake3pp/detail/write_pipeline.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/parallel_backend.hpp>
#include <blake3pp/parallel_io.hpp>

#include "test_schedulers.hpp"
#include <doctest/doctest.h>

namespace {

namespace fs = std::filesystem;
namespace detail = blake3pp::detail;

// A generator whose output depends only on the offset, which is what the
// pipeline requires and what makes the expected digest computable.
inline std::byte byte_at(std::uint64_t off) noexcept {
  std::uint64_t x = off * 6364136223846793005ULL + 1442695040888963407ULL;
  x ^= x >> 33;
  return static_cast<std::byte>(x >> 24);
}

void fill_from_offset(std::span<std::byte> out, std::uint64_t off) noexcept {
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = byte_at(off + i);
  }
}

[[nodiscard]] blake3pp::digest expected_digest(std::uint64_t size) {
  std::vector<std::byte> all(static_cast<std::size_t>(size));
  fill_from_offset(all, 0);
  return blake3pp::hash(std::span<const std::byte>{all});
}

struct temp_path {
  fs::path path;
  temp_path() {
    static const unsigned run = std::random_device{}();
    path = fs::temp_directory_path() /
           ("blake3pp_wp_" + std::to_string(run) + "_" +
            std::to_string(counter++));
  }
  ~temp_path() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  static inline int counter = 0;
};

[[nodiscard]] blake3pp::digest hash_of(const fs::path& p) {
  return blake3pp::hash_file(p);
}

[[nodiscard]] std::uintmax_t size_of(const fs::path& p) {
  std::error_code ec;
  const auto n = fs::file_size(p, ec);
  return ec ? ~std::uintmax_t{0} : n;
}

}  // namespace

TEST_SUITE("write_pipeline") {

TEST_CASE("a generated file is exactly what the generator described") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::vector<std::uint64_t> sizes{
      0, 1, win + 1, 2 * win, 3 * win + 4321};
  for (const bool direct : {false, true}) {
    for (const unsigned qd : {2u, 32u}) {
      for (const std::uint64_t size : sizes) {
        CAPTURE(direct);
        CAPTURE(qd);
        CAPTURE(size);
        const temp_path out;
        blake3pp::write_file(out.path, size, fill_from_offset, sched,
                             {.window_bytes = win,
                              .queue_depth = qd,
                              .direct_io = direct});
        CHECK(size_of(out.path) == size);
        if (size > 0) {
          CHECK(hash_of(out.path) == expected_digest(size));
        }
      }
    }
  }
}

TEST_CASE("the write pipeline runs on an inline scheduler and on one thread") {
  constexpr std::size_t win = 64 * 1024;
  const std::uint64_t size = 3 * win + 77;
  SUBCASE("inline") {
    // The fill then runs inside the driver's own poll, which must not
    // deadlock.
    const temp_path out;
    blake3pp::write_file(out.path, size, fill_from_offset,
                         b3test::inline_scheduler{}, {.window_bytes = win});
    CHECK(size_of(out.path) == size);
    CHECK(hash_of(out.path) == expected_digest(size));
  }
  SUBCASE("one thread") {
    b3test::one_thread_pool pool;
    const temp_path out;
    blake3pp::write_file(out.path, size, fill_from_offset,
                         pool.get_scheduler(), {.window_bytes = win});
    CHECK(size_of(out.path) == size);
    CHECK(hash_of(out.path) == expected_digest(size));
  }
}

TEST_CASE("a stopped write pipeline ends once and does not invent a length") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::uint64_t size = 64 * win;
  const temp_path out;

  detail::io_driver drv({true, true}, 4);
  detail::io_driver::file file(
      drv, out.path,
      detail::io_driver::open_options{.write = true, .preallocate_bytes = size});
  blake3pp::ex::inplace_stop_source src;
  src.request_stop();
  std::error_code ec;
  try {
    detail::run_write_pipeline(drv, file, sched, fill_from_offset, size,
                               {.window_bytes = win,
                                .queue_depth = 4,
                                .stop_token = src.get_token()});
  } catch (const std::system_error& e) {
    ec = e.code();
  }
  CHECK(ec == std::errc::operation_canceled);
  CHECK(drv.in_flight() == 0);
}

}  // TEST_SUITE
