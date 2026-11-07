#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <blake3pp/blake3pp.hpp>
#include <doctest/doctest.h>

#include "kernel/kernel.hpp"

namespace {

// doctest stringifies a `const char*` as its ADDRESS, so an arch name has to
// reach CAPTURE as a string_view to be readable.
[[nodiscard]] std::string_view arch_name(blake3pp::arch a) noexcept {
  return blake3pp::to_string(a);
}

std::vector<std::byte> make_input(std::size_t len) {
  std::vector<std::byte> v(len);
  for (std::size_t i = 0; i < len; ++i) {
    v[i] = static_cast<std::byte>(i % 251);
  }
  return v;
}

TEST_SUITE("incremental") {

// Splitting the input across update() calls at any offset must not change
// the digest. The offsets straddle every internal boundary: block (64),
// chunk (1024), and the buffered-last-block laziness in between.
TEST_CASE("split at awkward offsets") {
  const auto input = make_input(4097);
  const auto expected = blake3pp::hash(input);

  for (const std::size_t split :
       {std::size_t{1}, std::size_t{63}, std::size_t{64}, std::size_t{65},
        std::size_t{1023}, std::size_t{1024}, std::size_t{1025},
        std::size_t{2048}, std::size_t{4096}}) {
    CAPTURE(split);
    blake3pp::hasher h;
    h.update(std::span{input}.first(split));
    h.update(std::span{input}.subspan(split));
    CHECK(h.finalize() == expected);
  }
}

// Large enough that the SIMD batch fast path engages (multiple full
// hash_many batches on every wide arch), split at offsets that leave the
// hasher mid-chunk, exactly on chunk boundaries, and mid-batch.
TEST_CASE("large input splits cross the batch fast path") {
  const auto input = make_input(300 * 1024 + 7);
  const auto expected = blake3pp::hash(input);

  for (const std::size_t split :
       {std::size_t{1}, std::size_t{1024}, std::size_t{1500},
        std::size_t{16 * 1024}, std::size_t{17 * 1024 + 3},
        std::size_t{299 * 1024}}) {
    CAPTURE(split);
    blake3pp::hasher h;
    h.update(std::span{input}.first(split));
    h.update(std::span{input}.subspan(split));
    CHECK(h.finalize() == expected);
  }
}

// The wide path in update() reduces every chunk but the one holding the last
// byte, choosing per step between a whole aligned subtree and a batch of
// single-chunk CVs. Which shape a length takes is a property of that length
// and of the running kernel's lane count, so every chunk count from one to
// past four batches is its own case, against a reference that cannot take
// the path at all: updating a chunk at a time never leaves more than one
// whole chunk ahead, so it compresses block by block through chunk_update.
TEST_CASE("the wide path agrees with chunk-at-a-time for every length") {
  constexpr std::size_t max_chunks = 4 * blake3pp::kern::max_simd_degree + 3;
  const auto input = make_input(max_chunks * 1024 + 1024);

  for (const auto a : blake3pp::all_arches()) {
    if (a == blake3pp::arch::auto_detect || !blake3pp::is_available(a)) {
      continue;
    }
    CAPTURE(arch_name(a));
    const auto* ops = blake3pp::detail::resolve(a);
    REQUIRE(ops != nullptr);
    for (std::size_t chunks = 1; chunks <= max_chunks; ++chunks) {
      for (const std::size_t tail :
           {std::size_t{0}, std::size_t{1}, std::size_t{64},
            std::size_t{1023}}) {
        const std::size_t len = chunks * 1024 + tail;
        CAPTURE(chunks);
        CAPTURE(tail);
        const auto bytes = std::span{input}.first(len);

        blake3pp::hasher serial{ops};
        for (std::size_t at = 0; at < len; at += 1024) {
          serial.update(bytes.subspan(at, std::min<std::size_t>(1024, len - at)));
        }
        blake3pp::hasher wide{ops};
        wide.update(bytes);
        CHECK(wide.finalize() == serial.finalize());
      }
    }
  }
}

// The same, entered at a chunk counter that is not subtree-aligned, which is
// where the batch is trimmed to reach an aligned position: three chunk-sized
// updates leave the counter at three, and the update that follows must still
// build the tree the chunk-at-a-time reference builds.
TEST_CASE("the wide path agrees when it starts at a misaligned counter") {
  constexpr std::size_t prefix = 3 * 1024;
  constexpr std::size_t max_chunks = 4 * blake3pp::kern::max_simd_degree + 3;
  const auto input = make_input(prefix + max_chunks * 1024 + 7);

  for (const auto a : blake3pp::all_arches()) {
    if (a == blake3pp::arch::auto_detect || !blake3pp::is_available(a)) {
      continue;
    }
    CAPTURE(arch_name(a));
    const auto* ops = blake3pp::detail::resolve(a);
    REQUIRE(ops != nullptr);
    const std::span<const std::byte> bytes{input};

    blake3pp::hasher serial{ops};
    for (std::size_t at = 0; at < bytes.size(); at += 1024) {
      serial.update(
          bytes.subspan(at, std::min<std::size_t>(1024, bytes.size() - at)));
    }
    blake3pp::hasher wide{ops};
    for (std::size_t at = 0; at < prefix; at += 1024) {
      wide.update(bytes.subspan(at, 1024));
    }
    wide.update(bytes.subspan(prefix));
    CHECK(wide.finalize() == serial.finalize());
  }
}

// update_last() states that the message ends, which lets its final chunk
// join a wide batch: the root is then the top parent rather than that
// chunk. The digest must not notice, at any length, on any kernel.
TEST_CASE("update_last agrees with update for every length") {
  constexpr std::size_t max_chunks = 4 * blake3pp::kern::max_simd_degree + 3;
  const auto input = make_input(max_chunks * 1024 + 1024);

  for (const auto a : blake3pp::all_arches()) {
    if (a == blake3pp::arch::auto_detect || !blake3pp::is_available(a)) {
      continue;
    }
    CAPTURE(arch_name(a));
    const auto* ops = blake3pp::detail::resolve(a);
    REQUIRE(ops != nullptr);
    for (std::size_t len = 0; len <= 3 * 1024 + 7; ++len) {
      CAPTURE(len);
      const auto bytes = std::span{input}.first(len);
      blake3pp::hasher streamed{ops};
      streamed.update(bytes);
      blake3pp::hasher last{ops};
      last.update_last(bytes);
      CHECK(last.finalize() == streamed.finalize());
      CHECK(last.count() == len);
    }
    // Past the sweep, every whole chunk count: these are the lengths that
    // take the new path, one subtree emission per step of the ladder.
    for (std::size_t chunks = 1; chunks <= max_chunks; ++chunks) {
      CAPTURE(chunks);
      const auto bytes = std::span{input}.first(chunks * 1024);
      blake3pp::hasher streamed{ops};
      streamed.update(bytes);
      blake3pp::hasher last{ops};
      last.update_last(bytes);
      CHECK(last.finalize() == streamed.finalize());
      CHECK(last.count() == chunks * 1024);
    }
  }
}

// The message not ending where update_last() said it did: the held subtree
// goes back on the stack, so the digest is the one-stream digest. Every
// first piece that takes the batched path, against every continuation that
// reaches a different branch of it.
TEST_CASE("update after update_last is the whole message") {
  const auto input = make_input(80 * 1024);
  for (const auto a : blake3pp::all_arches()) {
    if (a == blake3pp::arch::auto_detect || !blake3pp::is_available(a)) {
      continue;
    }
    CAPTURE(arch_name(a));
    const auto* ops = blake3pp::detail::resolve(a);
    REQUIRE(ops != nullptr);
    for (const std::size_t first : {2048u, 3072u, 16384u, 17408u, 33792u}) {
      for (const std::size_t rest : {1u, 512u, 1024u, 2048u, 9216u}) {
        CAPTURE(first);
        CAPTURE(rest);
        const auto bytes = std::span{input}.first(first + rest);
        blake3pp::hasher streamed{ops};
        streamed.update(bytes);
        blake3pp::hasher resumed{ops};
        resumed.update_last(bytes.first(first));
        resumed.update(bytes.subspan(first));
        CHECK(resumed.count() == first + rest);
        CHECK(resumed.finalize() == streamed.finalize());
        // And again on the piece that followed, which the hasher is now
        // free to call the last one.
        blake3pp::hasher twice{ops};
        twice.update_last(bytes.first(first));
        twice.update_last(bytes.subspan(first));
        CHECK(twice.finalize() == streamed.finalize());
      }
    }
  }
}

// Zero bytes are a valid input to both entry points and must leave the
// state alone. The two shapes that broke were a full chunk still open (an
// empty update_last() closed it, so finalization lost its root chunk) and
// a held subtree (an empty update() put it back on the stack with no chunk
// after it). The prefixes reach every other state too: fresh, partial
// block, full block, partial chunk, and a chunk ended by update_last().
TEST_CASE("empty updates leave the digest alone") {
  const auto input = make_input(8 * 1024);
  const std::array<std::byte, blake3pp::key_size> key{std::byte{7}};
  enum class mode { plain, keyed, derive };
  enum class empty_call { update, update_last, both };

  for (const auto a : blake3pp::all_arches()) {
    if (a == blake3pp::arch::auto_detect || !blake3pp::is_available(a)) {
      continue;
    }
    CAPTURE(arch_name(a));
    const auto* ops = blake3pp::detail::resolve(a);
    REQUIRE(ops != nullptr);
    const auto make = [&](mode m) {
      switch (m) {
        case mode::keyed: return blake3pp::hasher::keyed(key, ops);
        case mode::derive:
          return blake3pp::hasher::derive_key("blake3pp empty updates", ops);
        case mode::plain: break;
      }
      return blake3pp::hasher{ops};
    };
    for (const mode m : {mode::plain, mode::keyed, mode::derive}) {
      for (const std::size_t first : {0u, 5u, 64u, 100u, 1024u, 2048u, 3072u}) {
        for (const bool first_last : {false, true}) {
          for (const empty_call e : {empty_call::update,
                                     empty_call::update_last,
                                     empty_call::both}) {
            for (const std::size_t rest : {0u, 1u, 1024u, 2048u}) {
              CAPTURE(static_cast<int>(m));
              CAPTURE(first);
              CAPTURE(first_last);
              CAPTURE(static_cast<int>(e));
              CAPTURE(rest);
              const auto bytes = std::span{input}.first(first + rest);
              blake3pp::hasher h = make(m);
              if (first_last) {
                h.update_last(bytes.first(first));
              } else {
                h.update(bytes.first(first));
              }
              for (int i = 0; i < 3; ++i) {
                if (e != empty_call::update_last) {
                  h.update(std::span<const std::byte>{});
                }
                if (e != empty_call::update) {
                  h.update_last(std::string_view{});
                }
              }
              h.update(bytes.subspan(first));

              blake3pp::hasher expected = make(m);
              expected.update(bytes);
              CHECK(h.count() == bytes.size());
              CHECK(h.finalize() == expected.finalize());
              std::array<std::byte, 200> xof{};
              std::array<std::byte, 200> expected_xof{};
              h.finalize_xof().fill(xof);
              expected.finalize_xof().fill(expected_xof);
              CHECK(xof == expected_xof);
            }
          }
        }
      }
    }
  }
}

// The extended output stream comes off the same root node, so a reader
// taken after update_last() must match one taken after update().
TEST_CASE("update_last leaves the same XOF stream") {
  const auto input = make_input(8 * 1024);
  blake3pp::hasher streamed;
  streamed.update(std::span{input});
  blake3pp::hasher last;
  last.update_last(std::span{input});

  std::array<std::byte, 200> a{};
  std::array<std::byte, 200> b{};
  streamed.finalize_xof().fill(a);
  last.finalize_xof().fill(b);
  CHECK(a == b);
  CHECK(last.finalize<64>() == streamed.finalize<64>());
}

// It is the tail of a message whose earlier bytes went through update(),
// which is how a streaming caller that learns of the end uses it: at a
// chunk boundary, mid-chunk, and after a pushed subtree.
TEST_CASE("update_last ends a message that update() began") {
  const auto input = make_input(40 * 1024 + 11);
  const auto expected = blake3pp::hash(input);

  for (const std::size_t split :
       {std::size_t{0}, std::size_t{1}, std::size_t{1024},
        std::size_t{3 * 1024}, std::size_t{3 * 1024 + 5},
        std::size_t{32 * 1024}, std::size_t{40 * 1024 + 11}}) {
    CAPTURE(split);
    blake3pp::hasher h;
    h.update(std::span{input}.first(split));
    h.update_last(std::span{input}.subspan(split));
    CHECK(h.finalize() == expected);
    CHECK(h.count() == input.size());
  }
}

// reset() has to drop the held subtree with everything else, or the next
// message finalizes off the previous one's root.
TEST_CASE("reset clears what update_last held") {
  const auto input = make_input(8 * 1024);
  blake3pp::hasher h;
  h.update_last(std::span{input});
  const auto first = h.finalize();
  h.reset();
  CHECK(h.count() == 0);
  h.update_last(std::span{input}.first(4 * 1024));
  CHECK(h.finalize() == blake3pp::hash(std::span{input}.first(4 * 1024)));
  CHECK(h.finalize() != first);
}

// Deep subtree recursion (8 MiB = 8192 chunks, 13 tree levels) with splits
// that force subtree offloads at misaligned counters.
TEST_CASE("multi-megabyte splits cross deep subtrees") {
  const auto input = make_input(8 * 1024 * 1024 + 5);
  const auto expected = blake3pp::hash(input);

  for (const std::size_t split :
       {std::size_t{3 * 1024 + 1}, std::size_t{1024 * 1024},
        std::size_t{5 * 1024 * 1024 + 333}}) {
    CAPTURE(split);
    blake3pp::hasher h;
    h.update(std::span{input}.first(split));
    h.update(std::span{input}.subspan(split));
    CHECK(h.finalize() == expected);
  }
}

TEST_CASE("byte at a time") {
  const auto input = make_input(3073);
  const auto expected = blake3pp::hash(input);

  blake3pp::hasher h;
  for (std::size_t i = 0; i < input.size(); ++i) {
    h.update(std::span{input}.subspan(i, 1));
  }
  CHECK(h.finalize() == expected);
}

// finalize() is const: it must neither disturb further updates nor be
// disturbed by them.
TEST_CASE("finalize is non-destructive") {
  const auto input = make_input(2049);

  blake3pp::hasher h;
  h.update(std::span{input}.first(1500));
  const auto mid = h.finalize();
  CHECK(mid == blake3pp::hash(std::span{input}.first(1500)));

  h.update(std::span{input}.subspan(1500));
  CHECK(h.finalize() == blake3pp::hash(input));
}

// The by-value extended-output form is a pure convenience over the span
// overload; both must produce the same stream, whose first 32 bytes are
// the plain digest.
TEST_CASE("finalize<N> matches the span overload and the digest prefix") {
  const auto input = make_input(4097);
  blake3pp::hasher h;
  h.update(input);

  const auto by_value = h.finalize<131>();
  std::array<std::byte, 131> by_span{};
  h.finalize(std::span<std::byte>{by_span});
  CHECK(by_value == by_span);

  const auto d = h.finalize();
  CHECK(std::equal(d.bytes.begin(), d.bytes.end(), by_value.begin()));
}

// take<N> is fill() by value: consecutive takes walk the same stream a
// single fill would produce, advancing the position identically.
TEST_CASE("output_reader take<N> reads and advances like fill") {
  const auto input = make_input(100);
  blake3pp::hasher h;
  h.update(input);

  auto r1 = h.finalize_xof();
  const auto a = r1.take<40>();
  const auto b = r1.take<24>();
  CHECK(r1.position() == 64);

  auto r2 = h.finalize_xof();
  std::array<std::byte, 64> whole{};
  r2.fill(whole);
  CHECK(std::equal(a.begin(), a.end(), whole.begin()));
  CHECK(std::equal(b.begin(), b.end(), whole.begin() + 40));
}

// BLAKE3's output ends at 2^64 - 1 bytes; past that it is unspecified,
// but a reader near the end must still give the same bytes however a
// request is split, as upstream's block-counter readers do. Each start is
// read one byte at a time, then in one fill and in split fills across the
// boundary, on every kernel; none of it may be the stream's start again.
TEST_CASE("XOF fills agree however they are split across byte 2^64 - 1") {
  constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  constexpr std::size_t len = 300;
  for (const auto a : blake3pp::all_arches()) {
    if (a == blake3pp::arch::auto_detect || !blake3pp::is_available(a)) {
      continue;
    }
    CAPTURE(arch_name(a));
    blake3pp::hasher h{blake3pp::detail::resolve(a)};
    h.update(std::string_view{"xof end"});
    std::array<std::byte, len> head{};
    h.finalize_xof().fill(head);

    // The reference: one byte per fill, which never takes the bulk path.
    const auto bytewise = [&](std::uint64_t start,
                              std::array<std::byte, len>& out) {
      auto r = h.finalize_xof();
      r.seek(start);
      for (std::byte& b : out) {
        r.fill(std::span{&b, 1});
      }
      return r;
    };

    for (const std::uint64_t start :
         {max - 299, max - 200, max - 69, max - 63, max - 5, max - 1, max}) {
      CAPTURE(start);
      std::array<std::byte, len> want{};
      bytewise(start, want);
      CHECK(want != head);

      for (const std::size_t first :
           {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{64},
            std::size_t{100}, std::size_t{236}}) {
        CAPTURE(first);
        auto r = h.finalize_xof();
        r.seek(start);
        CHECK(r.position() == start);
        std::array<std::byte, len> got{};
        r.fill(std::span{got}.first(first));
        r.fill(std::span{got}.subspan(first));
        CHECK(got == want);
        // And both readers are at the same place in the stream.
        std::array<std::byte, len> ignored{};
        auto ref = bytewise(start, ignored);
        CHECK(r.take<70>() == ref.take<70>());
      }
    }
    // position() is exact up to the last byte.
    auto r = h.finalize_xof();
    r.seek(max - 63);
    std::array<std::byte, 63> tail{};
    r.fill(tail);
    CHECK(r.position() == max);
  }
}

TEST_CASE("count reports bytes absorbed across every ingestion path") {
  const auto input = make_input(300 * 1024 + 7);
  blake3pp::hasher h;
  CHECK(h.count() == 0);
  h.update(std::span{input}.first(100));           // buffered path
  CHECK(h.count() == 100);
  h.update(std::span{input}.subspan(100, 200 * 1024));  // subtree fast path
  CHECK(h.count() == 100 + 200 * 1024);
  h.update(std::span{input}.subspan(100 + 200 * 1024));
  CHECK(h.count() == input.size());
  h.reset();
  CHECK(h.count() == 0);
}

// push_subtree_cv() after update() has consumed an exact chunk multiple.
// The hasher keeps its last full chunk open (it may still turn out to be
// ROOT), so the seam has to close it out before the subtree lands, or the
// subtree's counter is off by one and that chunk is silently dropped.
// This is the shape update_file() produces when files hash in sequence.
TEST_CASE("push_subtree_cv follows an update() ending on a chunk boundary") {
  constexpr std::size_t chunk = blake3pp::chunk_size;
  const auto input = make_input(8 * chunk + 100);
  const auto expected = blake3pp::hash(input);
  const std::span<const std::byte> bytes{input};

  // Leads are 2-chunk aligned positions, as the seam requires.
  for (const std::size_t lead : {std::size_t{2 * chunk}, std::size_t{4 * chunk},
                                 std::size_t{6 * chunk}}) {
    CAPTURE(lead);
    blake3pp::hasher h;
    h.update(bytes.first(lead));
    REQUIRE(h.count() == lead);

    // Subtree of the next 2 chunks at counter lead/chunk, then the rest
    // through update() so the final chunk stays with the hasher.
    std::array<std::uint32_t, 8> cv;
    blake3pp::detail::compress_subtree_cv(
        blake3pp::detail::resolve(h.selected_arch()), bytes.data() + lead,
        2, lead / chunk, h.key_words(), h.mode_flags(), cv);
    h.push_subtree_cv(cv, 2);
    CHECK(h.count() == lead + 2 * chunk);
    h.update(bytes.subspan(lead + 2 * chunk));
    CHECK(h.finalize() == expected);
  }
}

TEST_CASE("reset reuses the instance") {
  blake3pp::hasher h;
  h.update("some earlier message");
  h.reset();
  h.update("hello");
  blake3pp::hasher fresh;
  fresh.update("hello");
  CHECK(h.finalize() == fresh.finalize());
}

TEST_CASE("string_view overload matches span") {
  const std::string_view sv = "The quick brown fox jumps over the lazy dog";
  CHECK(blake3pp::hash(sv) ==
        blake3pp::hash(std::as_bytes(std::span{sv.data(), sv.size()})));
}

// The free to_hex covers any length; digest::to_hex is it over 32 bytes.
TEST_CASE("free to_hex matches digest::to_hex and handles any length") {
  const auto d = blake3pp::hash(std::string_view{"hex me"});
  CHECK(blake3pp::to_hex(d.bytes) == d.to_hex());

  blake3pp::hasher h;
  h.update(std::string_view{"hex me"});
  const auto wide = h.finalize<48>();
  const std::string s = blake3pp::to_hex(wide);
  CHECK(s.size() == 96);
  CHECK(s.substr(0, 64) == d.to_hex());

  std::array<char, 96> chars{};
  blake3pp::to_hex(wide, chars);
  CHECK(std::string_view{chars.data(), chars.size()} == s);
  CHECK(blake3pp::to_hex(std::span<const std::byte>{}).empty());
}

TEST_CASE("digest to_hex format") {
  const auto d = blake3pp::hash(std::string_view{});
  CHECK(d.to_hex().size() == 64u);
  CHECK(d.to_hex().find_first_not_of("0123456789abcdef") ==
        std::string::npos);
}

}  // TEST_SUITE

}  // namespace
