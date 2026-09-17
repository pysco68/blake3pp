#pragma once

// The wide word's compile-time-amount rotate, and the per-ISA escape
// hatches that make it fast. BLAKE3's g function is rotate-bound: every
// rotate sits on the serial critical path in the shape rot<N>(x ^ y), so
// each ISA's best SPELLING of that pair is worth real percentages and is
// collected here, behind one pair of functions:
//
//   rot<N>(a)        rotate right by a compile-time amount
//   xor_rot<N>(x,y)  the fused form g actually uses
//
// Every escape below defends a measurement and answers to a tuning switch
// (cmake/ArchKernels.cmake) so it can be re-measured on hardware it has
// not been measured on. The scalar word overloads live in kernel.cpp.

#include <bit>
#include <cstddef>
#include <type_traits>

#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR)
#include <arm_neon.h>
#endif

// SVE2 fixed-length TUs get the fused xor+rotate (XAR); see xor_rot.
#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR) && \
    defined(__ARM_FEATURE_SVE2) && defined(__ARM_FEATURE_SVE_BITS)
#include <arm_sve.h>
#define BLAKE3PP_HAVE_SVE2_XAR 1
#endif

// RVV+Zvbb fixed-vlen TUs get the single-instruction rotate; see xor_rot.
#if defined(__riscv) && !defined(BLAKE3PP_FORCE_SCALAR) && \
    defined(__riscv_zvbb) && defined(__riscv_v_fixed_vlen)
#include <riscv_vector.h>
#define BLAKE3PP_HAVE_ZVBB_VROR 1
#endif

#include "kernel/force_inline.hpp"
#include "kernel/shuffle.hpp"
#include "kernel/simd_facade.hpp"

#ifndef BLAKE3PP_ARCH_NS
#error "rotate.hpp is kernel-TU-internal; compile with -DBLAKE3PP_ARCH_NS=<variant>"
#endif

