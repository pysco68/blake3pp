#pragma once

// Shuffle backend: xsimd's own portable two-input constant shuffle.
//
// The fallback for frontends without vector extensions; in practice MSVC,
// which has no __builtin_shufflevector. There xsimd decomposes each shuffle
// into swizzle(x)+swizzle(y)+select (~3 uops), still far ahead of the scalar
// staging gather. On GCC/Clang xsimd lowers it through
// __builtin_shufflevector, so this backend and vext are instruction-
// identical there (verified by object-histogram diff), which is why the
// selection in shuffle.hpp can prefer either without a performance cliff.
//
// Requires the xsimd provider: it shuffles u32v::impl directly, no cast.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "kernel/force_inline.hpp"
#include "kernel/simd_facade.hpp"

namespace blake3pp::kern::BLAKE3PP_ARCH_NS::shuffle_detail {

// Byte-rotate mask: dest byte i of each 32-bit element takes source byte
// ((i%4)+RB)%4, a little-endian rotr by 8*RB bits, the same pattern the
// vext backend's rot_bytes encodes. Lane-local by construction, which is what
// makes xsimd 14.3's constant u8 swizzle emit a single vpshufb (its
// is_cross_lane check) instead of a cross-lane fixup.
template <int RB>
struct rot_bytes_gen {
  static constexpr std::uint8_t get(std::size_t i, std::size_t) noexcept {
    return static_cast<std::uint8_t>((i / 4) * 4 + ((i % 4) + RB) % 4);
  }
};

struct xsimd_backend {
  // The batch's width IS W; there is no separate register type to name.
  template <std::size_t W>
  using reg = typename u32v::impl;

  template <std::size_t W>
  static constexpr bool supports = (W == 4 || W == 8 || W == 16) &&
                                   W == u32v::width &&
                                   std::endian::native == std::endian::little;

  // CRITICAL gate for the byte-rotate: the hardware must actually HAVE a
  // byte shuffle. MSVC has no /arch:SSE4.2, so the "sse42" kernel is
  // xsimd-sse2 there, and xsimd's sse2 u8 swizzle is a per-byte scalar
  // loop that measured 4x WORSE than shift-or. x86 needs ssse3+; NEON and
  // wasm carry their byte shuffles natively.
  static constexpr bool is_x86 =
      std::is_base_of_v<xsimd::sse2, typename u32v::impl::arch_type>;
  static constexpr bool has_byte_shuffle =
      !is_x86 ||
      std::is_base_of_v<xsimd::ssse3, typename u32v::impl::arch_type>;

  // W==16 stays on shift-or: avx512bw has no constant u8 swizzle in
  // xsimd 14.3 (and GNU compilers fuse shift-or into vprold there anyway).
  template <std::size_t W>
  static constexpr bool supports_byte_rot =
      supports<W> && (W == 4 || W == 8) && has_byte_shuffle;

  template <int... I, class V>
  static BLAKE3PP_FORCE_INLINE V shuf(V a, V b) noexcept {
    return xsimd::shuffle(
        a, b,
        xsimd::batch_constant<std::uint32_t, typename V::arch_type,
                              static_cast<std::uint32_t>(I)...>{});
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
    return u32v{v};
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE reg<W> from_word(u32v w) noexcept {
    return w.v;
  }

  template <int RB, std::size_t W>
  static BLAKE3PP_FORCE_INLINE u32v rot_bytes(u32v w) noexcept {
    using B = typename u32v::impl;
    const auto bytes = xsimd::bitwise_cast<std::uint8_t>(w.v);
    constexpr auto mask =
        xsimd::make_batch_constant<std::uint8_t, rot_bytes_gen<RB>,
                                   typename B::arch_type>();
    return u32v{
        xsimd::bitwise_cast<std::uint32_t>(xsimd::swizzle(bytes, mask))};
  }
};

}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS::shuffle_detail
