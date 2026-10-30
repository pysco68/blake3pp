// The stream source: a driver whose bytes arrive as a producer writes them.
//
// What is tested:
//
//  * the driver contract, as memory_source.cpp tests the in-memory one:
//    callbacks only from poll(), a wake that releases a blocked one, and
//    windows as long as the stream made them;
//  * the DIGEST, over every shape a stream can take: a producer that
//    keeps up, one that runs dry mid-window, a length that is an exact
//    multiple of the window, an empty stream, a dribbling producer, and
//    one that stops mid-window and closes. A window ended early reaches
//    the tree as the decomposition of a run of parts, and a wrong
//    decomposition is a wrong digest with nothing else to see, so every
//    case asserts the digest against the same bytes hashed sequentially;
//  * the hand-over condition: an empty stream with a part in hand ends a
//    window, a producer who keeps up never trips it, and with the early
//    hand-over off a dry stream is waited for;
//  * the read bound the driver asks within, and the Windows ask bound;
//  * the platform reader end to end over a real pipe, and the faults it
//    can see: EINTR, a non-blocking descriptor, a closed one, a writer
//    that vanishes, and on Windows PIPE_NOWAIT, a socket and a
//    message-mode pipe;
//  * the watch before a read, on a virtual clock.
//
// Most cases use a scripted reader, so what arrives when is the test's
// decision and not the scheduler's: every short flush below happens at a
// byte offset the case chose.

#include <algorithm>
#include <csignal>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
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
    // One read that succeeds and moves nothing, without ending the
    // stream: what a zero-length producer write looks like on Windows.
    bool empty_read = false;
    // The platform's read fails here: the reader throws, as
    // stream_reader does for anything but EINTR and EAGAIN.
    bool fail = false;
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
    asks_.push_back(out.size());
    std::size_t moved = 0;
    for (;;) {
      while (next_ < script_.size() && script_[next_].bytes == 0 &&
             !script_[next_].dry && !script_[next_].empty_read &&
             !script_[next_].fail) {
        ++next_;
      }
      if (next_ >= script_.size()) {
        // The script is spent: the end of the stream, unless this call
        // has bytes to report first.
        return moved > 0 ? result{.bytes = moved, .eof = false}
                         : result{.bytes = 0, .eof = true};
      }
      step& s = script_[next_];
      if (s.fail) {
        // A failure is reported once the bytes before it are out: a
        // real read either moves bytes or fails, never both.
        if (moved > 0) {
          return {.bytes = moved, .eof = false};
        }
        ++next_;
        ++failures_;
        throw std::system_error(std::make_error_code(std::errc::io_error),
                                "scripted read failure");
      }
      if (s.empty_read) {
        ++next_;
        ++empty_reads_;
        if (moved > 0) {
          return {.bytes = moved, .eof = false};
        }
        return {.bytes = 0, .eof = false};
      }
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

  // The cap this reader reads under, which the driver asks within. It
  // steps down after the first read when `shrink_bound_to` is set, which
  // is how a test sees whether the driver asks for it per read or caches
  // it for the window.
  [[nodiscard]] std::size_t read_bound() const noexcept {
    if (shrink_bound_to != 0 && !asks_.empty()) {
      return shrink_bound_to;
    }
    return max_ == ~std::size_t{0} ? 0 : max_;
  }

  // Every ask this reader was handed, in order.
  [[nodiscard]] const std::vector<std::size_t>& asks() const noexcept {
    return asks_;
  }

  std::size_t shrink_bound_to = 0;

  std::size_t widen(std::size_t) noexcept { return 0; }

  // How many times the script left the stream empty.
  [[nodiscard]] std::uint64_t dries() const noexcept { return dries_; }
  [[nodiscard]] std::uint64_t empty_reads() const noexcept {
    return empty_reads_;
  }
  [[nodiscard]] std::uint64_t failures() const noexcept { return failures_; }

 private:
  std::span<const std::byte> src_;
  std::vector<step> script_;
  std::size_t max_;
  std::size_t at_ = 0;
  std::size_t next_ = 0;
  std::uint64_t dries_ = 0;
  std::uint64_t empty_reads_ = 0;
  std::uint64_t failures_ = 0;
  std::vector<std::size_t> asks_;
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

// How a scripted run is shaped. `early_hand_over` is stated by every
// case that depends on it instead of taken from the platform default, so
// both shapes are exercised on every platform: update_stream() picks the
// shipped default, and the driver only obeys.
struct stream_run {
  std::size_t window_bytes = 64 * 1024;
  std::size_t max_per_call = ~std::size_t{0};
  unsigned windows = 0;
  bool early_hand_over = true;
};

// The shipped geometry over a scripted reader: plan_stream() and
// stream_flush_quantum() are the same ones update_stream() uses, so what
// runs here is the path blake3ppsum takes, with the arrivals scripted.
outcome run_stream(std::span<const std::byte> bytes,
                   std::vector<scripted_reader::step> script,
                   const stream_run& how = {}) {
  const std::size_t window_bytes = how.window_bytes;
  const std::size_t max_per_call = how.max_per_call;
  const unsigned windows = how.windows;
  const bool early_hand_over = how.early_hand_over;
  auto sched = blake3pp::get_parallel_scheduler();
  blake3pp::hasher h;
  const detail::pipeline_plan plan =
      detail::plan_stream(h.count(), window_bytes, windows);
  test_driver drv(
      scripted_reader{bytes, std::move(script), max_per_call},
      {.flush_quantum = detail::stream_flush_quantum<budget>(plan.window_bytes),
       .early_hand_over = early_hand_over});
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

// A real pipe and a producer on it, for the cases that read through the
// platform's reader.
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
  // loop's own arithmetic, with no hand-over in the picture. A read cut
  // short by the bound is not a dry stream, which the Windows reader
  // depends on once its ask bound has been learnt.
  const auto bytes = pattern(200 * 1024 + 511, 23);
  const outcome o = run_stream(bytes, all_at_once(bytes.size()),
                               {.max_per_call = 1024});
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.short_flushes == 0);
}

