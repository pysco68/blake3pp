// The stream source: a driver whose bytes arrive rather than sit.
//
// Three things are being tested, and only the first is about the driver:
//
//  * the driver contract, as memory_source.cpp tests the in-memory one:
//    callbacks only from poll(), a wake that releases a blocked one, and
//    windows whose length is the driver's answer rather than the grid's
//    question;
//  * the DIGEST, over every shape a stream can take -- a producer that
//    keeps up, one the flush deadline has to interrupt mid-stream, a
//    length that is an exact multiple of the window, an empty stream.
//    A window the deadline ended early reaches the tree as the
//    decomposition of a run of parts, and a decomposition that is wrong
//    is a wrong digest with nothing else to see, so every case here
//    asserts the digest against the same bytes hashed sequentially;
//  * the deadline itself: that it fires when the producer is behind, and
//    that it does not when the producer keeps up.
//
// The reader is scripted rather than a second process, so what arrives
// when is the test's decision and not the scheduler's: every short flush
// below happens at a byte offset the case chose.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/file_pipeline.hpp>
#include <blake3pp/detail/pipe_driver.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/parallel_backend.hpp>
#include <blake3pp/parallel_io.hpp>
#include <blake3pp/trace.hpp>
#include <doctest/doctest.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

namespace detail = blake3pp::detail;

std::vector<std::byte> pattern(std::size_t len, std::uint32_t seed) {
  std::vector<std::byte> v(len);
  std::uint32_t x = seed;
  for (auto& b : v) {
    x = x * 1'664'525u + 1'013'904'223u;
    b = static_cast<std::byte>(x >> 24);
  }
  return v;
}

// A reader whose arrivals are a script: how many bytes come next, or a
// deadline that passes with nothing, or the end of the stream.
//
// It ignores the deadline it is handed and reports a timeout where the
// script says so, which is what makes a short flush land at a chosen
// offset instead of wherever a real producer happened to be.
class scripted_reader {
 public:
  struct step {
    std::size_t bytes = 0;  // deliver this many, in as many calls as it takes
    bool timeout = false;   // the deadline passes with nothing available
  };

  struct result {
    std::size_t bytes = 0;
    bool eof = false;
    bool timed_out = false;
  };

  scripted_reader(std::span<const std::byte> source, std::vector<step> script,
                  std::size_t max_per_call = ~std::size_t{0})
      : src_(source), script_(std::move(script)), max_(max_per_call) {}

  [[nodiscard]] result read_some(std::span<std::byte> out, std::int64_t) {
    if (out.empty()) {
      return {};
    }
    while (next_ < script_.size() && script_[next_].bytes == 0 &&
           !script_[next_].timeout) {
      ++next_;
    }
    if (next_ >= script_.size()) {
      return {.bytes = 0, .eof = true, .timed_out = false};
    }
    step& s = script_[next_];
    if (s.timeout) {
      ++next_;
      ++timeouts_;
      return {.bytes = 0, .eof = false, .timed_out = true};
    }
    const std::size_t n =
        std::min({s.bytes, out.size(), max_, src_.size() - at_});
    if (n == 0) {
      return {.bytes = 0, .eof = true, .timed_out = false};
    }
    std::memcpy(out.data(), src_.data() + at_, n);
    at_ += n;
    s.bytes -= n;
    return {.bytes = n, .eof = false, .timed_out = false};
  }

  std::size_t widen(std::size_t) noexcept { return 0; }

  [[nodiscard]] std::uint64_t timeouts() const noexcept { return timeouts_; }

 private:
  std::span<const std::byte> src_;
  std::vector<step> script_;
  std::size_t max_;
  std::size_t at_ = 0;
  std::size_t next_ = 0;
  std::uint64_t timeouts_ = 0;
};

using test_driver = detail::basic_pipe_driver<scripted_reader>;

// The library's own default, named so it can be a template argument.
constexpr blake3pp::stack_budget budget = blake3pp::default_stack_budget;

// A whole script delivered in one go: a producer that always keeps up.
std::vector<scripted_reader::step> all_at_once(std::size_t bytes) {
  return {{.bytes = bytes, .timeout = false}};
}

