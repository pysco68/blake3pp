#include <cstddef>
#include <string>
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

TEST_CASE("known answer for the empty input") {
  CHECK(blake3pp::hash(std::string_view{}).to_hex() ==
        "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}

TEST_CASE("explicitly selecting the scalar arch matches") {
  for (const auto& c : blake3pp::testvec::cases) {
    CAPTURE(c.input_len);
    const auto input = make_input(c.input_len);
    blake3pp::hasher h{blake3pp::arch::scalar};
    h.update(input);
    CHECK(h.finalize().to_hex() == std::string(c.hash).substr(0, 64));
  }
}

}  // TEST_SUITE

}  // namespace
