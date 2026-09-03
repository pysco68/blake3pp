#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include <blake3pp/parallel.hpp>
#include <doctest/doctest.h>

#if defined(BLAKE3PP_EXECUTION_STDEXEC) && defined(__APPLE__)
#include <exec/libdispatch_queue.hpp>
#endif

namespace {

std::vector<std::byte> make_input(std::size_t len) {
  std::vector<std::byte> v(len);
  for (std::size_t i = 0; i < len; ++i) {
    v[i] = static_cast<std::byte>(i % 251);
  }
  return v;
}

TEST_SUITE("parallel") {

// The parallel decomposition must be invisible: identical digests to the
// sequential path at every size class (below the parallel threshold, right
// at partition boundaries, and deep multi-level trees).
TEST_CASE("parallel hash matches sequential at every size class") {
  auto sched = blake3pp::get_parallel_scheduler();

  for (const std::size_t len :
       {std::size_t{0}, std::size_t{1}, std::size_t{1024}, std::size_t{1025},
        std::size_t{31 * 1024}, std::size_t{32 * 1024},
        std::size_t{32 * 1024 + 1}, std::size_t{1024 * 1024},
        std::size_t{(4 * 1024 + 3) * 1024 + 17},
        std::size_t{32 * 1024 * 1024 + 1023}}) {
    CAPTURE(len);
    const auto input = make_input(len);
    CHECK(blake3pp::hash(input, sched) == blake3pp::hash(input));
  }
}

TEST_CASE("parallel_hasher matches sequential across streaming patterns") {
  // Small window + odd bite size force many flushes and window straddles.
  const blake3pp::parallel_hasher_options opts{.window_bytes = 128 * 1024};

  for (const std::size_t len :
       {std::size_t{0}, std::size_t{100}, std::size_t{128 * 1024},
        std::size_t{128 * 1024 + 1}, std::size_t{512 * 1024},
        std::size_t{(3 * 128 + 55) * 1024 + 77}}) {
    CAPTURE(len);
    const auto input = make_input(len);
    blake3pp::parallel_hasher ph{blake3pp::get_parallel_scheduler(), opts};
    std::size_t pos = 0;
    while (pos < input.size()) {
      const std::size_t bite = std::min<std::size_t>(77777, len - pos);
      ph.update(std::span{input}.subspan(pos, bite));
      pos += bite;
    }
    CHECK(ph.finalize() == blake3pp::hash(input));
  }
}

TEST_CASE("parallel_hasher checkpoints and resets like hasher") {
  const auto input = make_input(700 * 1024 + 3);

  blake3pp::parallel_hasher ph{blake3pp::get_parallel_scheduler(),
                               {.window_bytes = 128 * 1024}};
  ph.update(std::span{input}.first(300 * 1024));
  // finalize() is non-destructive: a mid-stream checkpoint...
  CHECK(ph.finalize() == blake3pp::hash(std::span{input}.first(300 * 1024)));
  // ...and hashing continues correctly afterwards.
  ph.update(std::span{input}.subspan(300 * 1024));
  CHECK(ph.count() == input.size());
  CHECK(ph.finalize() == blake3pp::hash(input));

  ph.reset();
  ph.update("fresh start");
  CHECK(ph.finalize() == blake3pp::hash(std::string_view{"fresh start"}));
}

TEST_CASE("keyed mode propagates through every parallel quadrant") {
  auto sched = blake3pp::get_parallel_scheduler();
  std::array<std::byte, 32> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(i * 7 + 1);
  }
  const std::span<const std::byte, 32> key_span{key};
  const auto input = make_input(9 * 1024 * 1024 + 137);
  const auto expected = blake3pp::keyed_hash(key_span, input);
  CHECK(expected != blake3pp::hash(input));  // the key matters

  // keyed one-shot, multi-core
  CHECK(blake3pp::keyed_hash(key_span, input, sched) == expected);

  // keyed incremental, multi-core
  blake3pp::parallel_hasher ph{sched, key_span, {.window_bytes = 512 * 1024}};
  ph.update(std::span{input}.first(1024 * 1024 + 3));
  ph.update(std::span{input}.subspan(1024 * 1024 + 3));
  CHECK(ph.finalize() == expected);
}

TEST_CASE("derive_key mode matches sequential through parallel_hasher") {
  auto sched = blake3pp::get_parallel_scheduler();
  const auto material = make_input(3 * 1024 * 1024 + 41);
  constexpr std::string_view context = "blake3pp tests 2026-09 derive";

  blake3pp::hasher seq = blake3pp::hasher::derive_key(context);
  seq.update(material);

  blake3pp::parallel_hasher ph{sched, context, {.window_bytes = 512 * 1024}};
  ph.update(material);
  CHECK(ph.finalize() == seq.finalize());
  CHECK(ph.finalize() != blake3pp::hash(material));  // the context matters
}

// The XOF finalize family must produce the sequential hasher's stream
// bit for bit, and stay non-destructive on the parallel side too.
TEST_CASE("parallel_hasher XOF forms match the sequential stream") {
  auto sched = blake3pp::get_parallel_scheduler();
  const auto input = make_input(5 * 1024 * 1024 + 259);

  blake3pp::hasher seq;
  seq.update(input);

  blake3pp::parallel_hasher ph{sched, {.window_bytes = 512 * 1024}};
  ph.update(input);

  CHECK(ph.finalize<131>() == seq.finalize<131>());

  std::array<std::byte, 200> wide{};
  ph.finalize(wide);
  std::array<std::byte, 200> wide_seq{};
  seq.finalize(wide_seq);
  CHECK(wide == wide_seq);

  auto r = ph.finalize_xof();
  r.seek(1'000'000);
  auto rs = seq.finalize_xof();
  rs.seek(1'000'000);
  CHECK(r.take<64>() == rs.take<64>());

  // Still non-destructive: the plain digest survives all of the above.
  CHECK(ph.finalize() == seq.finalize());
}

TEST_CASE("parallel hash is deterministic across runs") {
  auto sched = blake3pp::get_parallel_scheduler();
  const auto input = make_input(8 * 1024 * 1024 + 7);
  const auto first = blake3pp::hash(input, sched);
  for (int r = 0; r < 5; ++r) {
    CHECK(blake3pp::hash(input, sched) == first);
  }
}

#if defined(BLAKE3PP_EXECUTION_STDEXEC) && defined(__APPLE__)
// The scheduler-parameterized design meeting the platform's native
// runtime: stdexec's libdispatch scheduler submits the same bulk work to
// GCD's global pool instead of a thread pool the process owns. Nothing in
// the engine knows the difference, which is the point.
TEST_CASE("GCD (libdispatch) scheduler drives the engine unchanged") {
  exec::libdispatch_queue queue;
  auto sched = queue.get_scheduler();

  for (const std::size_t len :
       {std::size_t{1024 * 1024}, std::size_t{8 * 1024 * 1024 + 7}}) {
    CAPTURE(len);
    const auto input = make_input(len);
    CHECK(blake3pp::hash(input, sched) == blake3pp::hash(input));
  }

  blake3pp::parallel_hasher ph{sched, {.window_bytes = 512 * 1024}};
  const auto input = make_input(3 * 1024 * 1024 + 41);
  ph.update(input);
  CHECK(ph.finalize() == blake3pp::hash(input));
}
#endif

}  // TEST_SUITE

}  // namespace
