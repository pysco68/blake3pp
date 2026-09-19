#pragma once

// Provider: Parallelism TS v2 <experimental/simd> (libstdc++ from GCC 11
// on, including Clang against a correctly pinned libstdc++; libc++'s is too
// incomplete and fails the configure probe). Included by simd_facade.hpp,
// which selects exactly one provider.

#include <cstddef>
#include <cstdint>
#include <experimental/simd>

#include "kernel/force_inline.hpp"

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
namespace {

struct u32v {
  using impl = std::experimental::native_simd<std::uint32_t>;
  static constexpr std::size_t width = impl::size();
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl(x)};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    impl x;
    x.copy_from(p, std::experimental::element_aligned);
    return {x};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    v.copy_to(p, std::experimental::element_aligned);
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    return {(a.v >> n) | (a.v << (32 - n))};
  }
};

}  // namespace
}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
