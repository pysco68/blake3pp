// The stream source: a driver whose bytes arrive rather than sit.
//
// Three things are being tested, and only the first is about the driver:
//
//  * the driver contract, as memory_source.cpp tests the in-memory one:
//    callbacks only from poll(), a wake that releases a blocked one, and
//    windows whose length is the driver's answer rather than the grid's
//    question;
//  * the DIGEST, over every shape a stream can take -- a producer that
//    keeps up, one that runs dry mid-window, a length that is an exact
//    multiple of the window, an empty stream, a dribbling producer, and
//    one that stops mid-window and closes. A window ended early reaches
//    the tree as the decomposition of a run of parts, and a
//    decomposition that is wrong is a wrong digest with nothing else to
//    see, so every case here asserts the digest against the same bytes
//    hashed sequentially;
//  * the hand-over condition itself: that an empty stream with a part in
//    hand ends a window, and that a producer who keeps up never trips
//    it.
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

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
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
// moment with the stream empty, or the end of the stream.
//
// One call takes everything the script has queued, up to what it was
// asked for, and stops at a `dry` step, as a real blocking read of a
// pipe does; the driver's short-read signal depends on that. A `dry` step
// reached with bytes already in hand ends the call short of its ask, the
// way an emptied pipe does. Reached with nothing in hand, it is a pause
// the call waits through, the way a blocking read waits for the
// producer's next write. A short flush therefore lands at a byte offset
// the case chose.
//
// `max_per_call` is declared as the reader's own read bound, so the
// driver bounds its ask by it rather than being told a short read: that
// is the shipped Windows reader's shape, where every read is capped and
// none of the caps is a dry stream.
class scripted_reader {
 public:
  struct step {
    std::size_t bytes = 0;  // deliver this many, in as many calls as it takes
    bool dry = false;       // the stream is momentarily empty
  };

  struct result {
    std::size_t bytes = 0;
    bool eof = false;
  };

  scripted_reader(std::span<const std::byte> source, std::vector<step> script,
                  std::size_t max_per_call = ~std::size_t{0})
      : src_(source), script_(std::move(script)), max_(max_per_call) {}

  [[nodiscard]] result read_some(std::span<std::byte> out) {
    if (out.empty()) {
      return {};
    }
    std::size_t moved = 0;
    for (;;) {
      while (next_ < script_.size() && script_[next_].bytes == 0 &&
             !script_[next_].dry) {
        ++next_;
      }
      if (next_ >= script_.size()) {
        // The script is spent: the end of the stream, unless this call
        // has bytes to report first.
        return moved > 0 ? result{.bytes = moved, .eof = false}
                         : result{.bytes = 0, .eof = true};
      }
      step& s = script_[next_];
      if (s.dry) {
        ++next_;
        ++dries_;
        if (moved > 0) {
          // An emptied queue: the call comes back short of its ask.
          return {.bytes = moved, .eof = false};
        }
        // Nothing in hand, so the read waits for the producer's next
        // write, which is the step after this one.
        continue;
      }
      const std::size_t n =
          std::min({s.bytes, out.size() - moved, src_.size() - at_});
      if (n == 0) {
        return moved > 0 ? result{.bytes = moved, .eof = false}
                         : result{.bytes = 0, .eof = true};
      }
      std::memcpy(out.data() + moved, src_.data() + at_, n);
      at_ += n;
      s.bytes -= n;
      moved += n;
      if (moved == out.size()) {
        return {.bytes = moved, .eof = false};
      }
    }
  }

  // The cap this reader reads under, which the driver asks within.
  [[nodiscard]] std::size_t read_bound() const noexcept {
    return max_ == ~std::size_t{0} ? 0 : max_;
  }

  std::size_t widen(std::size_t) noexcept { return 0; }

  // How many times the script left the stream empty.
  [[nodiscard]] std::uint64_t dries() const noexcept { return dries_; }

 private:
  std::span<const std::byte> src_;
  std::vector<step> script_;
  std::size_t max_;
  std::size_t at_ = 0;
  std::size_t next_ = 0;
  std::uint64_t dries_ = 0;
};

using test_driver = detail::basic_pipe_driver<scripted_reader>;

