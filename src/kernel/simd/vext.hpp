#pragma once

// Provider: GNU/Clang vector extensions, for targets that neither xsimd
// nor a std provider covers. Included by simd_facade.hpp, which selects
// exactly one provider.
//
// The width is named by the build here rather than deduced, and that is
// the whole reason this provider exists. The std providers pick a native
// width from an ISA list, and on a target that list has never heard of,
// the width they pick is 1. Measured on mips64el with -mmsa, GCC 14:
// std::experimental::native_simd<uint32_t> reports width 1 and emits no
// vector instruction, while the same xor-then-rotate over an explicit
// 16-byte vector compiles to six MSA instructions. The compiler reaches
// the ISA perfectly; only the library's width guess does not.
//
// Everything below is ordinary C++ over a vector-extension type, so the
// instruction selection is the compiler's job on every target it knows.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "kernel/force_inline.hpp"

#ifndef BLAKE3PP_VEXT_BYTES
#error "the vext provider needs -DBLAKE3PP_VEXT_BYTES=<register bytes>"
#endif

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {

struct u32v {
  using impl [[gnu::vector_size(BLAKE3PP_VEXT_BYTES)]] = std::uint32_t;
  static constexpr std::size_t width =
      BLAKE3PP_VEXT_BYTES / sizeof(std::uint32_t);
  impl v;

  // Vector-scalar arithmetic broadcasts the scalar, so this is a splat
  // and folds to one instruction.
  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl{} + x};
  }
  // memcpy rather than a cast: the callers' buffers carry no vector
  // alignment, and every compiler folds a sizeof-register memcpy into
  // the target's unaligned load.
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    impl x;
    std::memcpy(&x, p, sizeof(x));
    return {x};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    std::memcpy(p, &v, sizeof(v));
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

}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
