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
#include <ratio>
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

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include <blake3pp/detail/write_pipeline.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/parallel_backend.hpp>
#include <blake3pp/detail/waiter.hpp>
#include <blake3pp/parallel_io.hpp>
#include <blake3pp/trace.hpp>

#include "modelled_device.hpp"
#include "scripted_order.hpp"
#include "temp_names.hpp"
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
    path = fs::temp_directory_path() /
           ("blake3pp_wp_" + b3test::process_salt() + "_" +
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

// A scripted driver, for the cases a real one only produces by luck:
// completion out of order, a short write, a failure or a stop at one
// chosen window, and optionally a modelled device under the sink.
struct write_script {
  std::vector<unsigned> order{};   // preference by window index
  std::size_t piece_bytes = 0;     // 0 writes a window in one go
  int fail_at_window = -1;
  int stop_at_window = -1;
  blake3pp::ex::inplace_stop_source* stop_source = nullptr;
  // A device under the sink, instead of writes that land the instant
  // they are polled: tooling/modelled_device.hpp's FIFO server, holding
  // each write for `service` behind (knee - 1) service times of delay.
  // Zero service is no device at all, which is what every other case
  // wants.
  std::chrono::nanoseconds service{};
  unsigned knee = 1;
};

class fake_write_driver {
 public:
  fake_write_driver(std::uint64_t size, std::size_t window, write_script s)
      : size_(size), window_(window), script_(std::move(s)),
        sink_(static_cast<std::size_t>(size)),
        device_(script_.service, script_.knee) {}

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
    const std::span<std::byte> s =
        detail::window_slice(pool_, slot, stride_, bytes);
    return {s, std::span<const std::byte>(s)};
  }

  void submit_read(file&, std::uint64_t, std::span<std::byte>,
                   detail::io_op&) {
    FAIL("the write pipeline must not read");
  }

  void submit_write(file&, std::uint64_t off, std::span<const std::byte> buf,
                    detail::io_op& op) {
    queued_.push_back(write{&op, off, buf, 0, false,
                            device_.active()
                                ? device_.admit()
                                : b3tool::modelled_device::instant{}});
    op.live = true;  // the driver owes this one a callback now
  }

  void flush() noexcept {}

  std::size_t poll(bool block) {
    if (queued_.empty()) {
      if (block) {
        waiter_.sleep();
        (void)waiter_.take();
      }
      return 0;
    }
    const std::size_t pick =
        b3test::scripted_pick(queued_, script_.order, window_);
    write& w = queued_[pick];
    if (!await_device(w)) {
      return 0;
    }
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
      op->live = false;
      op->done(op, std::make_error_code(std::errc::operation_canceled));
      return 1;
    }
    if (script_.fail_at_window >= 0 &&
        index == static_cast<unsigned>(script_.fail_at_window)) {
      auto* const op = w.op;
      queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(pick));
      op->live = false;
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
    op->live = false;
    op->done(op, std::error_code{});
    return 1;
  }

  void wake() noexcept { waiter_.wake(); }

  void cancel(detail::io_op& op) noexcept {
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
  // One file per case here, and nothing in the OS's hands: this file's
  // share of the queue is the queue.
  void drain(file&) noexcept { queued_.clear(); }

  [[nodiscard]] const std::vector<std::byte>& sink() const noexcept {
    return sink_;
  }
  [[nodiscard]] std::uint64_t final_size() const noexcept {
    return final_size_;
  }

 private:
  struct write {
    detail::io_op* op;
    std::uint64_t off;
    std::span<const std::byte> buf;
    std::size_t done;
    bool cancelled = false;
    // When the modelled device would land this one; the clock's epoch for
    // no device.
    b3tool::modelled_device::instant ready{};
  };

  // Holds a completion back until the device would have it, waiting a
  // slice toward it first. False leaves the write queued for a later
  // poll. The order script is not combined with a device: the server is
  // FIFO, so submission order is completion order.
  bool await_device(const write& w) {
    return w.ready == b3tool::modelled_device::instant{} ||
           device_.wait_toward(w.ready);
  }


  std::uint64_t size_;
  std::size_t window_;
  write_script script_;
  std::vector<std::byte> sink_;
  std::vector<std::byte> pool_;
  std::size_t stride_ = 0;
  std::uint64_t final_size_ = ~std::uint64_t{0};
  std::vector<write> queued_;
  b3tool::modelled_device device_;
  blake3pp::detail::waiter waiter_;
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

#if defined(_WIN32)
// NTFS zeroes ahead of every write that lands past the valid data length,
// so a preallocated file is written sparse for the run and made plain
// again at the trim; the waiver is taken only where the privilege is
// held, which a test session does not have, and falls to sparse too.
TEST_CASE("a preallocated write on Windows is sparse for the run and plain after") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::uint64_t size = 8 * win + 4321;
  for (const bool waive : {false, true}) {
    CAPTURE(waive);
    const temp_path out;
    const blake3pp::write_report r = blake3pp::write_file(
        out.path, size, fill_from_offset, sched,
        {.window_bytes = win, .waive_zero_fill = waive});
    MESSAGE("write backend: " << r.backend);
    CHECK(r.bytes == size);
    const bool sparse = r.backend.ends_with("+sparse");
    const bool valid = r.backend.ends_with("+validdata");
    // One of the two on NTFS; a file system without sparse files names
    // neither, which the message above shows.
    CHECK((sparse || valid || !r.backend.starts_with("iocp")));
    if (!waive) {
      CHECK(!valid);
    }
    CHECK(size_of(out.path) == size);
    CHECK(hash_of(out.path) == expected_digest(size));
    const DWORD attrs = ::GetFileAttributesW(out.path.c_str());
    REQUIRE(attrs != INVALID_FILE_ATTRIBUTES);
    CHECK((attrs & FILE_ATTRIBUTE_SPARSE_FILE) == 0);
  }
}
#endif