// What one run of the stream pipeline did.
struct outcome {
  blake3pp::digest digest;
  std::uint64_t windows = 0;
  std::uint64_t short_flushes = 0;
  std::uint64_t bytes = 0;
  std::vector<blake3pp::window_record> records;
};

// The shipped geometry over a scripted reader: plan_stream() and
// stream_flush_quantum() are the same ones update_stream() uses, so what
// runs here is the path blake3ppsum takes, with the arrivals scripted.
outcome run_stream(std::span<const std::byte> bytes,
                   std::vector<scripted_reader::step> script,
                   std::size_t window_bytes = 64 * 1024,
                   std::size_t max_per_call = ~std::size_t{0},
                   unsigned windows = 0) {
  auto sched = blake3pp::get_parallel_scheduler();
  blake3pp::hasher h;
  const detail::pipeline_plan plan =
      detail::plan_stream(h.count(), window_bytes, windows);
  test_driver drv(
      scripted_reader{bytes, std::move(script), max_per_call},
      {.flush_quantum = detail::stream_flush_quantum<budget>(plan.window_bytes)});
  test_driver::file f(drv);
  outcome out;
  out.records.resize(bytes.size() / 1024 + 64);
  blake3pp::trace_buffer trace(std::span{out.records});
  detail::run_window_pipeline<budget>(h, drv, f, sched, plan,
                                      {.window_bytes = plan.window_bytes,
                                       .trace = &trace});
  out.digest = h.finalize();
  out.windows = drv.windows_filled();
  out.short_flushes = drv.timer_flushes();
  out.bytes = drv.bytes_read();
  out.records.resize(trace.windows().size());
  return out;
}

std::size_t part_run_records(const outcome& o) {
  return static_cast<std::size_t>(
      std::ranges::count_if(o.records, [](const blake3pp::window_record& r) {
        return (r.flags & blake3pp::window_record::flag_part_run) != 0;
      }));
}

struct completion_probe {
  int calls = 0;
  std::thread::id ran_on{};
  static void on_done(detail::io_op* op, std::error_code) noexcept {
    auto* const p = static_cast<completion_probe*>(op->owner);
    p->calls++;
    p->ran_on = std::this_thread::get_id();
  }
};

}  // namespace

