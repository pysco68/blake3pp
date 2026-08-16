#pragma once

// The dispatch boundary between arch-agnostic code and per-architecture
// kernels. kernel.cpp is compiled once per variant (see
// cmake/ArchKernels.cmake) with -DBLAKE3PP_ARCH_NS=<name> and that variant's
// -m flags; each compilation exports exactly one symbol table,
// blake3pp::kern::<name>::ops.
//
// Everything that crosses this boundary is flat: byte pointers and uint32
// words. SIMD vector types must never appear here; their ABI (width,
// registers) changes with the -m flags of the defining TU, so passing one
// across TUs compiled with different flags is undefined behavior in practice.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include <blake3pp/dispatch.hpp>  // arch: each table names its variant

namespace blake3pp::kern {

// Strategy for the width-16 (AVX-512) message transpose. A process-wide
// runtime dial rather than a compile-time one: CPUID cannot express
// "double-pumped datapath" (Strix Point and full-width Zen 5 report
// identical AVX-512 feature bits), so the right strategy is a property
// you MEASURE, not detect; see blake3pp::tune_transpose16(). Kernels
// read this with one relaxed load per >=16 KiB batch; every value is
// correct, so racing a change against running hashes is benign.
enum class transpose16_mode : std::uint8_t {
  staging = 0,    // scalar gather through a staging array
  tree = 1,       // 4-stage radix-2 register shuffle network
  quartered = 2,  // 128-bit insert-loads + in-lane unpacks (default)
};
extern std::atomic<transpose16_mode> transpose16_active;  // dispatch.cpp


inline constexpr std::size_t block_len = 64;
inline constexpr std::size_t chunk_len = 1024;
inline constexpr std::size_t out_len = 32;

// Upper bound on any variant's simd_degree (AVX-512: 16 u32 lanes); sizes
// the caller-side staging buffers for hash_many batches.
inline constexpr std::size_t max_simd_degree = 16;

// Callers hand hash_many up to TWO batches worth of inputs at once (the
// subtree leaf granularity); bounds the batch-shaped staging buffers.
inline constexpr std::size_t max_batch_inputs = 2 * max_simd_degree;

// BLAKE3 IV (identical to BLAKE2s / SHA-256's first eight constants).
inline constexpr std::array<std::uint32_t, 8> iv = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

// Domain-separation flags (spec section 2.3).
inline constexpr std::uint32_t flag_chunk_start = 1u << 0;
inline constexpr std::uint32_t flag_chunk_end = 1u << 1;
inline constexpr std::uint32_t flag_parent = 1u << 2;
inline constexpr std::uint32_t flag_root = 1u << 3;
inline constexpr std::uint32_t flag_keyed_hash = 1u << 4;
inline constexpr std::uint32_t flag_derive_key_context = 1u << 5;
inline constexpr std::uint32_t flag_derive_key_material = 1u << 6;

// Per-variant entry points. POD, constexpr-initialized in each kernel TU:
// no heap, no vtable, no dynamic registration. One predicted indirect call
// per >=1 KiB of work; everything behind it inlines under the variant's own
// flags.
struct kernel_ops {
  // Which variant this table implements; the identity behind
  // hasher::selected_arch() and friends.
  arch variant;

  // How many chunks the variant hashes per hash_many step for full
  // utilization (1 for scalar, SIMD width otherwise).
  std::size_t simd_degree;

  // cv <- first 8 words of compress(cv, block, ...). block may be unaligned;
  // len is the number of meaningful bytes in it (1..64, or 0 for the empty
  // input's only block).
  void (*compress_in_place)(std::uint32_t cv[8],
                            const std::uint8_t block[block_len],
                            std::uint32_t len, std::uint64_t counter,
                            std::uint32_t flags) noexcept;

  // The full 64-byte compression output as LE bytes, the extended-output
  // (XOF) primitive: output block t of the stream is this applied to the
  // ROOT node with counter t.
  void (*compress_xof)(const std::uint32_t cv[8],
                       const std::uint8_t block[block_len], std::uint32_t len,
                       std::uint64_t counter, std::uint32_t flags,
                       std::uint8_t out[64]) noexcept;

  // Extended-output workhorse: fills num_blocks consecutive 64-byte
  // output blocks (counters counter .. counter+num_blocks-1) of ONE root
  // node into out. Lanes map to output counters, the mirror image of
  // hash_many.
  void (*xof_many)(const std::uint32_t cv[8],
                   const std::uint8_t block[block_len], std::uint32_t len,
                   std::uint64_t counter, std::uint32_t flags,
                   std::uint8_t* out, std::size_t num_blocks) noexcept;

  // Hash num_inputs inputs of `blocks` full 64-byte blocks each, writing one
  // 32-byte chaining value per input to out. Each input i starts from key
  // as its CV and uses counter (+ i when increment_counter). flags_start /
  // flags_end are OR'ed into the first / last block's flags. This is the
  // SIMD workhorse: lanes map to inputs.
  void (*hash_many)(const std::uint8_t* const* inputs, std::size_t num_inputs,
                    std::size_t blocks, const std::uint32_t key[8],
                    std::uint64_t counter, bool increment_counter,
                    std::uint32_t flags, std::uint32_t flags_start,
                    std::uint32_t flags_end, std::uint8_t* out) noexcept;
};

// Per-variant tables are declared where they are consumed: dispatch.cpp
// expands the build-generated blake3pp_kernel_registry.inc into extern
// declarations for every registered variant, so no hand-maintained list
// exists here. Only the always-present scalar oracle is declared for
// direct use (tests pin its semantics as the reference for every variant).
namespace scalar {
extern const kernel_ops ops;
}

}  // namespace blake3pp::kern
