// The write pipeline: windows generated on a pool, written by the driver.
//
// The check that matters is end to end: the file a generator describes
// is the file that ends up on disk, whatever order the windows were
// filled and written in. Hashing it is the cheapest way to say so, and
// the digest of a generator's output is computable without the pipeline.

#include <algorithm>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
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

// A driver with no device under it, for the cases a real one only
// produces by luck: completion out of order, a short write, a failure at
// one chosen window.
struct write_script {
  std::vector<unsigned> order{};   // preference by window index
  std::size_t piece_bytes = 0;     // 0 writes a window in one go
  int fail_at_window = -1;
  int stop_at_window = -1;
  blake3pp::ex::inplace_stop_source* stop_source = nullptr;
};

class fake_write_driver {
 public:
  fake_write_driver(std::uint64_t size, std::size_t window, write_script s)
      : size_(size), window_(window), script_(std::move(s)),
        sink_(static_cast<std::size_t>(size)) {}

  class file {
   public:
    explicit file(fake_write_driver& d) noexcept : drv_(&d) {}
    [[nodiscard]] std::uint64_t size() const noexcept { return drv_->size_; }
    [[nodiscard]] std::string_view name() const noexcept { return "fake"; }
    void set_size(std::uint64_t n) noexcept { drv_->final_size_ = n; }

   private:
    fake_write_driver* drv_;
  };

  void reserve(unsigned windows, std::size_t window_bytes) {
    stride_ = window_bytes;
    pool_.assign(window_bytes * windows, std::byte{});
  }

  [[nodiscard]] detail::window_storage window_storage_for(
      file&, unsigned slot, std::uint64_t, std::size_t bytes) noexcept {
    const std::span<std::byte> s{pool_.data() + slot * stride_, bytes};
    return {s, std::span<const std::byte>(s)};
  }

  void submit_read(file&, std::uint64_t, std::span<std::byte>,
                   detail::io_read_op&) {
    FAIL("the write pipeline must not read");
  }

  void submit_write(file&, std::uint64_t off, std::span<const std::byte> buf,
                    detail::io_read_op& op) {
    queued_.push_back(write{&op, off, buf, 0});
  }

  void flush() noexcept {}

  std::size_t poll(bool block) {
    if (queued_.empty()) {
      if (block) {
        std::unique_lock lock(m_);
        cv_.wait(lock, [this] { return woken_; });
        woken_ = false;
      }
      return 0;
    }
    const std::size_t pick = choose();
    write& w = queued_[pick];
    const auto index = static_cast<unsigned>(w.off / window_);
    if (script_.stop_at_window >= 0 && script_.stop_source != nullptr &&
        index == static_cast<unsigned>(script_.stop_at_window)) {
      script_.stop_source->request_stop();
      script_.stop_at_window = -1;
      return 0;
    }
    if (w.cancelled) {
      auto* const op = w.op;
      queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(pick));
      op->done(op, std::make_error_code(std::errc::operation_canceled));
      return 1;
    }
    if (script_.fail_at_window >= 0 &&
        index == static_cast<unsigned>(script_.fail_at_window)) {
      auto* const op = w.op;
      queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(pick));
      op->done(op, std::make_error_code(std::errc::io_error));
      return 1;
    }
    const std::size_t want = w.buf.size() - w.done;
    const std::size_t take =
        script_.piece_bytes == 0 ? want : std::min(script_.piece_bytes, want);
    std::copy_n(w.buf.data() + w.done, take,
                sink_.data() + static_cast<std::size_t>(w.off) + w.done);
    w.done += take;
    if (w.done < w.buf.size()) {
      return 0;  // still owed: the write takes another poll
    }
    auto* const op = w.op;
    queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(pick));
    op->done(op, std::error_code{});
    return 1;
  }

  void wake() noexcept {
    {
      const std::lock_guard lock(m_);
      woken_ = true;
    }
    cv_.notify_one();
  }

  void cancel(detail::io_read_op& op) noexcept {
    for (write& w : queued_) {
      if (w.op == &op) {
        w.cancelled = true;
        return;
      }
    }
  }

  [[nodiscard]] std::size_t in_flight() const noexcept {
    return queued_.size();
  }
  void drain() noexcept { queued_.clear(); }

  [[nodiscard]] const std::vector<std::byte>& sink() const noexcept {
    return sink_;
  }
  [[nodiscard]] std::uint64_t final_size() const noexcept {
    return final_size_;
  }

 private:
  struct write {
    detail::io_read_op* op;
    std::uint64_t off;
    std::span<const std::byte> buf;
    std::size_t done;
    bool cancelled = false;
  };

  [[nodiscard]] std::size_t choose() const noexcept {
    std::size_t best = 0;
    std::size_t best_rank = rank_of(queued_.front());
    for (std::size_t i = 1; i < queued_.size(); ++i) {
      const std::size_t r = rank_of(queued_[i]);
      if (r < best_rank) {
        best = i;
        best_rank = r;
      }
    }
    return best;
  }
  [[nodiscard]] std::size_t rank_of(const write& w) const noexcept {
    const auto index = static_cast<unsigned>(w.off / window_);
    for (std::size_t i = 0; i < script_.order.size(); ++i) {
      if (script_.order[i] == index) {
        return i;
      }
    }
    return script_.order.size() + index;
  }

  std::uint64_t size_;
  std::size_t window_;
  write_script script_;
  std::vector<std::byte> sink_;
  std::vector<std::byte> pool_;
  std::size_t stride_ = 0;
  std::uint64_t final_size_ = ~std::uint64_t{0};
  std::vector<write> queued_;
  std::mutex m_;
  std::condition_variable cv_;
  bool woken_ = false;
};

