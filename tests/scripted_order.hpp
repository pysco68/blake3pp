#pragma once

// Which queued transfer a test driver completes next, from its script's
// preference: the read and the write test drivers both let a case name the
// order windows land in.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace b3test {

// The position in `queued` of the transfer `order` prefers. A transfer
// ranks by where its window (off / window_bytes) stands in `order`; one
// the script does not name ranks after every one it does, by window, so
// an empty order is submission order by offset. `queued` is not empty.
template <class Transfer>
[[nodiscard]] std::size_t scripted_pick(const std::vector<Transfer>& queued,
                                        std::span<const unsigned> order,
                                        std::size_t window_bytes) noexcept {
  const auto rank_of = [&](const Transfer& t) noexcept {
    const auto index = static_cast<unsigned>(t.off / window_bytes);
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (order[i] == index) {
        return i;
      }
    }
    return order.size() + index;
  };
  std::size_t best = 0;
  std::size_t best_rank = rank_of(queued.front());
  for (std::size_t i = 1; i < queued.size(); ++i) {
    const std::size_t r = rank_of(queued[i]);
    if (r < best_rank) {
      best = i;
      best_rank = r;
    }
  }
  return best;
}

}  // namespace b3test
