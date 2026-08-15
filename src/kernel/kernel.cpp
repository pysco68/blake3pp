// The one kernel translation unit, compiled once per architecture variant by
// cmake/ArchKernels.cmake. M1 ships the scalar implementation; M2 replaces
// the inner loops with the simd_facade so this same file vectorizes under
// each variant's -m flags.

#include "kernel/kernel.hpp"

#include <bit>
#include <cstring>

#ifndef BLAKE3PP_ARCH_NS
#error "kernel.cpp must be compiled with -DBLAKE3PP_ARCH_NS=<variant> (use blake3pp_add_kernel)"
#endif

#define BLAKE3PP_STR2(x) #x
#define BLAKE3PP_STR(x) BLAKE3PP_STR2(x)

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
namespace {

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

// The quarter-round (spec section 2.2).
inline void g(std::uint32_t v[16], std::size_t a, std::size_t b, std::size_t c,
              std::size_t d, std::uint32_t mx, std::uint32_t my) noexcept {
  v[a] = v[a] + v[b] + mx;
  v[d] = std::rotr(v[d] ^ v[a], 16);
  v[c] = v[c] + v[d];
  v[b] = std::rotr(v[b] ^ v[c], 12);
  v[a] = v[a] + v[b] + my;
  v[d] = std::rotr(v[d] ^ v[a], 8);
  v[c] = v[c] + v[d];
  v[b] = std::rotr(v[b] ^ v[c], 7);
}

inline void round_fn(std::uint32_t v[16], const std::uint32_t m[16]) noexcept {
  // Columns.
  g(v, 0, 4, 8, 12, m[0], m[1]);
  g(v, 1, 5, 9, 13, m[2], m[3]);
  g(v, 2, 6, 10, 14, m[4], m[5]);
  g(v, 3, 7, 11, 15, m[6], m[7]);
  // Diagonals.
  g(v, 0, 5, 10, 15, m[8], m[9]);
  g(v, 1, 6, 11, 12, m[10], m[11]);
  g(v, 2, 7, 8, 13, m[12], m[13]);
  g(v, 3, 4, 9, 14, m[14], m[15]);
}

constexpr std::size_t msg_perm[16] = {2, 6,  3,  10, 7, 0,  4,  13,
                                      1, 11, 12, 5,  9, 14, 15, 8};

inline void permute(std::uint32_t m[16]) noexcept {
  std::uint32_t p[16];
  for (std::size_t i = 0; i < 16; ++i) {
    p[i] = m[msg_perm[i]];
  }
  std::memcpy(m, p, sizeof(p));
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

  round_fn(v, m);
  for (int r = 0; r < 6; ++r) {
    permute(m);
    round_fn(v, m);
  }

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

void hash_many(const std::uint8_t* const* inputs, std::size_t num_inputs,
               std::size_t blocks, const std::uint32_t key[8],
               std::uint64_t counter, bool increment_counter,
               std::uint32_t flags, std::uint32_t flags_start,
               std::uint32_t flags_end, std::uint8_t* out) noexcept {
  for (std::size_t i = 0; i < num_inputs; ++i) {
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

const kernel_ops ops = {
    BLAKE3PP_STR(BLAKE3PP_ARCH_NS),
    /*simd_degree=*/1,
    &compress_in_place,
    &hash_many,
};

}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