static_assert(detail::file_driver<fake_write_driver>);

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

// The sequential forms take the same generator without a scheduler: the
// bounded one writes exactly what it was asked for, the unbounded one
// stops where the generator stops, and both name the mechanism that
// carried the bytes.
TEST_CASE("sequential write_file writes the same file as the pipeline") {
  constexpr std::size_t win = 64 * 1024;
  const std::vector<std::uint64_t> sizes{0, 1, win, 2 * win + 4321};
  for (const bool direct : {false, true}) {
    for (const bool async : {false, true}) {
      for (const std::uint64_t size : sizes) {
        CAPTURE(direct);
        CAPTURE(async);
        CAPTURE(size);
        const temp_path out;
        const blake3pp::write_report wrote = blake3pp::write_file(
            out.path, size, fill_from_offset,
            {.window_bytes = win, .direct_io = direct, .async = async});
        CHECK(wrote.bytes == size);
        CHECK(!wrote.backend.empty());
        CHECK(size_of(out.path) == size);
        if (size > 0) {
          CHECK(hash_of(out.path) == expected_digest(size));
        }
      }
    }
  }
}

TEST_CASE("an unbounded write_file ends where the generator does") {
  constexpr std::size_t win = 64 * 1024;
  for (const std::uint64_t size : {std::uint64_t{0}, std::uint64_t{win - 7},
                                   std::uint64_t{2 * win},
                                   std::uint64_t{2 * win + 11}}) {
    CAPTURE(size);
    const temp_path out;
    std::uint64_t left = size;
    const blake3pp::write_report wrote = blake3pp::write_file(
        out.path,
        [&](std::span<std::byte> buf, std::uint64_t off) -> std::size_t {
          const std::size_t take = static_cast<std::size_t>(
              std::min<std::uint64_t>(buf.size(), left));
          fill_from_offset(buf.first(take), off);
          left -= take;
          return take;
        },
        {.window_bytes = win});
    CHECK(wrote.bytes == size);
    CHECK(size_of(out.path) == size);
    if (size > 0) {
      CHECK(hash_of(out.path) == expected_digest(size));
    }
  }
}

