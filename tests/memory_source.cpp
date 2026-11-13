// The in-memory source, and the two engines that now run on it.
//
// memory_driver is a file_driver like io_driver is, so the first suite
// below is the driver contract: callbacks only from poll(), a wake that
// releases a blocked one, an advisory cancel, and two "files" sharing a
// driver. It is NOT in tests/io_context.cpp -- that suite
// is about the platform backends and only builds with the file I/O
// layer, and the whole point of this driver is that the in-memory path
// does not need that layer.
//
// The rest is equivalence: hash_into and parallel_hasher have to agree
// with the plain sequential hasher over every shape of input, and the
// shapes that matter are the ones around a window boundary.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <blake3pp/detail/memory_driver.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/parallel_backend.hpp>
#include <doctest/doctest.h>

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

// One counted completion, with the thread it ran on: "inside poll()"
// means on the thread that called poll, which is the part a driver
// completing on a worker of its own would get wrong.
struct probe {
  int calls = 0;
  std::error_code ec{};
  std::thread::id ran_on{};
  static void on_done(detail::io_op* op, std::error_code e) noexcept {
    auto* const p = static_cast<probe*>(op->owner);
    p->calls++;
    p->ec = e;
    p->ran_on = std::this_thread::get_id();
  }
};

detail::io_op make_op(probe& p) noexcept {
  detail::io_op op{};
  op.done = &probe::on_done;
  op.owner = &p;
  return op;
}

}  // namespace

