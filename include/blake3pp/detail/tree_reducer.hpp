#pragma once

// Absorbing subtrees that arrive out of order.
//
// hasher::push_subtree_cv() takes complete subtrees strictly left to
// right: it is the binary counter of the spec, and a subtree that arrives
// early has nowhere to wait. A pipeline whose windows complete out of
// order therefore has to re-serialize them before absorbing. This holds
// them instead, merging each pair of siblings as soon as both exist, and
// hands what is left to a hasher in order at the end.
//
// Why merging early is sound: BLAKE3's tree is left-complete, so any chunk
// range [p, p + s) with s a power of two, p % s == 0 and p + s at or below
// the message's final chunk is a node of the final tree, whatever its
// siblings are doing. Two pending nodes (p, s) and (p + s, s) with
// p % 2s == 0 are then the two children of the node (p, 2s), which is a
// node of the tree by the same argument, and compressing them is the same
// compression the hasher would have performed later.

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/contract.hpp>

namespace blake3pp::detail {

// A pending-node set that merges siblings on arrival, in any order.
//
// Single-threaded by contract: no locks, no atomics, no thread safety.
// Never allocates; the caller owns the node storage, which bounds how many
// unmerged nodes may be in flight at once.
//
// Capacity: while the inserted ranges form a contiguous run broken by at
// most K holes, pending stays at or below (K + 1) * 2 * 54 -- each of the
// K + 1 contiguous segments decomposes into at most one ascending and one
// descending chain of aligned nodes, one per level, and a message at
// BLAKE3's 2^64-byte limit is 54 levels deep. An arbitrary insertion order
// has no such bound: inserting every other range merges nothing, so the
// storage has to hold one node per inserted range.
class tree_reducer {
 public:
  using cv_type = std::array<std::uint32_t, 8>;

  struct node {
    std::uint64_t first_chunk;  // absolute chunk index in the message
    std::uint64_t chunks;       // power of two
    cv_type cv;
  };

  // The key, flags and kernels are the destination hasher's:
  // resolve(h.selected_arch()), h.key_words(), h.mode_flags().
  tree_reducer(const kern::kernel_ops* ops,
               std::span<const std::uint32_t, 8> key, std::uint32_t base_flags,
               std::span<node> storage) noexcept
      : ops_(ops), base_flags_(base_flags), storage_(storage) {
    std::ranges::copy(key, key_.begin());
  }

  // Inserts the CV of the complete subtree over
  // [first_chunk, first_chunk + chunks) and merges upward while a sibling
  // is pending.
  // @pre chunks is a power of two; first_chunk % chunks == 0.
  // @pre The range is disjoint from every range inserted before. Checked in
  //      every build, because inserting one twice is a digest that is wrong
  //      and reports nothing.
  // @pre The range does not contain the message's final chunk. Not checked
  //      at all: the caller knows where the message ends and this does not.
  // @return false, with the reducer unchanged, if storage is full.
  // @throws std::logic_error if the range overlaps one already inserted.
  //         A full set of storage is a capacity answer the caller acts on;
  //         an overlap is a mistake no caller can act on.
  [[nodiscard]] bool insert(std::uint64_t first_chunk, std::uint64_t chunks,
                            std::span<const std::uint32_t, 8> cv) {
    assert(chunks > 0 && std::has_single_bit(chunks));
    assert(first_chunk % chunks == 0);
    // Every chunk inserted so far is covered by exactly one pending node: a
    // merge replaces two siblings with the one contiguous range they cover,
    // so nothing inserted is ever unaccounted for. Testing the new range
    // against the nodes is therefore exact, where arithmetic on totals is
    // only necessary -- a duplicate no larger than a hole in the covered
    // span satisfies the sums and is still the same chunks twice. The scan
    // is the one the merge loop below already walks.
    for (std::size_t i = 0; i < count_; ++i) {
      const node& n = storage_[i];
      require(first_chunk + chunks <= n.first_chunk ||
                  n.first_chunk + n.chunks <= first_chunk,
              "tree_reducer::insert(): a range overlapping one already "
              "inserted, which would absorb the same chunks twice");
    }
    node merged;
    merged.first_chunk = first_chunk;
    merged.chunks = chunks;
    // Counted loop rather than ranges::copy: the extent is 8 and MSVC
    // still lowers the algorithm to an out-of-line memmove call here.
    for (std::size_t i = 0; i < 8; ++i) {
      merged.cv[i] = cv[i];
    }
    // The sibling of (p, s) is at p ^ s with the same size, and each merge
    // frees the slot it came from, so a cascade never needs storage. Only
    // the first lookup can fail with a full array, which is why the
    // failure is reported before anything has been touched.
    bool any = false;
    for (;;) {
      const std::uint64_t sibling = merged.first_chunk ^ merged.chunks;
      const std::span<node> pending = storage_.first(count_);
      const auto it = std::ranges::find_if(pending, [&](const node& n) {
        return n.first_chunk == sibling && n.chunks == merged.chunks;
      });
      if (it == pending.end()) {
        break;
      }
      const node& other = *it;
      const bool merged_is_left = merged.first_chunk < other.first_chunk;
      // parent_cv tolerates out_cv aliasing either child, which is what
      // lets the parent take the incoming node's place.
      parent_cv(ops_, merged_is_left ? merged.cv : other.cv,
                merged_is_left ? other.cv : merged.cv, key_, base_flags_,
                merged.cv);
      merged.first_chunk = merged_is_left ? merged.first_chunk
                                          : other.first_chunk;
      merged.chunks *= 2;
      *it = storage_[count_ - 1];
      count_--;
      any = true;
    }
    if (!any && count_ == storage_.size()) {
      return false;
    }
    assert(count_ < storage_.size());
    storage_[count_] = merged;
    count_++;
    sorted_ = false;
    return true;
  }

