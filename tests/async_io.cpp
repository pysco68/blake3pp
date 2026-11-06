// The experimental async API: a context the caller drives and two
// senders over it.
//
// What these check that the blocking suites cannot: that several files
// share one arena and one thread, that a receiver's stop reaches the
// pipeline through the scope's stop seam, and that the four completion
// signals are the only way an operation ever ends.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <blake3pp/async_io.hpp>
#include <blake3pp/core.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/parallel_backend.hpp>
#include <doctest/doctest.h>

#include "temp_names.hpp"

namespace {
namespace fs = std::filesystem;
namespace async = blake3pp::async;
namespace ex = blake3pp::ex;

std::vector<std::byte> pattern(std::size_t n, std::uint32_t seed) {
  std::vector<std::byte> out(n);
  std::mt19937 rng(seed);
  for (std::byte& b : out) {
    b = static_cast<std::byte>(rng() & 0xffu);
  }
  return out;
}

// A file that deletes itself, named per process and per case so a
// parallel ctest run never collides.
class temp_file {
 public:
  explicit temp_file(std::span<const std::byte> content) {
    // A name of its own, with nothing POSIX-only in it: this suite runs
    // on every platform the library builds for.
    static std::atomic<unsigned> counter{0};
    path_ = fs::temp_directory_path() /
            ("blake3pp_async_" + b3test::process_salt() + "_" +
             std::to_string(counter.fetch_add(1)));
    std::ofstream out(path_, std::ios::binary);
    out.write(reinterpret_cast<const char*>(content.data()),
              static_cast<std::streamsize>(content.size()));
  }
  ~temp_file() {
    std::error_code ec;
    fs::remove(path_, ec);
  }
  temp_file(const temp_file&) = delete;
  temp_file& operator=(const temp_file&) = delete;

  [[nodiscard]] const fs::path& path() const noexcept { return path_; }

 private:
  fs::path path_;
};

// What a completion was. One operation must produce exactly one of
// these, which is the whole point of counting them.
struct record {
  unsigned values = 0;
  unsigned errors = 0;
  unsigned stops = 0;
  blake3pp::digest value{};
  std::error_code ec{};

  [[nodiscard]] unsigned total() const noexcept {
    return values + errors + stops;
  }
};

// A move-only receiver, which is what P2300 says a receiver may be.
// beman's continues_on needs a copyable one in places, so the second
// receiver below is the copyable shape; both have to work.
struct move_only_receiver {
  using receiver_concept = blake3pp::detail::ex_compat::receiver_tag;

  explicit move_only_receiver(record* r) noexcept : rec(r) {}
  move_only_receiver(move_only_receiver&&) = default;
  move_only_receiver& operator=(move_only_receiver&&) = default;
  move_only_receiver(const move_only_receiver&) = delete;
  move_only_receiver& operator=(const move_only_receiver&) = delete;

  record* rec;

  void set_value() && noexcept { ++rec->values; }
  void set_value(blake3pp::digest d) && noexcept {
    rec->value = d;
    ++rec->values;
  }
  void set_error(std::error_code ec) && noexcept {
    rec->ec = ec;
    ++rec->errors;
  }
  void set_error(std::exception_ptr) && noexcept { ++rec->errors; }
  void set_stopped() && noexcept { ++rec->stops; }
};

// The same, copyable, and carrying a stop token so the receiver's own
// stop can be exercised.
struct handle_receiver {
  using receiver_concept = blake3pp::detail::ex_compat::receiver_tag;

  struct env {
    ex::inplace_stop_token token;
    [[nodiscard]] ex::inplace_stop_token query(
        ex::get_stop_token_t) const noexcept {
      return token;
    }
  };

  record* rec;
  ex::inplace_stop_token token;

  [[nodiscard]] env get_env() const noexcept { return {token}; }

  void set_value() const noexcept { ++rec->values; }
  void set_value(blake3pp::digest d) const noexcept {
    rec->value = d;
    ++rec->values;
  }
  void set_error(std::error_code ec) const noexcept {
    rec->ec = ec;
    ++rec->errors;
  }
  void set_error(std::exception_ptr) const noexcept { ++rec->errors; }
  void set_stopped() const noexcept { ++rec->stops; }
};

// A small context: two windows of the smallest legal size, so the
// multi-file cases really do run out of windows.
async::io_context_options tight() {
  return {.window_bytes = 64 * 1024,
          .queue_depth = 2,
          .windows = 2,
          .direct_io = false};
}

}  // namespace

