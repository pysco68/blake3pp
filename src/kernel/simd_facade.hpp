#pragma once

// The u32-vector facade: one minimal wide-word type, four providers, chosen
// here and nowhere else.
//
//   BLAKE3PP_FORCE_SCALAR   width-1 plain uint32_t (the scalar kernel, and
//                           the correctness oracle for everything else)
//   BLAKE3PP_HAS_STD_SIMD   native C++26 std::simd (GCC 16's <simd>)
//   BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD
//                           Parallelism TS v2 <experimental/simd>
//   BLAKE3PP_HAS_XSIMD      xsimd polyfill (libc++, MSVC, anything else)
//
// Each provider defines the same struct u32v in simd/<provider>.hpp
// (broadcast/load/store, operator+ / operator^ / rotr), and none of them
// contains a single #ifdef. Adding a provider means adding a file and one
// arm below; the kernel source never learns which one it got.
//
// The width is whatever the TU's -m flags make native (SSE: 4, AVX2: 8,
// AVX-512: 16, NEON: 4), so the same kernel source vectorizes differently in
// every arch variant. The type lives INSIDE the arch namespace on purpose:
// its layout depends on the TU's flags, so a shared-namespace definition
// would be an ODR lie. It must never cross the kernel boundary.

#ifndef BLAKE3PP_ARCH_NS
#error "simd_facade.hpp is kernel-TU-internal; compile with -DBLAKE3PP_ARCH_NS=<variant>"
#endif

#include "kernel/force_inline.hpp"

#if defined(BLAKE3PP_FORCE_SCALAR)
#include "kernel/simd/scalar.hpp"
#elif defined(BLAKE3PP_HAS_STD_SIMD)
#include "kernel/simd/std_simd.hpp"
#elif defined(BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD)
#include "kernel/simd/std_experimental_simd.hpp"
#elif defined(BLAKE3PP_HAS_XSIMD)
#include "kernel/simd/xsimd.hpp"
#else
#error "No simd provider: expected BLAKE3PP_FORCE_SCALAR, BLAKE3PP_HAS_STD_SIMD, BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD or BLAKE3PP_HAS_XSIMD"
#endif
