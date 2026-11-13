#pragma once

// Little-endian 32-bit loads and stores for the kernels: the message
// words and chaining values are little-endian bytes on every target. A
// memcpy folds to one mov on every compiler, where the byte-wise shift-or
// idiom does not: MSVC emits that one as written. The byte-swap arm is
// what keeps a big-endian target correct. Shared by every kernel translation
// unit, so no BLAKE3PP_ARCH_NS here.

#include <bit>
#include <cstdint>
#include <cstring>

#include "kernel/force_inline.hpp"

namespace blake3pp::kern::endian {

BLAKE3PP_FORCE_INLINE constexpr std::uint32_t bswap32(std::uint32_t v) noexcept {
#if defined(__cpp_lib_byteswap)
  return std::byteswap(v);
#else
  return (v >> 24) | ((v >> 8) & 0x0000ff00u) | ((v << 8) & 0x00ff0000u) |
         (v << 24);
#endif
}

BLAKE3PP_FORCE_INLINE std::uint32_t load32(const std::uint8_t* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof v);
  if constexpr (std::endian::native == std::endian::big) {
    v = bswap32(v);
  }
  return v;
}

BLAKE3PP_FORCE_INLINE void store32(std::uint8_t* p, std::uint32_t v) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    v = bswap32(v);
  }
  std::memcpy(p, &v, sizeof v);
}

}  // namespace blake3pp::kern::endian
