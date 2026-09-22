// The order-free reducer must be indistinguishable from absorbing the same
// subtrees left to right: same digest as one update() over the whole
// message, same digest as the in-order push_subtree_cv path, and a pending
// set that is exactly the canonical decomposition of what has arrived.
// Every case therefore carries its own reference digest, and the structural
// cases check how many nodes an arrival order leaves unmerged, which is
// what sizes a pipeline's storage.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/tree_reducer.hpp>
#include <blake3pp/dispatch.hpp>
#include <doctest/doctest.h>

namespace {

using blake3pp::chunk_size;
using blake3pp::digest;
using blake3pp::hasher;
using blake3pp::detail::tree_reducer;

// Fixed so a failure reproduces from the reported parameters alone.
constexpr std::uint64_t seed = 0x5eed'b1a3'e300'0001ULL;

// How many random cases the two property tests run.
//
// They hash a couple of gigabytes between them, which is seconds on a
// plain lane and about four minutes each under GCC with address and
// undefined behaviour sanitizers -- long enough that a real stall and an
// honest case stop being distinguishable by a timeout. The sanitizer
// lanes run a prefix of the same sequence instead: the seed is fixed, so
// case i is case i whatever the count, and a failure a short lane finds
// reproduces in a long one.
#if defined(BLAKE3PP_TEST_REDUCED_CASES)
constexpr int order_cases = 12;
constexpr int split_cases = 6;
#else
constexpr int order_cases = 100;
constexpr int split_cases = 50;
#endif

// Deterministic, non-repeating bytes: a repeating pattern would hide an
// error that swaps two chunks or two CVs.
[[nodiscard]] std::vector<std::byte> pattern(std::size_t bytes) {
  std::vector<std::byte> out(bytes);
  std::uint32_t x = 0x243f'6a88;
  for (auto& b : out) {
    x = x * 1'664'525u + 1'013'904'223u;
    b = static_cast<std::byte>(x >> 24);
  }
  return out;
}

// One buffer for every case, prefixes of it being the messages: the cases
// hash a couple of gigabytes between them, and regenerating the bytes per
// case would cost more than the hashing does.
[[nodiscard]] std::span<const std::byte> message_bytes() {
  static const std::vector<std::byte> data = pattern(13'440 * chunk_size);
  return data;
}

// A string_view, not the const char* to_string() hands back: doctest
// stringifies a pointer as its value.
[[nodiscard]] std::string_view arch_name(blake3pp::arch a) noexcept {
  return blake3pp::to_string(a);
}

enum class mode { plain, keyed, derive_key };

[[nodiscard]] std::string_view mode_name(mode m) noexcept {
  switch (m) {
    case mode::keyed:
      return "keyed";
    case mode::derive_key:
      return "derive_key";
    case mode::plain:
      break;
  }
  return "plain";
}

[[nodiscard]] hasher make_hasher(mode m, blake3pp::arch a) noexcept {
  switch (m) {
    case mode::keyed: {
      std::array<std::byte, blake3pp::key_size> key{};
      for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::byte>(0x40u + i);
      }
      return hasher::keyed(key, a);
    }
    case mode::derive_key:
      return hasher::derive_key("blake3pp tree_reducer tests", a);
    case mode::plain:
      break;
  }
  return hasher(a);
}

// The aligned power-of-two decomposition of [lo, hi), computed without the
// reducer: at each position the largest aligned node that still fits. The
// reducer merges exactly while both children are present, so a contiguous
// run must collapse to this and nothing else.
struct range {
  std::uint64_t first_chunk;
  std::uint64_t chunks;
};

[[nodiscard]] std::vector<range> canonical(std::uint64_t lo,
                                           std::uint64_t hi) {
  std::vector<range> out;
  for (std::uint64_t p = lo; p < hi;) {
    std::uint64_t s = std::bit_floor(hi - p);
    if (p != 0) {
      s = std::min(s, p & (~p + 1));  // the alignment p already has
    }
    out.push_back(range{p, s});
    p += s;
  }
  return out;
}

void check_decomposition(std::span<const tree_reducer::node> got,
                         std::uint64_t lo, std::uint64_t hi) {
  const std::vector<range> want = canonical(lo, hi);
  REQUIRE(got.size() == want.size());
  for (std::size_t i = 0; i < want.size(); ++i) {
    CAPTURE(i);
    CHECK(got[i].first_chunk == want[i].first_chunk);
    CHECK(got[i].chunks == want[i].chunks);
  }
}

// Structural cases assert which nodes are pending, not what they hash to,
// so their CVs are labels rather than subtree hashes.
[[nodiscard]] tree_reducer::cv_type label(std::uint64_t index) noexcept {
  tree_reducer::cv_type cv{};
  for (std::size_t i = 0; i < cv.size(); ++i) {
    cv[i] = static_cast<std::uint32_t>(index * 8 + i);
  }
  return cv;
}

struct case_params {
  mode m = mode::plain;
  std::uint64_t window = 8;   // chunks per window
  std::uint64_t base = 0;     // windows absorbed through update() first
  std::uint64_t windows = 1;  // windows handed to the reducer
  std::size_t tail = 1;       // bytes following them
  bool split_halves = false;  // insert some windows as their two children
  bool clear_first = false;   // reuse a reducer that was used and cleared
  blake3pp::arch a = blake3pp::arch::auto_detect;
};

struct piece {
  std::uint64_t first_chunk;
  std::uint64_t chunks;
  tree_reducer::cv_type cv;
};

// One property case: hash the message three ways -- one update(), the
// in-order push_subtree_cv path, and the reducer fed in a random order --
// and require all three to agree. Returns the maximum pending() seen.
[[nodiscard]] std::size_t run_case(const case_params& p, std::mt19937_64& rng,
                                   std::span<tree_reducer::node> storage) {
  const std::uint64_t base_chunks = p.base * p.window;
  const std::uint64_t total_chunks = base_chunks + p.windows * p.window;
  const std::size_t bytes =
      static_cast<std::size_t>(total_chunks) * chunk_size + p.tail;
  const std::span<const std::byte> data = message_bytes().first(bytes);
  const std::size_t base_bytes =
      static_cast<std::size_t>(base_chunks) * chunk_size;
  const std::size_t tail_at =
      static_cast<std::size_t>(total_chunks) * chunk_size;

  hasher reference = make_hasher(p.m, p.a);
  reference.update(data);
  const digest want = reference.finalize();

  hasher h = make_hasher(p.m, p.a);
  h.update(data.first(base_bytes));
  const auto* ops = blake3pp::detail::resolve(h.selected_arch());
  REQUIRE(ops != nullptr);

  // The CVs the pipeline would hand over: one compress_subtree_cv per
  // window (or per half window), at the window's absolute chunk counter.
  std::vector<piece> pieces;
  pieces.reserve(static_cast<std::size_t>(p.windows) * 2);
  for (std::uint64_t w = 0; w < p.windows; ++w) {
    const std::uint64_t first = base_chunks + w * p.window;
    const bool split = p.split_halves && (rng() & 1) != 0;
    const std::uint64_t step = split ? p.window / 2 : p.window;
    for (std::uint64_t f = first; f < first + p.window; f += step) {
      piece pc;
      pc.first_chunk = f;
      pc.chunks = step;
      blake3pp::detail::compress_subtree_cv(
          ops, data.data() + static_cast<std::size_t>(f) * chunk_size,
          static_cast<std::size_t>(step), f, h.key_words(), h.mode_flags(),
          pc.cv);
      pieces.push_back(pc);
    }
  }

  hasher in_order = make_hasher(p.m, p.a);
  in_order.update(data.first(base_bytes));
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    in_order.push_subtree_cv(pieces[i].cv, pieces[i].chunks);
  }
  in_order.update(data.subspan(tail_at));
  CHECK(in_order.finalize() == want);