TEST_SUITE("pipe_source") {

// ------------------------------------------------------------------
// The driver contract.

TEST_CASE("a stream window is as long as the stream made it") {
  const auto bytes = pattern(100 * 1024, 11);
  test_driver drv(scripted_reader{bytes, all_at_once(bytes.size())},
                  {.flush_quantum = 16 * 1024});
  test_driver::file f(drv);

  // Nothing has arrived, so the end is not in sight yet.
  CHECK(f.size() == detail::streaming_size);
  CHECK(f.name() == "pipe");

  drv.reserve(4, 64 * 1024);
  // A full window, plus the one byte that proves it is not the last.
  const detail::window_storage w0 = drv.window_storage_for(f, 0, 0, 64 * 1024);
  CHECK(w0.view.size() == 64 * 1024);
  CHECK(w0.read_into.empty());
  CHECK(f.size() == detail::streaming_size);
  CHECK(std::ranges::equal(w0.view, std::span(bytes).first(64 * 1024)));

  // What is left is shorter than the ask, and with it the end arrives.
  const detail::window_storage w1 =
      drv.window_storage_for(f, 1, 64 * 1024, 64 * 1024);
  CHECK(w1.view.size() == 100 * 1024 - 64 * 1024);
  CHECK(f.size() == 100 * 1024);
  CHECK(std::ranges::equal(w1.view, std::span(bytes).subspan(64 * 1024)));
}

TEST_CASE("a stream reports its reads only from poll") {
  const auto bytes = pattern(64 * 1024, 12);
  test_driver drv(scripted_reader{bytes, all_at_once(bytes.size())},
                  {.flush_quantum = 16 * 1024});
  test_driver::file f(drv);
  drv.reserve(2, 32 * 1024);
  (void)drv.window_storage_for(f, 0, 0, 32 * 1024);

  completion_probe p;
  detail::io_op op{};
  op.done = &completion_probe::on_done;
  op.owner = &p;
  drv.submit_read(f, 0, {}, op);
  CHECK(p.calls == 0);
  CHECK(op.live);
  CHECK(drv.in_flight() == 1);
  CHECK(drv.poll(false) == 1);
  CHECK(p.calls == 1);
  CHECK(p.ran_on == std::this_thread::get_id());
  CHECK(!op.live);
  CHECK(drv.in_flight() == 0);
}

TEST_CASE("a blocked poll is released by a wake") {
  const auto bytes = pattern(1024, 13);
  test_driver drv(scripted_reader{bytes, all_at_once(bytes.size())},
                  {.flush_quantum = 16 * 1024});
  std::thread waker([&drv] {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    drv.wake();
  });
  CHECK(drv.poll(true) == 0);
  waker.join();
}

// ------------------------------------------------------------------
// The digest, over the shapes a stream can take.

TEST_CASE("a stream that keeps up never fires the deadline") {
  for (const std::size_t len : {std::size_t{1},        std::size_t{1023},
                                std::size_t{1024},     std::size_t{64 * 1024},
                                std::size_t{65 * 1024}, std::size_t{192 * 1024},
                                std::size_t{200 * 1024 + 7}}) {
    CAPTURE(len);
    const auto bytes = pattern(len, 21);
    const outcome o = run_stream(bytes, all_at_once(len));
    CHECK(o.digest == blake3pp::hash(bytes));
    CHECK(o.bytes == len);
    // The byte count is the only bound a producer that keeps up reaches.
    CHECK(o.short_flushes == 0);
    CHECK(part_run_records(o) == 0);
  }
}

TEST_CASE("a stream whose length is a whole number of windows") {
  // The case the one byte of read-ahead exists for: without it the last
  // window would be inserted as a subtree containing the message's final
  // chunk, which is a wrong digest and no error.
  for (const unsigned n : {1u, 2u, 3u, 4u, 7u}) {
    CAPTURE(n);
    const auto bytes = pattern(n * 64 * 1024, 22);
    const outcome o = run_stream(bytes, all_at_once(bytes.size()));
    CHECK(o.digest == blake3pp::hash(bytes));
    CHECK(o.short_flushes == 0);
  }
}

TEST_CASE("an empty stream is the empty digest") {
  const outcome o = run_stream({}, {});
  CHECK(o.digest == blake3pp::hash(std::span<const std::byte>{}));
  CHECK(o.bytes == 0);
  CHECK(o.windows == 0);
}

TEST_CASE("a stream delivered in dribbles still hashes correctly") {
  // Every read_some() returns at most one chunk, so a window takes
  // dozens of calls: the fill loop's own arithmetic, with no deadline in
  // the picture.
  const auto bytes = pattern(200 * 1024 + 511, 23);
  const outcome o = run_stream(bytes, all_at_once(bytes.size()), 64 * 1024,
                               1024);
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.short_flushes == 0);
}

// ------------------------------------------------------------------
// The deadline.

TEST_CASE("the deadline ends a window early and the short run still folds") {
  // The producer delivers 40 KiB of a 64 KiB window and then stalls. The
  // quantum is 16 KiB, so the flush is two parts (32 KiB) and the 8 KiB
  // left over stays in hand -- which is also what proves the flushed
  // prefix is not the message's end. Every window after it is off the
  // grid until one brings it back, and each of those reaches the tree as
  // the decomposition of a run of parts rather than as one subtree.
  //
  // Fails if the deadline never fires (short_flushes would be zero), and
  // fails with a wrong digest if a short run is decomposed wrongly.
  const auto bytes = pattern(300 * 1024, 31);
  const outcome o = run_stream(bytes, {{.bytes = 40 * 1024},
                                       {.timeout = true},
                                       {.bytes = bytes.size() - 40 * 1024}});
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.bytes == bytes.size());
  CHECK(o.short_flushes == 1);
  // Two windows are runs of parts rather than subtrees: the one the
  // deadline ended, and the one that brings the grid back to its
  // boundary.
  CHECK(part_run_records(o) == 2);
}

