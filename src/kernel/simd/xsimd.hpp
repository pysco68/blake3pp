#pragma once

// Provider: the xsimd polyfill, for libc++, MSVC, and anything else without
// a usable standard simd. Included by simd_facade.hpp, which selects
// exactly one provider.

#include <cstddef>
#include <cstdint>
#include <xsimd/xsimd.hpp>

#include "kernel/force_inline.hpp"

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
namespace {

struct u32v {
  using impl = xsimd::batch<std::uint32_t>;
  static constexpr std::size_t width = impl::size;
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl(x)};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    return {impl::load_unaligned(p)};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    v.store_unaligned(p);
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    return {xsimd::rotr(a.v, n)};
  }
};

}  // namespace
}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
