// The subtree reductions in src/core/subtree.hpp -- the grouped sweep the
// library uses by default and the single-leaf fold a tight target opts
// into -- must produce the tree the spec defines. They are checked against
// an oracle that shares none of their code: every block through the scalar
// kernel's compress_in_place, the chunk state and parent nodes of the
// serial hasher, and the tree built pairwise by recursion. Every
// available kernel runs over every subtree size the engine can hand it,
// since the group size follows the kernel's simd_degree and the
// interesting sizes are the ones around it.
//
// The same file covers detail::fold_sibling_cvs, which takes the other way
// in: the parallel engine hashes a window's parts separately, and the fold
// joins their chaining values into the one the window would have had if a
// single core had hashed it.

#include <algorithm>
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

// The same bytes as std::byte, which is the currency of the public
// seams; the fold's inputs come from detail::compress_subtree_cv.
[[nodiscard]] std::vector<std::byte> pattern_bytes(std::size_t bytes) {
  std::vector<std::byte> out(bytes);
  std::uint32_t x = 0x243f'6a88;
  for (auto& b : out) {
    x = x * 1'664'525u + 1'013'904'223u;
    b = static_cast<std::byte>(x >> 24);
  }
  return out;
}

// One buffer for every fold case, the cases hashing prefixes of it.
[[nodiscard]] std::span<const std::byte> fold_bytes() {
  static const std::vector<std::byte> data =
      pattern_bytes(8192 * blake3pp::chunk_size);
  return data;
}

// A mode's key schedule and domain flags, taken from a hasher in that
// mode: what the engine passes the seam alongside the part CVs.
struct mode_key {
  std::string_view name;
  std::array<std::uint32_t, 8> key;
  std::uint32_t flags;
};

[[nodiscard]] mode_key from_hasher(std::string_view name,
                                   const blake3pp::hasher& h) {
  mode_key m{name, {}, h.mode_flags()};
  const auto words = h.key_words();
  for (std::size_t i = 0; i < 8; ++i) {
    m.key[i] = words[i];
  }
  return m;
}

[[nodiscard]] std::vector<mode_key> modes() {
  std::array<std::byte, blake3pp::key_size> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(0x40u + i);
  }
  return {from_hasher("plain", blake3pp::hasher{}),
          from_hasher("keyed", blake3pp::hasher::keyed(key)),
          from_hasher("derive_key",
                      blake3pp::hasher::derive_key("blake3pp fold tests"))};
}

// One case: the parts' CVs folded must equal the CV of the whole range.
void check_fold(const blake3pp::kern::kernel_ops* ops, std::size_t part,
                std::size_t parts, const mode_key& m,
                std::uint64_t counter) {
  const std::size_t total = part * parts;
  const std::span<const std::byte> data =
      fold_bytes().first(total * blake3pp::chunk_size);
  std::vector<std::array<std::uint32_t, 8>> cvs(parts);
  for (std::size_t i = 0; i < parts; ++i) {
    blake3pp::detail::compress_subtree_cv(
        ops, data.data() + i * part * blake3pp::chunk_size, part,
        counter + i * part, m.key, m.flags, cvs[i]);
  }
  std::array<std::uint32_t, 8> folded{};
  blake3pp::detail::fold_sibling_cvs(ops, cvs, m.key, m.flags, folded);
  std::array<std::uint32_t, 8> whole{};
  blake3pp::detail::compress_subtree_cv(ops, data.data(), total, counter,
                                        m.key, m.flags, whole);
  CHECK(folded == whole);
}

