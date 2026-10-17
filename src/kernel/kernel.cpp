// The one kernel translation unit, compiled once per architecture variant by
// cmake/ArchKernels.cmake. The compression core is written once over a wide
// word type W: instantiated with plain uint32_t it is the scalar compress;
// instantiated with the simd facade's u32v it hashes width-many independent
// inputs at once, one per lane (BLAKE3's hash_many strategy).

#include "kernel/endian.hpp"
#include "kernel/kernel.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

#ifndef BLAKE3PP_ARCH_NS
#error "kernel.cpp must be compiled with -DBLAKE3PP_ARCH_NS=<variant> (use blake3pp_add_kernel)"
#endif

#include "kernel/rotate.hpp"
#include "kernel/uninitialized.hpp"
#include "kernel/simd_facade.hpp"
#include "kernel/transpose.hpp"

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {
namespace {

static_assert(u32v::width <= max_simd_degree,
              "BLAKE3PP_MAX_SIMD_DEGREE is narrower than this kernel's "
              "simd_degree: it sizes the staging buffers this kernel fills");

// A SIMD variant whose provider found no width for the target would
// compile to a one-lane kernel: correct, as slow as the scalar one, and
// registered under a name that says otherwise -- a dispatch table whose
// avx2 entry is scalar. It happens where a provider has no backend for an
// ISA the compiler itself supports (GCC 16's <simd> carries its arch
// traits under x86 only, so a std-provider build elsewhere deduces one
// lane), which is a build to decline rather than ship.
//
// The scalar variant is the one that means it, and BLAKE3PP_FORCE_SCALAR
// is how a target asks for it by name.
static_assert(u32v::width > 1 || arch::BLAKE3PP_ARCH_NS == arch::scalar,
              "this simd provider has no vector width for the target: the "
              "variant would be a one-lane kernel under a vector name. Pick "
              "another BLAKE3PP_SIMD_PROVIDER for it, or build the scalar "
              "variant instead");

// Little-endian load and store, spelled memcpy plus byteswap instead of
// the byte-wise shift-or idiom. GCC and Clang fold both spellings to a
// single mov on LE targets. MSVC 19.51 does not recognize the shift-or
// idiom at all: it emitted the four movzx/shl/or per word verbatim, ten
// instructions per message word in the scalar kernel's block loop.
//
// The memcpy folds to one mov on every compiler, and the byteswap arm
// keeps the endian independence of the shift-or spelling.
//
// std::byteswap is C++23; the C++20 presets get the shift-mask spelling,
// which every compiler folds to a single bswap. Feature-tested rather
// than __cplusplus-gated, and needed even though the call sits in a
// discarded if-constexpr branch: non-dependent names in discarded
// branches must still exist.
using endian::bswap32;
using endian::load32;
using endian::store32;

// The clang that zig 0.16 bundles, 21.1.0, merges the byte-wise word
// loads of the message below into element-width vector loads reading the
// caller's pointer directly. Where a misaligned vector access faults that
// turns a legal load into a bus error: RISC-V lets an implementation
// refuse such an access and the SpacemiT X60 (RVV 1.0) does, while x86
// and AArch64 perform them in hardware.
//
// Measured on one preprocessed source, counting loads whose base is the
// block parameter in compress_in_place:
//
//   zig 0.16 (clang 21.1.0)        three
//   Ubuntu clang 21.1.8            none
//   zig 0.17-dev (clang 22.1.8)    none
//   Ubuntu clang 22.1.8            none
//   GCC 15                         none
//
// So a 21.1.x patch release, or zig's own build of it, is the dividing
// line. The version bound below is conservative: it also covers clang 21
// builds that do not need it.
#if defined(__riscv) && defined(__riscv_v) && defined(__clang__) && __clang_major__ < 22
#define BLAKE3PP_ALIGN_MESSAGE_BLOCK 1
#else
#define BLAKE3PP_ALIGN_MESSAGE_BLOCK 0
#endif

template <int N>
BLAKE3PP_FORCE_INLINE std::uint32_t rot(std::uint32_t x) noexcept {
  return std::rotr(x, N);
}

// Scalar twin of the wide xor_rot in transpose.hpp: the rounds are written
// against the fused form so SVE2's XAR can claim it; everywhere else the
// compilers fold this back to the eor + ror pair.
template <int N>
BLAKE3PP_FORCE_INLINE std::uint32_t xor_rot(std::uint32_t x,
                                            std::uint32_t y) noexcept {
  return std::rotr(x ^ y, N);
}

// The spec's 64-bit block counter enters the state as two u32 words
// (v[12]/v[13], t0/t1): this pair is the one place the kernel knowingly
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
// Scheduling, measured on znver3. This plain sequential-g spelling is the
// fastest of three schedules. llvm-mca shows it latency-bound, at 459
// cycles per block against a 196 port floor, and IPC 2.56 where upstream's
// hand-scheduled asm reaches 3.50. Exposing more ILP in source is slower:
//
//   - Interleaving two independent batches doubles live state past the 16
//     architectural registers, 2.66 -> 1.83 GiB/s.
//   - Staging the four quartets' micro-steps helps narrow widths but
//     pessimizes AVX2 spill placement on both compilers, ratio against
//     upstream 0.85 -> 0.71.
//   - Staging via index arrays defeats SROA entirely, Clang 0.77 GiB/s.
//
// The residual against hand-written assembly is scheduler quality, and
// source-level reordering cannot reliably buy it back.
template <class W>
BLAKE3PP_FORCE_INLINE void g(W v[16], std::size_t a, std::size_t b, std::size_t c,
              std::size_t d, W mx, W my) noexcept {
  v[a] = v[a] + v[b] + mx;
  v[d] = xor_rot<16>(v[d], v[a]);
  v[c] = v[c] + v[d];
  v[b] = xor_rot<12>(v[b], v[c]);
  v[a] = v[a] + v[b] + my;
  v[d] = xor_rot<8>(v[d], v[a]);
  v[c] = v[c] + v[d];
  v[b] = xor_rot<7>(v[b], v[c]);
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
// (Apple M2, clang 22: 1.61 vs 1.59 GiB/s sequential). The difference
// exists only because the round core is register-resident (see the
// always_inline note in simd_facade.hpp); with the core outlined to
// memory, both spellings compile identically.
template <std::size_t R, class W>
BLAKE3PP_FORCE_INLINE void round_fn_staged(W v[16], const W m[16]) noexcept {
  constexpr const std::array<std::uint8_t, 16>& s = msg_schedule[R];
  // Columns.
  v[0] = v[0] + v[4] + m[s[0]];
  v[1] = v[1] + v[5] + m[s[2]];
  v[2] = v[2] + v[6] + m[s[4]];
  v[3] = v[3] + v[7] + m[s[6]];
  v[12] = xor_rot<16>(v[12], v[0]);
  v[13] = xor_rot<16>(v[13], v[1]);
  v[14] = xor_rot<16>(v[14], v[2]);
  v[15] = xor_rot<16>(v[15], v[3]);
  v[8] = v[8] + v[12];
  v[9] = v[9] + v[13];
  v[10] = v[10] + v[14];
  v[11] = v[11] + v[15];
  v[4] = xor_rot<12>(v[4], v[8]);
  v[5] = xor_rot<12>(v[5], v[9]);
  v[6] = xor_rot<12>(v[6], v[10]);
  v[7] = xor_rot<12>(v[7], v[11]);
  v[0] = v[0] + v[4] + m[s[1]];
  v[1] = v[1] + v[5] + m[s[3]];
  v[2] = v[2] + v[6] + m[s[5]];
  v[3] = v[3] + v[7] + m[s[7]];
  v[12] = xor_rot<8>(v[12], v[0]);
  v[13] = xor_rot<8>(v[13], v[1]);
  v[14] = xor_rot<8>(v[14], v[2]);
  v[15] = xor_rot<8>(v[15], v[3]);
  v[8] = v[8] + v[12];
  v[9] = v[9] + v[13];
  v[10] = v[10] + v[14];
  v[11] = v[11] + v[15];
  v[4] = xor_rot<7>(v[4], v[8]);
  v[5] = xor_rot<7>(v[5], v[9]);
  v[6] = xor_rot<7>(v[6], v[10]);
  v[7] = xor_rot<7>(v[7], v[11]);
  // Diagonals: quartet i is (i, {5,6,7,4}[i], {10,11,8,9}[i],
  // {15,12,13,14}[i]).
  v[0] = v[0] + v[5] + m[s[8]];
  v[1] = v[1] + v[6] + m[s[10]];
  v[2] = v[2] + v[7] + m[s[12]];
  v[3] = v[3] + v[4] + m[s[14]];
  v[15] = xor_rot<16>(v[15], v[0]);
  v[12] = xor_rot<16>(v[12], v[1]);
  v[13] = xor_rot<16>(v[13], v[2]);
  v[14] = xor_rot<16>(v[14], v[3]);
  v[10] = v[10] + v[15];
  v[11] = v[11] + v[12];
  v[8] = v[8] + v[13];
  v[9] = v[9] + v[14];
  v[5] = xor_rot<12>(v[5], v[10]);
  v[6] = xor_rot<12>(v[6], v[11]);
  v[7] = xor_rot<12>(v[7], v[8]);
  v[4] = xor_rot<12>(v[4], v[9]);
  v[0] = v[0] + v[5] + m[s[9]];
  v[1] = v[1] + v[6] + m[s[11]];
  v[2] = v[2] + v[7] + m[s[13]];
  v[3] = v[3] + v[4] + m[s[15]];
  v[15] = xor_rot<8>(v[15], v[0]);
  v[12] = xor_rot<8>(v[12], v[1]);
  v[13] = xor_rot<8>(v[13], v[2]);
  v[14] = xor_rot<8>(v[14], v[3]);
  v[10] = v[10] + v[15];
  v[11] = v[11] + v[12];
  v[8] = v[8] + v[13];
  v[9] = v[9] + v[14];
  v[5] = xor_rot<7>(v[5], v[10]);
  v[6] = xor_rot<7>(v[6], v[11]);
  v[7] = xor_rot<7>(v[7], v[8]);
  v[4] = xor_rot<7>(v[4], v[9]);
}

// Measured on clang/Apple M2 only. GCC 15 on aarch64 compiles both spellings
// to the SAME schedule (the objects differ in register naming alone, and
// llvm-mca gives identical cycle counts on apple-m2, neoverse-n1 and
// neoverse-v2), so this gate is live but inert there.
//
// Set by cmake/ArchKernels.cmake from -DBLAKE3PP_KERNEL_STAGED_ROUNDS=
// auto|on|off; the fallback repeats that default. Off restores the
// sequential round for re-measurement.
#ifndef BLAKE3PP_KERNEL_STAGED_ROUNDS
#define BLAKE3PP_KERNEL_STAGED_ROUNDS 1
#endif

// Set by cmake/ArchKernels.cmake like the other switches; off restores the
// register-held message for the A/B (see all_rounds below).
#ifndef BLAKE3PP_KERNEL_PIN_MESSAGE
#define BLAKE3PP_KERNEL_PIN_MESSAGE 1
#endif

#if defined(__aarch64__) && BLAKE3PP_KERNEL_STAGED_ROUNDS
constexpr bool staged_rounds = (u32v::width == 4);
#else
constexpr bool staged_rounds = false;
#endif

template <class W>
BLAKE3PP_FORCE_INLINE void all_rounds(W v[16], const W m[16]) noexcept {
  // The message array's address escapes once, so it must live in memory and
  // the round body reads it through memory operands instead of holding 16
  // vectors of it in registers. At full AVX-512 width the state alone is 16
  // of the 32 zmm, so the message cannot fit beside it and what the compiler
  // keeps there it pays for twice over; freeing those registers is worth
  // 3-6% on the chunk shape alone (Zen 5, clang 22, 32 inputs per call) and
  // 7.8% on the kernel row end to end, which docs/building.md quotes. An empty
  // asm with an input-only constraint, so nothing is clobbered and the state
  // stays register-resident: a memory clobber here forces the state out too
  // and loses 4%.
  //
  // Narrower widths do not want it: measured +0.8% and -1.8% on avx2 and
  // -0.1% on sse42, all inside the noise, because at eight lanes and 16
  // registers there is room for both. Hence the width gate, and x86 because
  // that is where it is measured; a wide SVE or RVV target would have to
  // measure its own.
#if defined(__AVX512F__) && (defined(__GNUC__) || defined(__clang__)) && \
    BLAKE3PP_KERNEL_PIN_MESSAGE
  // Wide-word-only, like the staged_rounds gate below: u32v::width is the
  // translation unit's width and says nothing about W. The scalar compress
  // in this same TU -- the incremental hasher's partial block, and
  // hash_many's remainder below pad_remainder_min -- has sixteen
  // general-purpose registers and no register pressure to relieve, and
  // nothing was measured for it.
  if constexpr (std::is_same_v<W, u32v> && u32v::width >= 16) {
    asm volatile("" : : "r"(m));
  }
#endif
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

// A caller's 64-byte block in a form the word loads may read.
//
// See BLAKE3PP_ALIGN_MESSAGE_BLOCK: where a compiler merges the sixteen
// word loads into element-width vector loads, a block the caller aligned
// to nothing faults on the targets that require the type's alignment.
// Every path that reads a block through load32() goes through this, so
// the mitigation cannot be complete for one of them and missing for
// another.
//
// Staging unconditionally does not work: the optimizer forwards the copy
// and rebuilds the loads from the original pointer. The aligned case,
// which is the common one, pays one predictable branch and no copy.
class readable_block {
 public:
  explicit BLAKE3PP_FORCE_INLINE readable_block(
      const std::uint8_t* block) noexcept
      : at_(block) {
#if BLAKE3PP_ALIGN_MESSAGE_BLOCK
    if ((std::bit_cast<std::uintptr_t>(at_) &
         (alignof(std::uint32_t) - 1)) != 0) {
      std::memcpy(staged_, at_, block_len);
      at_ = staged_;
    }
    at_ = static_cast<const std::uint8_t*>(
        __builtin_assume_aligned(at_, alignof(std::uint32_t)));
#endif
  }
  readable_block(const readable_block&) = delete;
  readable_block& operator=(const readable_block&) = delete;

  [[nodiscard]] BLAKE3PP_FORCE_INLINE const std::uint8_t* get()
      const noexcept {
    return at_;
  }

 private:
#if BLAKE3PP_ALIGN_MESSAGE_BLOCK
  alignas(std::uint32_t) std::uint8_t staged_ BLAKE3PP_CXXATTR_UNINITIALIZED
      [block_len];
#endif
  const std::uint8_t* at_;
};

BLAKE3PP_FORCE_INLINE void compress(const std::uint32_t cv[8],
                     const std::uint8_t block[block_len], std::uint32_t len,
                     std::uint64_t counter, std::uint32_t flags,
                     std::array<std::uint32_t, 16>& out) noexcept {
  // See readable_block: where the merged load would fault, an unaligned
  // block is copied once so that the address really is aligned.
  const readable_block in(block);
  const std::uint8_t* const src = in.get();

  std::uint32_t m BLAKE3PP_CXXATTR_UNINITIALIZED [16];
  for (std::size_t i = 0; i < 16; ++i) {
    m[i] = load32(src + 4 * i);
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
  // A counted loop, not std::copy_n: MSVC lowers copy_n of 8 uint32_t to an
  // out-of-line std::_Copy_memmove_n call rather than 32 bytes of inline
  // moves, and in hash_many's block loop below that call lands in the
  // innermost loop. clang-cl inlines it. Both inline the explicit loop.
  for (std::size_t i = 0; i < 8; ++i) {
    cv[i] = out[i];
  }
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
// words. An aggregate rather than std::pair: MSVC leaves the pair's
// constructor out of line for the 64-byte AVX-512 vectors, a call in
// every hash_batch and xof_wide prologue.
struct counter_words {
  u32v lo;
  u32v hi;
};

BLAKE3PP_FORCE_INLINE counter_words counter_lanes(
    std::uint64_t counter, bool increment_counter) noexcept {
  std::uint32_t lo BLAKE3PP_CXXATTR_UNINITIALIZED [u32v::width];
  std::uint32_t hi BLAKE3PP_CXXATTR_UNINITIALIZED [u32v::width];
  for (std::size_t lane = 0; lane < u32v::width; ++lane) {
    const std::uint64_t c = counter + (increment_counter ? lane : 0);
    lo[lane] = counter_lo(c);
    hi[lane] = counter_hi(c);
  }
  return {u32v::load(lo), u32v::load(hi)};
}

// The two wide compressions below set up the same 16-word state and pick
// block flags the same way, and folding either into a shared helper was
// measured and rejected: the state initialiser costs the chunk path 1 to 3%
// on Zen 3+ (gcc 16, four alternating rounds, 3.69-3.77 against 3.81
// GiB/s), because it forces the XOF's broadcast chaining values through an
// array and changes the register allocation of hash_batch's block loop.
// The duplication is two sites in one file; the throughput is not.

// Fills u32v::width consecutive XOF output blocks: the root node's cv and
// message are BROADCAST (identical in every lane); only the counter varies
// per lane. No input transpose exists at all; the store is the only
// lane-major step.
void xof_wide(const std::uint32_t cv[8], const std::uint8_t block[block_len],
              std::uint32_t len, std::uint64_t counter, std::uint32_t flags,
              std::uint8_t* out) noexcept {
  const readable_block in(block);

  u32v m[16];
  for (std::size_t i = 0; i < 16; ++i) {
    m[i] = u32v::broadcast(load32(in.get() + 4 * i));
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

  store_cvs_transposed<W>(cv, out);
}

// Smallest remainder hash_many pads out to a full-width batch instead of
// finishing through the scalar compress, measured per lane count on the
// one-block parent shape. At W=16 (Emerald Rapids, full-width AVX-512) a
// padded batch beats three scalar blocks and loses to two; at W=8 and W=4
// (Zen 3+) it already beats two, by 25% and 15%. Widths without a
// measurement never pad.
constexpr std::size_t pad_remainder_min = u32v::width >= 16  ? 3
                                          : u32v::width == 8 ? 2
                                          : u32v::width == 4 ? 2
                                                             : u32v::width;

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
  // A remainder of at least pad_remainder_min inputs runs as one full-width
  // batch, the unused lanes aimed at the last real input (every lane reads
  // valid memory) and their outputs dropped. The wide batch costs the same
  // whatever the lane count and the scalar loop a block per input, so the
  // threshold is where one batch undercuts that many scalar blocks.
  if constexpr (u32v::width > 1) {
    if (const std::size_t rem = num_inputs - i; rem >= pad_remainder_min) {
      const std::uint8_t* padded[u32v::width];
      for (std::size_t l = 0; l < u32v::width; ++l) {
        padded[l] = inputs[i + (l < rem ? l : rem - 1)];
      }
      std::uint8_t tmp[u32v::width * out_len];
      hash_batch(padded, blocks, key, counter + (increment_counter ? i : 0),
                 increment_counter, flags, flags_start, flags_end, tmp);
      std::memcpy(out + i * out_len, tmp, rem * out_len);
      i = num_inputs;
    }
  }
  for (; i < num_inputs; ++i) {
    std::array<std::uint32_t, 8> cv;
    for (std::size_t j = 0; j < 8; ++j) {
      cv[j] = key[j];
    }
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
      for (std::size_t j = 0; j < 8; ++j) {
        cv[j] = wide[j];
      }
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