// The write side records like the read side: one record per window with
// the write's stages in the same stamps, admitted before filled, filled
// before landed, landed before released; and one agent record per fill
// when agent storage is given.
TEST_CASE("a traced write_file records every window's stages") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::uint64_t size = 6 * win + 4321;
  const std::size_t windows = (size + win - 1) / win;
  std::vector<blake3pp::window_record> records(windows + 1);
  std::vector<blake3pp::agent_record> agents(windows + 1);
  blake3pp::trace_buffer trace(records, agents);
  const temp_path out;
  blake3pp::write_file(out.path, size, fill_from_offset, sched,
                       {.window_bytes = win, .queue_depth = 3, .trace = &trace});
  CHECK(hash_of(out.path) == expected_digest(size));
  REQUIRE(trace.windows().size() == windows);
  bool last_seen = false;
  std::uint64_t bytes = 0;
  for (const blake3pp::window_record& w : trace.windows()) {
    CHECK(w.t_ready >= w.t_wait_begin);
    CHECK(w.t_joined >= w.t_ready);
    CHECK(w.t_absorbed >= w.t_joined);
    CHECK(w.t_released >= w.t_absorbed);
    CHECK(w.agents_active == 1);
    CHECK(w.slot < 3);
    bytes += w.bytes;
    last_seen = last_seen || (w.flags & blake3pp::window_record::flag_last) != 0;
  }
  CHECK(bytes == size);
  CHECK(last_seen);
  CHECK(trace.agents().size() == windows);
  CHECK(trace.driver().iterations > 0);
}

// A generator that throws, on the pool, inside a window already counted in
// flight: the chain carries the exception, the run stops, and write_file
// rethrows it. Nothing is submitted for that window before the fill, so the
// file is whatever landed before it -- and never a length nobody produced.
TEST_CASE("a write generator's exception comes back out of write_file") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::uint64_t size = 8 * win;
  for (const unsigned qd : {2u, 4u}) {
    CAPTURE(qd);
    const temp_path out;
    std::atomic<unsigned> fills{0};
    // The third window to be filled throws, whichever offset that is: the
    // windows are filled concurrently and in no particular order.
    auto fill = [&fills](std::span<std::byte> w, std::uint64_t off) {
      if (fills.fetch_add(1, std::memory_order_relaxed) == 2) {
        throw std::runtime_error("the generator gave up");
      }
      fill_from_offset(w, off);
    };
    CHECK_THROWS_AS(blake3pp::write_file(out.path, size, fill, sched,
                                        {.window_bytes = win,
                                         .queue_depth = qd}),
                    std::runtime_error);
    // The file exists and is not the full length: a length invented for
    // bytes nobody produced is what this must never do.
    CHECK(size_of(out.path) <= size);
  }
  // The error_code form cannot tell a generator's exception from the other
  // things that escape a pipeline, and says so by reporting one.
  {
    const temp_path out;
    std::error_code ec;
    auto fill = [](std::span<std::byte>, std::uint64_t) -> void {
      throw std::runtime_error("no");
    };
    blake3pp::write_file(out.path, size, fill, sched, ec,
                         {.window_bytes = win});
    CHECK(ec);
  }
}