// The oracle: a subtree's CV from one compress_in_place per block, on the
// scalar kernel whatever kernel is under test, and the parents built
// pairwise. No hash_many anywhere, which is what every reduction under
// test is made of.
[[nodiscard]] std::array<std::uint32_t, 8> serial_subtree_cv(
    const std::uint8_t* input, std::size_t chunks, std::uint64_t counter,
    std::span<const std::uint32_t, 8> key, std::uint32_t flags) {
  const blake3pp::kern::kernel_ops& scalar =
      *blake3pp::detail::resolve(blake3pp::arch::scalar);
  std::array<std::uint32_t, 8> cv{};
  if (chunks == 1) {
    blake3pp::detail::chunk_state cs{};
    blake3pp::core::chunk_init(cs, key, counter);
    blake3pp::core::chunk_update(scalar, cs, input, blake3pp::kern::chunk_len,
                                 flags);
    blake3pp::core::chaining_value(scalar,
                                   blake3pp::core::chunk_output(cs, flags), cv);
    return cv;
  }
  const std::size_t half = chunks / 2;
  const auto left = serial_subtree_cv(input, half, counter, key, flags);
  const auto right =
      serial_subtree_cv(input + half * blake3pp::kern::chunk_len, half,
                        counter + half, key, flags);
  blake3pp::core::chaining_value(
      scalar, blake3pp::core::parent_output(left, right, key, flags), cv);
  return cv;
}

// What a part reduction stopped at `stop` must hand back: the CVs of
// equal adjacent subtrees, min(chunks, stop, 2 * simd_degree) of them --
// a stop past the kernel's last full-width generation gets that one.
[[nodiscard]] std::vector<std::array<std::uint32_t, 8>> serial_part_cvs(
    const blake3pp::kern::kernel_ops& ops, const std::uint8_t* input,
    std::size_t chunks, std::uint64_t counter,
    std::span<const std::uint32_t, 8> key, std::uint32_t flags,
    std::size_t stop) {
  const std::size_t parts =
      std::min(chunks, std::min(stop, 2 * ops.simd_degree));
  const std::size_t sub = chunks / parts;
  std::vector<std::array<std::uint32_t, 8>> cvs;
  for (std::size_t i = 0; i < parts; ++i) {
    cvs.push_back(serial_subtree_cv(input + i * sub * blake3pp::kern::chunk_len,
                                    sub, counter + i * sub, key, flags));
  }
  return cvs;
}

// One part reduction at a chosen burst size, trimmed to the CV count the
// oracle gives for the same stop.
template <std::size_t Leaves>
[[nodiscard]] std::vector<std::array<std::uint32_t, 8>> reduce_part(
    const blake3pp::kern::kernel_ops& ops,
    const std::vector<std::uint8_t>& data, std::size_t chunks,
    std::size_t stop, std::span<const std::uint32_t, 8> key, std::size_t n) {
  std::vector<std::array<std::uint32_t, 8>> cvs(64);
  const std::size_t got = blake3pp::core::compress_subtree_to_cvs_grouped<Leaves>(
      ops, data.data(), chunks, 3, key, 0, cvs, stop);
  CHECK(got == n);
  cvs.resize(got);
  return cvs;
}

}  // namespace

TEST_SUITE("subtree_fold") {

TEST_CASE("the reductions agree with the oracle on every kernel and size") {
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
      // A non-zero counter and a mode flag travel through every path: the
      // counter feeds every chunk's compression, the flag every node's.
      for (const std::uint64_t counter : {std::uint64_t{0}, std::uint64_t{64}}) {
        CAPTURE(counter);
        for (const std::uint32_t flags :
             {std::uint32_t{0}, std::uint32_t{blake3pp::kern::flag_keyed_hash}}) {
          CAPTURE(flags);
          const auto expected =
              serial_subtree_cv(data.data(), chunks, counter, key, flags);
          std::array<std::uint32_t, 8> from_fold{};
          std::array<std::uint32_t, 8> from_group{};
          // The fold: the sweep with one leaf per burst, which a
          // BLAKE3PP_SUBTREE_FOLD build selects.
          blake3pp::core::compress_subtree_to_cv_grouped<1>(
              *ops, data.data(), chunks, counter, key, flags, from_fold);
          blake3pp::core::compress_subtree_to_cv_grouped<>(
              *ops, data.data(), chunks, counter, key, flags, from_group);
          CHECK(from_fold == expected);
          CHECK(from_group == expected);
        }
      }
    }
  }
}

