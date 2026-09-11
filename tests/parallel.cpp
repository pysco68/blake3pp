#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
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

  // 16 MiB and 16 MiB + 1 fill 1023 and 1024 parts of 16 KiB, 16 MiB +
  // 1025 is the first input split into 32 KiB parts, and 32 MiB + 1023
  // fills all 1024 again at that size.
  for (const std::size_t len :
       {std::size_t{0}, std::size_t{1}, std::size_t{1024}, std::size_t{1025},
        std::size_t{31 * 1024}, std::size_t{32 * 1024},
        std::size_t{32 * 1024 + 1}, std::size_t{1024 * 1024},
        std::size_t{(4 * 1024 + 3) * 1024 + 17},
        std::size_t{16 * 1024 * 1024}, std::size_t{16 * 1024 * 1024 + 1},
        std::size_t{16 * 1024 * 1024 + 1025},
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

TEST_CASE("parallel_hasher fills every part slot of a 16 MiB window") {
  // A 16 MiB window is 1024 parts of 16 KiB, the engine's whole CV table;
  // two windows and a tail cover the full fan-out, a window straddle and
  // the sequential finish.
  const blake3pp::parallel_hasher_options opts{.window_bytes = 16 * 1024 * 1024};
  const auto input = make_input(2 * 16 * 1024 * 1024 + 77);
  blake3pp::parallel_hasher ph{blake3pp::get_parallel_scheduler(), opts};
  const std::size_t bite = 5 * 1024 * 1024 + 3;
  for (std::size_t pos = 0; pos < input.size(); pos += bite) {
    ph.update(std::span{input}.subspan(pos, std::min(bite, input.size() - pos)));
  }
  CHECK(ph.finalize() == blake3pp::hash(input));
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

  // derive_key one-shot, multi-core
  CHECK(blake3pp::derive_key(context, material, sched) == seq.finalize());
}

// core.hpp's one-shots come in span and string_view spellings; the
// scheduler-taking pairs must accept the same arguments, and the
// scheduler constraint must keep them out of the sequential overloads'
// way (an arch or a string_view in the scheduler's seat is not a match).
TEST_CASE("multi-core one-shots mirror the sequential spellings") {
  auto sched = blake3pp::get_parallel_scheduler();
  const std::string text(2 * 1024 * 1024 + 9, 'q');
  const std::string_view sv{text};
  std::array<std::byte, 32> key{};
  key[3] = std::byte{42};
  const std::span<const std::byte, 32> key_span{key};

  CHECK(blake3pp::hash(sv, sched) == blake3pp::hash(sv));
  CHECK(blake3pp::keyed_hash(key_span, sv, sched) ==
        blake3pp::keyed_hash(key_span, sv));
  CHECK(blake3pp::derive_key("ctx 2026-09", sv, sched) ==
        blake3pp::derive_key("ctx 2026-09", sv));
  // Sequential overloads still resolve with parallel.hpp included.
  CHECK(blake3pp::hash(std::as_bytes(std::span{sv})) == blake3pp::hash(sv));
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

// The scheduler-taking fill() must be indistinguishable from
// output_reader::fill(): same bytes, same position afterwards, from
// aligned and unaligned starting offsets, across segment boundaries and
// through the sequential fallback for requests of one segment or less.
TEST_CASE("parallel fill matches the sequential output stream") {
  auto sched = blake3pp::get_parallel_scheduler();
  blake3pp::hasher h;
  h.update(std::string_view{"parallel xof fill"});
  constexpr std::size_t segment = 4096;

  for (const std::uint64_t start : {std::uint64_t{0}, std::uint64_t{100},
                                    std::uint64_t{1} << 40}) {
    for (const std::size_t len :
         {std::size_t{64}, segment, segment + 1, 3 * segment,
          10 * segment + 777, std::size_t{100 * 1024 + 3}}) {
      CAPTURE(start);
      CAPTURE(len);
      auto seq = h.finalize_xof();
      seq.seek(start);
      std::vector<std::byte> expected(len);
      seq.fill(expected);

      auto par = h.finalize_xof();
      par.seek(start);
      std::vector<std::byte> got(len);
      blake3pp::fill(par, got, sched, segment);
      CHECK(got == expected);
      CHECK(par.position() == seq.position());

      // And the stream continues from where the fill left it.
      CHECK(par.take<32>() == seq.take<32>());
    }
  }

  // The free sequential spelling is the member.
  {
    auto a = h.finalize_xof();
    auto b = h.finalize_xof();
    std::vector<std::byte> va(300);
    std::vector<std::byte> vb(300);
    a.fill(va);
    blake3pp::fill(b, vb);
    CHECK(va == vb);
    CHECK(a.position() == b.position());
  }

  // A segment request below one block is rounded up to a block.
  auto seq = h.finalize_xof();
  std::vector<std::byte> expected(1000);
  seq.fill(expected);
  auto par = h.finalize_xof();
  std::vector<std::byte> got(1000);
  blake3pp::fill(par, got, sched, 7);
  CHECK(got == expected);
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
