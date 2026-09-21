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

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include <blake3pp/core.hpp>

namespace blake3pp::detail {

// A pending-node set that merges siblings on arrival, in any order.
//
// Single-threaded by contract: no locks, no atomics, no thread safety.
// Never allocates; the caller owns the node storage, which bounds how many
// unmerged nodes may be in flight at once.
//
// Capacity: while the inserted ranges form a contiguous run broken by at
// most K holes, pending stays at or below (K + 1) * 2 * 64 -- each of the
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
    for (std::size_t i = 0; i < 8; ++i) {
      key_[i] = key[i];
    }
  }

  // Inserts the CV of the complete subtree over
  // [first_chunk, first_chunk + chunks) and merges upward while a sibling
  // is pending.
  // @pre chunks is a power of two; first_chunk % chunks == 0.
  // @pre The range is disjoint from every range inserted before.
  // @pre The range does not contain the message's final chunk.
  // These are checked by assert() only, and the last one not at all: the
  // caller knows where the message ends and this does not.
  // @return false, with the reducer unchanged, if storage is full.
  [[nodiscard]] bool insert(std::uint64_t first_chunk, std::uint64_t chunks,
                            std::span<const std::uint32_t, 8> cv) noexcept {
    assert(chunks > 0 && std::has_single_bit(chunks));
    assert(first_chunk % chunks == 0);
    assert(!overlaps_pending(first_chunk, chunks));
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
      const std::size_t sib =
          find(merged.first_chunk ^ merged.chunks, merged.chunks);
      if (sib == npos) {
        break;
      }
      const node& other = storage_[sib];
      const bool merged_is_left = merged.first_chunk < other.first_chunk;
      // parent_cv tolerates out_cv aliasing either child, which is what
      // lets the parent take the incoming node's place.
      parent_cv(ops_, merged_is_left ? merged.cv : other.cv,
                merged_is_left ? other.cv : merged.cv, key_, base_flags_,
                merged.cv);
      merged.first_chunk = merged_is_left ? merged.first_chunk
                                          : other.first_chunk;
      merged.chunks *= 2;
      storage_[sib] = storage_[count_ - 1];
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
      // Insertion sort: pending counts are tens of nodes, and the array is
      // nearly ordered whenever the ranges arrive nearly in order.
      for (std::size_t i = 1; i < count_; ++i) {
        const node held = storage_[i];
        std::size_t j = i;
        while (j > 0 && storage_[j - 1].first_chunk > held.first_chunk) {
          storage_[j] = storage_[j - 1];
          j--;
        }
        storage_[j] = held;
      }
      sorted_ = true;
    }
    return std::span<const node>(storage_.data(), count_);
  }

  // Pushes every pending node into h in order, then clears.
  // @pre The pending ranges are contiguous and start at
  //      h.count() / chunk_size; h sits on a chunk boundary.
  // @pre h has the key and flags this reducer was built with.
  void drain_into(hasher& h) noexcept {
    const std::span<const node> nodes = sorted();
    assert(h.count() % chunk_size == 0);
    assert(nodes.empty() ||
           nodes.front().first_chunk == h.count() / chunk_size);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      assert(i == 0 || nodes[i].first_chunk ==
                           nodes[i - 1].first_chunk + nodes[i - 1].chunks);
      h.push_subtree_cv(nodes[i].cv, nodes[i].chunks);
    }
    clear();
  }

  void clear() noexcept {
    count_ = 0;
    sorted_ = true;
  }

 private:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  // Linear scan: a map over tens of entries costs more than it saves, and
  // the nodes are contiguous.
  [[nodiscard]] std::size_t find(std::uint64_t first_chunk,
                                 std::uint64_t chunks) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (storage_[i].first_chunk == first_chunk &&
          storage_[i].chunks == chunks) {
        return i;
      }
    }
    return npos;
  }

  // Only what is still pending can be checked; a range that was already
  // merged away is gone. Enough to catch a caller inserting twice.
  [[nodiscard]] bool overlaps_pending(std::uint64_t first_chunk,
                                      std::uint64_t chunks) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (first_chunk < storage_[i].first_chunk + storage_[i].chunks &&
          storage_[i].first_chunk < first_chunk + chunks) {
        return true;
      }
    }
    return false;
  }

  const kern::kernel_ops* ops_;
  cv_type key_;
  std::uint32_t base_flags_;
  std::span<node> storage_;
  std::size_t count_ = 0;
  bool sorted_ = true;
};

// Folds a run of equal-sized, adjacent part CVs into the nodes the
// reducer would have ended up with, so a window's parts reach it as a
// handful of subtrees instead of one node each.
//
// Parts [i0, i1) each cover part_chunks chunks, and part 0 sits at
// absolute chunk base_chunk (a multiple of part_chunks). Grouping is by
// absolute position, not by count: a group of 2^j parts is a node of the
// tree only when its first part's absolute index is a multiple of 2^j,
// which is why this cannot be a simple halving. Taking the largest legal
// group at each step leaves the canonical decomposition -- one ascending
// chain and one descending one, at most one node per level.
//
// part_cvs is scratch: fold_sibling_cvs clobbers what it folds. Writes
// nodes left to right into out and returns how many; out needs 2 * 54
// entries to cover any run at BLAKE3's 2^64-byte limit.
//
// Pool-side and provider-free: it runs on whichever agent finished the
// bulk, and the driver only inserts what it produced.
[[nodiscard]] inline std::size_t fold_aligned_runs(
    const kern::kernel_ops* ops, std::span<const std::uint32_t, 8> key,
    std::uint32_t base_flags, std::span<tree_reducer::cv_type> part_cvs,
    std::size_t i0, std::size_t i1, std::uint64_t base_chunk,
    std::uint64_t part_chunks, std::span<tree_reducer::node> out) noexcept {
  assert(part_chunks > 0 && std::has_single_bit(part_chunks));
  assert(base_chunk % part_chunks == 0);
  assert(i1 <= part_cvs.size());
  std::size_t count = 0;
  std::size_t i = i0;
  while (i < i1) {
    // The largest aligned group that starts here and still fits.
    const std::uint64_t first_part = base_chunk / part_chunks + i;
    std::size_t parts = 1;
    while (parts * 2 <= i1 - i && first_part % (2 * parts) == 0) {
      parts *= 2;
    }
    assert(count < out.size() &&
           "fold_aligned_runs needs 2 * 54 nodes for the worst run");
    tree_reducer::node& node = out[count++];
    node.first_chunk = base_chunk + static_cast<std::uint64_t>(i) * part_chunks;
    node.chunks = static_cast<std::uint64_t>(parts) * part_chunks;
    if (parts == 1) {
      node.cv = part_cvs[i];
    } else {
      fold_sibling_cvs(ops, part_cvs.subspan(i, parts), key, base_flags,
                       node.cv);
    }
    i += parts;
  }
  return count;
}

}  // namespace blake3pp::detail
