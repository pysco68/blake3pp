#pragma once

// Inlining enforcement for the kernel's call chains.
//
// Plain `inline` is a suggestion compilers decline for the facade's call
// chains, which defeats the one-TU-full-inlining design this library is
// built on. MSVC at /O2 /Ob3 compiled the whole kernel as a call graph
// (hash_batch with 9 calls, rounds as functions); __forceinline is honored.
// Clang 22 on aarch64 outlines the ~900-instruction all_rounds<u32v> at
// -O3, and an outlined round core keeps v[] and m[] IN MEMORY, every
// micro-step a load/compute/store round-trip (measured: 15% behind
// upstream's NEON kernel from this alone). always_inline restores the
// design.
//
// It is NOT a no-op on x86-64, contrary to what this comment claimed until
// the A/B below was actually run (256 MiB, best of 3, interleaved, Zen 3+):
// GCC 16 scalar +26% / sse42 +9% / avx2 +40%, Clang 22 scalar +18% /
// sse42 +12% / avx2 +6%. Both frontends outline without it: clang the
// whole all_rounds<u32v>, GCC the index_sequence lambda inside it. The
// reference-kernel bench rows moved <3% across the same runs, which is the
// noise floor those numbers stand above.
//
// Both spellings must be applied: always_inline/__forceinline on a function
// does NOT propagate into a lambda it defines, and every compiler measured
// here was observed outlining exactly the round-fold lambda while inlining
// its enclosing function.

// Set by cmake/ArchKernels.cmake from -DBLAKE3PP_KERNEL_INLINE_ENFORCEMENT=
// auto|on|off; the fallback below repeats that default for tooling and for
// consumers building these sources outside our CMake. Turning it off
// restores the pre-enforcement spelling so one tree can be built both ways
// and the numbers above re-measured on other hardware.
#ifndef BLAKE3PP_KERNEL_INLINE_ENFORCEMENT
#define BLAKE3PP_KERNEL_INLINE_ENFORCEMENT 1
#endif

#if !BLAKE3PP_KERNEL_INLINE_ENFORCEMENT
#define BLAKE3PP_FORCE_INLINE inline
#define BLAKE3PP_LAMBDA_FORCE_INLINE
#elif defined(_MSC_VER) && !defined(__clang__)
#define BLAKE3PP_FORCE_INLINE __forceinline
// Lambdas have no keyword position for __forceinline; MSVC accepts the
// [[msvc::forceinline]] attribute after the parameter list instead.
#define BLAKE3PP_LAMBDA_FORCE_INLINE [[msvc::forceinline]]
#else
#define BLAKE3PP_FORCE_INLINE __attribute__((always_inline)) inline
#define BLAKE3PP_LAMBDA_FORCE_INLINE __attribute__((always_inline))
#endif