TEST_CASE("the shipped read bound does not fragment a stream") {
  // A 1 MiB bound under an 8 MiB window, a bound the Windows reader's
  // ask can settle on: a producer that keeps up must still fill whole
  // windows.
  const auto bytes = pattern(3 * 1024 * 1024 + 7, 24);
  const outcome o = run_stream(bytes, all_at_once(bytes.size()),
                               {.window_bytes = 8 * 1024 * 1024,
                                .max_per_call = 1024 * 1024});
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
  const outcome o = run_stream(bytes,
                               {{.bytes = 40 * 1024},
                                {.dry = true},
                                {.bytes = bytes.size() - 40 * 1024}},
                               {.early_hand_over = true});
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
                  {.flush_quantum = 16 * 1024, .early_hand_over = true});
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
  const outcome o = run_stream(bytes, std::move(script),
                               {.windows = 4, .early_hand_over = true});
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
  const outcome o = run_stream(bytes,
                               {{.bytes = 20 * 1024},
                                {.dry = true},
                                {.bytes = bytes.size() - 20 * 1024}},
                               {.early_hand_over = true});
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
  const outcome o = run_stream(bytes,
                               {{.bytes = 64 * 1024},
                                {.dry = true},
                                {.bytes = bytes.size() - 64 * 1024}},
                               {.early_hand_over = true});
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
      run_stream(bytes, {{.bytes = bytes.size()}, {.dry = true}},
                 {.early_hand_over = true});
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
  const outcome under = run_stream(bytes,
                                   {{.bytes = 80 * 1024},
                                    {.dry = true},
                                    {.bytes = bytes.size() - 80 * 1024}},
                                   {.window_bytes = 256 * 1024,
                                    .early_hand_over = true});
  CHECK(under.digest == blake3pp::hash(bytes));
  CHECK(under.short_flushes == 0);

  const outcome over = run_stream(bytes,
                                  {{.bytes = 200 * 1024},
                                   {.dry = true},
                                   {.bytes = bytes.size() - 200 * 1024}},
                                  {.window_bytes = 256 * 1024,
                                   .early_hand_over = true});
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
      run_stream(bytes, std::move(script),
                 {.windows = 4, .early_hand_over = true});
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.bytes == len);
  CHECK(o.short_flushes > 0);
  CHECK(part_run_records(o) >= o.short_flushes);
}

// ------------------------------------------------------------------
// Two exits: with the early hand-over off, a window ends when it is full
// or when the stream ends, and a dry stream is waited for. The same
// scripts as above, so the difference is the option and nothing else.

TEST_CASE("without the early hand-over a dry stream is waited for") {
  // The script that hands over twice above. Off, the 40 KiB in hand
  // waits for the rest, every window but the last is the whole 64 KiB,
  // and the stream's 44 KiB remainder is the last window.
  const auto bytes = pattern(300 * 1024, 31);
  const outcome o = run_stream(bytes,
                               {{.bytes = 40 * 1024},
                                {.dry = true},
                                {.bytes = bytes.size() - 40 * 1024}},
                               {.early_hand_over = false});
  CHECK(o.digest == blake3pp::hash(bytes));
  CHECK(o.bytes == bytes.size());
  CHECK(window_lengths(o) ==
        std::vector<std::uint64_t>{64 * 1024, 64 * 1024, 64 * 1024, 64 * 1024,
                                   44 * 1024});
  CHECK(o.short_flushes == 0);
}

TEST_CASE("without the early hand-over a dribbling producer fills the window") {
  // The direct form of the case above it: 40 KiB arrives, the stream is
  // empty, and the window still comes back whole.
  const auto bytes = pattern(300 * 1024, 38);
  std::vector<scripted_reader::step> script;
  for (std::size_t at = 0; at < bytes.size(); at += 40 * 1024) {
    script.push_back({.bytes = std::min<std::size_t>(40 * 1024,
                                                     bytes.size() - at)});
    script.push_back({.dry = true});
  }
  test_driver drv(scripted_reader{bytes, std::move(script)},
                  {.flush_quantum = 16 * 1024, .early_hand_over = false});
  test_driver::file f(drv);
  drv.reserve(4, 64 * 1024);

  const detail::window_storage w0 = drv.window_storage_for(f, 0, 0, 64 * 1024);
  CHECK(w0.view.size() == 64 * 1024);
  CHECK(std::ranges::equal(w0.view, std::span(bytes).first(64 * 1024)));
  CHECK(f.size() == detail::streaming_size);
  CHECK(drv.short_flushes() == 0);
  CHECK(drv.windows_filled() == 1);
}