// The burst size is a tuning parameter, so every value of it has to produce
// the same tree: one leaf per group is the fold's shape, and a group wider
// than the whole subtree is the degenerate single-group case.
TEST_CASE("every group size reduces to the same CV") {
  const std::array<std::uint32_t, 8> key = blake3pp::kern::iv;
  for (const auto a : blake3pp::available_arches()) {
    CAPTURE(arch_name(a));
    const auto* ops = blake3pp::detail::resolve(a);
    REQUIRE(ops != nullptr);
    for (std::size_t chunks = 2; chunks <= 512; chunks *= 2) {
      CAPTURE(chunks);
      const auto data = pattern(chunks * blake3pp::chunk_size);
      const auto expected = serial_subtree_cv(data.data(), chunks, 7, key, 0);
      std::array<std::uint32_t, 8> g1{};
      std::array<std::uint32_t, 8> g4{};
      std::array<std::uint32_t, 8> g64{};
      blake3pp::core::compress_subtree_to_cv_grouped<1>(
          *ops, data.data(), chunks, 7, key, 0, g1);
      blake3pp::core::compress_subtree_to_cv_grouped<4>(
          *ops, data.data(), chunks, 7, key, 0, g4);
      blake3pp::core::compress_subtree_to_cv_grouped<64>(
          *ops, data.data(), chunks, 7, key, 0, g64);
      CHECK(g1 == expected);
      CHECK(g4 == expected);
      CHECK(g64 == expected);
    }
  }
}

// The part-shaped reduction, which stops several CVs short of the root:
// it has to agree with the oracle CV for CV at every stop, since the
// engine folds those CVs across parts afterwards and a different split of
// the same tree would still hash to the same root while handing the fold
// something else.
TEST_CASE("the part reductions agree at every stop") {
  const std::array<std::uint32_t, 8> key = blake3pp::kern::iv;
  for (const auto a : blake3pp::available_arches()) {
    CAPTURE(arch_name(a));
    const auto* ops = blake3pp::detail::resolve(a);
    REQUIRE(ops != nullptr);
    for (std::size_t chunks = 1; chunks <= 2048; chunks *= 2) {
      CAPTURE(chunks);
      const auto data = pattern(chunks * blake3pp::chunk_size);
      // Every stop the engine can ask for, from the whole subtree as one
      // CV to one CV per chunk, which is what a part narrower than the
      // kernel's lanes leaves.
      for (std::size_t stop = 1; stop <= 64; stop *= 2) {
        CAPTURE(stop);
        const auto expected =
            serial_part_cvs(*ops, data.data(), chunks, 11, key, 0, stop);
        std::vector<std::array<std::uint32_t, 8>> grouped(64);
        // The third shape is what a BLAKE3PP_SUBTREE_FOLD build selects:
        // the sweep with one leaf per burst and the switch's level count
        // as its stack bound. It has to agree too, since a part's CVs
        // reach the same tree whichever build produced them.
        std::vector<std::array<std::uint32_t, 8>> folded(64);
        const std::size_t n_grp =
            blake3pp::core::compress_subtree_to_cvs_grouped<>(
                *ops, data.data(), chunks, 11, key, 0, grouped, stop);
        const std::size_t n_fold =
            blake3pp::core::compress_subtree_to_cvs<12>(
                *ops, data.data(), chunks, 11, key, 0, folded, stop);
        REQUIRE(n_grp == expected.size());
        REQUIRE(n_fold == expected.size());
        grouped.resize(n_grp);
        folded.resize(n_fold);
        CHECK(grouped == expected);
        CHECK(folded == expected);
      }
    }
  }
}