// The library's own default, named so it can be a template argument.
constexpr blake3pp::stack_budget budget = blake3pp::default_stack_budget;

// A whole script delivered in one go: a producer that always keeps up.
std::vector<scripted_reader::step> all_at_once(std::size_t bytes) {
  return {{.bytes = bytes, .dry = false}};
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
  out.short_flushes = drv.short_flushes();
  out.bytes = drv.bytes_read();
  out.records.resize(trace.windows().size());
  return out;
}

// What each window's length was, in order: the sharpest statement of
// what a stream shape did, because a hand-over is a window that came
// out shorter than the grid asked for.
std::vector<std::uint64_t> window_lengths(const outcome& o) {
  std::vector<std::uint64_t> v;
  v.reserve(o.records.size());
  for (const blake3pp::window_record& r : o.records) {
    v.push_back(r.bytes);
  }
  return v;
}

std::size_t part_run_records(const outcome& o) {
  return static_cast<std::size_t>(
      std::ranges::count_if(o.records, [](const blake3pp::window_record& r) {
        return (r.flags & blake3pp::window_record::flag_part_run) != 0;
      }));
}

// A real pipe and a producer on it: the one place in this file that
// touches the OS, and the only platform-shaped code in it.
struct os_pipe {
#if defined(_WIN32)
  using end = HANDLE;
  static constexpr end none = nullptr;
#else
  using end = int;
  static constexpr end none = -1;
#endif

  end r = none;
  end w = none;

  os_pipe() = default;
  os_pipe(const os_pipe&) = delete;
  os_pipe& operator=(const os_pipe&) = delete;
  ~os_pipe() {
    close_one(w);
    close_one(r);
  }

  [[nodiscard]] bool open() {
#if defined(_WIN32)
    return CreatePipe(&r, &w, nullptr, 0) != 0;
#else
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
      return false;
    }
    r = fds[0];
    w = fds[1];
    return true;
#endif
  }

  void close_read() { close_one(r); }
  void close_write() { close_one(w); }

  // Everything the buffer holds, in writes of at most `chunk`, then the
  // close that is the stream's end.
  void produce(std::span<const std::byte> bytes, std::size_t chunk) {
    std::size_t at = 0;
    while (at < bytes.size()) {
      const std::size_t want = std::min(chunk, bytes.size() - at);
#if defined(_WIN32)
      DWORD moved = 0;
      if (WriteFile(w, bytes.data() + at, static_cast<DWORD>(want), &moved,
                    nullptr) == 0 ||
          moved == 0) {
        break;
      }
      at += static_cast<std::size_t>(moved);
#else
      const auto n = ::write(w, bytes.data() + at, want);
      if (n <= 0) {
        break;
      }
      at += static_cast<std::size_t>(n);
#endif
    }
    close_write();
  }

 private:
  static void close_one(end& h) {
    if (h == none) {
      return;
    }
#if defined(_WIN32)
    CloseHandle(h);
#else
    ::close(h);
#endif
    h = none;
  }
};

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

  // What is left is shorter than the ask, so every window after the
  // first is as long as the stream made it and the last one brings the
  // end with it. The tail of a stream reads exactly like a producer that
  // ran dry (a read short of its ask), so it may arrive as more than one
  // window. No byte may be lost or reordered.
  std::size_t at = 64 * 1024;
  unsigned slot = 1;
  for (;;) {
    const detail::window_storage w =
        drv.window_storage_for(f, slot % 4, at, 64 * 1024);
    if (w.view.empty()) {
      break;
    }
    CHECK(w.view.size() <= 64 * 1024);
    CHECK(std::ranges::equal(w.view, std::span(bytes).subspan(at,
                                                              w.view.size())));
    at += w.view.size();
    ++slot;
  }
  CHECK(at == 100 * 1024);
  CHECK(f.size() == 100 * 1024);
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

TEST_CASE("a stream that keeps up is never handed over early") {
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
  // The reader declares a 1 KiB read bound, so a window takes dozens of
  // calls and the driver asks within the bound every time: the fill
  // loop's own arithmetic, with no hand-over in the picture. A bound is
  // not a dry stream, which is the distinction the shipped Windows
  // reader depends on -- every one of its reads is capped.
  const auto bytes = pattern(200 * 1024 + 511, 23);
  const outcome o = run_stream(bytes, all_at_once(bytes.size()), 64 * 1024,
                               1024);
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.short_flushes == 0);
}