// The ec forms report what the throwing ones raise: a path that cannot be
// created is an error_code, not an exception.
TEST_CASE("sequential write_file reports failure through ec") {
  const fs::path bad = fs::path("/nonexistent/blake3pp/no/such/dir") / "f";
  std::error_code ec;
  const blake3pp::write_report wrote =
      blake3pp::write_file(bad, 4096, fill_from_offset, ec);
  CHECK(ec);
  CHECK(wrote.bytes == 0);
  CHECK_THROWS_AS(blake3pp::write_file(bad, 4096, fill_from_offset),
                  std::system_error);

  ec.clear();
  const blake3pp::write_report none = blake3pp::write_file(
      bad,
      [](std::span<std::byte>, std::uint64_t) -> std::size_t { return 0; }, ec);
  CHECK(ec);
  CHECK(none.bytes == 0);
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

// What a real device produces only by luck: completion out of order, a
// write that takes several polls, a failure at one chosen window.
TEST_CASE("the write pipeline survives a driver that misbehaves") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::uint64_t size = 6 * win + 123;

  SUBCASE("out of order and in pieces") {
    fake_write_driver drv(size, win,
                          {.order = {4, 2, 0, 5, 1, 3},
                           .piece_bytes = win / 3 + 1});
    fake_write_driver::file f(drv);
    detail::run_write_pipeline(drv, f, sched, fill_from_offset, size,
                               {.window_bytes = win, .queue_depth = 4});
    CHECK(drv.final_size() == size);
    std::vector<std::byte> want(static_cast<std::size_t>(size));
    fill_from_offset(want, 0);
    CHECK(drv.sink() == want);
  }

  SUBCASE("an error at one window ends the run once") {
    fake_write_driver drv(size, win, {.fail_at_window = 3});
    fake_write_driver::file f(drv);
    std::error_code ec;
    int throws = 0;
    try {
      detail::run_write_pipeline(drv, f, sched, fill_from_offset, size,
                                 {.window_bytes = win, .queue_depth = 4});
    } catch (const std::system_error& e) {
      ++throws;
      ec = e.code();
    }
    CHECK(throws == 1);
    CHECK(ec == std::errc::io_error);
    CHECK(drv.in_flight() == 0);
    // No length was invented for bytes that were never produced.
    CHECK(drv.final_size() == ~std::uint64_t{0});
  }

  SUBCASE("a stop mid-file ends the run once") {
    blake3pp::ex::inplace_stop_source src;
    fake_write_driver drv(size, win,
                          {.stop_at_window = 2, .stop_source = &src});
    fake_write_driver::file f(drv);
    std::error_code ec;
    try {
      detail::run_write_pipeline(drv, f, sched, fill_from_offset, size,
                                 {.window_bytes = win,
                                  .queue_depth = 4,
                                  .stop_token = src.get_token()});
    } catch (const std::system_error& e) {
      ec = e.code();
    }
    CHECK(ec == std::errc::operation_canceled);
    CHECK(drv.in_flight() == 0);
    CHECK(drv.final_size() == ~std::uint64_t{0});
  }
}

// The real driver, with writes on the platform's engine: a stop
// mid-file must land or reap everything in flight and report
// operation_canceled. Under the sanitizer lanes this is also where a
// write from a freed window buffer would show.
TEST_CASE("a real write pipeline stops mid-file with writes in flight") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::uint64_t size = 48 * win;
  int stopped = 0;
  constexpr int attempts = 6;
  for (int i = 0; i < attempts; ++i) {
    const temp_path out;
    detail::io_driver drv({true, true}, 4);
    detail::io_driver::file file(
        drv, out.path,
        detail::io_driver::open_options{.write = true,
                                        .preallocate_bytes = size});
    blake3pp::ex::inplace_stop_source src;
    std::thread stopper([&src, i] {
      std::this_thread::sleep_for(std::chrono::microseconds(40 * i));
      src.request_stop();
    });
    std::error_code ec;
    try {
      detail::run_write_pipeline(drv, file, sched, fill_from_offset, size,
                                 {.window_bytes = win,
                                  .queue_depth = 4,
                                  .stop_token = src.get_token()});
    } catch (const std::system_error& e) {
      ec = e.code();
    }
    stopper.join();
    CHECK(drv.in_flight() == 0);
    if (ec) {
      CHECK(ec == std::errc::operation_canceled);
      ++stopped;
    }
  }
  MESSAGE("stopped mid-file on " << stopped << " of " << attempts);
}

}  // TEST_SUITE
