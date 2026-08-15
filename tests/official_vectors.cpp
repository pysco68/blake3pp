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
