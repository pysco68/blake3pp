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
// design; where the compiler already inlined (x86 GCC/Clang, verified
// byte-identical there) it changes nothing.
//
// Both spellings must be applied: always_inline/__forceinline on a function
// does NOT propagate into a lambda it defines, and both compilers were
// observed outlining exactly the round-fold lambda while inlining its
// enclosing function.

#if defined(_MSC_VER) && !defined(__clang__)
#define BLAKE3PP_FORCE_INLINE __forceinline
// Lambdas have no keyword position for __forceinline; MSVC accepts the
// [[msvc::forceinline]] attribute after the parameter list instead.
#define BLAKE3PP_LAMBDA_FORCE_INLINE [[msvc::forceinline]]
#else
#define BLAKE3PP_FORCE_INLINE __attribute__((always_inline)) inline
#define BLAKE3PP_LAMBDA_FORCE_INLINE __attribute__((always_inline))
#endif