TEST_CASE("without the early hand-over a stream dry on every window hashes") {
  // The scripts that keep the grid off its boundary when the hand-over is
  // on. Off, the grid never leaves it: no window is a run of parts.
  for (const std::size_t burst : {std::size_t{4096}, std::size_t{37 * 1024}}) {
    CAPTURE(burst);
    const std::size_t len = 1024 * 1024;
    const auto bytes = pattern(len, 32);
    std::vector<scripted_reader::step> script;
    for (std::size_t at = 0; at < len; at += burst) {
      script.push_back({.bytes = std::min(burst, len - at)});
      script.push_back({.dry = true});
    }
    const outcome o = run_stream(bytes, std::move(script),
                                 {.windows = 4, .early_hand_over = false});
    CHECK(o.digest == blake3pp::hash(bytes));
    CHECK(o.bytes == len);
    CHECK(o.short_flushes == 0);
    CHECK(o.windows == len / (64 * 1024));
    CHECK(part_run_records(o) == 0);
  }
}

TEST_CASE("the read bound is asked for before every read") {
  // On Windows the bound is an observation that moves between reads, so
  // the driver must not hold the one it had at the start of the window.
  // The reader answers 32 KiB before its first read and 8 KiB after it.
  const auto bytes = pattern(200 * 1024, 39);
  scripted_reader reader{bytes, all_at_once(bytes.size()), 32 * 1024};
  reader.shrink_bound_to = 8 * 1024;
  test_driver drv(std::move(reader), {.flush_quantum = 16 * 1024});
  test_driver::file f(drv);
  drv.reserve(4, 64 * 1024);

  const detail::window_storage w0 = drv.window_storage_for(f, 0, 0, 64 * 1024);
  CHECK(w0.view.size() == 64 * 1024);
  CHECK(std::ranges::equal(w0.view, std::span(bytes).first(64 * 1024)));
  const std::vector<std::size_t>& asks = drv.reader().asks();
  REQUIRE(asks.size() >= 2);
  CHECK(asks[0] == 32 * 1024);
  CHECK(std::all_of(asks.begin() + 1, asks.end(),
                    [](std::size_t a) { return a <= 8 * 1024; }));
  CHECK(drv.short_flushes() == 0);
}

#if defined(_WIN32)
TEST_CASE("the Windows ask bound follows what the reads deliver") {
  using bound_t = detail::read_ask_bound;
  constexpr std::size_t K = 1024;
  constexpr std::size_t M = 1024 * 1024;
  constexpr unsigned n = bound_t::window_reads;

  SUBCASE("a run starts unbounded and a short read measures the queue") {
    bound_t b;
    CHECK(b.bound() == 0);
    b.note(8 * M, 256 * K);
    CHECK(b.bound() == 256 * K);
  }
  SUBCASE("a producer that fills the whole ask is never bounded") {
    bound_t b;
    for (unsigned i = 0; i < 4 * n; ++i) {
      b.note(8 * M, 8 * M);
    }
    CHECK(b.bound() == 0);
  }
  SUBCASE("a window's remainder capping the ask teaches nothing") {
    bound_t b;
    b.note(8 * M, 1 * M);
    for (unsigned i = 0; i < 4 * n; ++i) {
      b.note(4 * K, 4 * K);  // asked less than the bound, came back whole
    }
    CHECK(b.bound() == 1 * M);
  }
  SUBCASE("growth is probed for after a full window of capped reads") {
    bound_t b;
    b.note(8 * M, 256 * K);
    for (unsigned i = 0; i + 1 < n; ++i) {
      b.note(256 * K, 256 * K);
    }
    CHECK(b.bound() == 256 * K);
    b.note(256 * K, 256 * K);
    CHECK(b.bound() == 512 * K);
    // The probe over-asks once and the short read brings it back.
    b.note(512 * K, 256 * K);
    CHECK(b.bound() == 256 * K);
  }
  SUBCASE("a producer that speeds up is followed by doubling") {
    bound_t b;
    b.note(8 * M, 8 * K);
    unsigned reads = 0;
    while (b.bound() < 1 * M && reads < 64 * n) {
      b.note(b.bound(), b.bound());  // the queue fills every ask
      ++reads;
    }
    CHECK(b.bound() >= 1 * M);
    CHECK(reads == 7 * n);  // 8 KiB to 1 MiB is seven doublings
  }
  SUBCASE("a producer that slows down is followed within the window") {
    bound_t b;
    b.note(8 * M, 1 * M);
    for (unsigned i = 0; i + 1 < n; ++i) {
      b.note(1 * M, 8 * K);
      CHECK(b.bound() == 1 * M);  // still remembers the large read
    }
    // The large read is still one of the last `n` short ones until a
    // full window of small ones has replaced it.
    b.note(1 * M, 8 * K);
    CHECK(b.bound() == 8 * K);
  }
  SUBCASE("an observation rounds up to a power of two") {
    bound_t b;
    b.note(8 * M, 6138);  // what a 4 KiB producer's reads average
    CHECK(b.bound() == 8 * K);
  }
  SUBCASE("a bound a byte short of the producer's write does not collapse") {
    // The one-byte read-ahead shifts the reads off the write boundaries.
    // A rule that learned only from short reads saw a full read of
    // W - 1 and a short read of 1 for every write, and fell to one byte.
    constexpr std::size_t W = 1 * M;
    bound_t b;
    b.note(8 * M, W - 1);
    std::size_t pending = 1;  // the write's byte the read-ahead left
    for (int i = 0; i < 400; ++i) {
      const std::size_t ask = b.bound();
      REQUIRE(ask != 0);
      const std::size_t got = std::min(ask, pending);
      pending -= got;
      if (pending == 0) {
        pending = W;  // the producer's next write
      }
      b.note(ask, got);
      CHECK(b.bound() >= W);
    }
  }
  SUBCASE("one short read at a window boundary does not collapse it") {
    bound_t b;
    for (unsigned i = 0; i < n; ++i) {
      b.note(2 * M, 1 * M);
    }
    b.note(1 * M, 3 * K);  // the remainder's short read
    CHECK(b.bound() == 1 * M);
  }
}
#endif