// The automatic depth: a request is clamped like any other; zero becomes
// one window per thread, bounded by the bytes-in-flight budget and by the
// range every depth has.
TEST_CASE("an automatic write depth follows the threads within the budget") {
  using blake3pp::detail::resolve_write_queue_depth;
  using blake3pp::detail::rounded_window_bytes;
  constexpr std::size_t mib = 1024 * 1024;
  // The 64 MiB and 1 GiB windows the cases below name are what the
  // platform's window cap lets through (see the read plan test): on 32-bit
  // size_t both round down to 8 MiB, and the thread counts scale by the
  // same ratio, so each expectation is computed from the rounded window.
  const std::size_t w64 = rounded_window_bytes(64 * mib);
  const unsigned t64 = 32 * static_cast<unsigned>(64 * mib / w64);
  const std::size_t w1g = rounded_window_bytes(1024 * mib);
  const unsigned t1g = 32 * static_cast<unsigned>(1024 * mib / w1g);
  CHECK(resolve_write_queue_depth(6, 8 * mib, 32) == 6);
  CHECK(resolve_write_queue_depth(1, 8 * mib, 32) == 2);
  CHECK(resolve_write_queue_depth(0, 8 * mib, 32) == 32);
  CHECK(resolve_write_queue_depth(0, 8 * mib, 16) == 16);
  CHECK(resolve_write_queue_depth(0, 8 * mib, 1) == 2);
  CHECK(resolve_write_queue_depth(0, 8 * mib, 0) == 2);
  // 256 MiB in flight bounds the product: 32 windows of 8 MiB, and 4 of
  // 64 MiB where the window is not rounded down.
  CHECK(resolve_write_queue_depth(0, 8 * mib, 1024) == 32);
  CHECK(resolve_write_queue_depth(0, 64 * mib, t64) == (256 * mib) / w64);
  CHECK(resolve_write_queue_depth(0, 1024 * mib, t1g) ==
        std::max<unsigned>(2u, std::min<unsigned>(
            t1g, static_cast<unsigned>((256 * mib) / w1g))));
  if constexpr (sizeof(std::size_t) == 8) {
    // Where nothing rounds the windows down, the figures themselves.
    CHECK(resolve_write_queue_depth(0, 64 * mib, 32) == 4);
    CHECK(resolve_write_queue_depth(0, 1024 * mib, 32) == 2);
  }
  // A request outranks the budget: the caller stated it.
  CHECK(resolve_write_queue_depth(64, 64 * mib, 32) == 64);
  static_assert(resolve_write_queue_depth(0, 8 * mib, 32) == 32);
}

// The depth the device sees is the depth the caller asked for, and the
// run reports it back the way a read run does.
TEST_CASE("a fixed write depth is the depth, start to finish") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::uint64_t size = 96 * win;

  blake3pp::trace_buffer trace{std::span<blake3pp::window_record>{}};
  fake_write_driver drv(size, win, {});
  fake_write_driver::file f(drv);
  detail::run_write_pipeline(drv, f, sched, fill_from_offset, size,
                             {.window_bytes = win,
                              .queue_depth = 4,
                              .trace = &trace});
  CHECK(drv.final_size() == size);
  const auto& d = trace.driver();
  CHECK(d.queue_depth_final == 4);
  CHECK(d.queue_depth_peak == 4);
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

