// The one kernel translation unit, compiled once per architecture variant by
// cmake/ArchKernels.cmake. The compression core is written once over a wide
// word type W: instantiated with plain uint32_t it is the scalar compress;
// instantiated with the simd facade's u32v it hashes width-many independent
// inputs at once, one per lane (BLAKE3's hash_many strategy).

#include "kernel/kernel.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <type_traits>
#include <utility>

#ifndef BLAKE3PP_ARCH_NS
#error "kernel.cpp must be compiled with -DBLAKE3PP_ARCH_NS=<variant> (use blake3pp_add_kernel)"
#endif

#include "kernel/simd_facade.hpp"
#include "kernel/transpose.hpp"

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
namespace {

static_assert(u32v::width <= max_simd_degree);

// Byte-wise little-endian load/store: endian-independent, and every compiler
// folds it to a single mov on LE targets.
BLAKE3PP_FORCE_INLINE std::uint32_t load32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

BLAKE3PP_FORCE_INLINE void store32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

template <int N>
BLAKE3PP_FORCE_INLINE std::uint32_t rot(std::uint32_t x) noexcept {
  return std::rotr(x, N);
}

// The spec's 64-bit block counter enters the state as two u32 words
// (v[12]/v[13], t0/t1): this pair is the one place the kernel deliberately
// truncates.
BLAKE3PP_FORCE_INLINE std::uint32_t counter_lo(std::uint64_t c) noexcept {
  return static_cast<std::uint32_t>(c);
}
BLAKE3PP_FORCE_INLINE std::uint32_t counter_hi(std::uint64_t c) noexcept {
  return static_cast<std::uint32_t>(c >> 32);
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
//
// Scheduling note (all measured on znver3): this plain sequential-g
// spelling is the best of three schedules tried. llvm-mca shows it
// latency-bound (459 cycles/block vs a 196 port floor, IPC 2.56 where
// upstream's hand-scheduled asm reaches 3.50), yet every attempt to
// expose more ILP in source made things worse. Interleaving two independent
// batches doubled live state past the 16 architectural registers (2.66 ->
// 1.83 GiB/s); staging the four quartets' micro-steps helped narrow widths
// but pessimized AVX2 spill placement on both compilers (ratio vs upstream
// 0.85 -> 0.71); and staging via index arrays defeated SROA entirely
// (Clang 0.77 GiB/s). The residual vs hand-written assembly is scheduler
// quality, and source-level reordering cannot reliably buy it back.
template <class W>
BLAKE3PP_FORCE_INLINE void g(W v[16], std::size_t a, std::size_t b, std::size_t c,
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
// of register-indexed loads.
template <std::size_t R, class W>
BLAKE3PP_FORCE_INLINE void round_fn(W v[16], const W m[16]) noexcept {
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

// The same round, quartet-staged: the four g's identical micro-steps run
// batched (all four first-adds, then all four rot16s, and so on) instead of
// each g to completion; this is upstream blake3_neon.c's ordering. Every
// step is four independent dependency chains where sequential g serializes
// one. The cost is live state: all 16 state words plus message operands in
// flight at once. On x86's 16 architectural registers that loses (measured;
// see the scheduling note on g); aarch64's 32 vector registers hold the
// whole working set, and the spelling measured a small consistent win there
// (Apple M2, clang 22: 1.61 vs 1.59 GiB/s sequential). The difference only
// exists at all because the round core is register-resident (see the
// always_inline note in simd_facade.hpp); when it was outlined-to-memory,
// both spellings compiled identically.
template <std::size_t R, class W>
BLAKE3PP_FORCE_INLINE void round_fn_staged(W v[16], const W m[16]) noexcept {
  constexpr const std::array<std::uint8_t, 16>& s = msg_schedule[R];
  // Columns.
  v[0] = v[0] + v[4] + m[s[0]];
  v[1] = v[1] + v[5] + m[s[2]];
  v[2] = v[2] + v[6] + m[s[4]];
  v[3] = v[3] + v[7] + m[s[6]];
  v[12] = rot<16>(v[12] ^ v[0]);
  v[13] = rot<16>(v[13] ^ v[1]);
  v[14] = rot<16>(v[14] ^ v[2]);
  v[15] = rot<16>(v[15] ^ v[3]);
  v[8] = v[8] + v[12];
  v[9] = v[9] + v[13];
  v[10] = v[10] + v[14];
  v[11] = v[11] + v[15];
  v[4] = rot<12>(v[4] ^ v[8]);
  v[5] = rot<12>(v[5] ^ v[9]);
  v[6] = rot<12>(v[6] ^ v[10]);
  v[7] = rot<12>(v[7] ^ v[11]);
  v[0] = v[0] + v[4] + m[s[1]];
  v[1] = v[1] + v[5] + m[s[3]];
  v[2] = v[2] + v[6] + m[s[5]];
  v[3] = v[3] + v[7] + m[s[7]];
  v[12] = rot<8>(v[12] ^ v[0]);
  v[13] = rot<8>(v[13] ^ v[1]);
  v[14] = rot<8>(v[14] ^ v[2]);
  v[15] = rot<8>(v[15] ^ v[3]);
  v[8] = v[8] + v[12];
  v[9] = v[9] + v[13];
  v[10] = v[10] + v[14];
  v[11] = v[11] + v[15];
  v[4] = rot<7>(v[4] ^ v[8]);
  v[5] = rot<7>(v[5] ^ v[9]);
  v[6] = rot<7>(v[6] ^ v[10]);
  v[7] = rot<7>(v[7] ^ v[11]);
  // Diagonals: quartet i is (i, {5,6,7,4}[i], {10,11,8,9}[i],
  // {15,12,13,14}[i]).
  v[0] = v[0] + v[5] + m[s[8]];
  v[1] = v[1] + v[6] + m[s[10]];
  v[2] = v[2] + v[7] + m[s[12]];
  v[3] = v[3] + v[4] + m[s[14]];
  v[15] = rot<16>(v[15] ^ v[0]);
  v[12] = rot<16>(v[12] ^ v[1]);
  v[13] = rot<16>(v[13] ^ v[2]);
  v[14] = rot<16>(v[14] ^ v[3]);
  v[10] = v[10] + v[15];
  v[11] = v[11] + v[12];
  v[8] = v[8] + v[13];
  v[9] = v[9] + v[14];
  v[5] = rot<12>(v[5] ^ v[10]);
  v[6] = rot<12>(v[6] ^ v[11]);
  v[7] = rot<12>(v[7] ^ v[8]);
  v[4] = rot<12>(v[4] ^ v[9]);
  v[0] = v[0] + v[5] + m[s[9]];
  v[1] = v[1] + v[6] + m[s[11]];
  v[2] = v[2] + v[7] + m[s[13]];
  v[3] = v[3] + v[4] + m[s[15]];
  v[15] = rot<8>(v[15] ^ v[0]);
  v[12] = rot<8>(v[12] ^ v[1]);
  v[13] = rot<8>(v[13] ^ v[2]);
  v[14] = rot<8>(v[14] ^ v[3]);
  v[10] = v[10] + v[15];
  v[11] = v[11] + v[12];
  v[8] = v[8] + v[13];
  v[9] = v[9] + v[14];
  v[5] = rot<7>(v[5] ^ v[10]);
  v[6] = rot<7>(v[6] ^ v[11]);
  v[7] = rot<7>(v[7] ^ v[8]);
  v[4] = rot<7>(v[4] ^ v[9]);
}

// Measured on clang/Apple M2 only. GCC 15 on aarch64 compiles both spellings
// to the SAME schedule (the objects differ in register naming alone, and
// llvm-mca gives identical cycle counts on apple-m2, neoverse-n1 and
// neoverse-v2), so this gate is live but inert there.
//
// Set by cmake/ArchKernels.cmake from -DBLAKE3PP_KERNEL_STAGED_ROUNDS=
// auto|on|off; the fallback repeats that default. Off restores the
// sequential round for re-measurement on a core with a clock.
#ifndef BLAKE3PP_KERNEL_STAGED_ROUNDS
#define BLAKE3PP_KERNEL_STAGED_ROUNDS 1
#endif

#if defined(__aarch64__) && BLAKE3PP_KERNEL_STAGED_ROUNDS
constexpr bool staged_rounds = (u32v::width == 4);
#else
constexpr bool staged_rounds = false;
#endif

template <class W>
BLAKE3PP_FORCE_INLINE void all_rounds(W v[16], const W m[16]) noexcept {
  [&]<std::size_t... R>(std::index_sequence<R...>)
      BLAKE3PP_LAMBDA_FORCE_INLINE {
    // The gate is wide-word-only: the scalar compress in this same TU keeps
    // the sequential spelling.
    if constexpr (staged_rounds && std::is_same_v<W, u32v>) {
      (round_fn_staged<R>(v, m), ...);
    } else {
      (round_fn<R>(v, m), ...);
    }
  }(std::make_index_sequence<7>{});
}

BLAKE3PP_FORCE_INLINE void compress(const std::uint32_t cv[8],
                     const std::uint8_t block[block_len], std::uint32_t len,
                     std::uint64_t counter, std::uint32_t flags,
                     std::array<std::uint32_t, 16>& out) noexcept {
  std::uint32_t m[16];
  for (std::size_t i = 0; i < 16; ++i) {
    m[i] = load32(block + 4 * i);
  }

  std::array<std::uint32_t, 16> v = {
      cv[0], cv[1], cv[2], cv[3],
      cv[4], cv[5], cv[6], cv[7],
      iv[0], iv[1], iv[2], iv[3],
      counter_lo(counter), counter_hi(counter),
      len,   flags,
  };

  all_rounds(v.data(), m);

  for (std::size_t i = 0; i < 8; ++i) {
    v[i] ^= v[i + 8];
    v[i + 8] ^= cv[i];
  }
  out = v;
}

void compress_in_place(std::uint32_t cv[8], const std::uint8_t block[block_len],
                       std::uint32_t len, std::uint64_t counter,
                       std::uint32_t flags) noexcept {
  std::array<std::uint32_t, 16> out;
  compress(cv, block, len, counter, flags, out);
  std::copy_n(out.begin(), 8, cv);
}

void compress_xof(const std::uint32_t cv[8],
                  const std::uint8_t block[block_len], std::uint32_t len,
                  std::uint64_t counter, std::uint32_t flags,
                  std::uint8_t out[64]) noexcept {
  std::array<std::uint32_t, 16> wide;
  compress(cv, block, len, counter, flags, wide);
  for (std::size_t i = 0; i < 16; ++i) {
    store32(out + 4 * i, wide[i]);
  }
}

// Every lane's 64-bit counter, split lane-wise into the two u32 state
// words.
BLAKE3PP_FORCE_INLINE std::pair<u32v, u32v> counter_lanes(
    std::uint64_t counter, bool increment_counter) noexcept {
  std::uint32_t lo[u32v::width];
  std::uint32_t hi[u32v::width];
  for (std::size_t lane = 0; lane < u32v::width; ++lane) {
    const std::uint64_t c = counter + (increment_counter ? lane : 0);
    lo[lane] = counter_lo(c);
    hi[lane] = counter_hi(c);
  }
  return {u32v::load(lo), u32v::load(hi)};
}

// Fills u32v::width consecutive XOF output blocks: the root node's cv and
// message are BROADCAST (identical in every lane); only the counter varies
// per lane. No input transpose exists at all; the store is the only
// lane-major step.
void xof_wide(const std::uint32_t cv[8], const std::uint8_t block[block_len],
              std::uint32_t len, std::uint64_t counter, std::uint32_t flags,
              std::uint8_t* out) noexcept {
  constexpr std::size_t W = u32v::width;

  u32v m[16];
  for (std::size_t i = 0; i < 16; ++i) {
    m[i] = u32v::broadcast(load32(block + 4 * i));
  }

  const auto [ctr_lo, ctr_hi] = counter_lanes(counter, true);

  u32v v[16];
  for (std::size_t j = 0; j < 8; ++j) {
    v[j] = u32v::broadcast(cv[j]);
  }
  for (std::size_t j = 0; j < 4; ++j) {
    v[8 + j] = u32v::broadcast(iv[j]);
  }
  v[12] = ctr_lo;
  v[13] = ctr_hi;
  v[14] = u32v::broadcast(len);
  v[15] = u32v::broadcast(flags);

  all_rounds(v, m);

  u32v wide[16];
  for (std::size_t j = 0; j < 8; ++j) {
    wide[j] = v[j] ^ v[j + 8];
    wide[j + 8] = v[j + 8] ^ u32v::broadcast(cv[j]);
  }
  store_transposed(wide, out);
}

void xof_many(const std::uint32_t cv[8], const std::uint8_t block[block_len],
              std::uint32_t len, std::uint64_t counter, std::uint32_t flags,
              std::uint8_t* out, std::size_t num_blocks) noexcept {
  std::size_t i = 0;
  if constexpr (u32v::width > 1) {
    for (; i + u32v::width <= num_blocks; i += u32v::width) {
      xof_wide(cv, block, len, counter + i, flags, out + i * 64);
    }
  }
  for (; i < num_blocks; ++i) {
    compress_xof(cv, block, len, counter + i, flags, out + i * 64);
  }
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

  const auto [ctr_lo, ctr_hi] = counter_lanes(counter, increment_counter);

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
    v[14] = u32v::broadcast(block_len);
    v[15] = u32v::broadcast(block_flags);

    all_rounds(v, m);

    for (std::size_t j = 0; j < 8; ++j) {
      cv[j] = v[j] ^ v[j + 8];
    }
  }

  std::uint32_t lanes[W];
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
    std::array<std::uint32_t, 8> cv;
    std::copy_n(key, 8, cv.begin());
    const std::uint64_t ctr = counter + (increment_counter ? i : 0);
    for (std::size_t b = 0; b < blocks; ++b) {
      std::uint32_t f = flags;
      if (b == 0) {
        f |= flags_start;
      }
      if (b == blocks - 1) {
        f |= flags_end;
      }
      // The inlined compress (not the exported compress_in_place): keeps
      // the chaining value in registers across the block loop instead of a
      // call plus CV store/reload round-trip per 64-byte block.
      std::array<std::uint32_t, 16> wide;
      compress(cv.data(), inputs[i] + b * block_len, block_len, ctr, f,
               wide);
      std::copy_n(wide.begin(), 8, cv.begin());
    }
    for (std::size_t w = 0; w < 8; ++w) {
      store32(out + i * out_len + 4 * w, cv[w]);
    }
  }
}

}  // namespace

// Namespace-scope const defaults to internal linkage; the explicit extern
// declaration keeps `ops` exported without relying on any header having
// declared this TU's variant namespace.
extern const kernel_ops ops;
const kernel_ops ops = {
    // The namespace token doubles as the enum ID, the same single-source-
    // of-truth convention the generated registry relies on.
    arch::BLAKE3PP_ARCH_NS,
    /*simd_degree=*/u32v::width,
    &compress_in_place,
    &compress_xof,
    &xof_many,
    &hash_many,
};

}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