// ------------------------------------------------------------------
// The real reader, end to end.

TEST_CASE("update_stream over a real pipe matches the sequential hasher") {
#if defined(__EMSCRIPTEN__)
  // Emscripten's pipe is an in-memory emulation that answers EAGAIN to a
  // read of an empty pipe, even once the writer has closed (see the
  // vanished-writer case), so a case that reads a live producer to its
  // end cannot finish there.
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
      // The report's account of the run, from the counters: every
      // window was filled or ended by the stream's end unless the
      // driver may hand over early, and what the reader reports about
      // the watch and the pipe is the platform's.
      CHECK(r.early_hand_over == blake3pp::detail::stream_hands_over_early());
      if (!r.early_hand_over) {
        CHECK(r.short_flushes == 0);
      }
      CHECK(r.short_flushes <= r.windows);
      CHECK(r.watch.caught + r.watch.slept <= r.watch.probes);
      if (!r.watch.measured) {
        CHECK(r.watch.probes == 0);
      }
#if defined(__linux__)
      CHECK(r.stream_buffer_reported);
      CHECK(r.stream_buffer_bytes >= 64 * 1024);
      CHECK(r.watch.measured);
#elif defined(__APPLE__)
      CHECK_FALSE(r.stream_buffer_reported);
      CHECK_FALSE(r.watch.measured);
#elif defined(_WIN32)
      CHECK_FALSE(r.stream_buffer_reported);
      CHECK_FALSE(r.watch.measured);
      // The ask bound is learnt from reads that come back short, so a
      // stream this small may end before one does; when it has been
      // learnt it is a power of two, as read_ask_bound promises.
      if (r.read_ask_bound_bytes != 0) {
        CHECK(std::has_single_bit(r.read_ask_bound_bytes));
      }
#endif
    }
  }
}

// ------------------------------------------------------------------
// Faults the reader can see. Each must end either as the digest of
// exactly the bytes a genuine end of stream delivered, or with no digest
// and the error out of run_window_pipeline(). Nothing asserts on time.

TEST_CASE("end of stream exactly on a window boundary, one byte past it, and inside the read-ahead") {
  // Three lengths around the boundary the one-byte read-ahead exists
  // for, delivered by a producer that runs dry between every write so
  // the early hand-over and the read-ahead both engage.
  constexpr std::size_t w = 64 * 1024;
  for (const std::size_t len : {2 * w, 2 * w + 1, 2 * w - 1, 3 * w, 3 * w + 1}) {
    for (const std::size_t piece : {std::size_t{4096}, w / 2, w}) {
      CAPTURE(len);
      CAPTURE(piece);
      const auto bytes = pattern(len, 61);
      std::vector<scripted_reader::step> script;
      for (std::size_t at = 0; at < len; at += piece) {
        script.push_back({.bytes = std::min(piece, len - at)});
        script.push_back({.dry = true});
      }
      const outcome o = run_stream(bytes, script);
      CHECK(o.digest == blake3pp::hash(bytes));
      CHECK(o.bytes == len);
    }
  }
}

