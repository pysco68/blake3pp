#pragma once

// Shuffle backend: GNU/Clang vector extensions.
//
// GCC and Clang both provide __builtin_shufflevector over vector-extension
// types with compile-time indices, and both instruction-select a shuffle
// whose indices exactly match a hardware macro into that single instruction.
// This backend works for EVERY simd provider: the provider's register is
// bit_cast in and out, which is free because all of them are register-sized
// and trivially copyable.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

#include "kernel/force_inline.hpp"
#include "kernel/simd_facade.hpp"

namespace blake3pp::kern::BLAKE3PP_ARCH_NS::shuffle_detail {
namespace {

// GCC raises -Wpsabi for vector types wider than the TU's -m flags allow
// natively. These types never appear in any cross-TU signature (that is the
// entire point of the kernel's design), so the ABI concern is moot; GCC's
// own <simd> internals suppress the warning on the same reasoning.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"

template <std::size_t W>
struct vext;
template <>
struct vext<4> {
  typedef std::uint32_t type __attribute__((vector_size(16)));
};
template <>
struct vext<8> {
  typedef std::uint32_t type __attribute__((vector_size(32)));
};
template <>
struct vext<16> {
  typedef std::uint32_t type __attribute__((vector_size(64)));
};

// Byte view of the same register, for the byte-granular rotates.
template <std::size_t W>
struct bext;
template <>
struct bext<4> {
  typedef std::uint8_t type __attribute__((vector_size(16)));
};
template <>
struct bext<8> {
  typedef std::uint8_t type __attribute__((vector_size(32)));
};

struct vext_backend {
  template <std::size_t W>
  using reg = typename vext<W>::type;

  template <std::size_t W>
  static constexpr bool supports =
      (W == 4 || W == 8 || W == 16) &&
      std::endian::native == std::endian::little &&
      sizeof(typename u32v::impl) == 4 * W &&
      std::is_trivially_copyable_v<typename u32v::impl>;

  // Byte-granular rotate: rotr by a multiple of 8 bits is a byte
  // permutation within each 32-bit element, and the lane-local constexpr
  // pattern is exactly vpshufb (NEON: tbl, and clang recognizes the 16-bit
  // case as rev32): one uop instead of the three (shift, shift, or) a
  // generic rotate costs on ISAs without a native rotate instruction.
  template <std::size_t W>
  static constexpr bool supports_byte_rot = supports<W> && (W == 4 || W == 8);

  template <int... I, class V>
  static BLAKE3PP_FORCE_INLINE V shuf(V a, V b) noexcept {
    return __builtin_shufflevector(a, b, I...);
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE reg<W> load(const std::uint8_t* p) noexcept {
    reg<W> r;
    std::memcpy(&r, p, sizeof(r));
    return r;
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE void store(std::uint8_t* p, reg<W> v) noexcept {
    std::memcpy(p, &v, sizeof(v));
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE u32v to_word(reg<W> v) noexcept {
    return u32v{std::bit_cast<typename u32v::impl>(v)};
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE reg<W> from_word(u32v w) noexcept {
    return std::bit_cast<reg<W>>(w.v);
  }

  template <int RB, std::size_t W>
  static BLAKE3PP_FORCE_INLINE u32v rot_bytes(u32v w) noexcept {
    using B = typename bext<W>::type;
    const B b = std::bit_cast<B>(from_word<W>(w));
    const B r = [&]<std::size_t... I>(std::index_sequence<I...>) {
      // Little-endian: rotr by 8*RB bits moves source byte (j+RB)%4 into
      // destination byte j of each element.
      return __builtin_shufflevector(b, b,
                                     ((I / 4) * 4 + ((I % 4) + RB) % 4)...);
    }(std::make_index_sequence<4 * W>{});
    return to_word<W>(std::bit_cast<reg<W>>(r));
  }
};

#pragma GCC diagnostic pop

}  // namespace
}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS::shuffle_detail