TEST_SUITE("async_io") {

TEST_CASE("a file hashes to the same digest through the async API") {
  const auto content = pattern(3 * 64 * 1024 + 501, 11);
  const temp_file f(content);
  const blake3pp::digest want = blake3pp::hash(content);

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  record rec;
  auto op = ex::connect(async::async_hash_file(ctx, f.path(), sched),
                        move_only_receiver{&rec});
  // Nothing completes inside start(): the context is what reports.
  ex::start(op);
  CHECK(rec.total() == 0);
  ctx.run_until_idle();
  CHECK(rec.total() == 1);
  CHECK(rec.values == 1);
  CHECK(rec.value == want);
}

// Tracing over the senders is a type, not an option: the operation state
// is fixed at connect, so the tracer goes in as an argument and the
// default records nothing. With a buffer, every window of the file gets a
// record, the pool windows carry their agents' stamp, and the batch form
// fills one buffer with every file's windows.
TEST_CASE("the senders record into a buffer handed in as a tracer") {
  constexpr std::size_t win = 64 * 1024;
  const auto content = pattern(5 * win + 4097, 14);
  const temp_file f(content);
  const blake3pp::digest want = blake3pp::hash(content);
  const std::size_t windows_per_file = (content.size() + win - 1) / win;

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  std::vector<blake3pp::window_record> windows(3 * windows_per_file);
  std::vector<blake3pp::agent_record> agents(windows.size() * 64);
  blake3pp::trace_buffer trace(windows, agents);

  SUBCASE("one file, hashed") {
    record rec;
    auto op = ex::connect(async::async_hash_file(ctx, f.path(), sched, {},
                                                 blake3pp::buffer_tracer{&trace}),
                          move_only_receiver{&rec});
    ex::start(op);
    ctx.run_until_idle();
    REQUIRE(rec.values == 1);
    CHECK(rec.value == want);
    REQUIRE(trace.windows().size() == windows_per_file);
    bool parallel = false;
    for (const blake3pp::window_record& w : trace.windows()) {
      CHECK(w.t_ready >= w.t_wait_begin);
      CHECK(w.t_joined >= w.t_ready);
      parallel = parallel || (w.flags & blake3pp::window_record::flag_parallel) != 0;
    }
    CHECK(parallel);
    CHECK(trace.agents().size() > 0);
  }

  SUBCASE("one file, streamed into a hasher") {
    blake3pp::hasher h;
    record rec;
    auto op = ex::connect(async::async_update_file(ctx, h, f.path(), sched, {},
                                                   blake3pp::buffer_tracer{&trace}),
                          move_only_receiver{&rec});
    ex::start(op);
    ctx.run_until_idle();
    REQUIRE(rec.values == 1);
    CHECK(h.finalize() == want);
    CHECK(trace.windows().size() == windows_per_file);
  }

  SUBCASE("three files, one buffer") {
    const std::array<std::filesystem::path, 3> paths{f.path(), f.path(),
                                                     f.path()};
    std::size_t ended = 0;
    record rec;
    auto op = ex::connect(
        async::async_update_files(
            ctx, paths, sched, {.in_flight = 2},
            [](std::size_t) { return blake3pp::hasher{}; },
            [&](std::size_t, blake3pp::hasher& h,
                const async::file_outcome& o) noexcept {
              if (o.ok() && h.finalize() == want) {
                ++ended;
              }
            },
            blake3pp::buffer_tracer{&trace}),
        move_only_receiver{&rec});
    ex::start(op);
    ctx.run_until_idle();
    CHECK(ended == 3);
    CHECK(trace.windows().size() == 3 * windows_per_file);
  }

  SUBCASE("the default records nothing") {
    record rec;
    auto op = ex::connect(async::async_hash_file(ctx, f.path(), sched),
                          move_only_receiver{&rec});
    ex::start(op);
    ctx.run_until_idle();
    CHECK(rec.value == want);
    CHECK(trace.windows().empty());
  }
}

TEST_CASE("async_update_file streams into the caller's hasher") {
  const auto head = pattern(1000, 12);
  const auto content = pattern(2 * 64 * 1024, 13);
  const temp_file f(content);

  blake3pp::hasher want;
  want.update(head);
  want.update(content);

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  blake3pp::hasher h;
  h.update(head);
  record rec;
  auto op = ex::connect(async::async_update_file(ctx, h, f.path(), sched),
                        move_only_receiver{&rec});
  ex::start(op);
  ctx.run_until_idle();
  CHECK(rec.total() == 1);
  CHECK(rec.values == 1);
  CHECK(h.finalize() == want.finalize());
}

TEST_CASE("the copyable handle receiver works too") {
  const auto content = pattern(64 * 1024 + 7, 14);
  const temp_file f(content);

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  record rec;
  ex::inplace_stop_source src;
  auto op = ex::connect(async::async_hash_file(ctx, f.path(), sched),
                        handle_receiver{&rec, src.get_token()});
  ex::start(op);
  ctx.run_until_idle();
  CHECK(rec.values == 1);
  CHECK(rec.value == blake3pp::hash(content));
}

TEST_CASE("the sender reports the context as its completion scheduler") {
  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();
  const temp_file f(pattern(64 * 1024, 15));

  auto sndr = async::async_hash_file(ctx, f.path(), sched);
  const auto env = ex::get_env(sndr);
  // Every channel completes on the context, not just the value one: an
  // adaptor that wants to stay on this thread after an error has to be
  // able to ask.
  CHECK(ex::get_completion_scheduler<ex::set_value_t>(env) == ctx.scheduler());
  CHECK(ex::get_completion_scheduler<ex::set_error_t>(env) == ctx.scheduler());
  CHECK(ex::get_completion_scheduler<ex::set_stopped_t>(env) ==
        ctx.scheduler());
}

TEST_CASE("then and let_value compose over the sender") {
  const auto content = pattern(2 * 64 * 1024 + 99, 16);
  const temp_file f(content);
  const blake3pp::digest want = blake3pp::hash(content);

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  bool seen = false;
  blake3pp::digest got{};
  record rec;
  auto work = ex::let_value(
      async::async_hash_file(ctx, f.path(), sched),
      [&](blake3pp::digest d) noexcept {
        got = d;
        seen = true;
        return ex::just(d);
      });
  auto op = ex::connect(std::move(work), move_only_receiver{&rec});
  ex::start(op);
  ctx.run_until_idle();
  CHECK(seen);
  CHECK(got == want);
  CHECK(rec.values == 1);
}

TEST_CASE("when_all over two files completes once with both digests") {
  const auto a = pattern(2 * 64 * 1024 + 11, 30);
  const auto b = pattern(3 * 64 * 1024 + 22, 31);
  const temp_file fa(a);
  const temp_file fb(b);

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  blake3pp::digest ga{};
  blake3pp::digest gb{};
  record rec;
  auto work = ex::then(
      ex::when_all(async::async_hash_file(ctx, fa.path(), sched),
                   async::async_hash_file(ctx, fb.path(), sched)),
      [&](blake3pp::digest da, blake3pp::digest db) noexcept {
        ga = da;
        gb = db;
      });
  auto op = ex::connect(std::move(work), move_only_receiver{&rec});
  ex::start(op);
  ctx.run_until_idle();
  CHECK(rec.total() == 1);
  CHECK(rec.values == 1);
  CHECK(ga == blake3pp::hash(a));
  CHECK(gb == blake3pp::hash(b));
}

// The context's thread and the thread starting operations are usually
// different ones: a thread runs the context, producers start files on
// it. start() must therefore be safe against a loop that is walking its
// list at that moment, and an operation started while the loop is
// parked must wake it. Under TSan this is the case that sees a torn
// list; without it, it sees a hang.
TEST_CASE("operations started from another thread join a running context") {
  const auto a = pattern(2 * 64 * 1024 + 11, 40);
  const auto b = pattern(5 * 64 * 1024 + 3, 41);
  const temp_file fa(a);
  const temp_file fb(b);

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();
  std::thread runner([&] { ctx.run(); });

  // Both start() calls run here, while the runner drives the context.
  auto first = ex::sync_wait(
      ex::when_all(async::async_hash_file(ctx, fa.path(), sched),
                   async::async_hash_file(ctx, fb.path(), sched)));
  REQUIRE(first.has_value());
  const auto [da, db] = *first;
  CHECK(da == blake3pp::hash(a));
  CHECK(db == blake3pp::hash(b));

  // And once more with the loop idle and parked in the driver: the
  // start has to wake it.
  auto second = ex::sync_wait(async::async_hash_file(ctx, fb.path(), sched));
  REQUIRE(second.has_value());
  CHECK(std::get<0>(*second) == blake3pp::hash(b));

  ctx.request_stop();
  runner.join();
}

// sync_wait() drives the context on the calling thread until the one
// operation it was given completes: no runner thread, no operation
// storage, the values back as a tuple. A composed sender works too.
TEST_CASE("sync_wait drives the context until its sender completes") {
  const auto a = pattern(2 * 64 * 1024 + 11, 50);
  const auto b = pattern(3 * 64 * 1024 + 5, 51);
  const temp_file fa(a);
  const temp_file fb(b);
  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  auto one = async::sync_wait(ctx, async::async_hash_file(ctx, fa.path(), sched));
  REQUIRE(one.has_value());
  CHECK(std::get<0>(*one) == blake3pp::hash(a));

  auto both = async::sync_wait(
      ctx, ex::when_all(async::async_hash_file(ctx, fa.path(), sched),
                        async::async_hash_file(ctx, fb.path(), sched)));
  REQUIRE(both.has_value());
  const auto [da, db] = *both;
  CHECK(da == blake3pp::hash(a));
  CHECK(db == blake3pp::hash(b));

  // An error_code completion arrives as a system_error.
  CHECK_THROWS_AS(async::sync_wait(ctx, async::async_hash_file(
                                            ctx, fa.path() / "nope", sched)),
                  std::system_error);

  // And the update form, whose value is nothing.
  blake3pp::hasher h;
  auto none = async::sync_wait(ctx, async::async_update_file(ctx, h, fb.path(), sched));
  REQUIRE(none.has_value());
  CHECK(h.finalize() == blake3pp::hash(b));
}

// Many files, a bounded number at a time, over a context with fewer
// windows than files: every outcome is reported with its hasher, a
// missing file is an outcome and not the end of the run, and the
// operation completes once.
TEST_CASE("async_update_files reports every file and completes once") {
  constexpr std::size_t files = 6;
  std::vector<std::vector<std::byte>> contents;
  std::vector<std::unique_ptr<temp_file>> temps;
  std::vector<fs::path> paths;
  for (std::size_t i = 0; i < files; ++i) {
    contents.push_back(pattern(64 * 1024 * (i + 1) + 7 * i, 60 + static_cast<std::uint32_t>(i)));
    temps.push_back(std::make_unique<temp_file>(contents.back()));
    paths.push_back(temps.back()->path());
  }
  paths.insert(paths.begin() + 2, temps[0]->path() / "missing");  // the third file does not exist

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();
  std::vector<std::optional<blake3pp::digest>> got(paths.size());
  std::vector<bool> failed(paths.size());
  unsigned calls = 0;
  unsigned made = 0;
  auto work = async::async_update_files(
      ctx, paths, sched, {.in_flight = 2},
      [&](std::size_t) {
        ++made;
        return blake3pp::hasher{};
      },
      [&](std::size_t i, blake3pp::hasher& h, const async::file_outcome& o) noexcept {
        ++calls;
        if (o.ok()) {
          got[i] = h.finalize();
        } else {
          failed[i] = true;
        }
      });
  auto done = async::sync_wait(ctx, std::move(work));
  REQUIRE(done.has_value());
  CHECK(calls == paths.size());
  CHECK(made == paths.size());
  for (std::size_t i = 0, c = 0; i < paths.size(); ++i) {
    CAPTURE(i);
    if (i == 2) {
      CHECK(!got[i].has_value());
      CHECK(failed[i]);
      continue;
    }
    CHECK(!failed[i]);
    REQUIRE(got[i].has_value());
    CHECK(*got[i] == blake3pp::hash(contents[c++]));
  }

  // Nothing to do completes too, with no callback.
  calls = 0;
  auto empty = async::sync_wait(
      ctx, async::async_update_files(
               ctx, std::span<const fs::path>{}, sched, {},
               [](std::size_t) { return blake3pp::hasher{}; },
               [&](std::size_t, blake3pp::hasher&, const async::file_outcome&) noexcept { ++calls; }));
  REQUIRE(empty.has_value());
  CHECK(calls == 0);
}

// A stop from the RECEIVER, not from the context: the many-files operation
// has to reach the files already in flight as well as the ones it has not
// started, because a when_all sibling failing is how a caller cancels it.
TEST_CASE("a receiver's stop ends async_update_files, in flight and not") {
  constexpr std::size_t files = 6;
  std::vector<std::vector<std::byte>> contents;
  std::vector<std::unique_ptr<temp_file>> temps;
  std::vector<fs::path> paths;
  for (std::size_t i = 0; i < files; ++i) {
    contents.push_back(pattern(32 * 64 * 1024 + i, 70 + static_cast<std::uint32_t>(i)));
    temps.push_back(std::make_unique<temp_file>(contents.back()));
    paths.push_back(temps.back()->path());
  }

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();
  unsigned calls = 0;
  unsigned stopped = 0;
  ex::inplace_stop_source src;
  auto work = async::async_update_files(
      ctx, paths, sched, {.in_flight = 2},
      [](std::size_t) { return blake3pp::hasher{}; },
      [&](std::size_t, blake3pp::hasher&, const async::file_outcome& o) noexcept {
        ++calls;
        if (o.stopped) {
          ++stopped;
        }
        // The stop arrives once the first file has been reported, so some
        // files are in flight and some have not started.
        src.request_stop();
      });
  record rec;
  auto op = ex::connect(std::move(work), handle_receiver{&rec, src.get_token()});
  ex::start(op);
  ctx.run_until_idle();
  // Every file is accounted for exactly once, the operation completes
  // exactly once, and the stop was not silently ignored.
  CHECK(calls == paths.size());
  CHECK(stopped > 0);
  CHECK(rec.total() == 1);
  CHECK(rec.stops == 1);
  CHECK(!ctx.stop_requested());  // the receiver's stop is not the context's
}

// A byte budget for the arena is the knob a caller actually has.
TEST_CASE("arena_bytes sizes the window count") {
  async::io_context by_bytes({.window_bytes = 64 * 1024,
                              .queue_depth = 2,
                              .arena_bytes = 8 * 64 * 1024,
                              .direct_io = false});
  CHECK(by_bytes.windows() == 8);
  async::io_context floor({.window_bytes = 64 * 1024,
                           .queue_depth = 4,
                           .arena_bytes = 64 * 1024,
                           .direct_io = false});
  CHECK(floor.windows() == 4);  // never fewer than the queue depth
}

TEST_CASE("several files run on one context at once") {
  // Four files, two windows: the pool runs out, and a pipeline that
  // finds none resumes when another gives one back.
  constexpr unsigned files = 4;
  std::vector<std::vector<std::byte>> contents;
  std::vector<std::unique_ptr<temp_file>> temps;
  std::vector<blake3pp::digest> want;
  for (unsigned i = 0; i < files; ++i) {
    contents.push_back(pattern(3 * 64 * 1024 + 64 * i, 20 + i));
    temps.push_back(std::make_unique<temp_file>(contents.back()));
    want.push_back(blake3pp::hash(contents.back()));
  }

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  std::vector<record> recs(files);
  using sender_t = decltype(async::async_hash_file(
      ctx, temps[0]->path(), sched));
  using op_t = ex::connect_result_t<sender_t, move_only_receiver>;
  // An operation state is immovable, so it is constructed in place
  // through a conversion function, as the pipeline's own cells are.
  struct make_op {
    async::io_context* ctx;
    const fs::path* path;
    decltype(blake3pp::get_parallel_scheduler()) sched;
    record* rec;
    operator op_t() const {
      return ex::connect(async::async_hash_file(*ctx, *path, sched),
                         move_only_receiver{rec});
    }
  };
  std::vector<std::optional<op_t>> ops(files);
  for (unsigned i = 0; i < files; ++i) {
    ops[i].emplace(make_op{&ctx, &temps[i]->path(), sched, &recs[i]});
    ex::start(*ops[i]);
  }
  ctx.run_until_idle();
  for (unsigned i = 0; i < files; ++i) {
    CAPTURE(i);
    CHECK(recs[i].total() == 1);
    CHECK(recs[i].values == 1);
    CHECK(recs[i].value == want[i]);
  }
}

TEST_CASE("a file that does not exist reports an error, not an exception") {
  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  record rec;
  auto op = ex::connect(
      async::async_hash_file(ctx, "/nonexistent/blake3pp/async", sched),
      move_only_receiver{&rec});
  ex::start(op);
  CHECK(rec.total() == 0);
  ctx.run_until_idle();
  CHECK(rec.total() == 1);
  CHECK(rec.errors == 1);
}

TEST_CASE("an empty file completes with the hasher untouched") {
  const temp_file f({});
  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  record rec;
  auto op = ex::connect(async::async_hash_file(ctx, f.path(), sched),
                        move_only_receiver{&rec});
  ex::start(op);
  ctx.run_until_idle();
  CHECK(rec.values == 1);
  CHECK(rec.value == blake3pp::hash(std::span<const std::byte>{}));
}

TEST_CASE("a receiver stopped before the start completes set_stopped once") {
  const auto content = pattern(4 * 64 * 1024, 21);
  const temp_file f(content);

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  ex::inplace_stop_source src;
  src.request_stop();
  record rec;
  auto op = ex::connect(async::async_hash_file(ctx, f.path(), sched),
                        handle_receiver{&rec, src.get_token()});
  ex::start(op);
  CHECK(rec.total() == 0);
  ctx.run_until_idle();
  CHECK(rec.total() == 1);
  CHECK(rec.stops == 1);
}

TEST_CASE("the context's stop ends every started operation exactly once") {
  // Large enough that a stop from another thread lands mid-file on most
  // attempts; the assertion holds either way, since a run that finished
  // first still completes exactly once.
  const auto content = pattern(64 * 64 * 1024, 22);
  const temp_file f(content);

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  record rec;
  auto op = ex::connect(async::async_hash_file(ctx, f.path(), sched),
                        move_only_receiver{&rec});
  ex::start(op);
  std::thread stopper([&] { ctx.request_stop(); });
  ctx.run();
  stopper.join();
  CHECK(rec.total() == 1);
  CHECK(rec.errors == 0);
}

TEST_CASE("run returns on request_stop with nothing started") {
  async::io_context ctx(tight());
  std::thread stopper([&] { ctx.request_stop(); });
  ctx.run();  // blocks until the stop arrives
  stopper.join();
  CHECK(!ctx.stop_requested());
}

TEST_CASE("a context with nothing started returns from run_until_idle") {
  async::io_context ctx(tight());
  ctx.run_until_idle();
  ctx.run_until_idle();
  CHECK(!ctx.stop_requested());
}

TEST_CASE("a context is reusable after its operations complete") {
  const auto content = pattern(64 * 1024 + 3, 23);
  const temp_file f(content);
  const blake3pp::digest want = blake3pp::hash(content);

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();

  for (int i = 0; i < 3; ++i) {
    CAPTURE(i);
    record rec;
    auto op = ex::connect(async::async_hash_file(ctx, f.path(), sched),
                          move_only_receiver{&rec});
    ex::start(op);
    ctx.run_until_idle();
    CHECK(rec.values == 1);
    CHECK(rec.value == want);
  }
}

TEST_CASE("the async digest matches the blocking one over many sizes") {
  async::io_context ctx({.window_bytes = 64 * 1024,
                         .queue_depth = 4,
                         .windows = 4,
                         .direct_io = false});
  auto sched = blake3pp::get_parallel_scheduler();

  for (const std::size_t len : {std::size_t{1}, std::size_t{1024},
                                std::size_t{64 * 1024 - 1},
                                std::size_t{64 * 1024},
                                std::size_t{64 * 1024 + 1},
                                std::size_t{5 * 64 * 1024},
                                std::size_t{5 * 64 * 1024 + 4097}}) {
    CAPTURE(len);
    const auto content = pattern(len, static_cast<std::uint32_t>(len));
    const temp_file f(content);
    record rec;
    auto op = ex::connect(async::async_hash_file(ctx, f.path(), sched),
                          move_only_receiver{&rec});
    ex::start(op);
    ctx.run_until_idle();
    CHECK(rec.values == 1);
    CHECK(rec.value == blake3pp::hash(content));
  }
}

TEST_CASE("a keyed async hash matches the keyed in-memory one") {
  std::array<std::byte, blake3pp::key_size> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(i + 1);
  }
  const auto content = pattern(3 * 64 * 1024, 24);
  const temp_file f(content);

  blake3pp::hasher want = blake3pp::hasher::keyed(key);
  want.update(content);

  async::io_context ctx(tight());
  auto sched = blake3pp::get_parallel_scheduler();
  record rec;
  auto op = ex::connect(
      async::async_hash_file(ctx, f.path(), sched, {.mode = blake3pp::hash_mode::keyed(key)}),
      move_only_receiver{&rec});
  ex::start(op);
  ctx.run_until_idle();
  CHECK(rec.values == 1);
  CHECK(rec.value == want.finalize());
}

}  // TEST_SUITE
