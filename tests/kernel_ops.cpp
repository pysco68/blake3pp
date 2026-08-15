// Exercises the kernel dispatch seam directly: hash_many is the API the
// SIMD variants (M2) implement with lanes-as-inputs, so its scalar semantics
// are pinned down here as the oracle every variant must match.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <blake3pp/dispatch.hpp>
#include <doctest/doctest.h>

#include "kernel/kernel.hpp"

namespace {

using namespace blake3pp::kern;

TEST_SUITE("kernel_ops") {

TEST_CASE("arch introspection invariants") {
  const auto compiled = blake3pp::compiled_arches();
  REQUIRE(!compiled.empty());
  CHECK(compiled.back() == blake3pp::arch::scalar);  // fallback always in

  const auto avail = blake3pp::available_arches();
  REQUIRE(!avail.empty());
  CHECK(avail.front() == blake3pp::best_available());

  for (const auto a : avail) {
    CAPTURE(blake3pp::to_string(a));
    CHECK(blake3pp::is_available(a));
    CHECK(std::find(compiled.begin(), compiled.end(), a) != compiled.end());
  }
  // Anything compiled but not available must be a CPU limitation, and
  // resolve() must still hand back a usable table for it.
  for (const auto a : compiled) {
    CHECK(blake3pp::detail::resolve(a) != nullptr);
  }
}

TEST_CASE("scalar table is populated") {
  CHECK(std::string_view{scalar::ops.name} == "scalar");
  CHECK(scalar::ops.simd_degree == 1u);
  CHECK(scalar::ops.compress_in_place != nullptr);
  CHECK(scalar::ops.compress_xof != nullptr);
  CHECK(scalar::ops.hash_many != nullptr);
}

TEST_CASE("compress_xof's first 32 bytes agree with compress_in_place") {
  std::uint8_t block[block_len];
  for (std::size_t i = 0; i < block_len; ++i) {
    block[i] = static_cast<std::uint8_t>(i * 3 + 1);
  }
  std::uint32_t cv[8];
  std::memcpy(cv, iv, sizeof(cv));
  std::uint8_t wide[64];
  scalar::ops.compress_xof(cv, block, block_len, 42, flag_root, wide);
  scalar::ops.compress_in_place(cv, block, block_len, 42, flag_root);
  for (std::size_t w = 0; w < 8; ++w) {
    std::uint32_t word = 0;
    std::memcpy(&word, wide + 4 * w, 4);  // LE host assumed in tests
    CHECK(word == cv[w]);
  }
}

// Every variant's hash_many against the scalar oracle, on an input count
// that exercises both the full-batch path and the serial remainder.
TEST_CASE("hash_many agrees across all available arches") {
  constexpr std::size_t num_inputs = 33;  // not a multiple of any lane width
  constexpr std::size_t blocks = chunk_len / block_len;
  std::vector<std::uint8_t> data(num_inputs * blocks * block_len);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::uint8_t>(i % 251);
  }
  const std::uint8_t* inputs[num_inputs];
  for (std::size_t i = 0; i < num_inputs; ++i) {
    inputs[i] = data.data() + i * blocks * block_len;
  }

  std::vector<std::uint8_t> expected(num_inputs * out_len);
  scalar::ops.hash_many(inputs, num_inputs, blocks, iv, 100,
                        /*increment_counter=*/true, 0, flag_chunk_start,
                        flag_chunk_end, expected.data());

  for (const auto a :
       {blake3pp::arch::sse42, blake3pp::arch::avx2, blake3pp::arch::avx512,
        blake3pp::arch::neon}) {
    if (!blake3pp::is_available(a)) {
      continue;
    }
    CAPTURE(blake3pp::to_string(a));
    const auto* ops = blake3pp::detail::resolve(a);
    REQUIRE(ops != nullptr);
    CHECK(ops->simd_degree > 1u);
    std::vector<std::uint8_t> out(num_inputs * out_len);
    ops->hash_many(inputs, num_inputs, blocks, iv, 100,
                   /*increment_counter=*/true, 0, flag_chunk_start,
                   flag_chunk_end, out.data());
    CHECK(out == expected);
  }
}

TEST_CASE("hash_many matches a per-input compress loop") {
  // Four "chunks" of 2 blocks each, deterministic contents.
  constexpr std::size_t num_inputs = 4;
  constexpr std::size_t blocks = 2;
  std::vector<std::uint8_t> data(num_inputs * blocks * block_len);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::uint8_t>((i * 7 + 3) % 251);
  }
  const std::uint8_t* inputs[num_inputs];
  for (std::size_t i = 0; i < num_inputs; ++i) {
    inputs[i] = data.data() + i * blocks * block_len;
  }

  constexpr std::uint64_t counter = 17;
  std::uint8_t out[num_inputs * out_len];
  scalar::ops.hash_many(inputs, num_inputs, blocks, iv, counter,
                        /*increment_counter=*/true, 0, flag_chunk_start,
                        flag_chunk_end, out);

  for (std::size_t i = 0; i < num_inputs; ++i) {
    CAPTURE(i);
    std::uint32_t cv[8];
    std::memcpy(cv, iv, sizeof(cv));
    for (std::size_t b = 0; b < blocks; ++b) {
      std::uint32_t flags = 0;
      if (b == 0) flags |= flag_chunk_start;
      if (b == blocks - 1) flags |= flag_chunk_end;
      scalar::ops.compress_in_place(cv, inputs[i] + b * block_len,
                                    static_cast<std::uint32_t>(block_len),
                                    counter + i, flags);
    }
    std::uint8_t expected[out_len];
    for (std::size_t w = 0; w < 8; ++w) {
      for (std::size_t byte = 0; byte < 4; ++byte) {
        expected[4 * w + byte] = static_cast<std::uint8_t>(cv[w] >> (8 * byte));
      }
    }
    CHECK(std::memcmp(out + i * out_len, expected, out_len) == 0);
  }
}

}  // TEST_SUITE

}  // namespace