TEST_SUITE("memory_source") {

TEST_CASE("a memory window is a view, and nothing is allocated for it") {
  const auto bytes = pattern(40 * 1024, 3);
  detail::memory_driver drv;
  detail::memory_driver::file f(drv, bytes);

  CHECK(f.size() == bytes.size());
  CHECK(f.name() == "memory");

  drv.reserve(4, 8 * 1024);
  const detail::window_storage w = drv.window_storage_for(f, 2, 16 * 1024, 8 * 1024);
  // Nothing to fill: the bytes never move.
  CHECK(w.read_into.empty());
  CHECK(w.view.size() == 8 * 1024);
  CHECK(w.view.data() == bytes.data() + 16 * 1024);
}

TEST_CASE("memory_driver reports its reads only from poll") {
  const auto bytes = pattern(64 * 1024, 4);
  detail::memory_driver drv;
  detail::memory_driver::file f(drv, bytes);
  drv.reserve(2, 32 * 1024);

  probe pa;
  probe pb;
  detail::io_op op_a = make_op(pa);
  detail::io_op op_b = make_op(pb);

  drv.submit_read(f, 0, {}, op_a);
  drv.submit_read(f, 32 * 1024, {}, op_b);
  CHECK(pa.calls == 0);
  CHECK(pb.calls == 0);
  drv.flush();
  CHECK(pa.calls == 0);
  CHECK(pb.calls == 0);
  CHECK(drv.in_flight() == 2);

  const std::size_t ran = drv.poll(true);
  CHECK(ran == 2);
  CHECK(pa.calls == 1);
  CHECK(pb.calls == 1);
  CHECK(!pa.ec);
  CHECK(!pb.ec);
  CHECK(pa.ran_on == std::this_thread::get_id());
  CHECK(drv.in_flight() == 0);
}

TEST_CASE("a cancelled memory read reports once, as cancelled") {
  const auto bytes = pattern(16 * 1024, 5);
  detail::memory_driver drv;
  detail::memory_driver::file f(drv, bytes);
  drv.reserve(2, 8 * 1024);

  probe pa;
  probe pb;
  detail::io_op op_a = make_op(pa);
  detail::io_op op_b = make_op(pb);
  drv.submit_read(f, 0, {}, op_a);
  drv.submit_read(f, 8 * 1024, {}, op_b);
  drv.cancel(op_a);
  // Advisory and never a completion in itself.
  CHECK(pa.calls == 0);
  CHECK(drv.in_flight() == 2);

  drv.poll(true);
  CHECK(pa.calls == 1);
  CHECK(pb.calls == 1);
  // Nothing was read, so the cancel has nothing to lose the race to.
  CHECK(pa.ec == std::errc::operation_canceled);
  CHECK(!pb.ec);
  CHECK(drv.in_flight() == 0);
}

TEST_CASE("a memory driver drains without reporting, and stays usable") {
  const auto bytes = pattern(16 * 1024, 6);
  detail::memory_driver drv;
  detail::memory_driver::file f(drv, bytes);
  drv.reserve(1, 16 * 1024);

  probe pa;
  detail::io_op op_a = make_op(pa);
  drv.submit_read(f, 0, {}, op_a);
  drv.drain();
  CHECK(pa.calls == 0);
  CHECK(drv.in_flight() == 0);

  probe pb;
  detail::io_op op_b = make_op(pb);
  drv.submit_read(f, 0, {}, op_b);
  drv.poll(true);
  CHECK(pb.calls == 1);
}

TEST_CASE("wake releases a memory driver blocked in poll") {
  detail::memory_driver drv;
  std::thread waker([&drv] {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    drv.wake();
  });
  // Nothing queued, so this returns only because of the wake.
  const std::size_t ran = drv.poll(true);
  waker.join();
  CHECK(ran == 0);
}

// A wake nobody consumed must not keep anything alive or block the
// destructor: the last thing a driver sees can be a wake for a poll that
// never comes.
TEST_CASE("a memory driver destroyed with a wake pending tears down cleanly") {
  {
    detail::memory_driver drv;
    drv.wake();
  }
  CHECK(true);  // reaching here without a hang or a crash is the check
}

TEST_CASE("two spans share one memory driver") {
  const auto a = pattern(8 * 1024, 7);
  const auto b = pattern(8 * 1024, 8);
  detail::memory_driver drv;
  detail::memory_driver::file fa(drv, a);
  detail::memory_driver::file fb(drv, b);
  drv.reserve(2, 8 * 1024);

  CHECK(drv.window_storage_for(fa, 0, 0, 8 * 1024).view.data() == a.data());
  CHECK(drv.window_storage_for(fb, 1, 0, 8 * 1024).view.data() == b.data());

  probe pa;
  probe pb;
  detail::io_op op_a = make_op(pa);
  detail::io_op op_b = make_op(pb);
  drv.submit_read(fa, 0, {}, op_a);
  drv.submit_read(fb, 0, {}, op_b);
  CHECK(drv.poll(true) == 2);
  CHECK(pa.calls == 1);
  CHECK(pb.calls == 1);
}

// ------------------------------------------------------------------
// hash_into: every shape of input agrees with the sequential hasher.

TEST_CASE("hash_into agrees with the sequential hasher at every size") {
  auto sched = blake3pp::get_parallel_scheduler();
  const std::size_t win = blake3pp::detail::memory_window_bytes(8u << 20);
  const std::size_t part = blake3pp::chunk_size * blake3pp::detail::min_part_chunks;
  const std::vector<std::size_t> sizes{
      0, 1, 1024, part, blake3pp::detail::min_pipeline_bytes - 1,
      blake3pp::detail::min_pipeline_bytes, win - 1, win, win + 1,
      3 * win + 4321};
  const auto bytes = pattern(3 * win + 8192, 11);
  std::array<std::byte, blake3pp::key_size> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(i * 7 + 1);
  }

  for (const std::size_t n : sizes) {
    CAPTURE(n);
    const std::span<const std::byte> in{bytes.data(), n};
    CHECK(blake3pp::hash(in, sched) == blake3pp::hash(in));
    CHECK(blake3pp::keyed_hash(key, in, sched) == blake3pp::keyed_hash(key, in));
    CHECK(blake3pp::derive_key("ctx", in, sched) ==
          blake3pp::derive_key("ctx", in));
    // The stack budget changes the part count and the table size, and
    // must change nothing else.
    CHECK(blake3pp::hash<blake3pp::stack_budget{64}>(in, sched) ==
          blake3pp::hash(in));
    CHECK(blake3pp::hash<blake3pp::stack_budget{1024}>(in, sched) ==
          blake3pp::hash(in));
  }
}