TEST_CASE("the shipped read bound does not fragment a stream") {
  // 1 MiB is the bound the Windows reader asks within, and a window is
  // eight of them: a producer that keeps up must still fill whole
  // windows, or the bound would be paying for hand-overs it was meant to
  // avoid.
  const auto bytes = pattern(3 * 1024 * 1024 + 7, 24);
  const outcome o = run_stream(bytes, all_at_once(bytes.size()),
                               8 * 1024 * 1024, 1024 * 1024);
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.bytes == bytes.size());
  CHECK(o.short_flushes == 0);
  CHECK(part_run_records(o) == 0);
}

// ------------------------------------------------------------------
// The hand-over: an empty stream with a part in hand.

TEST_CASE("an empty stream ends a window early and the short run still folds") {
  // The producer delivers 40 KiB of a 64 KiB window and then stalls. The
  // quantum is 16 KiB, so the flush is two parts (32 KiB) and the 8 KiB
  // left over stays in hand -- which is also what proves the flushed
  // prefix is not the message's end. Every window after it is off the
  // grid until one brings it back, and each of those reaches the tree as
  // the decomposition of a run of parts rather than as one subtree.
  //
  // The run ends with a second hand-over, at the end of the stream: the
  // last read came back short of its ask because there was nothing more
  // to come. A paused producer gives the same observation, and the two
  // cannot be told apart until the read after it reports the end.
  // 32 KiB of the 44 KiB remainder goes out as whole parts and the 12 KiB
  // left is the last window.
  //
  // Fails if the hand-over never happens, and fails with a wrong digest
  // if a short run is decomposed wrongly.
  const auto bytes = pattern(300 * 1024, 31);
  const outcome o = run_stream(bytes, {{.bytes = 40 * 1024},
                                       {.dry = true},
                                       {.bytes = bytes.size() - 40 * 1024}});
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.bytes == bytes.size());
  CHECK(window_lengths(o) ==
        std::vector<std::uint64_t>{32 * 1024, 32 * 1024, 64 * 1024, 64 * 1024,
                                   64 * 1024, 32 * 1024, 12 * 1024});
  CHECK(o.short_flushes == 2);
  // Windows that are runs of parts rather than one subtree: the one that
  // ended early, the one that brings the grid back to its boundary, and
  // the hand-over at the end of the stream.
  CHECK(part_run_records(o) == 3);
}

TEST_CASE("a dribbling producer hands a window over before it is full") {
  // The hand-over itself: the stream has not ended (the driver has
  // 260 KiB of it still to come and says so through size()), and the
  // first window comes back shorter than the 64 KiB that was asked for.
  // A reader that waited for the byte count would hold the pool idle
  // through every pause a slow producer takes.
  const auto bytes = pattern(300 * 1024, 38);
  std::vector<scripted_reader::step> script;
  for (std::size_t at = 0; at < bytes.size(); at += 40 * 1024) {
    script.push_back({.bytes = std::min<std::size_t>(40 * 1024,
                                                     bytes.size() - at)});
    script.push_back({.dry = true});
  }
  test_driver drv(scripted_reader{bytes, std::move(script)},
                  {.flush_quantum = 16 * 1024});
  test_driver::file f(drv);
  drv.reserve(4, 64 * 1024);

  const detail::window_storage w0 = drv.window_storage_for(f, 0, 0, 64 * 1024);
  // 40 KiB arrived of a 64 KiB ask, and two whole parts of it go out.
  CHECK(w0.view.size() == 32 * 1024);
  CHECK(std::ranges::equal(w0.view, std::span(bytes).first(32 * 1024)));
  // The end of the stream is nowhere in sight: this was a hand-over and
  // not a last window.
  CHECK(f.size() == detail::streaming_size);
  CHECK(drv.short_flushes() == 1);
  CHECK(drv.windows_filled() == 1);
}

