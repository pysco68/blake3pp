#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <ostream>
#include <string_view>
#include <vector>

#include <blake3pp/blake3pp.hpp>
#include <doctest/doctest.h>

namespace {

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
