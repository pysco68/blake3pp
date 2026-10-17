#pragma once

// Provider: Intel's xvec, an independent implementation of the C++26
// data-parallel library (std::simd) in namespace xvec::simd, header-only,
// GCC 13 / clang 17 and later. The same spelling as std_simd.hpp with the
// namespace swapped: vec<T> is native width for the TU's target flags
// (16/32/64 bytes on x86 by SSE/AVX/AVX-512, 16 elsewhere through the
// generic, compiler-vector-backed implementation). Included by
// simd_facade.hpp, which selects exactly one provider.

#include <cstddef>
#include <cstdint>
#include <span>
#include <xvec/simd>

#include "kernel/force_inline.hpp"

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {

struct u32v {
  using impl = xvec::simd::vec<std::uint32_t>;
  static constexpr std::size_t width = impl::size();
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl(x)};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    return {xvec::simd::unchecked_load<impl>(
        std::span<const std::uint32_t>(p, width))};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    xvec::simd::unchecked_store(v, std::span<std::uint32_t>(p, width));
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    // xvec carries the proposed rotr; its generic form is the shift-or
    // idiom, which pattern-matches to native rotates where they exist.
    return {xvec::simd::rotr(a.v, n)};
  }
};

}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