TEST_CASE("a read that moves nothing without ending the stream is asked again") {
  // What a zero-length producer write looks like through the seam:
  // the fill loop neither ends the window nor counts it as the end of
  // the stream, and the digest is of every byte that followed.
  constexpr std::size_t w = 64 * 1024;
  const auto bytes = pattern(3 * w + 777, 62);
  std::vector<scripted_reader::step> script = {
      {.empty_read = true},
      {.bytes = w / 2},
      {.empty_read = true},
      {.empty_read = true},
      {.bytes = w},
      {.dry = true},
      {.empty_read = true},
      {.bytes = bytes.size() - w - w / 2},
      {.empty_read = true},
  };
  blake3pp::hasher h;
  const detail::pipeline_plan plan = detail::plan_stream(h.count(), w, 0);
  test_driver drv(scripted_reader{bytes, script},
                  {.flush_quantum = detail::stream_flush_quantum<budget>(plan.window_bytes)});
  test_driver::file f(drv);
  detail::run_window_pipeline<budget>(h, drv, f, blake3pp::get_parallel_scheduler(),
                                      plan, {.window_bytes = plan.window_bytes});
  CHECK(drv.reader().empty_reads() == 5);
  CHECK(drv.bytes_read() == bytes.size());
  CHECK(h.finalize() == blake3pp::hash(bytes));
}

TEST_CASE("a read that fails mid-window propagates and leaves no digest") {
  // The reader throws from inside window_storage_for() on the loop
  // thread. run_window_pipeline() must rethrow that error, the hasher
  // must still be where it was (nothing absorbed from the broken
  // window), and the driver must owe nothing: no read outstanding, so
  // nothing can complete into freed storage afterwards.
  constexpr std::size_t w = 64 * 1024;
  const auto bytes = pattern(5 * w, 63);
  for (const std::size_t good : {std::size_t{0}, w / 2, 2 * w, 2 * w + 4096}) {
    CAPTURE(good);
    std::vector<scripted_reader::step> script;
    if (good > 0) {
      script.push_back({.bytes = good});
      script.push_back({.dry = true});
    }
    script.push_back({.fail = true});
    script.push_back({.bytes = bytes.size() - good});
    blake3pp::hasher h;
    const std::uint64_t count_before = h.count();
    const detail::pipeline_plan plan = detail::plan_stream(h.count(), w, 0);
    test_driver drv(scripted_reader{bytes, script},
                    {.flush_quantum = detail::stream_flush_quantum<budget>(plan.window_bytes)});
    test_driver::file f(drv);
    bool threw = false;
    try {
      detail::run_window_pipeline<budget>(h, drv, f, blake3pp::get_parallel_scheduler(),
                                          plan, {.window_bytes = plan.window_bytes});
    } catch (const std::system_error& e) {
      threw = true;
      CHECK(e.code() == std::errc::io_error);
    }
    CHECK(threw);
    CHECK(drv.reader().failures() == 1);
    CHECK(drv.in_flight() == 0);
    // Windows that were handed over before the failure were hashed;
    // the failing window was not. Either way the result is not the
    // digest of the stream and the caller has an exception, not a
    // digest, to act on.
    CHECK(h.count() >= count_before);
    CHECK(h.count() <= good);
    CHECK(drv.bytes_read() <= good);
  }
}

TEST_CASE("a failure on the very first read leaves the hasher untouched") {
  blake3pp::hasher h;
  const auto before = h.finalize();
  const detail::pipeline_plan plan = detail::plan_stream(h.count(), 64 * 1024, 0);
  test_driver drv(scripted_reader{{}, {{.fail = true}}},
                  {.flush_quantum = detail::stream_flush_quantum<budget>(plan.window_bytes)});
  test_driver::file f(drv);
  CHECK_THROWS_AS(detail::run_window_pipeline<budget>(
                      h, drv, f, blake3pp::get_parallel_scheduler(), plan,
                      {.window_bytes = plan.window_bytes}),
                  std::system_error);
  CHECK(h.count() == 0);
  CHECK(h.finalize() == before);
  CHECK(drv.in_flight() == 0);
}

#if !defined(_WIN32)
namespace {

// Deliveries seen by the handler, so a test can show its signals landed
// rather than being sent into the void.
std::atomic<unsigned> signals_delivered{0};

void count_signal(int) { signals_delivered.fetch_add(1, std::memory_order_relaxed); }

// Installs a handler for SIGUSR1 without SA_RESTART, so a blocking
// read() or poll() in the reader returns EINTR when the signal lands,
// and restores the previous disposition when destroyed.
struct interrupting_signal {
  struct sigaction previous{};
  interrupting_signal() {
    struct sigaction sa{};
    sa.sa_handler = count_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, &previous);
  }
  ~interrupting_signal() { sigaction(SIGUSR1, &previous, nullptr); }
};

}  // namespace