  tree_reducer r(ops, h.key_words(), h.mode_flags(), storage);
  if (p.clear_first) {
    // A reducer that has been used and cleared must behave like a new one.
    for (std::uint64_t w = 0; w < 3; ++w) {
      REQUIRE(r.insert(w * 4, 4, label(w)));
    }
    r.clear();
    REQUIRE(r.pending() == 0);
  }
  std::shuffle(pieces.begin(), pieces.end(), rng);
  std::size_t max_pending = 0;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    REQUIRE(r.insert(pieces[i].first_chunk, pieces[i].chunks, pieces[i].cv));
    max_pending = std::max(max_pending, r.pending());
  }
  check_decomposition(r.sorted(), base_chunks, total_chunks);
  r.drain_into(h);
  CHECK(r.pending() == 0);
  h.update(data.subspan(tail_at));
  CHECK(h.finalize() == want);
  return max_pending;
}

}  // namespace

TEST_SUITE("tree_reducer") {

TEST_CASE("windows arriving in any order absorb as the message they are") {
  std::mt19937_64 rng(seed);
  // A uniformly random order merges almost nothing until late, so the
  // storage has to hold one node per window.
  std::array<tree_reducer::node, 256> storage;
  std::size_t max_pending = 0;
  for (const mode m : {mode::plain, mode::keyed, mode::derive_key}) {
    CAPTURE(mode_name(m));
    for (int i = 0; i < order_cases; ++i) {
      static constexpr std::uint64_t window_sizes[] = {2, 4, 8, 16, 64};
      case_params p;
      p.m = m;
      p.window = window_sizes[rng() % 5];
      p.base = rng() % 10;
      p.windows = 1 + rng() % 200;
      p.tail = 1 + static_cast<std::size_t>(
                       rng() % (p.window * chunk_size));
      CAPTURE(p.window);
      CAPTURE(p.base);
      CAPTURE(p.windows);
      CAPTURE(p.tail);
      max_pending = std::max(max_pending, run_case(p, rng, storage));
    }
  }
  MESSAGE("max pending over 300 random orders: " << max_pending);
}

TEST_CASE("windows split into their two children reduce the same way") {
  std::mt19937_64 rng(seed + 1);
  std::array<tree_reducer::node, 256> storage;
  std::size_t max_pending = 0;
  for (const mode m : {mode::plain, mode::keyed, mode::derive_key}) {
    CAPTURE(mode_name(m));
    for (int i = 0; i < split_cases; ++i) {
      static constexpr std::uint64_t window_sizes[] = {4, 8, 16, 64};
      case_params p;
      p.m = m;
      p.window = window_sizes[rng() % 4];
      p.base = rng() % 10;
      // Half of the windows arrive as two pieces, so the node count is up
      // to twice the window count and the storage above is the limit.
      p.windows = 1 + rng() % 96;
      p.tail = 1 + static_cast<std::size_t>(
                       rng() % (p.window * chunk_size));
      p.split_halves = true;
      CAPTURE(p.window);
      CAPTURE(p.base);
      CAPTURE(p.windows);
      CAPTURE(p.tail);
      max_pending = std::max(max_pending, run_case(p, rng, storage));
    }
  }
  MESSAGE("max pending over 150 mixed-size orders: " << max_pending);
}

TEST_CASE("one withheld window leaves a logarithmic number of nodes") {
  constexpr std::uint64_t window = 2;
  constexpr std::uint64_t windows = 256;
  hasher h;
  const auto* ops = blake3pp::detail::resolve(h.selected_arch());
  REQUIRE(ops != nullptr);
  std::array<tree_reducer::node, 64> storage;
  tree_reducer r(ops, h.key_words(), h.mode_flags(), storage);
  std::size_t max_pending = 0;
  for (std::uint64_t w = 1; w < windows; ++w) {
    REQUIRE(r.insert(w * window, window, label(w)));
    max_pending = std::max(max_pending, r.pending());
  }
  // The run [1, 256) is one ascending and one descending chain of aligned
  // nodes over an 8-level tree; nothing about the straggler adds to it.
  CHECK(max_pending <= 2 * 8);
  REQUIRE(r.insert(0, window, label(0)));
  check_decomposition(r.sorted(), 0, windows * window);
  MESSAGE("max pending behind one straggler: " << max_pending);
}

TEST_CASE("windows arriving within K of their turn need (K + 1) chains") {
  constexpr std::uint64_t window = 2;
  constexpr std::uint64_t windows = 256;
  constexpr std::uint64_t k = 8;
  std::mt19937_64 rng(seed + 2);

  // An arrival order in which no window is more than K places from its
  // own: what a pipeline with K windows in flight delivers.
  std::vector<std::uint64_t> order;
  std::vector<bool> placed(windows, false);
  std::uint64_t lowest = 0;
  for (std::uint64_t t = 0; t < windows; ++t) {
    while (lowest < windows && placed[lowest]) {
      lowest++;
    }
    std::uint64_t pick = lowest;
    if (t < k || lowest + k != t) {
      std::vector<std::uint64_t> candidates;
      for (std::uint64_t w = lowest; w < windows && w <= t + k; ++w) {
        if (!placed[w]) {
          candidates.push_back(w);
        }
      }
      pick = candidates[rng() % candidates.size()];
    }
    placed[pick] = true;
    order.push_back(pick);
  }
  for (std::uint64_t t = 0; t < windows; ++t) {
    REQUIRE(order[t] <= t + k);
    REQUIRE(order[t] + k >= t);
  }

  hasher h;
  const auto* ops = blake3pp::detail::resolve(h.selected_arch());
  REQUIRE(ops != nullptr);
  std::array<tree_reducer::node, 256> storage;
  tree_reducer r(ops, h.key_words(), h.mode_flags(), storage);
  std::size_t max_pending = 0;
  for (std::uint64_t t = 0; t < windows; ++t) {
    REQUIRE(r.insert(order[t] * window, window, label(order[t])));
    max_pending = std::max(max_pending, r.pending());
  }
  CHECK(max_pending <= (k + 1) * 2 * 8);
  check_decomposition(r.sorted(), 0, windows * window);
  MESSAGE("max pending at displacement " << k << ": " << max_pending);
}

TEST_CASE("an order that merges nothing costs one node per insert") {
  constexpr std::uint64_t window = 2;
  constexpr std::uint64_t windows = 256;
  hasher h;
  const auto* ops = blake3pp::detail::resolve(h.selected_arch());
  REQUIRE(ops != nullptr);
  std::array<tree_reducer::node, 256> storage;
  tree_reducer r(ops, h.key_words(), h.mode_flags(), storage);
  std::size_t inserted = 0;
  for (std::uint64_t w = 0; w < windows; w += 2) {
    REQUIRE(r.insert(w * window, window, label(w)));
    inserted++;
    REQUIRE(r.pending() == inserted);
  }
  for (std::uint64_t w = 1; w < windows; w += 2) {
    REQUIRE(r.insert(w * window, window, label(w)));
  }
  check_decomposition(r.sorted(), 0, windows * window);
}

TEST_CASE("a full store refuses only what it cannot merge") {
  hasher h;
  const auto* ops = blake3pp::detail::resolve(h.selected_arch());
  REQUIRE(ops != nullptr);
  std::array<tree_reducer::node, 4> storage;
  tree_reducer r(ops, h.key_words(), h.mode_flags(), storage);
  for (std::uint64_t w = 0; w < 4; ++w) {
    REQUIRE(r.insert(w * 2, 1, label(w)));
  }
  REQUIRE(r.pending() == 4);
  std::vector<range> before;
  for (const auto& n : r.sorted()) {
    before.push_back(range{n.first_chunk, n.chunks});
  }

  CHECK_FALSE(r.insert(8, 1, label(8)));
  CHECK(r.pending() == 4);
  const std::span<const tree_reducer::node> after = r.sorted();
  REQUIRE(after.size() == before.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    CAPTURE(i);
    CHECK(after[i].first_chunk == before[i].first_chunk);
    CHECK(after[i].chunks == before[i].chunks);
  }

  // A merge takes the sibling's slot, so it needs no free one.
  CHECK(r.insert(1, 1, label(9)));
  CHECK(r.pending() == 4);
}

TEST_CASE("every available variant reduces the same way") {
  // available_arches(), not compiled_arches(): resolve() hands back a
  // fallback table for a variant this CPU cannot run, so iterating the
  // compiled set would claim coverage it does not have.
  for (const auto a : blake3pp::available_arches()) {
    CAPTURE(arch_name(a));
    std::mt19937_64 rng(seed + 3);
    std::array<tree_reducer::node, 64> storage;
    case_params p;
    p.window = 8;
    p.base = 1;
    p.windows = 16;
    p.tail = 1000;
    p.a = a;
    const std::size_t max_pending = run_case(p, rng, storage);
    CHECK(max_pending <= 16);
  }
}

TEST_CASE("parent_cv is the node its two children hang from") {
  hasher h;
  const auto* ops = blake3pp::detail::resolve(h.selected_arch());
  REQUIRE(ops != nullptr);
  // compress_subtree_cv needs two chunks, so the smallest pair of children
  // it can produce is the halves of a four-chunk subtree.
  for (const std::size_t chunks : {std::size_t{4}, std::size_t{16}}) {
    CAPTURE(chunks);
    const std::span<const std::byte> data =
        message_bytes().first(chunks * chunk_size);
    const std::size_t half = chunks / 2;
    tree_reducer::cv_type whole{};
    tree_reducer::cv_type left{};
    tree_reducer::cv_type right{};
    blake3pp::detail::compress_subtree_cv(ops, data.data(), chunks, 0,
                                          h.key_words(), h.mode_flags(),
                                          whole);
    blake3pp::detail::compress_subtree_cv(ops, data.data(), half, 0,
                                          h.key_words(), h.mode_flags(), left);
    blake3pp::detail::compress_subtree_cv(
        ops, data.data() + half * chunk_size, half, half, h.key_words(),
        h.mode_flags(), right);

    tree_reducer::cv_type out{};
    blake3pp::detail::parent_cv(ops, left, right, h.key_words(),
                                h.mode_flags(), out);
    CHECK(out == whole);

    tree_reducer::cv_type in_left = left;
    blake3pp::detail::parent_cv(ops, in_left, right, h.key_words(),
                                h.mode_flags(), in_left);
    CHECK(in_left == whole);

    tree_reducer::cv_type in_right = right;
    blake3pp::detail::parent_cv(ops, left, in_right, h.key_words(),
                                h.mode_flags(), in_right);
    CHECK(in_right == whole);
  }
}

TEST_CASE("clear() returns the reducer to empty") {
  hasher h;
  const auto* ops = blake3pp::detail::resolve(h.selected_arch());
  REQUIRE(ops != nullptr);
  std::array<tree_reducer::node, 256> storage;
  tree_reducer r(ops, h.key_words(), h.mode_flags(), storage);
  for (std::uint64_t w = 0; w < 5; ++w) {
    REQUIRE(r.insert(w * 4, 4, label(w)));
  }
  r.clear();
  CHECK(r.pending() == 0);
  CHECK(r.sorted().empty());

  std::mt19937_64 rng(seed + 4);
  case_params p;
  p.m = mode::keyed;
  p.window = 8;
  p.base = 2;
  p.windows = 37;
  p.tail = 5000;
  p.clear_first = true;
  static_cast<void>(run_case(p, rng, storage));
}

// fold_aligned_runs has one job: produce exactly the nodes the reducer
// would have produced from the same parts, so that a window folded on a
// pool thread is indistinguishable from one inserted part by part.
TEST_CASE("fold_aligned_runs matches inserting every part") {
  using blake3pp::detail::cv_run;
  using blake3pp::detail::fold_aligned_runs;
  const auto* ops = blake3pp::detail::resolve(blake3pp::arch::auto_detect);
  hasher h;
  std::mt19937_64 rng(seed);

  std::array<tree_reducer::node, 2 * 54> folded_storage{};
  std::vector<tree_reducer::node> nodes_a(256);
  std::vector<tree_reducer::node> nodes_b(256);

  for (int round = 0; round < 400; ++round) {
    // A part size that is a power of two, a base that is a multiple of
    // it, and any sub-run of the parts.
    const std::uint64_t part_chunks = std::uint64_t{1} << (rng() % 6);
    const std::size_t parts = 1 + rng() % 40;
    const std::uint64_t base_part = rng() % 1000;
    const std::uint64_t base_chunk = base_part * part_chunks;
    const std::size_t i0 = rng() % parts;
    const std::size_t i1 = i0 + 1 + rng() % (parts - i0);
    CAPTURE(part_chunks);
    CAPTURE(base_chunk);
    CAPTURE(i0);
    CAPTURE(i1);

    std::vector<tree_reducer::cv_type> cvs(parts);
    for (auto& cv : cvs) {
      for (auto& word : cv) {
        word = static_cast<std::uint32_t>(rng());
      }
    }
    const std::vector<tree_reducer::cv_type> original = cvs;

    // What the helper produces, inserted as nodes.
    tree_reducer a(ops, h.key_words(), h.mode_flags(), nodes_a);
    const std::size_t n = fold_aligned_runs(
        ops, h.key_words(), h.mode_flags(),
        cv_run{std::span(cvs), i0, i1, base_chunk, part_chunks},
        std::span(folded_storage));
    REQUIRE(n > 0);
    REQUIRE(n <= folded_storage.size());
    for (std::size_t k = 0; k < n; ++k) {
      REQUIRE(a.insert(folded_storage[k].first_chunk,
                       folded_storage[k].chunks, folded_storage[k].cv));
    }

    // What the reducer produces from the same parts, one at a time.
    tree_reducer b(ops, h.key_words(), h.mode_flags(), nodes_b);
    for (std::size_t i = i0; i < i1; ++i) {
      REQUIRE(b.insert(base_chunk + i * part_chunks, part_chunks, original[i]));
    }

    const auto left = a.sorted();
    const auto right = b.sorted();
    REQUIRE(left.size() == right.size());
    for (std::size_t k = 0; k < left.size(); ++k) {
      CAPTURE(k);
      CHECK(left[k].first_chunk == right[k].first_chunk);
      CHECK(left[k].chunks == right[k].chunks);
      // The CVs are the claim: the grouped route folds with
      // fold_sibling_cvs, lanes-wide, and the per-part route merges with
      // the reducer's own scalar parent_cv. Equal positions would pass
      // with two different trees behind them.
      CHECK(left[k].cv == right[k].cv);
    }
    // The canonical decomposition is at most one node per level twice
    // over, which is what bounds the helper's output storage.
    CHECK(n <= 2 * 54);
  }
}

TEST_CASE("fold_aligned_runs leaves an empty run alone") {
  using blake3pp::detail::cv_run;
  using blake3pp::detail::fold_aligned_runs;
  const auto* ops = blake3pp::detail::resolve(blake3pp::arch::auto_detect);
  hasher h;
  std::vector<tree_reducer::cv_type> cvs(4);
  std::array<tree_reducer::node, 2 * 54> out{};
  CHECK(fold_aligned_runs(ops, h.key_words(), h.mode_flags(),
                          cv_run{std::span(cvs), 2, 2, 0, 16},
                          std::span(out)) == 0);
}

}  // TEST_SUITE