TEST_CASE("hash_into agrees on every arch the build can reach") {
  auto sched = blake3pp::get_parallel_scheduler();
  const std::size_t win = blake3pp::detail::memory_window_bytes(8u << 20);
  const auto bytes = pattern(2 * win + 777, 12);
  const std::span<const std::byte> in{bytes};
  for (const auto a : blake3pp::available_arches()) {
    CAPTURE(blake3pp::to_string(a));
    // The sequential reference for a named variant is a hasher built on
    // it; core's hash() always takes the best available.
    blake3pp::hasher ref(a);
    ref.update(in);
    CHECK(blake3pp::hash(in, sched, a) == ref.finalize());
  }
}

// ------------------------------------------------------------------
// parallel_hasher: the boundary is where this can go wrong.

namespace {

// Feeds `parts` to a parallel_hasher and to a plain one, and compares.
// The plain hasher is the definition of correct here; every split of the
// same bytes has to reach the same digest.
template <class Sched>
void check_split(Sched sched, std::span<const std::byte> msg,
                 std::size_t window, const std::vector<std::size_t>& parts,
                 int mode) {
  std::array<std::byte, blake3pp::key_size> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(i * 3 + 2);
  }
  blake3pp::parallel_hasher_options opts{};
  opts.window_bytes = window;
  blake3pp::parallel_hasher ph =
      mode == 0 ? blake3pp::parallel_hasher{sched, opts}
      : mode == 1
          ? blake3pp::parallel_hasher{
                sched, std::span<const std::byte, blake3pp::key_size>{key},
                opts}
          : blake3pp::parallel_hasher{sched, std::string_view{"ctx"}, opts};
  blake3pp::hasher ref = mode == 0   ? blake3pp::hasher{}
                         : mode == 1 ? blake3pp::hasher::keyed(key)
                                     : blake3pp::hasher::derive_key("ctx");
  std::size_t at = 0;
  for (std::size_t n : parts) {
    n = std::min(n, msg.size() - at);
    const std::span<const std::byte> p = msg.subspan(at, n);
    ph.update(p);
    ref.update(p);
    at += n;
    CHECK(ph.count() == ref.count());
  }
  CHECK(ph.finalize() == ref.finalize());
}

}  // namespace

// A message that ends exactly on a window boundary is the case that gets
// this wrong: the final window would look like an ordinary interior one,
// and a subtree pushed into the hasher must never contain the message's
// last chunk. Every split below is fixed, so a failure names itself.
TEST_CASE("parallel_hasher agrees across boundary-exact splits") {
  auto sched = blake3pp::get_parallel_scheduler();
  const auto msg = pattern(4 * 1024 * 1024 + 4096, 13);
  for (const std::size_t w : {std::size_t{64 * 1024}, std::size_t{1024 * 1024}}) {
    CAPTURE(w);
    for (int mode = 0; mode < 3; ++mode) {
      CAPTURE(mode);
      const std::span<const std::byte> m{msg};
      // Whole windows, delivered whole.
      check_split(sched, m.first(w), w, {w}, mode);
      check_split(sched, m.first(2 * w), w, {2 * w}, mode);
      check_split(sched, m.first(3 * w), w, {3 * w}, mode);
      // Whole windows, delivered one window at a time.
      check_split(sched, m.first(3 * w), w, {w, w, w}, mode);
      // Split exactly on a boundary, in two calls.
      check_split(sched, m.first(3 * w), w, {2 * w, w}, mode);
      // Split mid-window, so a call has to finish a part-built window.
      check_split(sched, m.first(3 * w), w, {w / 2, w, w, w / 2}, mode);
      check_split(sched, m.first(3 * w), w, {w + 1, w - 1, w}, mode);
      check_split(sched, m.first(3 * w), w, {1, 3 * w - 1}, mode);
      check_split(sched, m.first(3 * w), w, {3 * w - 1, 1}, mode);
      // A tail past the last boundary.
      check_split(sched, m.first(2 * w + 4096), w, {2 * w + 4096}, mode);
      check_split(sched, m.first(2 * w + 4096), w, {w, w, 4096}, mode);
    }
  }
}