// The burst size is a tuning parameter here too, and a part's stop makes
// the two cases of the sweep differ: a group wider than one output CV's
// chunks emits several CVs at once, a narrower one sweeps per CV.
TEST_CASE("every group size reduces a part the same way") {
  const std::array<std::uint32_t, 8> key = blake3pp::kern::iv;
  for (const auto a : blake3pp::available_arches()) {
    CAPTURE(arch_name(a));
    const auto* ops = blake3pp::detail::resolve(a);
    REQUIRE(ops != nullptr);
    for (std::size_t chunks = 2; chunks <= 1024; chunks *= 2) {
      CAPTURE(chunks);
      const auto data = pattern(chunks * blake3pp::chunk_size);
      for (const std::size_t stop : {std::size_t{2}, std::size_t{16}}) {
        CAPTURE(stop);
        const auto expected =
            serial_part_cvs(*ops, data.data(), chunks, 3, key, 0, stop);
        const std::size_t n = expected.size();
        for (const auto& got : {reduce_part<1>(*ops, data, chunks, stop, key, n),
                                reduce_part<4>(*ops, data, chunks, stop, key, n),
                                reduce_part<64>(*ops, data, chunks, stop, key,
                                                n)}) {
          CHECK(got == expected);
        }
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
  blake3pp::core::compress_subtree_to_cv_grouped<1, 54>(
      *ops, data.data(), 2048, 0, key, 0, wide);
  blake3pp::core::compress_subtree_to_cv_grouped<1, 11>(
      *ops, data.data(), 2048, 0, key, 0, tight);
  CHECK(tight == wide);
}

TEST_CASE("the fold reduces part CVs to the CV of the whole range") {
  const std::vector<mode_key> ms = modes();
  for (const auto a : blake3pp::available_arches()) {
    CAPTURE(arch_name(a));
    const auto* ops = blake3pp::detail::resolve(a);
    REQUIRE(ops != nullptr);
    // The fold's shape follows the number of CVs alone, the part size only
    // deciding what each one is, so the largest part counts run at the
    // smallest part. 512 parts of 16 chunks is the shape an 8 MiB window
    // hands it at the default budget.
    struct shape {
      std::size_t part;
      std::size_t parts;
    };
    for (const shape s :
         {shape{2, 2}, shape{2, 4}, shape{2, 8}, shape{2, 64}, shape{2, 512},
          shape{2, 1024}, shape{16, 2}, shape{16, 4}, shape{16, 8},
          shape{16, 64}, shape{16, 512}}) {
      CAPTURE(s.part);
      CAPTURE(s.parts);
      for (const auto& m : ms) {
        CAPTURE(m.name);
        // Counter 0 and an aligned counter further in: the counter feeds
        // every chunk's compression, so a fold that dropped it would still
        // agree at 0.
        for (const std::uint64_t counter :
             {std::uint64_t{0}, static_cast<std::uint64_t>(s.part * s.parts)}) {
          CAPTURE(counter);
          check_fold(ops, s.part, s.parts, m, counter);
        }
      }
    }
  }
}

TEST_CASE("the fold crosses the batch its hash_many calls are shaped to") {
  // One call takes kern::max_batch_inputs parents, so a generation runs in
  // batches of twice that many children. The counts below sit on both
  // sides of one batch and of several.
  constexpr std::size_t per_call = 2 * blake3pp::kern::max_batch_inputs;
  const std::vector<mode_key> ms = modes();
  const auto* widest = blake3pp::detail::resolve(blake3pp::arch::auto_detect);
  const auto* scalar = blake3pp::detail::resolve(blake3pp::arch::scalar);
  REQUIRE(widest != nullptr);
  REQUIRE(scalar != nullptr);
  for (const auto* ops : {widest, scalar}) {
    for (const std::size_t parts :
         {per_call / 4, per_call / 2, per_call, 2 * per_call, 4 * per_call}) {
      CAPTURE(parts);
      check_fold(ops, 2, parts, ms.front(), 0);
    }
  }
}

TEST_CASE("the fold may write its result into the array it folded") {
  const auto* ops = blake3pp::detail::resolve(blake3pp::arch::auto_detect);
  REQUIRE(ops != nullptr);
  const mode_key m = modes().front();
  constexpr std::size_t part = 2;
  constexpr std::size_t parts = 64;
  const std::span<const std::byte> data =
      fold_bytes().first(part * parts * blake3pp::chunk_size);
  std::vector<std::array<std::uint32_t, 8>> cvs(parts);
  for (std::size_t i = 0; i < parts; ++i) {
    blake3pp::detail::compress_subtree_cv(
        ops, data.data() + i * part * blake3pp::chunk_size, part,
        i * part, m.key, m.flags, cvs[i]);
  }
  std::vector<std::array<std::uint32_t, 8>> copy = cvs;
  std::array<std::uint32_t, 8> apart{};
  blake3pp::detail::fold_sibling_cvs(ops, copy, m.key, m.flags, apart);
  blake3pp::detail::fold_sibling_cvs(ops, cvs, m.key, m.flags, cvs[0]);
  CHECK(cvs[0] == apart);
}

}  // TEST_SUITE