TEST_CASE("a stream that runs dry on every window still hashes correctly") {
  // A stall after every burst: the grid spends the whole run off its
  // boundary, so nearly every window is a run of parts, and several such
  // windows are in flight at once. Each needs an edge-node buffer of its
  // own.
  const std::size_t len = 1024 * 1024;
  const auto bytes = pattern(len, 32);
  std::vector<scripted_reader::step> script;
  for (std::size_t at = 0; at < len; at += 37 * 1024) {
    script.push_back({.bytes = std::min<std::size_t>(37 * 1024, len - at)});
    script.push_back({.dry = true});
  }
  const outcome o = run_stream(bytes, std::move(script), 64 * 1024,
                               ~std::size_t{0}, 4);
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.bytes == len);
  CHECK(o.short_flushes > 4);
  CHECK(part_run_records(o) >= o.short_flushes);
}

TEST_CASE("an empty stream with less than a flush in hand waits instead") {
  // 20 KiB of a 64 KiB window is one quantum plus change: a flush would
  // have to keep a part back to prove it is not the end, and one part is
  // not worth a round trip through the pool. An EMPTY stream is also
  // proof that the producer is not waiting on this reader, so the fill
  // blocks and the window ends on its byte count after all.
  const auto bytes = pattern(130 * 1024, 33);
  const outcome o = run_stream(bytes, {{.bytes = 20 * 1024},
                                       {.dry = true},
                                       {.bytes = bytes.size() - 20 * 1024}});
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.short_flushes == 0);
}

TEST_CASE("a window that filled but whose read-ahead has not arrived") {
  // The window's byte count is reached by a read that came back whole,
  // so nothing yet says the stream is dry, and the byte that proves the
  // window is not the message's end is read for on its own. That read
  // blocks, and the window goes out at its full length: it has all its
  // bytes, and a stream with nothing in it is a producer that is not
  // waiting on this reader, so there is nothing the wait gives up.
  //
  // The one hand-over in the run is the stream's end, 32 KiB of the
  // 44 KiB remainder, as in the case above.
  const auto bytes = pattern(300 * 1024, 34);
  const outcome o = run_stream(bytes, {{.bytes = 64 * 1024},
                                       {.dry = true},
                                       {.bytes = bytes.size() - 64 * 1024}});
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.bytes == bytes.size());
  CHECK(window_lengths(o) ==
        std::vector<std::uint64_t>{64 * 1024, 64 * 1024, 64 * 1024, 64 * 1024,
                                   32 * 1024, 12 * 1024});
  CHECK(o.short_flushes == 1);
}

TEST_CASE("a producer that stops mid-window and closes") {
  // The stall hands over 32 KiB of the 40 KiB that arrived; the close
  // that follows makes the 8 KiB in hand the last window of the message,
  // so the end lands on a window the driver never asked the grid for.
  const auto bytes = pattern(40 * 1024, 35);
  const outcome o =
      run_stream(bytes, {{.bytes = bytes.size()}, {.dry = true}});
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.bytes == bytes.size());
  CHECK(o.short_flushes == 1);
}

TEST_CASE("half the window is the floor for handing one over") {
  // A 256 KiB window on a 16 KiB quantum: the floor is 128 KiB, so an
  // empty stream with 80 KiB in hand waits and one with 200 KiB in hand
  // hands over the 192 KiB that are whole parts. The floor is what keeps
  // a reader faster than its producer from turning every buffer's worth
  // into a round trip through the pool.
  const auto bytes = pattern(600 * 1024, 37);
  const outcome under = run_stream(bytes, {{.bytes = 80 * 1024},
                                           {.dry = true},
                                           {.bytes = bytes.size() - 80 * 1024}},
                                   256 * 1024);
  CHECK(under.digest == blake3pp::hash(bytes));
  CHECK(under.short_flushes == 0);

  const outcome over = run_stream(bytes, {{.bytes = 200 * 1024},
                                          {.dry = true},
                                          {.bytes = bytes.size() - 200 * 1024}},
                                  256 * 1024);
  CHECK(over.digest == blake3pp::hash(bytes));
  CHECK(over.bytes == bytes.size());
  CHECK(over.short_flushes == 1);
}