// The fixed splits above are the cases reasoning predicts; this is the
// one that finds what it did not. Seeded, so a failure reproduces.
TEST_CASE("parallel_hasher agrees over random split sequences") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t w = 1024 * 1024;
#if defined(BLAKE3PP_TEST_REDUCED_CASES)
  constexpr unsigned seeds = 4;
  const auto msg = pattern(6 * w, 14);
#else
  constexpr unsigned seeds = 24;
  const auto msg = pattern(20 * w, 14);
#endif
  for (unsigned seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    std::mt19937 rng(seed);
    // From one byte to three windows, so both the sub-window path and
    // the multi-window one are hit, many times each.
    std::uniform_int_distribution<std::size_t> pick(1, 3 * w);
    std::vector<std::size_t> parts;
    for (std::size_t at = 0; at < msg.size();) {
      const std::size_t n = std::min(pick(rng), msg.size() - at);
      parts.push_back(n);
      at += n;
    }
    check_split(sched, std::span<const std::byte>{msg}, w, parts,
                static_cast<int>(seed % 3));
  }
}

// ------------------------------------------------------------------
// Stop, over a source that cannot fail.

// A memory source has no error channel: there is no read to go wrong.
// Stop is the only way out other than success, and it has to end the run
// exactly once, like it does over a device.
TEST_CASE("a stop over a memory source ends the run once") {
  auto sched = blake3pp::get_parallel_scheduler();
  const std::size_t win = 1024 * 1024;
  const auto bytes = pattern(8 * win, 15);

  SUBCASE("requested before the run starts") {
    blake3pp::ex::inplace_stop_source src;
    src.request_stop();
    detail::memory_driver drv;
    detail::memory_driver::file f(drv, bytes);
    blake3pp::hasher h;
    std::error_code ec;
    try {
      detail::run_window_pipeline<blake3pp::default_stack_budget>(
          h, drv, f, sched,
          detail::plan_memory_pipeline(0, win, bytes.size()),
          {.stop_token = src.get_token()});
    } catch (const std::system_error& e) {
      ec = e.code();
    }
    CHECK(ec == std::errc::operation_canceled);
    CHECK(drv.in_flight() == 0);
  }

  SUBCASE("requested from another thread while it runs") {
    blake3pp::ex::inplace_stop_source src;
    detail::memory_driver drv;
    detail::memory_driver::file f(drv, bytes);
    blake3pp::hasher h;
    std::thread stopper([&src] { src.request_stop(); });
    std::error_code ec;
    try {
      detail::run_window_pipeline<blake3pp::default_stack_budget>(
          h, drv, f, sched,
          detail::plan_memory_pipeline(0, win, bytes.size()),
          {.stop_token = src.get_token()});
    } catch (const std::system_error& e) {
      ec = e.code();
    }
    stopper.join();
    // A race it is allowed to win: the span is small enough that it may
    // finish first. Either way nothing is left owed.
    if (ec) {
      CHECK(ec == std::errc::operation_canceled);
    }
    CHECK(drv.in_flight() == 0);
  }
}

// What the bench's virtual source checks, over the source that replaced
// it as the library's own: the pipeline's digest over a span is the
// sequential hasher's over the same bytes.
TEST_CASE("the pipeline over a memory source hashes what it was given") {
  auto sched = blake3pp::get_parallel_scheduler();
  const std::size_t win = 1024 * 1024;
  for (const std::size_t n : {win, 3 * win, 3 * win + 99}) {
    CAPTURE(n);
    const auto bytes = pattern(n, 16);
    detail::memory_driver drv;
    detail::memory_driver::file f(drv, bytes);
    blake3pp::hasher h;
    detail::run_window_pipeline<blake3pp::default_stack_budget>(
        h, drv, f, sched, detail::plan_memory_pipeline(0, win, n), {});
    CHECK(h.finalize() == blake3pp::hash(std::span<const std::byte>{bytes}));
  }
}

}  // TEST_SUITE
