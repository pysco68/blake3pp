#include <cstddef>
#include <span>
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

TEST_CASE("digest to_hex format") {
  const auto d = blake3pp::hash(std::string_view{});
  CHECK(d.to_hex().size() == 64u);
  CHECK(d.to_hex().find_first_not_of("0123456789abcdef") ==
        std::string::npos);
}

}  // TEST_SUITE

}  // namespace