TEST_CASE("a producer that runs dry after every write still hashes correctly") {
  // The fragmentation shape: 4 KiB arrives, the stream is empty, 4 KiB
  // arrives. Below two parts in hand the fill blocks rather than hand
  // over, so every flush is still a run of whole parts however small the
  // producer's writes are.
  const std::size_t len = 512 * 1024;
  const auto bytes = pattern(len, 36);
  std::vector<scripted_reader::step> script;
  for (std::size_t at = 0; at < len; at += 4096) {
    script.push_back({.bytes = std::min<std::size_t>(4096, len - at)});
    script.push_back({.dry = true});
  }
  const outcome o =
      run_stream(bytes, std::move(script), 64 * 1024, ~std::size_t{0}, 4);
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.bytes == len);
  CHECK(o.short_flushes > 0);
  CHECK(part_run_records(o) >= o.short_flushes);
}

// ------------------------------------------------------------------
// The real reader, end to end.

TEST_CASE("update_stream over a real pipe matches the sequential hasher") {
#if defined(__EMSCRIPTEN__)
  // Emscripten's in-memory pipe answers EAGAIN to a read of an empty pipe
  // even after the write end has closed, so a case that reads a live
  // producer to its end cannot finish there.
  MESSAGE("skipped: an Emscripten pipe never reports the end of the stream");
  return;
#endif
  // The scripted cases above cover the driver's arithmetic; this covers
  // the platform's read, including the chunk size that leaves the pipe
  // empty between writes.
  for (const std::size_t len :
       {std::size_t{0}, std::size_t{1}, std::size_t{70'001},
        std::size_t{8 * 1024 * 1024}, std::size_t{8 * 1024 * 1024 + 4096}}) {
    for (const std::size_t chunk : {std::size_t{4096}, ~std::size_t{0}}) {
      CAPTURE(len);
      CAPTURE(chunk);
      const auto bytes = pattern(len, 41);
      os_pipe p;
      REQUIRE(p.open());
      std::thread producer([&p, &bytes, chunk] { p.produce(bytes, chunk); });
      blake3pp::hasher h;
      const blake3pp::stream_report r =
          blake3pp::update_stream(h, p.r, blake3pp::get_parallel_scheduler());
      producer.join();
      p.close_read();
      CHECK(h.finalize() == blake3pp::hash(bytes));
      CHECK(r.bytes == len);
      CHECK(r.window_bytes == blake3pp::detail::default_window_bytes);
    }
  }
}

// ------------------------------------------------------------------
// The watch before a read: its shape, from what it counts.
//
// A scripted platform drives stream_watch with a virtual clock. Every
// operation advances the clock by what the real one costs in round
// figures -- a probe 100 ns, a relax 50 ns, a sleep whatever remains to
// the arrival plus a wake-up -- and arrivals come at the scripted gap
// after each wait begins. Nothing here is a duration the machine
// measures: the assertions are on counters.

namespace {

struct scripted_watch_platform {
  std::int64_t t = 0;
  std::int64_t next_arrival = 0;
  std::int64_t wake_latency = 30'000;  // the real figure is tens of us
  [[nodiscard]] std::int64_t now() const noexcept { return t; }
  [[nodiscard]] bool probe() noexcept {
    t += 100;
    return t >= next_arrival;
  }
  void sleep() noexcept { t = std::max(t, next_arrival) + wake_latency; }
  void relax() noexcept { t += 50; }
  // Begins a wait whose data arrives `gap` from now.
  void arrive_in(std::int64_t gap) noexcept { next_arrival = t + gap; }
};

}  // namespace

TEST_CASE("a producer whose gaps sit inside the ceiling is never slept on") {
  detail::stream_watch w;
  scripted_watch_platform p;
  for (int i = 0; i < 64; ++i) {
    p.arrive_in(5'000);
    w.wait(p);
  }
  const auto& c = w.stats();
  CHECK(c.slept == 0);
  CHECK(c.caught == 64);
  // One opening probe and then one per interval until the data at 5 us,
  // give or take the probe's own cost.
  const std::uint64_t paced = 5'000 / detail::stream_watch::probe_interval_ns;
  CHECK(c.probes >= 64 * paced);
  CHECK(c.probes <= 64 * (paced + 2));
  CHECK(w.budget() == detail::stream_watch::ceiling_ns);
}

TEST_CASE("a producer whose gaps exceed the ceiling decays the budget to the floor") {
  detail::stream_watch w;
  scripted_watch_platform p;
  // Halved on every time-out, 16 us to the floor in a handful of waits.
  for (int i = 0; i < 8; ++i) {
    p.arrive_in(100'000);
    w.wait(p);
  }
  CHECK(w.stats().slept == 8);
  CHECK(w.stats().caught == 0);
  CHECK(w.budget() == detail::stream_watch::floor_ns);
  // And a short sleep re-arms it by one step.
  p.wake_latency = 2'000;
  p.arrive_in(5'000);
  w.wait(p);
  CHECK(w.budget() == detail::stream_watch::floor_ns + detail::stream_watch::step_ns);
}

TEST_CASE("at the floor every wait still makes one paced probe") {
  // The floor is one probe interval, and the promise is that a wait at
  // the floor probes once before it sleeps. A loop that tested the
  // budget before the probe made none, and a producer whose gaps sit
  // around a microsecond was slept on every time.
  detail::stream_watch w;
  scripted_watch_platform p;
  for (int i = 0; i < 8; ++i) {
    p.arrive_in(100'000);
    w.wait(p);
  }
  REQUIRE(w.budget() == detail::stream_watch::floor_ns);
  const auto before = w.stats();
  p.arrive_in(100'000);
  w.wait(p);
  // The opening probe and exactly one paced probe, then the sleep.
  CHECK(w.stats().probes - before.probes == 2);
  CHECK(w.stats().slept - before.slept == 1);
  // And data that arrives inside that one interval is caught, not slept on.
  p.arrive_in(detail::stream_watch::probe_interval_ns - 200);
  w.wait(p);
  CHECK(w.stats().slept - before.slept == 1);
  CHECK(w.stats().caught - before.caught == 1);
}

#if !defined(_WIN32)
TEST_CASE("a slow producer is waited for and the descriptor is left as found") {
#if defined(__EMSCRIPTEN__)
  // Emscripten's in-memory pipe answers EAGAIN to a read of an empty pipe
  // even after the write end has closed, so a case that reads a live
  // producer to its end cannot finish there.
  MESSAGE("skipped: an Emscripten pipe never reports the end of the stream");
  return;
#endif
  // The reader watches an empty pipe before it sleeps; a producer that
  // pauses for longer than the watch must still be waited for, and the
  // waiting must not reach the descriptor's flags, which standard input
  // shares with the parent shell.
  const auto bytes = pattern(3 * 64 * 1024 + 17, 43);
  os_pipe p;
  REQUIRE(p.open());
  const int flags_before = ::fcntl(p.r, F_GETFL);
  std::thread producer([&p, &bytes] {
    std::size_t at = 0;
    while (at < bytes.size()) {
      const std::size_t want = std::min<std::size_t>(64 * 1024, bytes.size() - at);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      const auto n = ::write(p.w, bytes.data() + at, want);
      REQUIRE(n > 0);
      at += static_cast<std::size_t>(n);
    }
    p.close_write();
  });
  blake3pp::hasher h;
  const blake3pp::stream_report r =
      blake3pp::update_stream(h, p.r, blake3pp::get_parallel_scheduler());
  producer.join();
  CHECK(::fcntl(p.r, F_GETFL) == flags_before);
  CHECK((flags_before & O_NONBLOCK) == 0);
  p.close_read();
  CHECK(h.finalize() == blake3pp::hash(bytes));
  CHECK(r.bytes == bytes.size());
}
#endif

TEST_CASE("a hasher off the window grid cannot be streamed into") {
  os_pipe p;
  REQUIRE(p.open());
  blake3pp::hasher h;
  const auto head = pattern(1024, 42);
  h.update(head);
  CHECK_THROWS_AS(
      blake3pp::update_stream(h, p.r, blake3pp::get_parallel_scheduler()),
      std::logic_error);
  p.close_write();
  p.close_read();
}

}  // TEST_SUITE
