#pragma once

// The u32-vector facade: one minimal wide-word type, three providers.
//
//   BLAKE3PP_FORCE_SCALAR   width-1 plain uint32_t (the scalar kernel, and
//                           the correctness oracle for everything else)
//   BLAKE3PP_HAS_STD_SIMD   native C++26 std::simd (GCC 16's <simd>)
//   BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD
//                           Parallelism TS v2 <experimental/simd>
//                           (libstdc++ from GCC 11 on, incl. Clang against a
//                           correctly pinned libstdc++; libc++'s is too
//                           incomplete and fails the configure probe)
//   BLAKE3PP_HAS_XSIMD      xsimd polyfill (libc++ and anything else)
//
// The width is whatever the TU's -m flags make native (SSE: 4, AVX2: 8,
// AVX-512: 16, NEON: 4), so the same kernel source vectorizes differently in
// every arch variant. The type lives INSIDE the arch namespace on purpose:
// its layout depends on the TU's flags, so a shared-namespace definition
// would be an ODR lie. It must never cross the kernel boundary.

#include <bit>
#include <cstddef>
#include <cstdint>

#ifndef BLAKE3PP_ARCH_NS
#error "simd_facade.hpp is kernel-TU-internal; compile with -DBLAKE3PP_ARCH_NS=<variant>"
#endif

// Plain `inline` is a suggestion compilers decline for the facade's call
// chains, which defeats the one-TU-full-inlining design this library is
// built on. MSVC at /O2 /Ob3 compiled the whole kernel as a call graph
// (hash_batch with 9 calls, rounds as functions); __forceinline is honored.
// Clang 22 on aarch64 outlines the ~900-instruction all_rounds<u32v> at
// -O3, and an outlined round core keeps v[] and m[] IN MEMORY, every
// micro-step a load/compute/store round-trip (measured: 15% behind
// upstream's NEON kernel from this alone). always_inline restores the
// design; where the compiler already inlined (x86 GCC/Clang, verified
// byte-identical there) it changes nothing.
#if defined(_MSC_VER) && !defined(__clang__)
#define BLAKE3PP_FORCE_INLINE __forceinline
// Lambdas have no keyword position for __forceinline; MSVC accepts the
// [[msvc::forceinline]] attribute after the parameter list instead. Empty
// elsewhere (GNU compilers would warn about the unknown attribute, and
// inline the single-call-site lambda anyway).
#define BLAKE3PP_LAMBDA_FORCE_INLINE [[msvc::forceinline]]
#else
#define BLAKE3PP_FORCE_INLINE __attribute__((always_inline)) inline
// The lambda needs its own marker: always_inline on the enclosing function
// does not propagate, and clang 22/aarch64 outlines the round-fold lambda
// specifically (all_rounds inlines, its lambda body does not).
#define BLAKE3PP_LAMBDA_FORCE_INLINE __attribute__((always_inline))
#endif

#if defined(BLAKE3PP_FORCE_SCALAR)
// no vector headers
#elif defined(BLAKE3PP_HAS_STD_SIMD)
#include <simd>
#include <span>
#elif defined(BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD)
#include <experimental/simd>
#elif defined(BLAKE3PP_HAS_XSIMD)
#include <xsimd/xsimd.hpp>
#endif

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {

#if defined(BLAKE3PP_FORCE_SCALAR)

struct u32v {
  using impl = std::uint32_t;
  static constexpr std::size_t width = 1;
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept { return {x}; }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept { return {p[0]}; }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept { p[0] = v; }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept { return {a.v + b.v}; }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept { return {a.v ^ b.v}; }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept { return {std::rotr(a.v, n)}; }
};

#elif defined(BLAKE3PP_HAS_STD_SIMD)

struct u32v {
  using impl = std::simd::vec<std::uint32_t>;
  static constexpr std::size_t width = impl::size();
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept { return {impl(x)}; }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    return {std::simd::unchecked_load<impl>(std::span<const std::uint32_t>(p, width))};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    std::simd::unchecked_store(v, std::span<std::uint32_t>(p, width));
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept { return {a.v + b.v}; }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept { return {a.v ^ b.v}; }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    // No simd rotate in the MVP; the shift-or idiom pattern-matches to
    // native rotates where they exist (AVX-512 vprord).
    return {(a.v >> n) | (a.v << (32 - n))};
  }
};

#elif defined(BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD)

struct u32v {
  using impl = std::experimental::native_simd<std::uint32_t>;
  static constexpr std::size_t width = impl::size();
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept { return {impl(x)}; }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    impl x;
    x.copy_from(p, std::experimental::element_aligned);
    return {x};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    v.copy_to(p, std::experimental::element_aligned);
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept { return {a.v + b.v}; }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept { return {a.v ^ b.v}; }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    return {(a.v >> n) | (a.v << (32 - n))};
  }
};

#elif defined(BLAKE3PP_HAS_XSIMD)

struct u32v {
  using impl = xsimd::batch<std::uint32_t>;
  static constexpr std::size_t width = impl::size;
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept { return {impl(x)}; }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    return {impl::load_unaligned(p)};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept { v.store_unaligned(p); }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept { return {a.v + b.v}; }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept { return {a.v ^ b.v}; }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept { return {xsimd::rotr(a.v, n)}; }
};

#else
#error "No simd provider: expected BLAKE3PP_FORCE_SCALAR, BLAKE3PP_HAS_STD_SIMD, BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD or BLAKE3PP_HAS_XSIMD"
#endif

}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