TEST_CASE("EINTR during a blocking read or watch is retried without losing bytes") {
#if defined(__EMSCRIPTEN__)
  // Emscripten's pipe is an in-memory emulation that answers EAGAIN to a
  // read of an empty pipe, even once the writer has closed (see the
  // vanished-writer case), so a case that reads a live producer to its
  // end cannot finish there.
  MESSAGE("skipped: an Emscripten pipe never reports the end of the stream");
  return;
#endif
  // The producer writes in small pieces with the reader asleep or
  // watching between them, and a second thread sends SIGUSR1 to the
  // reading thread throughout. Every interrupted call must be retried:
  // a reader that returned the interruption as a short or zero read
  // would end the stream early with the digest of a prefix.
  //
  // Outside Darwin the reader waits in poll() before every read(): the
  // watch's zero-timeout probes, then a blocking one if nothing came. The
  // interruptions land in poll(), and the read()-level EINTR branch in
  // read_some() is reached only when a signal arrives between the two
  // calls. The interrupted read() and EAGAIN branches are Darwin
  // coverage, since the watch is compiled out there and nothing waits in
  // front of read(). Counting the read()-level EINTRs needs readshim,
  // which this binary does not load; cli_fault_eintr_storm counts them,
  // and this test counts the deliveries.
  const interrupting_signal guard;
  const unsigned delivered_before = signals_delivered.load();
  const auto bytes = pattern(6 * 64 * 1024 + 1234, 64);
  os_pipe p;
  REQUIRE(p.open());
  const pthread_t reader_thread = pthread_self();
  std::atomic<bool> done{false};
  std::atomic<unsigned> signals{0};
  std::thread producer([&] {
    std::size_t at = 0;
    while (at < bytes.size()) {
      const std::size_t want = std::min<std::size_t>(4096, bytes.size() - at);
      const auto n = ::write(p.w, bytes.data() + at, want);
      REQUIRE(n > 0);
      at += static_cast<std::size_t>(n);
      pthread_kill(reader_thread, SIGUSR1);
      ++signals;
    }
    p.close_write();
    // Paced, so the reader still reaches the end: an unpaced storm keeps
    // a FreeBSD reader re-entering the handler and restarting poll() for
    // minutes, under KVM as well as under emulation.
    while (!done.load()) {
      pthread_kill(reader_thread, SIGUSR1);
      ++signals;
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  });
  blake3pp::hasher h;
  const blake3pp::stream_report r =
      blake3pp::update_stream(h, p.r, blake3pp::get_parallel_scheduler());
  done.store(true);
  producer.join();
  p.close_read();
  CHECK(signals.load() > 0);
  CHECK(signals_delivered.load() - delivered_before > 0);
  CHECK(r.bytes == bytes.size());
  CHECK(h.finalize() == blake3pp::hash(bytes));
}

TEST_CASE("a descriptor handed over non-blocking is read to the end") {
#if defined(__EMSCRIPTEN__)
  // Emscripten's pipe is an in-memory emulation that answers EAGAIN to a
  // read of an empty pipe, even once the writer has closed (see the
  // vanished-writer case), so a case that reads a live producer to its
  // end cannot finish there.
  MESSAGE("skipped: an Emscripten pipe never reports the end of the stream");
  return;
#endif
  // O_NONBLOCK on the read end: an empty pipe answers EAGAIN rather
  // than blocking, and the reader must wait for readiness and retry,
  // with the watch's probes in front of it, until the producer closes.
  //
  // Outside Darwin the watch waits for readiness before each read(), so
  // the read()-level EAGAIN branch in read_some() is reached only when
  // another reader empties the pipe between the two calls, which no test
  // here makes happen deterministically. That branch is Darwin coverage.
  const auto bytes = pattern(4 * 64 * 1024 + 99, 65);
  os_pipe p;
  REQUIRE(p.open());
  REQUIRE(::fcntl(p.r, F_SETFL, ::fcntl(p.r, F_GETFL) | O_NONBLOCK) == 0);
  std::thread producer([&] {
    std::size_t at = 0;
    while (at < bytes.size()) {
      const std::size_t want = std::min<std::size_t>(3000, bytes.size() - at);
      const auto n = ::write(p.w, bytes.data() + at, want);
      REQUIRE(n > 0);
      at += static_cast<std::size_t>(n);
      std::this_thread::yield();
    }
    p.close_write();
  });
  blake3pp::hasher h;
  const blake3pp::stream_report r =
      blake3pp::update_stream(h, p.r, blake3pp::get_parallel_scheduler());
  producer.join();
  CHECK((::fcntl(p.r, F_GETFL) & O_NONBLOCK) != 0);
  p.close_read();
  CHECK(r.bytes == bytes.size());
  CHECK(h.finalize() == blake3pp::hash(bytes));
}

TEST_CASE("a closed descriptor is an error, not an empty stream") {
  // EBADF must throw. Reading it as the end of the stream would hand
  // back the empty digest for a stream that was never read.
  os_pipe p;
  REQUIRE(p.open());
  const int fd = p.r;
  p.close_read();
  blake3pp::hasher h;
  CHECK_THROWS_AS(blake3pp::update_stream(h, fd, blake3pp::get_parallel_scheduler()),
                  std::system_error);
  CHECK(h.count() == 0);
}

TEST_CASE("a pipe whose writer vanished mid-stream ends with the bytes it sent, not an error") {
#if defined(__EMSCRIPTEN__)
  // Emscripten's in-memory pipe answers EAGAIN to a read of an empty pipe
  // even after the write end has closed, so the end of the stream this
  // case waits for never arrives there.
  MESSAGE("skipped: an Emscripten pipe never reports the end of the stream");
  return;
#endif
  // The one case a reader cannot distinguish from a clean end: the
  // producer closes (or dies) after writing part of the stream. The read
  // of the empty pipe reports EOF, and the digest is of the bytes that
  // arrived. Where the watch runs, it sleeps at most once per window plus
  // two.
  const auto bytes = pattern(64 * 1024 + 4096 * 3, 66);
  os_pipe p;
  REQUIRE(p.open());
  std::thread producer([&] { p.produce(bytes, 4096); });
  blake3pp::hasher h;
  const blake3pp::stream_report r =
      blake3pp::update_stream(h, p.r, blake3pp::get_parallel_scheduler());
  producer.join();
  p.close_read();
  CHECK(r.bytes == bytes.size());
  CHECK(h.finalize() == blake3pp::hash(bytes));
  if (r.watch.measured) {
    CHECK(r.watch.slept <= r.windows + 2);
  }
}
#endif

#if defined(_WIN32)
TEST_CASE("a pipe whose read end is in PIPE_NOWAIT mode is read to the end") {
  // A parent may hand over stdin with PIPE_NOWAIT set, in which case an
  // empty pipe answers ERROR_NO_DATA rather than blocking. The reader
  // must wait and retry, never fail and never read it as the end, and
  // must leave the mode as it found it: the handle is shared.
  const auto bytes = pattern(3 * 64 * 1024 + 4567, 67);
  os_pipe p;
  REQUIRE(p.open());
  DWORD mode = PIPE_NOWAIT;
  REQUIRE(SetNamedPipeHandleState(p.r, &mode, nullptr, nullptr) != 0);
  std::thread producer([&p, &bytes] {
    std::size_t at = 0;
    while (at < bytes.size()) {
      const std::size_t want = std::min<std::size_t>(4096, bytes.size() - at);
      DWORD moved = 0;
      REQUIRE(WriteFile(p.w, bytes.data() + at, static_cast<DWORD>(want), &moved,
                        nullptr) != 0);
      at += moved;
      std::this_thread::yield();
    }
    p.close_write();
  });
  blake3pp::hasher h;
  const blake3pp::stream_report r =
      blake3pp::update_stream(h, p.r, blake3pp::get_parallel_scheduler());
  producer.join();
  DWORD state = 0;
  REQUIRE(GetNamedPipeHandleStateA(p.r, &state, nullptr, nullptr, nullptr, nullptr, 0) != 0);
  CHECK((state & PIPE_NOWAIT) != 0);
  p.close_read();
  CHECK(r.bytes == bytes.size());
  CHECK(h.finalize() == blake3pp::hash(bytes));
}
#endif

#if defined(_WIN32)
// Runs `body` on a thread and ends the process if it has not returned
// within `limit`: the failures these cases guard are a read that never
// ends, which no CHECK can report.
template <class Body>
void within(std::chrono::seconds limit, const char* what, Body body) {
  std::atomic<bool> done{false};
  std::thread t([&] {
    body();
    done.store(true);
  });
  const auto until = std::chrono::steady_clock::now() + limit;
  while (!done.load() && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!done.load()) {
    std::fprintf(stderr, "%s: still reading after %llds\n", what,
                 static_cast<long long>(limit.count()));
    std::abort();
  }
  t.join();
}

// GetFileType calls a socket a pipe, but a non-overlapped socket whose
// peer closed answers every read with success and nothing moved, and
// never with an error. Read as a pipe's zero-length message, that is a
// read retried for ever on a core; it is the end of the stream.
TEST_CASE("a socket on the read end ends where its peer closed") {
  WSADATA wsa{};
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
  // Without WSA_FLAG_OVERLAPPED, as a socket handed over as stdin is; an
  // accepted socket takes the listener's attributes.
  const SOCKET listener =
      WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
  REQUIRE(listener != INVALID_SOCKET);
  sockaddr_in at{};
  at.sin_family = AF_INET;
  at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(bind(listener, reinterpret_cast<sockaddr*>(&at), sizeof at) == 0);
  int at_len = sizeof at;
  REQUIRE(getsockname(listener, reinterpret_cast<sockaddr*>(&at), &at_len) ==
          0);
  REQUIRE(listen(listener, 1) == 0);
  const SOCKET writer =
      WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
  REQUIRE(writer != INVALID_SOCKET);
  REQUIRE(connect(writer, reinterpret_cast<sockaddr*>(&at), sizeof at) == 0);
  const SOCKET reader = accept(listener, nullptr, nullptr);
  REQUIRE(reader != INVALID_SOCKET);
  closesocket(listener);

  const auto bytes = pattern(3 * 64 * 1024 + 321, 71);
  std::thread producer([&] {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
      const int n = send(
          writer, reinterpret_cast<const char*>(bytes.data() + sent),
          static_cast<int>(std::min<std::size_t>(8192, bytes.size() - sent)),
          0);
      if (n <= 0) {
        break;
      }
      sent += static_cast<std::size_t>(n);
    }
    shutdown(writer, SD_SEND);
    closesocket(writer);
  });
  blake3pp::hasher h;
  blake3pp::stream_report r{};
  within(std::chrono::seconds(30), "socket stream", [&] {
    r = blake3pp::update_stream(h, reinterpret_cast<HANDLE>(reader),
                                blake3pp::get_parallel_scheduler());
  });
  producer.join();
  closesocket(reader);
  WSACleanup();
  CHECK(r.bytes == bytes.size());
  CHECK(h.finalize() == blake3pp::hash(bytes));
}
#endif

