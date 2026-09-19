#pragma once

// Provider: native C++26 std::simd (GCC 16's <simd>, spelled
// std::simd::vec<T>). Included by simd_facade.hpp, which selects exactly
// one provider.

#include <cstddef>
#include <cstdint>
#include <simd>
#include <span>

#include "kernel/force_inline.hpp"

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
namespace {

struct u32v {
  using impl = std::simd::vec<std::uint32_t>;
  static constexpr std::size_t width = impl::size();
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl(x)};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    return {std::simd::unchecked_load<impl>(
        std::span<const std::uint32_t>(p, width))};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    std::simd::unchecked_store(v, std::span<std::uint32_t>(p, width));
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    // No simd rotate in the MVP; the shift-or idiom pattern-matches to
    // native rotates where they exist (AVX-512 vprord).
    return {(a.v >> n) | (a.v << (32 - n))};
  }
};

}  // namespace
}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