  [[nodiscard]] std::size_t pending() const noexcept { return count_; }

  // The pending nodes, ordered by first_chunk. Invalidated by insert().
  [[nodiscard]] std::span<const node> sorted() noexcept {
    if (!sorted_) {
      std::ranges::sort(storage_.first(count_), {}, &node::first_chunk);
      sorted_ = true;
    }
    return std::span<const node>(storage_.data(), count_);
  }

  // Pushes every pending node into h in order, then clears.
  // @pre The pending ranges are contiguous and start at
  //      h.count() / chunk_size; h sits on a chunk boundary. Checked in
  //      every build: a hole between two of them puts the node after it at
  //      the wrong position in the tree, which is a wrong digest and
  //      nothing else.
  // @pre h has the key and flags this reducer was built with. Not checked:
  //      nothing here can see them.
  // @throws std::logic_error if the pending run has a hole or does not
  //         start where the hasher stands.
  void drain_into(hasher& h) {
    const std::span<const node> nodes = sorted();
    require(h.count() % chunk_size == 0,
            "tree_reducer::drain_into(): a hasher part-way through a chunk");
    require(nodes.empty() || nodes.front().first_chunk == h.count() / chunk_size,
            "tree_reducer::drain_into(): the pending run does not start "
            "where the hasher stands");
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      require(i == 0 || nodes[i].first_chunk ==
                            nodes[i - 1].first_chunk + nodes[i - 1].chunks,
              "tree_reducer::drain_into(): a hole in the pending run, which "
              "would absorb the nodes after it at the wrong position");
      h.push_subtree_cv(nodes[i].cv, nodes[i].chunks);
    }
    clear();
  }

  void clear() noexcept {
    count_ = 0;
    sorted_ = true;
  }

 private:
  const kern::kernel_ops* ops_;
  cv_type key_;
  std::uint32_t base_flags_;
  std::span<node> storage_;
  std::size_t count_ = 0;
  // Whether the pending nodes are in first_chunk order, which sorted()
  // restores on demand and every insert() gives up.
  bool sorted_ = true;
};

// A run of equal-sized, adjacent part CVs: parts [first, last) of cvs,
// each covering part_chunks chunks, with cvs[0] at absolute chunk
// base_chunk (a multiple of part_chunks). cvs is scratch to
// fold_aligned_runs, which clobbers what it folds.
struct cv_run {
  std::span<tree_reducer::cv_type> cvs;
  std::size_t first;
  std::size_t last;
  std::uint64_t base_chunk;
  std::uint64_t part_chunks;
};

// Folds a run of part CVs into the nodes the reducer would have ended up
// with, so a window's parts reach it as a handful of subtrees instead of
// one node each.
//
// Grouping is by absolute position, not by count: a group of 2^j parts
// is a node of the tree only when its first part's absolute index is a
// multiple of 2^j, which is why this cannot be a simple halving. Taking
// the largest legal group at each step leaves the canonical
// decomposition -- one ascending chain and one descending one, at most
// one node per level.
//
// Writes nodes left to right into out and returns how many; out needs
// 2 * 54 entries to cover any run at BLAKE3's 2^64-byte limit.
//
// Pool-side and provider-free: it runs on whichever agent finished the
// bulk, and the driver only inserts what it produced.
[[nodiscard]] inline std::size_t fold_aligned_runs(
    const kern::kernel_ops* ops, std::span<const std::uint32_t, 8> key,
    std::uint32_t base_flags, const cv_run& run,
    std::span<tree_reducer::node> out) {
  assert(run.last <= run.cvs.size());
  // Checked in every build: the alignment arithmetic below divides by
  // part_chunks and tests the quotient, so a size that is not a power of two
  // or a base that is not a multiple of it yields nodes claiming ranges that
  // are not nodes of the tree -- a wrong digest whose sizes still sum
  // correctly, which is exactly what the reducer's own check cannot catch.
  require(run.part_chunks > 0 && std::has_single_bit(run.part_chunks),
          "fold_aligned_runs(): a part size that is not a power of two");
  require(run.base_chunk % run.part_chunks == 0,
          "fold_aligned_runs(): a run that does not start on a part "
          "boundary");
  std::size_t count = 0;
  std::size_t i = run.first;
  while (i < run.last) {
    // The largest aligned group that starts here and still fits.
    const std::uint64_t first_part = run.base_chunk / run.part_chunks + i;
    std::size_t parts = 1;
    while (parts * 2 <= run.last - i && first_part % (2 * parts) == 0) {
      parts *= 2;
    }
    // Checked in every build: past the end this writes 48 bytes into
    // whatever follows the caller's array. The bound it needs is
    // 2 * bit_width(parts), which nodes_for() states and the pipeline
    // reserves, so a short span is a caller that computed it differently.
    require(count < out.size(),
            "fold_aligned_runs(): an output span shorter than the run's "
            "decomposition (2 * 54 nodes covers any run)");
    tree_reducer::node& node = out[count++];
    // i and parts stay std::size_t (they also index and size run.cvs), but
    // multiplying either by the std::uint64_t part_chunks already widens
    // it to std::uint64_t by the usual arithmetic conversions -- lossless
    // on any size_t width, so no explicit cast earns its keep here.
    node.first_chunk = run.base_chunk + i * run.part_chunks;
    node.chunks = parts * run.part_chunks;
    if (parts == 1) {
      node.cv = run.cvs[i];
    } else {
      fold_sibling_cvs(ops, run.cvs.subspan(i, parts), key, base_flags,
                       node.cv);
    }
    i += parts;
  }
  return count;
}

}  // namespace blake3pp::detail