#if defined(_WIN32)
// A message-mode named pipe, the read end in message read mode, and the
// write end connected to it. The anonymous pipes everywhere else in this
// file are byte mode, which never shows either case below.
struct message_pipe {
  HANDLE r = INVALID_HANDLE_VALUE;
  HANDLE w = INVALID_HANDLE_VALUE;

  bool open() {
    static std::atomic<unsigned> serial{0};
    char name[96];
    std::snprintf(name, sizeof name, "\\\\.\\pipe\\blake3pp-test-%lu-%u",
                  GetCurrentProcessId(), serial.fetch_add(1));
    r = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND,
                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                         1, 0, 1 << 20, 0, nullptr);
    if (r == INVALID_HANDLE_VALUE) {
      return false;
    }
    w = CreateFileA(name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                    nullptr);
    if (w == INVALID_HANDLE_VALUE) {
      return false;
    }
    return ConnectNamedPipe(r, nullptr) != 0 ||
           GetLastError() == ERROR_PIPE_CONNECTED;
  }
  // One WriteFile per message; a zero length writes an empty message.
  void send(std::span<const std::byte> m) const {
    DWORD moved = 0;
    REQUIRE(WriteFile(w, m.data(), static_cast<DWORD>(m.size()), &moved,
                      nullptr) != 0);
    REQUIRE(moved == m.size());
  }
  void close_write() {
    CloseHandle(w);
    w = INVALID_HANDLE_VALUE;
  }
  ~message_pipe() {
    if (w != INVALID_HANDLE_VALUE) {
      CloseHandle(w);
    }
    if (r != INVALID_HANDLE_VALUE) {
      CloseHandle(r);
    }
  }
};

