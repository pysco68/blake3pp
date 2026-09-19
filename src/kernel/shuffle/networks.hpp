#pragma once

// The radix-2 shuffle networks, written ONCE, over any backend supplying a
// two-input constant shuffle.
//
// A W x W word transpose decomposes into log2(W) radix-2 stages whose index
// patterns are precisely the in-lane 32-bit unpacks (vpunpckl/hdq), the
// in-lane 64-bit unpacks (vpunpckl/hqdq), and the 128-bit lane merges
// (vperm2i128 / vinserti128): 24 single-uop shuffles for the AVX2 8x8 case,
// verified on GCC 16 (integer domain) and Clang 22 (same network, float
// domain: vunpcklps/vunpcklpd/vperm2f128).
//
// The index lists below are the whole point of this file: they are hardware
// macro-ops spelled as permutations, they were derived by simulation, and
// they must never drift between backends. Op::shuf<I...> is the only thing
// a backend has to provide to run them.

#include <cstddef>

#include "kernel/force_inline.hpp"

namespace blake3pp::kern::BLAKE3PP_ARCH_NS::shuffle_detail {
namespace {

// 4x4: two radix-2 stages (32-bit unpacks, then 64-bit unpacks).
template <class Op, class V>
BLAKE3PP_FORCE_INLINE void transpose_net(const V (&r)[4], V (&out)[4]) noexcept {
  const V a0 = Op::template shuf<0, 4, 1, 5>(r[0], r[1]);
  const V a1 = Op::template shuf<2, 6, 3, 7>(r[0], r[1]);
  const V a2 = Op::template shuf<0, 4, 1, 5>(r[2], r[3]);
  const V a3 = Op::template shuf<2, 6, 3, 7>(r[2], r[3]);
  out[0] = Op::template shuf<0, 1, 4, 5>(a0, a2);
  out[1] = Op::template shuf<2, 3, 6, 7>(a0, a2);
  out[2] = Op::template shuf<0, 1, 4, 5>(a1, a3);
  out[3] = Op::template shuf<2, 3, 6, 7>(a1, a3);
}

// 8x8: three radix-2 stages. The index sets are lane-local on purpose:
// they are exactly vpunpckl/hdq, vpunpckl/hqdq, and the final cross-lane
// merge vperm2i128/vinserti128.
template <class Op, class V>
BLAKE3PP_FORCE_INLINE void transpose_net(const V (&r)[8], V (&out)[8]) noexcept {
  const V a0 = Op::template shuf<0,  8,  1,  9, 4, 12,  5, 13>(r[0], r[1]);
  const V a1 = Op::template shuf<2, 10,  3, 11, 6, 14,  7, 15>(r[0], r[1]);
  const V a2 = Op::template shuf<0,  8,  1,  9, 4, 12,  5, 13>(r[2], r[3]);
  const V a3 = Op::template shuf<2, 10,  3, 11, 6, 14,  7, 15>(r[2], r[3]);
  const V a4 = Op::template shuf<0,  8,  1,  9, 4, 12,  5, 13>(r[4], r[5]);
  const V a5 = Op::template shuf<2, 10,  3, 11, 6, 14,  7, 15>(r[4], r[5]);
  const V a6 = Op::template shuf<0,  8,  1,  9, 4, 12,  5, 13>(r[6], r[7]);
  const V a7 = Op::template shuf<2, 10,  3, 11, 6, 14,  7, 15>(r[6], r[7]);

  const V b0 = Op::template shuf<0,  1,  8,  9, 4,  5, 12, 13>(a0, a2);
  const V b1 = Op::template shuf<2,  3, 10, 11, 6,  7, 14, 15>(a0, a2);
  const V b2 = Op::template shuf<0,  1,  8,  9, 4,  5, 12, 13>(a1, a3);
  const V b3 = Op::template shuf<2,  3, 10, 11, 6,  7, 14, 15>(a1, a3);
  const V b4 = Op::template shuf<0,  1,  8,  9, 4,  5, 12, 13>(a4, a6);
  const V b5 = Op::template shuf<2,  3, 10, 11, 6,  7, 14, 15>(a4, a6);
  const V b6 = Op::template shuf<0,  1,  8,  9, 4,  5, 12, 13>(a5, a7);
  const V b7 = Op::template shuf<2,  3, 10, 11, 6,  7, 14, 15>(a5, a7);
  
  out[0] = Op::template shuf<0, 1, 2, 3,  8,  9, 10, 11>(b0, b4);
  out[1] = Op::template shuf<0, 1, 2, 3,  8,  9, 10, 11>(b1, b5);
  out[2] = Op::template shuf<0, 1, 2, 3,  8,  9, 10, 11>(b2, b6);
  out[3] = Op::template shuf<0, 1, 2, 3,  8,  9, 10, 11>(b3, b7);
  out[4] = Op::template shuf<4, 5, 6, 7, 12, 13, 14, 15>(b0, b4);
  out[5] = Op::template shuf<4, 5, 6, 7, 12, 13, 14, 15>(b1, b5);
  out[6] = Op::template shuf<4, 5, 6, 7, 12, 13, 14, 15>(b2, b6);
  out[7] = Op::template shuf<4, 5, 6, 7, 12, 13, 14, 15>(b3, b7);
}

// 16x16: four radix-2 stages. Index lists derived from the same recursive
// construction (verified by simulation): in-lane dword unpacks
// (vpunpckl/hdq), in-lane qword unpacks (vpunpckl/hqdq), then two levels
// of 128-bit-block merges, AVX-512's vshufi32x4 territory; even a
// generic lowering lands on vpermt2d (any two-source dword permute, one
// uop). 64 two-register shuffles replace the 256 scalar load/stores of
// the staging gather.
template <class Op, class V>
BLAKE3PP_FORCE_INLINE void transpose_net(const V (&r)[16],
                                         V (&out)[16]) noexcept {
  V a[16];
  for (std::size_t g = 0; g < 8; ++g) {
    a[2 * g] =     Op::template shuf<0, 16, 1, 17, 4, 20, 5, 21,  8, 24,  9, 25, 12, 28, 13, 29>(r[2 * g], r[2 * g + 1]);
    a[2 * g + 1] = Op::template shuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15, 31>(r[2 * g], r[2 * g + 1]);
  }
  V b[16];
  for (std::size_t q = 0; q < 4; ++q) {
    const std::size_t k = 4 * q;
    b[k + 0] = Op::template shuf<0, 1, 16, 17, 4, 5, 20, 21,  8,  9, 24, 25, 12, 13, 28, 29>(a[k + 0], a[k + 2]);
    b[k + 1] = Op::template shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(a[k + 0], a[k + 2]);
    b[k + 2] = Op::template shuf<0, 1, 16, 17, 4, 5, 20, 21,  8,  9, 24, 25, 12, 13, 28, 29>(a[k + 1], a[k + 3]);
    b[k + 3] = Op::template shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(a[k + 1], a[k + 3]);
  }
  V c[16];
  for (std::size_t h = 0; h < 2; ++h) {
    const std::size_t k = 8 * h;
    for (std::size_t j = 0; j < 4; ++j) {
      c[k + j] =     Op::template shuf<0, 1, 2, 3, 16, 17, 18, 19,  8,  9, 10, 11, 24, 25, 26, 27>(b[k + j], b[k + j + 4]);
      c[k + j + 4] = Op::template shuf<4, 5, 6, 7, 20, 21, 22, 23, 12, 13, 14, 15, 28, 29, 30, 31>(b[k + j], b[k + j + 4]);
    }
  }
  for (std::size_t j = 0; j < 8; ++j) {
    out[j] =     Op::template shuf<0, 1,  2,  3,  4,  5,  6,  7, 16, 17, 18, 19, 20, 21, 22, 23>(c[j], c[j + 8]);
    out[j + 8] = Op::template shuf<8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31>(c[j], c[j + 8]);
  }
}

// The two in-lane stages alone, factored for the quartered W==16 form: with
// block-level transposition already done by 128-bit addressing, a 4x4
// transpose per 128-bit lane finishes the job (same s1/s2 index lists as
// the full tree; all vpunpck, no cross-lane traffic).
template <class Op, class V>
BLAKE3PP_FORCE_INLINE void inlane_4x4(const V (&r)[4], V (&t)[4]) noexcept {
  const V a0 = Op::template shuf<0, 16, 1, 17, 4, 20, 5, 21,  8, 24,  9, 25, 12, 28, 13, 29>(r[0], r[1]);
  const V a1 = Op::template shuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15, 31>(r[0], r[1]);
  const V a2 = Op::template shuf<0, 16, 1, 17, 4, 20, 5, 21,  8, 24,  9, 25, 12, 28, 13, 29>(r[2], r[3]);
  const V a3 = Op::template shuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15, 31>(r[2], r[3]);
  t[0] = Op::template shuf<0, 1, 16, 17, 4, 5, 20, 21,  8,  9, 24, 25, 12, 13, 28, 29>(a0, a2);
  t[1] = Op::template shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(a0, a2);
  t[2] = Op::template shuf<0, 1, 16, 17, 4, 5, 20, 21,  8,  9, 24, 25, 12, 13, 28, 29>(a1, a3);
  t[3] = Op::template shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(a1, a3);
}

}  // namespace
}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS::shuffle_detail
