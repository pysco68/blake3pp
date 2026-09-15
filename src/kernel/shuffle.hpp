#pragma once

// Selects the TU's shuffle backend: the one place in the kernel where a
// preprocessor conditional decides anything about shuffles.
//
// Two backends implement the same tiny interface (reg<W>, supports<W>,
// shuf<I...>, load/store, to_word/from_word, rot_bytes<RB,W>), so the
// networks in shuffle/networks.hpp and the transposes in transpose.hpp are
// written exactly once and neither contains a backend conditional.
//
// Preference is vext-then-xsimd because vext works for EVERY provider (it
// bit_casts the provider's register), whereas the xsimd backend needs the
// xsimd provider. In practice that means: every GNU-frontend build uses
// vector extensions, and MSVC (the only frontend without
// __builtin_shufflevector) uses xsimd's portable shuffle. On GCC/Clang the
// two lower to identical instructions anyway (xsimd's shuffle IS
// __builtin_shufflevector there), so the preference costs nothing.
//
// When neither is available (BLAKE3PP_FORCE_SCALAR, or MSVC-on-arm64 below)
// BLAKE3PP_HAVE_SHUFFLE stays undefined and every transpose falls back to
// the scalar staging gather. The gate must be a preprocessor one, not just
// `if constexpr`: a discarded constexpr branch still name-looks-up its
// non-dependent identifiers, so machinery that does not exist in this TU
// must not even be NAMED.

// Testing override: -DBLAKE3PP_SHUFFLE_FORCE_XSIMD picks the xsimd backend
// on a GNU frontend too, so the path MSVC actually takes can be compiled
// and run anywhere. Worth the three lines: twice already, kernel bugs have
// hidden in a platform path nobody could execute locally.

#include "kernel/simd_facade.hpp"

// BLAKE3PP_KERNEL_SHUFFLE_TREE (cmake/ArchKernels.cmake) off leaves the
// kernel without a shuffle backend: transposes go through the scalar
// staging gather and the byte-granular rotates through shift-or, the
// spelling before the bypass, kept buildable so the pair can be measured.
#ifndef BLAKE3PP_KERNEL_SHUFFLE_TREE
#define BLAKE3PP_KERNEL_SHUFFLE_TREE 1
#endif

#if defined(BLAKE3PP_FORCE_SCALAR) || !BLAKE3PP_KERNEL_SHUFFLE_TREE
// Width 1: nothing to transpose. Or the bypass switched off.

#elif (defined(__GNUC__) || defined(__clang__)) && \
    !defined(BLAKE3PP_SHUFFLE_FORCE_XSIMD)
#define BLAKE3PP_HAVE_SHUFFLE 1
#include "kernel/shuffle/vext.hpp"
namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
using shuffle_backend = shuffle_detail::vext_backend;
}

#elif defined(BLAKE3PP_HAS_XSIMD) && !(defined(_M_ARM64) && !defined(__clang__))
// The pure-MSVC-arm64 exclusion: xsimd 14.3's neon64 swizzle (which the
// shuffle decomposition instantiates on non-builtin frontends) returns
// through a vreinterpretq_* chain that MSVC's arm64_neon.h defines as
// no-ops over one shared __n128 type, so a batch<uint8_t> lands in a
// batch<uint32_t> return seat and C2440 follows. Until that is fixed
// upstream, cl-on-arm64 keeps NEON rounds but stages its transposes.
#define BLAKE3PP_HAVE_SHUFFLE 1
#include "kernel/shuffle/xsimd.hpp"
namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
using shuffle_backend = shuffle_detail::xsimd_backend;
}
#endif

#if defined(BLAKE3PP_HAVE_SHUFFLE)
#include "kernel/shuffle/networks.hpp"
#endif