TEST_CASE("a deadline that fires on every window still hashes correctly") {
  // A stall after every burst: the grid spends the whole run off its
  // boundary, so nearly every window is a run of parts, and several such
  // windows are in flight at once. Each needs an edge-node buffer of its
  // own.
  const std::size_t len = 1024 * 1024;
  const auto bytes = pattern(len, 32);
  std::vector<scripted_reader::step> script;
  for (std::size_t at = 0; at < len; at += 37 * 1024) {
    script.push_back({.bytes = std::min<std::size_t>(37 * 1024, len - at)});
    script.push_back({.timeout = true});
  }
  const outcome o = run_stream(bytes, std::move(script), 64 * 1024,
                               ~std::size_t{0}, 4);
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.bytes == len);
  CHECK(o.short_flushes > 4);
  CHECK(part_run_records(o) >= o.short_flushes);
}

TEST_CASE("a deadline with less than a flush in hand waits instead") {
  // 20 KiB of a 64 KiB window is one quantum plus change: a flush would
  // have to keep a part back to prove it is not the end, and one part is
  // not worth a round trip through the pool. The deadline passes, the
  // fill waits, and the window ends on its byte count after all.
  const auto bytes = pattern(130 * 1024, 33);
  const outcome o = run_stream(bytes, {{.bytes = 20 * 1024},
                                       {.timeout = true},
                                       {.bytes = bytes.size() - 20 * 1024}});
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.short_flushes == 0);
}

TEST_CASE("the flush deadline is one window at the fast-producer floor") {
  // The figure, not a threshold: a producer at or above the floor fills
  // a window inside the deadline and never trips it.
  CHECK(detail::pipe_flush_after(8 * 1024 * 1024) ==
        std::chrono::nanoseconds{15'625'000});
  CHECK(detail::pipe_flush_after(64 * 1024 * 1024) ==
        std::chrono::nanoseconds{125'000'000});
  const double window = 8.0 * 1024 * 1024;
  const double seconds =
      static_cast<double>(detail::pipe_flush_after(8 * 1024 * 1024).count()) /
      1e9;
  CHECK(window / seconds ==
        doctest::Approx(static_cast<double>(
            detail::pipe_fast_producer_bytes_per_s)));
}

// ------------------------------------------------------------------
// The real reader, end to end.

#if !defined(_WIN32)
TEST_CASE("update_stream over a real pipe matches the sequential hasher") {
#if defined(__EMSCRIPTEN__)
  // Emscripten's in-memory pipe answers EAGAIN to a read of an empty pipe
  // even after the write end has closed, so a case that reads a live
  // producer to its end cannot finish there.
  MESSAGE("skipped: an Emscripten pipe never reports the end of the stream");
  return;
#endif
  for (const std::size_t len :
       {std::size_t{0}, std::size_t{1}, std::size_t{70'001},
        std::size_t{8 * 1024 * 1024}, std::size_t{8 * 1024 * 1024 + 4096}}) {
    CAPTURE(len);
    const auto bytes = pattern(len, 41);
    int fds[2] = {-1, -1};
    REQUIRE(::pipe(fds) == 0);
    std::thread producer([&bytes, w = fds[1]] {
      std::size_t at = 0;
      while (at < bytes.size()) {
        const auto n = ::write(w, bytes.data() + at, bytes.size() - at);
        if (n <= 0) {
          break;
        }
        at += static_cast<std::size_t>(n);
      }
      ::close(w);
    });
    blake3pp::hasher h;
    const blake3pp::stream_report r = blake3pp::update_stream(
        h, fds[0], blake3pp::get_parallel_scheduler());
    producer.join();
    ::close(fds[0]);
    CHECK(h.finalize() == blake3pp::hash(bytes));
    CHECK(r.bytes == len);
    CHECK(r.window_bytes == blake3pp::detail::default_window_bytes);
  }
}

TEST_CASE("a hasher off the window grid cannot be streamed into") {
  int fds[2] = {-1, -1};
  REQUIRE(::pipe(fds) == 0);
  blake3pp::hasher h;
  const auto head = pattern(1024, 42);
  h.update(head);
  CHECK_THROWS_AS(blake3pp::update_stream(
                      h, fds[0], blake3pp::get_parallel_scheduler()),
                  std::logic_error);
  ::close(fds[0]);
  ::close(fds[1]);
}
#endif

}  // TEST_SUITE