// The tuning switches, set by cmake/ArchKernels.cmake from
// -DBLAKE3PP_KERNEL_*_ROTATE=auto|on|off; each fallback repeats that
// default (the headers are also parsed by tooling that passes none of
// these flags). Off restores the generic spelling for re-measurement.
#ifndef BLAKE3PP_KERNEL_SRI_ROTATE
#define BLAKE3PP_KERNEL_SRI_ROTATE 1
#endif
#ifndef BLAKE3PP_KERNEL_XAR_ROTATE
#define BLAKE3PP_KERNEL_XAR_ROTATE 1
#endif
#ifndef BLAKE3PP_KERNEL_VROR_ROTATE
#define BLAKE3PP_KERNEL_VROR_ROTATE 1
#endif

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
namespace {

#if defined(BLAKE3PP_HAVE_SVE2_XAR)
typedef svuint32_t sve_fixed_u32
    __attribute__((arm_sve_vector_bits(__ARM_FEATURE_SVE_BITS)));
#endif

#if defined(BLAKE3PP_HAVE_ZVBB_VROR)
typedef vuint32m1_t rvv_fixed_u32
    __attribute__((riscv_rvv_vector_bits(__riscv_v_fixed_vlen)));
#endif

// Which SOURCE spelling reaches the one-instruction rotate is a property of
// the compiler, not of the hardware. Measured for rot(d ^ a, N), the shape
// g actually uses, at both 128- and 256-bit width:
//
//                       clang 22        GCC 14/16
//   generic shift-or    1x pshufb       pslld+psrld+por
//   byte shuffle        N=16: pshuflw+pshufhw    1x pshufb
//                       N=8:  1x pshufb          1x pshufb
//
// Mirror images: LLVM canonicalizes the shift-or rotate idiom straight to
// pshufb, but lowers the byte-shuffle spelling to the 16-bit-lane pair for N==16
// (both are legal; its cost model dislikes materializing the mask, even
// though upstream's asm hoists exactly that mask out of the loop). GCC
// never forms a shuffle from shift-or and needs the byte spelling. Neither
// form is portable-optimal, so the choice is made here per compiler.
// Forcing it with _mm256_shuffle_epi8 does NOT work: InstCombine folds a
// constant-mask pshufb intrinsic back into a generic shuffle and lowers it
// the same way. The split is stable across the clang range this project
// builds with (18.1, 20.1 and 22 all lower both spellings identically),
// so the predicate is a compiler-family choice, not a version workaround.
//
// Worth +4.0% avx2 and +4.2% sse42 on clang 22 / Zen 3+ (256 MiB, best of
// 3, interleaved; the reference-kernel rows moved 0.8% over the same runs).
// Not because it shrinks the loop; it does not: 1489 -> 1486 instructions,
// because the freed pshuflw/pshufhw pair comes back as spills once the mask
// occupies a register all loop long. What shortens is g's SERIAL chain, two
// dependent shuffles down to one. Same lesson as the aarch64 sri escape:
// on this kernel the metric is critical-path length, not instruction count.
// BLAKE3PP_KERNEL_ROT16_PER_COMPILER (cmake/ArchKernels.cmake) off gives
// clang the byte shuffle for 16 like every other compiler: the spelling
// before the split, kept buildable so the pair can be re-measured.
#ifndef BLAKE3PP_KERNEL_ROT16_PER_COMPILER
#define BLAKE3PP_KERNEL_ROT16_PER_COMPILER 1
#endif
template <int N>
constexpr bool prefer_byte_rot() noexcept {
#if defined(__clang__) && BLAKE3PP_KERNEL_ROT16_PER_COMPILER && \
    (defined(__x86_64__) || defined(__i386__) || defined(_M_X64))
  return N == 8;  // N == 16 is one instruction cheaper as generic shift-or
#else
  return N == 16 || N == 8;
#endif
}

// Compile-time-amount rotate for the wide word, in preference order: a
// single byte shuffle for the 16- and 8-bit amounts where that is the
// better spelling (above), then the aarch64 shl+sri pair, then the generic
// shift-or.
//
// W is a defaulted template parameter rather than something read directly
// off u32v, so that the discarded constexpr branches stay dependent.
// Non-dependent constructs in a discarded branch are still instantiated.
template <int N, std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE u32v rot(u32v a) noexcept {
  // The shift-or spelling three providers fall back to is undefined at 0
  // and 32; BLAKE3 only ever rotates by 16, 12, 8 and 7.
  static_assert(N > 0 && N < 32, "rotate amount must be in (0, 32)");
#if defined(BLAKE3PP_HAVE_SHUFFLE)
  if constexpr (prefer_byte_rot<N>() && shuffle_backend::supports_byte_rot<W>) {
    return shuffle_backend::rot_bytes<N / 8, W>(a);
  }
#endif
#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR) && \
    BLAKE3PP_KERNEL_SRI_ROTATE
  // The rotate amounts with no byte-granular shuffle (12 and 7): shl+sri
  // instead of the shl+usra clang selects for the generic shift-or. SRI and
  // USRA cost the same two instructions, but SRI is a cycle faster on Apple
  // cores, and these rotates sit on g's serial critical path. This was the
  // entire residual against upstream's blake3_neon.c, whose explicit
  // intrinsics reach sri directly (their PR #319 measured the same):
  // 1.61 -> 1.71 GiB/s on Apple M2 / clang 22, exactly upstream's number;
  // the two hash loops are otherwise instruction-for-instruction identical.
  // (sri also wins on Neoverse V2, +12%, and N1, +8%, in alternating A/B
  // pairs of the static clang build; the SRI_ROTATE switch exists to
  // re-measure. GCC 15 selects usra without it exactly as clang does.)
  if constexpr (W == 4 && sizeof(typename u32v::impl) == 16 &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    // Immediately-invoked generic lambda: `if constexpr` only shields
    // DEPENDENT constructs from the discarded branch, and everything here
    // is concrete: a wider-than-NEON aarch64 TU (fixed-length SVE) would
    // hard-error on the 16-byte bit_cast at template definition time even
    // though the branch is never taken. Routing a.v through a deduced
    // parameter restores the dependency. (At SVE VL=128 the branch IS
    // taken, and validly: Z0-Z31 alias V0-V31, so the NEON sri applies.)
    return [](auto impl) BLAKE3PP_LAMBDA_FORCE_INLINE {
      const uint32x4_t x = std::bit_cast<uint32x4_t>(impl);
      return u32v{std::bit_cast<decltype(impl)>(
          vsriq_n_u32(vshlq_n_u32(x, 32 - N), x, N))};
    }(a.v);
  }
#endif
  return rotr(a, N);
}

// The fused xor-then-rotate the kernel's g function is made of:
// every rotate in BLAKE3 has the shape rot<N>(x ^ y). SVE2's XAR does the
// pair in ONE instruction (rotate right of the exclusive-or), replacing
// either eor+tbl (N=16/8, byte-granular) or eor+shl+sri (N=12/7). It is
// the whole reason an SVE2 variant beats the NEON kernel at the same
// 128-bit width (+18% on Neoverse V2, entirely this). Everywhere else
// this is exactly rot<N>(x ^ y).
template <int N>
BLAKE3PP_FORCE_INLINE u32v xor_rot(u32v x, u32v y) noexcept {
#if defined(BLAKE3PP_HAVE_SVE2_XAR) && BLAKE3PP_KERNEL_XAR_ROTATE
  if constexpr (sizeof(typename u32v::impl) * 8 == __ARM_FEATURE_SVE_BITS &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    // Same dependent-lambda shield as rot's sri escape above: keeps the
    // bit_casts out of TUs whose impl is not the fixed-length SVE size.
    return [](auto ix, auto iy) BLAKE3PP_LAMBDA_FORCE_INLINE {
      // The provider's register itself where it exposes one, in place of
      // a reinterpret of the object holding it. Both spellings name the
      // same fixed-length SVE type, but clang under the MSVC ABI
      // declines to inline a reinterpret of the batch CLASS even with
      // always_inline: it emitted an out-of-line `ldr q0, [x0]; ret` and
      // spilled a live vector at every use, 448 calls and 224 spills in
      // hash_many, which is two per rotate. The same clang targeting
      // Linux, and GCC, fold it away. The SVE kernels are pinned to
      // xsimd (FORCE_XSIMD in cmake/KernelVariants.cmake), so the member
      // is always there; the reinterpret stays as the general spelling.
      if constexpr (requires { ix.data; }) {
        const sve_fixed_u32 r = svxar_n_u32(ix.data, iy.data, N);
        return u32v{decltype(ix){r}};
      } else {
        const sve_fixed_u32 r = svxar_n_u32(std::bit_cast<sve_fixed_u32>(ix),
                                            std::bit_cast<sve_fixed_u32>(iy),
                                            N);
        return u32v{std::bit_cast<decltype(ix)>(r)};
      }
    }(x.v, y.v);
  }
#endif
#if defined(BLAKE3PP_HAVE_ZVBB_VROR) && BLAKE3PP_KERNEL_VROR_ROTATE
  if constexpr (sizeof(typename u32v::impl) * 8 == __riscv_v_fixed_vlen &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    // Zvbb's vror is XAR minus the folded eor: base RVV has no rotate at
    // all, so the generic fallback is FOUR ops (vxor+vsll+vsrl+vor); this
    // is vxor+vror. Same dependent-lambda shield as the branches above.
    const u32v e = x ^ y;
    return [](auto impl) BLAKE3PP_LAMBDA_FORCE_INLINE {
      const rvv_fixed_u32 v = std::bit_cast<rvv_fixed_u32>(impl);
      const rvv_fixed_u32 r =
          __riscv_vror_vx_u32m1(v, N, __riscv_v_fixed_vlen / 32);
      return u32v{std::bit_cast<decltype(impl)>(r)};
    }(e.v);
  }
#endif
  return rot<N>(x ^ y);
}

}  // namespace
}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
