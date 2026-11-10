#pragma once

// How the parallel backend cuts a bulk shape into chunks. Internal to
// src/execution; the tests reach it through the src/ include path.

#include <algorithm>
#include <cstddef>

namespace blake3pp::detail {

// The end of the bulk chunk that starts at begin: chunk_length items, or
// the rest of shape when fewer remain. Requires begin < shape. Spelled so
// that begin + chunk_length is never formed, since it wraps for a shape
// near the top of size_t.
[[nodiscard]] constexpr std::size_t bulk_chunk_end(
    std::size_t begin, std::size_t chunk_length, std::size_t shape) noexcept {
  return begin + std::min(chunk_length, shape - begin);
}

}  // namespace blake3pp::detail
