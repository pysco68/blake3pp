// The one kernel translation unit, compiled once per architecture variant by
// cmake/ArchKernels.cmake. The compression core is written once over a wide
// word type W: instantiated with plain uint32_t it is the scalar compress;
// instantiated with the simd facade's u32v it hashes width-many independent
// inputs at once, one per lane (BLAKE3's hash_many strategy).

#include "kernel/kernel.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <utility>

#ifndef BLAKE3PP_ARCH_NS
#error "kernel.cpp must be compiled with -DBLAKE3PP_ARCH_NS=<variant> (use blake3pp_add_kernel)"
#endif

#include "kernel/simd_facade.hpp"
#include "kernel/transpose.hpp"

#define BLAKE3PP_STR2(x) #x
#define BLAKE3PP_STR(x) BLAKE3PP_STR2(x)

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
namespace {

static_assert(u32v::width <= max_simd_degree);

// Byte-wise little-endian load/store: endian-independent, and every compiler
// folds it to a single mov on LE targets.
inline std::uint32_t load32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void store32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

template <int N>
inline std::uint32_t rot(std::uint32_t x) noexcept {
  return std::rotr(x, N);
}

// Instead of physically permuting the 16 message words between rounds, index
// them through the accumulated permutation: msg_schedule[r][i] is the
// original word that round r reads at position i. With vectors this saves 16
// register-to-register moves per round.
constexpr std::uint8_t msg_perm[16] = {2, 6,  3,  10, 7, 0,  4,  13,
                                       1, 11, 12, 5,  9, 14, 15, 8};

consteval std::array<std::array<std::uint8_t, 16>, 7> make_msg_schedule() {
  std::array<std::array<std::uint8_t, 16>, 7> s{};
  for (std::uint8_t i = 0; i < 16; ++i) {
    s[0][i] = i;
  }
  for (std::size_t r = 1; r < 7; ++r) {
    for (std::size_t i = 0; i < 16; ++i) {
      s[r][i] = s[r - 1][msg_perm[i]];
    }
  }
  return s;
}

constexpr auto msg_schedule = make_msg_schedule();

// The quarter-round (spec section 2.2), generic over the word type.
template <class W>
inline void g(W v[16], std::size_t a, std::size_t b, std::size_t c,
              std::size_t d, W mx, W my) noexcept {
  v[a] = v[a] + v[b] + mx;
  v[d] = rot<16>(v[d] ^ v[a]);
  v[c] = v[c] + v[d];
  v[b] = rot<12>(v[b] ^ v[c]);
  v[a] = v[a] + v[b] + my;
  v[d] = rot<8>(v[d] ^ v[a]);
  v[c] = v[c] + v[d];
  v[b] = rot<7>(v[b] ^ v[c]);
}

// The round index is a template parameter so every schedule lookup is a
// compile-time constant: message operands stay directly addressable instead
// of register-indexed loads. This matters most under high register pressure
// (wide vectors), where indirect indexing defeats the register allocator.
template <std::size_t R, class W>
inline void round_fn(W v[16], const W m[16]) noexcept {
  constexpr const std::array<std::uint8_t, 16>& s = msg_schedule[R];
  // Columns.
  g(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
  g(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
  g(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
  g(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
  // Diagonals.
  g(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
  g(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
  g(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
  g(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
}

template <class W>
inline void all_rounds(W v[16], const W m[16]) noexcept {
  [&]<std::size_t... R>(std::index_sequence<R...>) {
    (round_fn<R>(v, m), ...);
  }(std::make_index_sequence<7>{});
}

inline void compress(const std::uint32_t cv[8],
                     const std::uint8_t block[block_len], std::uint32_t len,
                     std::uint64_t counter, std::uint32_t flags,
                     std::uint32_t out[16]) noexcept {
  std::uint32_t m[16];
  for (std::size_t i = 0; i < 16; ++i) {
    m[i] = load32(block + 4 * i);
  }

  std::uint32_t v[16] = {
      cv[0], cv[1], cv[2], cv[3],
      cv[4], cv[5], cv[6], cv[7],
      iv[0], iv[1], iv[2], iv[3],
      static_cast<std::uint32_t>(counter),
      static_cast<std::uint32_t>(counter >> 32),
      len,   flags,
  };

  all_rounds(v, m);

  for (std::size_t i = 0; i < 8; ++i) {
    v[i] ^= v[i + 8];
    v[i + 8] ^= cv[i];
  }
  std::memcpy(out, v, sizeof(v));
}

void compress_in_place(std::uint32_t cv[8], const std::uint8_t block[block_len],
                       std::uint32_t len, std::uint64_t counter,
                       std::uint32_t flags) noexcept {
  std::uint32_t out[16];
  compress(cv, block, len, counter, flags, out);
  std::memcpy(cv, out, 8 * sizeof(std::uint32_t));
}

// Hashes exactly u32v::width inputs, one per SIMD lane. State and message
// live transposed: each of the 16 words is a vector holding that word for
// every lane. Message transposition goes through a small staging array; the
// rounds, the dominant cost, are pure vertical vector ops.
void hash_batch(const std::uint8_t* const* inputs, std::size_t blocks,
                const std::uint32_t key[8], std::uint64_t counter,
                bool increment_counter, std::uint32_t flags,
                std::uint32_t flags_start, std::uint32_t flags_end,
                std::uint8_t* out) noexcept {
  constexpr std::size_t W = u32v::width;

  u32v cv[8];
  for (std::size_t j = 0; j < 8; ++j) {
    cv[j] = u32v::broadcast(key[j]);
  }

  std::uint32_t lanes[W];
  for (std::size_t lane = 0; lane < W; ++lane) {
    const std::uint64_t c = counter + (increment_counter ? lane : 0);
    lanes[lane] = static_cast<std::uint32_t>(c);
  }
  const u32v ctr_lo = u32v::load(lanes);
  for (std::size_t lane = 0; lane < W; ++lane) {
    const std::uint64_t c = counter + (increment_counter ? lane : 0);
    lanes[lane] = static_cast<std::uint32_t>(c >> 32);
  }
  const u32v ctr_hi = u32v::load(lanes);

  for (std::size_t b = 0; b < blocks; ++b) {
    std::uint32_t block_flags = flags;
    if (b == 0) {
      block_flags |= flags_start;
    }
    if (b == blocks - 1) {
      block_flags |= flags_end;
    }

    u32v m[16];
    load_transposed(inputs, b * block_len, m);

    u32v v[16];
    for (std::size_t j = 0; j < 8; ++j) {
      v[j] = cv[j];
    }
    for (std::size_t j = 0; j < 4; ++j) {
      v[8 + j] = u32v::broadcast(iv[j]);
    }
    v[12] = ctr_lo;
    v[13] = ctr_hi;
    v[14] = u32v::broadcast(static_cast<std::uint32_t>(block_len));
    v[15] = u32v::broadcast(block_flags);

    all_rounds(v, m);

    for (std::size_t j = 0; j < 8; ++j) {
      cv[j] = v[j] ^ v[j + 8];
    }
  }

  for (std::size_t j = 0; j < 8; ++j) {
    cv[j].store(lanes);
    for (std::size_t lane = 0; lane < W; ++lane) {
      store32(out + lane * out_len + 4 * j, lanes[lane]);
    }
  }
}

void hash_many(const std::uint8_t* const* inputs, std::size_t num_inputs,
               std::size_t blocks, const std::uint32_t key[8],
               std::uint64_t counter, bool increment_counter,
               std::uint32_t flags, std::uint32_t flags_start,
               std::uint32_t flags_end, std::uint8_t* out) noexcept {
  std::size_t i = 0;
  if constexpr (u32v::width > 1) {
    for (; i + u32v::width <= num_inputs; i += u32v::width) {
      hash_batch(inputs + i, blocks, key,
                 counter + (increment_counter ? i : 0), increment_counter,
                 flags, flags_start, flags_end, out + i * out_len);
    }
  }
  for (; i < num_inputs; ++i) {
    std::uint32_t cv[8];
    std::memcpy(cv, key, sizeof(cv));
    const std::uint64_t ctr = counter + (increment_counter ? i : 0);
    for (std::size_t b = 0; b < blocks; ++b) {
      std::uint32_t f = flags;
      if (b == 0) {
        f |= flags_start;
      }
      if (b == blocks - 1) {
        f |= flags_end;
      }
      compress_in_place(cv, inputs[i] + b * block_len,
                        static_cast<std::uint32_t>(block_len), ctr, f);
    }
    for (std::size_t w = 0; w < 8; ++w) {
      store32(out + i * out_len + 4 * w, cv[w]);
    }
  }
}

}  // namespace

// Namespace-scope const defaults to internal linkage; the explicit extern
// declaration keeps `ops` exported no matter which BLAKE3PP_HAS_KERNEL_*
// guards were visible in kernel.hpp for this TU.
extern const kernel_ops ops;
const kernel_ops ops = {
    BLAKE3PP_STR(BLAKE3PP_ARCH_NS),
    /*simd_degree=*/u32v::width,
    &compress_in_place,
    &hash_many,
};

}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
