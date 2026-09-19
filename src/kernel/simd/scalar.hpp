#pragma once

// Provider: none. Width-1 plain uint32_t: the scalar kernel, and the
// correctness oracle every vector variant is checked against.
// Included by simd_facade.hpp, which selects exactly one provider.

#include <bit>
#include <cstddef>
#include <cstdint>

#include "kernel/force_inline.hpp"

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
namespace {

struct u32v {
  using impl = std::uint32_t;
  static constexpr std::size_t width = 1;
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {x};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    return {p[0]};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    p[0] = v;
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    return {std::rotr(a.v, n)};
  }
};

}  // namespace
}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
