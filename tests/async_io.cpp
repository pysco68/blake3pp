// The experimental async API: a context the caller drives and two
// senders over it.
//
// What these check that the blocking suites cannot: that several files
// share one arena and one thread, that a receiver's stop reaches the
// pipeline through the seam Phase 5 built, and that the four completion
// signals are the only way an operation ever ends.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
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
    static const unsigned salt = std::random_device{}();
    path_ = fs::temp_directory_path() /
            ("blake3pp_async_" + std::to_string(salt) + "_" +
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
  auto where = ex::get_completion_scheduler<ex::set_value_t>(env);
  CHECK(where == ctx.scheduler());
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
      async::async_hash_file(ctx, f.path(), sched, {.key = key}),
      move_only_receiver{&rec});
  ex::start(op);
  ctx.run_until_idle();
  CHECK(rec.values == 1);
  CHECK(rec.value == want.finalize());
}

}  // TEST_SUITE
