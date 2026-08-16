#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <blake3pp/blake3pp.hpp>
#include <doctest/doctest.h>

#include "test_vectors.hpp"

namespace {

// The spec's test input: input_len bytes of the repeating pattern 0..250.
std::vector<std::byte> make_input(std::size_t len) {
  std::vector<std::byte> v(len);
  for (std::size_t i = 0; i < len; ++i) {
    v[i] = static_cast<std::byte>(i % 251);
  }
  return v;
}

TEST_SUITE("official_vectors") {

TEST_CASE("one-shot hash matches every official vector") {
  for (const auto& c : blake3pp::testvec::cases) {
    CAPTURE(c.input_len);
    const auto input = make_input(c.input_len);
    CHECK(blake3pp::hash(input).to_hex() == std::string(c.hash).substr(0, 64));
  }
}

TEST_CASE("keyed hash matches every official vector") {
  constexpr std::string_view key_str = blake3pp::testvec::key;
  static_assert(key_str.size() == 32);
  const auto key =
      std::as_bytes(std::span<const char, 32>{key_str.data(), 32});
  for (const auto& c : blake3pp::testvec::cases) {
    CAPTURE(c.input_len);
    const auto input = make_input(c.input_len);
    CHECK(blake3pp::keyed_hash(key, input).to_hex() ==
          std::string(c.keyed_hash).substr(0, 64));
  }
}

TEST_CASE("derive_key matches every official vector") {
  for (const auto& c : blake3pp::testvec::cases) {
    CAPTURE(c.input_len);
    const auto input = make_input(c.input_len);
    CHECK(blake3pp::derive_key(blake3pp::testvec::context, input).to_hex() ==
          std::string(c.derive_key).substr(0, 64));
  }
}

TEST_CASE("keyed and derive_key match vectors on every available arch") {
  constexpr std::string_view key_str = blake3pp::testvec::key;
  const auto key =
      std::as_bytes(std::span<const char, 32>{key_str.data(), 32});
  for (const auto a : blake3pp::available_arches()) {
    CAPTURE(blake3pp::to_string(a));
    for (const auto& c : blake3pp::testvec::cases) {
      CAPTURE(c.input_len);
      const auto input = make_input(c.input_len);
      blake3pp::hasher kh = blake3pp::hasher::keyed(key, a);
      kh.update(input);
      CHECK(kh.finalize().to_hex() ==
            std::string(c.keyed_hash).substr(0, 64));
      blake3pp::hasher dk =
          blake3pp::hasher::derive_key(blake3pp::testvec::context, a);
      dk.update(input);
      CHECK(dk.finalize().to_hex() ==
            std::string(c.derive_key).substr(0, 64));
    }
  }
}

// The official vectors carry 131 bytes of extended output per case; with
// XOF we can verify every byte of every mode, not just the 32-byte prefix.
TEST_CASE("extended output matches the full 131-byte vectors, all modes") {
  constexpr std::string_view key_str = blake3pp::testvec::key;
  const auto key =
      std::as_bytes(std::span<const char, 32>{key_str.data(), 32});

  const auto to_hex = [](std::span<const std::byte> bytes) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string s;
    for (const std::byte b : bytes) {
      s += alphabet[std::to_integer<unsigned>(b) >> 4];
      s += alphabet[std::to_integer<unsigned>(b) & 0xF];
    }
    return s;
  };

  for (const auto& c : blake3pp::testvec::cases) {
    CAPTURE(c.input_len);
    const auto input = make_input(c.input_len);
    std::vector<std::byte> out(std::string_view{c.hash}.size() / 2);  // 131

    blake3pp::hasher h;
    h.update(input);
    h.finalize(out);
    CHECK(to_hex(out) == c.hash);

    blake3pp::hasher kh = blake3pp::hasher::keyed(key);
    kh.update(input);
    kh.finalize(out);
    CHECK(to_hex(out) == c.keyed_hash);

    blake3pp::hasher dk =
        blake3pp::hasher::derive_key(blake3pp::testvec::context);
    dk.update(input);
    dk.finalize(out);
    CHECK(to_hex(out) == c.derive_key);
  }
}

TEST_CASE("output_reader streams, seeks, and agrees with the digest") {
  blake3pp::hasher h;
  h.update("xof me");

  std::vector<std::byte> big(1000);
  h.finalize_xof().fill(big);

  // Piecewise fills produce the same stream.
  {
    auto r = h.finalize_xof();
    std::vector<std::byte> pieced(big.size());
    std::size_t pos = 0;
    for (const std::size_t piece : {std::size_t{1}, std::size_t{7},
                                    std::size_t{64}, std::size_t{129}}) {
      r.fill(std::span{pieced}.subspan(pos, piece));
      pos += piece;
    }
    r.fill(std::span{pieced}.subspan(pos));
    CHECK(pieced == big);
  }

  // Seeking is random access into the same stream.
  {
    auto r = h.finalize_xof();
    r.seek(123);
    CHECK(r.position() == 123);
    std::vector<std::byte> window(100);
    r.fill(window);
    CHECK(r.position() == 223);
    CHECK(std::equal(window.begin(), window.end(), big.begin() + 123));
  }

  // Long fills take the lanes-wide xof_many path; they must equal the
  // block-at-a-time stream exactly.
  {
    auto bulk = h.finalize_xof();
    std::vector<std::byte> wide(64 * 1024 + 13);
    bulk.fill(wide);
    auto slow = h.finalize_xof();
    std::vector<std::byte> stepped(wide.size());
    for (std::size_t pos = 0; pos < stepped.size(); pos += 64) {
      slow.fill(std::span{stepped}.subspan(pos,
                                           std::min<std::size_t>(
                                               64, stepped.size() - pos)));
    }
    CHECK(wide == stepped);
  }

  // The digest is the stream's first 32 bytes.
  const auto d = h.finalize();
  CHECK(std::equal(d.bytes.begin(), d.bytes.end(), big.begin()));
}

TEST_CASE("known answer for the empty input") {
  CHECK(blake3pp::hash(std::string_view{}).to_hex() ==
        "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}

TEST_CASE("every available arch matches the official vectors") {
  for (const auto a :
       {blake3pp::arch::scalar, blake3pp::arch::sse42, blake3pp::arch::avx2,
        blake3pp::arch::avx512, blake3pp::arch::neon}) {
    if (!blake3pp::is_available(a)) {
      MESSAGE("skipping unavailable arch: " << blake3pp::to_string(a));
      continue;
    }
    CAPTURE(blake3pp::to_string(a));
    for (const auto& c : blake3pp::testvec::cases) {
      CAPTURE(c.input_len);
      const auto input = make_input(c.input_len);
      blake3pp::hasher h{a};
      h.update(input);
      CHECK(h.finalize().to_hex() == std::string(c.hash).substr(0, 64));
    }
  }
}

}  // TEST_SUITE

}  // namespace
