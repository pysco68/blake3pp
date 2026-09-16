// The two subtree reductions must be computationally identical: the
// recursion that holds a CV buffer per level, and the fold that walks groups
// of 2*simd_degree chunks into a binary-counter stack (src/core/subtree.hpp).
// Every compiled kernel runs both over every subtree size the engine can
// hand them, since the group size follows the kernel's simd_degree and the
// interesting sizes are the ones around it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

#include <blake3pp/core.hpp>
#include <doctest/doctest.h>

#include "core/subtree.hpp"
#include "kernel/kernel.hpp"

namespace {

[[nodiscard]] std::string_view arch_name(blake3pp::arch a) noexcept {
  return blake3pp::to_string(a);
}

// Deterministic, non-repeating bytes: a repeating pattern would hide an
// error that swaps two chunks or two CVs.
[[nodiscard]] std::vector<std::uint8_t> pattern(std::size_t bytes) {
  std::vector<std::uint8_t> out(bytes);
  std::uint32_t x = 0x243f'6a88;
  for (auto& b : out) {
    x = x * 1'664'525u + 1'013'904'223u;
    b = static_cast<std::uint8_t>(x >> 24);
  }
  return out;
}

}  // namespace

TEST_SUITE("subtree_fold") {

TEST_CASE("the fold and the recursion agree on every kernel and size") {
  const std::array<std::uint32_t, 8> key = blake3pp::kern::iv;
  // available_arches(), not compiled_arches(): resolve() hands back a
  // fallback table for a variant this CPU cannot run, so iterating the
  // compiled set would silently test the same kernel repeatedly and claim
  // coverage it does not have. A variant therefore has to meet this test on
  // a machine that can execute it.
  for (const auto a : blake3pp::available_arches()) {
    CAPTURE(arch_name(a));
    const auto* ops = blake3pp::detail::resolve(a);
    REQUIRE(ops != nullptr);
    // Sizes span one group and many: 2 chunks is the smallest subtree, 2048
    // is 64 groups of the widest kernel.
    for (std::size_t chunks = 2; chunks <= 2048; chunks *= 2) {
      CAPTURE(chunks);
      const auto data = pattern(chunks * blake3pp::chunk_size);
      // A non-zero counter and a mode flag travel through both paths: the
      // counter feeds every chunk's compression, the flag every node's.
      for (const std::uint64_t counter : {std::uint64_t{0}, std::uint64_t{64}}) {
        CAPTURE(counter);
        std::array<std::uint32_t, 8> from_recursion{};
        std::array<std::uint32_t, 8> from_fold{};
        blake3pp::core::compress_subtree_to_cv_recursive(
            *ops, data.data(), chunks, counter, key, 0, from_recursion);
        blake3pp::core::compress_subtree_to_cv_folded<>(
            *ops, data.data(), chunks, counter, key, 0, from_fold);
        CHECK(from_fold == from_recursion);
      }
    }
  }
}

TEST_CASE("a stack bound that just fits still folds") {
  // log2(num_chunks / group) + 1 entries are needed; the smallest group is
  // 2 chunks (scalar), so 2048 chunks need at most 11.
  const std::array<std::uint32_t, 8> key = blake3pp::kern::iv;
  const auto* ops = blake3pp::detail::resolve(blake3pp::arch::scalar);
  REQUIRE(ops != nullptr);
  const auto data = pattern(2048 * blake3pp::chunk_size);
  std::array<std::uint32_t, 8> wide{};
  std::array<std::uint32_t, 8> tight{};
  blake3pp::core::compress_subtree_to_cv_folded<54>(*ops, data.data(), 2048, 0,
                                                    key, 0, wide);
  blake3pp::core::compress_subtree_to_cv_folded<11>(*ops, data.data(), 2048, 0,
                                                    key, 0, tight);
  CHECK(tight == wide);
}

}  // TEST_SUITE