// A zero-length message mid-stream is a read that succeeds and moves
// nothing, which on a pipe is not the end: reading it as the end hashes
// the prefix and reports success.
TEST_CASE("a zero-length message on a message-mode pipe is not the end") {
  const auto bytes = pattern(6 * 4096 + 1000, 73);
  message_pipe p;
  REQUIRE(p.open());
  std::thread producer([&] {
    const std::span<const std::byte> all{bytes};
    p.send(all.first(4096));
    p.send(all.subspan(4096, 4096));
    p.send({});
    p.send(all.subspan(8192));
    p.close_write();
  });
  blake3pp::hasher h;
  blake3pp::stream_report r{};
  within(std::chrono::seconds(30), "message pipe", [&] {
    r = blake3pp::update_stream(h, p.r, blake3pp::get_parallel_scheduler());
  });
  producer.join();
  CHECK(r.bytes == bytes.size());
  CHECK(h.finalize() == blake3pp::hash(bytes));
}

// The ask bound learns the size of the messages so far, so a later
// message longer than that arrives as ERROR_MORE_DATA with part of it
// moved. That is a read that delivered bytes; the rest is the next read.
TEST_CASE("a message longer than the ask is read across two reads") {
  const auto bytes = pattern(32 * 4096 + 5123 + 777, 79);
  message_pipe p;
  REQUIRE(p.open());
  std::thread producer([&] {
    const std::span<const std::byte> all{bytes};
    std::size_t at = 0;
    for (int i = 0; i < 32; ++i, at += 4096) {
      p.send(all.subspan(at, 4096));
      // One message at a time, so each read comes back with one.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    p.send(all.subspan(at, 5123));
    p.send(all.subspan(at + 5123));
    p.close_write();
  });
  blake3pp::hasher h;
  blake3pp::stream_report r{};
  within(std::chrono::seconds(30), "message pipe", [&] {
    r = blake3pp::update_stream(h, p.r, blake3pp::get_parallel_scheduler());
  });
  producer.join();
  CHECK(r.bytes == bytes.size());
  CHECK(h.finalize() == blake3pp::hash(bytes));
}
#endif

TEST_CASE("a reader without a watch or an ask bound reports neither") {
  // The scripted reader has no watch() and its read_bound() is a
  // constant: the report takes what a reader has and says nothing about
  // the rest, which is what a platform without the facility gets.
  const auto bytes = pattern(3 * 64 * 1024, 17);
  test_driver drv(scripted_reader{bytes, all_at_once(bytes.size())},
                  {.flush_quantum = 16 * 1024});
  const auto w = detail::watch_report_of(drv.reader());
  CHECK_FALSE(w.measured);
  CHECK(w.probes == 0);
  CHECK(detail::read_ask_bound_of(drv.reader()) == drv.reader().read_bound());
  CHECK(drv.hands_over_early());
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
  // Emscripten's pipe is an in-memory emulation that answers EAGAIN to a
  // read of an empty pipe, even once the writer has closed (see the
  // vanished-writer case), so a case that reads a live producer to its
  // end cannot finish there.
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

TEST_CASE("the early hand-over default is per platform") {
  // Pinned to the literal: the driver and the report both read this
  // default, so a change to it must change this expectation.
#if defined(_WIN32)
  CHECK_FALSE(blake3pp::detail::stream_hands_over_early());
#else
  CHECK(blake3pp::detail::stream_hands_over_early());
#endif
}

}  // TEST_SUITE