// One call per window, each filling a window's worth or the tail: a size
// that is a whole number of windows, or zero, must not end with a call for
// nothing, which a generator indexing its source by offset could not serve.
TEST_CASE("sequential write_file calls the generator once per window") {
  constexpr std::size_t win = 64 * 1024;
  for (const std::uint64_t size :
       {std::uint64_t{0}, std::uint64_t{win}, std::uint64_t{3 * win},
        std::uint64_t{win + 1}}) {
    CAPTURE(size);
    const temp_path out;
    std::uint64_t calls = 0;
    std::uint64_t empty_calls = 0;
    const blake3pp::write_report wrote = blake3pp::write_file(
        out.path, size,
        [&](std::span<std::byte> bytes, std::uint64_t off) {
          ++calls;
          if (bytes.empty()) {
            ++empty_calls;
          }
          fill_from_offset(bytes, off);
        },
        {.window_bytes = win});
    CHECK(wrote.bytes == size);
    CHECK(empty_calls == 0);
    CHECK(calls == (size + win - 1) / win);
    CHECK(size_of(out.path) == size);
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

// The scheduler-taking form has the same pair, so a caller that reports
// through ec does not have to drop to the sequential path for it.
TEST_CASE("write_file over a scheduler reports failure through ec") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  const std::uint64_t size = 2 * win + 15;
  {
    const temp_path out;
    std::error_code ec;
    blake3pp::write_file(out.path, size, fill_from_offset, sched, ec,
                         {.window_bytes = win});
    CHECK(!ec);
    CHECK(size_of(out.path) == size);
    CHECK(hash_of(out.path) == expected_digest(size));
  }
  std::error_code ec;
  blake3pp::write_file(fs::path("/nonexistent/blake3pp/no/such/dir") / "f",
                       size, fill_from_offset, sched, ec,
                       {.window_bytes = win});
  CHECK(ec);
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

// --- The diagnosis blake3ppgen prints (blake3pp::diagnose_write) -----------
//
// The write side's readings differ from the read side's. The pool FILLS
// windows and the device drains them, so the arena and the queue depth
// are the same number (one window is one fill task) and there is no
// arena reading: the two candidates share those windows, and the reading
// is which held them longer. As on the read side, the runs here assert
// the geometry and the counters, and tests/diagnosis.cpp the readings.
namespace {

template <class Scheduler>
[[nodiscard]] blake3pp::diagnosis diagnose_write_run(
    Scheduler sched, std::size_t win, std::uint64_t size, unsigned depth,
    unsigned threads, write_script script = {}) {
  blake3pp::trace_buffer trace{std::span<blake3pp::window_record>{}};
  fake_write_driver drv(size, win, script);
  fake_write_driver::file f(drv);
  const auto t0 = std::chrono::steady_clock::now();
  detail::run_write_pipeline(drv, f, std::move(sched), fill_from_offset, size,
                             {.window_bytes = win,
                              .queue_depth = depth,
                              .trace = &trace});
  const auto wall = std::chrono::steady_clock::now() - t0;
  REQUIRE(drv.final_size() == size);
  return blake3pp::diagnose_write(trace.driver(), wall, threads, win);
}

// doctest streams a const char* as a pointer, so the line is built as a
// string first.
void report_write(const char* name, const blake3pp::diagnosis& r) {
  const auto pct = [](double v) {
    return std::to_string(static_cast<int>(100.0 * v)) + "%";
  };
  const std::string line =
      std::string(name) + ": " + blake3pp::to_string(r.label) + " (fill " +
      pct(r.fill_busy) + " of depth " + std::to_string(r.depth) +
      ", device " + pct(r.device_busy) + ", queued " + pct(r.device_queued) +
      ")";
  MESSAGE(line);
}

}  // namespace

TEST_CASE("a sink faster than the generator feeds the diagnosis coherent counters") {
  // The device takes a window the instant it is polled, so the windows
  // in flight spend their lives being filled, by one agent. The reading,
  // generator-limited, compares the fill's and the device's shares of the
  // wall clock, which load moves; it is asserted in tests/diagnosis.cpp
  // against hand-built counters. The run asserts the geometry and that
  // the counters cohere.
  b3test::one_thread_pool pool;
  constexpr std::size_t win = 256 * 1024;
  const std::uint64_t size = 48 * win;

  const auto r = diagnose_write_run(pool.get_scheduler(), win, size, 4, 1);
  report_write("instant sink", r);
  CHECK(r.arena_is_depth);
  CHECK(r.depth == 4);
  CHECK(r.measured());
  CHECK(r.transfers == 48u);
  CHECK(r.fill_busy > 0.0);
  CHECK(r.fill_busy <= 1.0);
  CHECK(r.device_busy >= 0.0);
  CHECK(r.device_busy <= 1.0);
}

TEST_CASE("a slow sink feeds the diagnosis coherent counters on the write side") {
  // One server holding each write for 8 ms against the whole pool
  // generating. The reading, device-limited, turns on the device's queue
  // share and on the fill's and the device's shares of the wall clock,
  // which load moves; it is asserted in tests/diagnosis.cpp. The run
  // asserts the geometry, the counters, and the floor the device model
  // guarantees.
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t win = 64 * 1024;
  constexpr std::chrono::nanoseconds service{8'000'000};
  const std::uint64_t size = 48 * win;

  const auto r =
      diagnose_write_run(sched, win, size, 2,
                         std::thread::hardware_concurrency(),
                         {.service = service, .knee = 1});
  report_write("slow sink", r);
  CHECK(r.arena_is_depth);
  CHECK(r.depth == 2);
  CHECK(r.measured());
  CHECK(r.long_enough());
  CHECK(r.transfers == 48u);
  // A write is timed from the moment it is handed to the driver, and
  // the device holds it at least one service time: load can only add.
  CHECK(r.min_service >= service);
  CHECK(r.min_service <= r.mean_service);
  CHECK(r.fill_busy >= 0.0);
  CHECK(r.fill_busy <= 1.0);
  CHECK(r.device_busy > 0.0);
  CHECK(r.device_busy <= 1.0);
}

}  // TEST_SUITE
