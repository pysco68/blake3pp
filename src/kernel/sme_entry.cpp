// The entry TU of a streaming (SME) kernel variant: the kernel_ops table
// and the four locally streaming forwarders that enter streaming mode,
// call the streaming-interface entries kernel.cpp exports for this
// variant, and leave it again. Compiled for SME WITHOUT SVE (clang:
// -march=armv8.6-a+sme; GCC has no such mode and gets the kernel's flags),
// so no SVE type or intrinsic can appear here and the compiler cannot
// place an SVE instruction in the non-streaming prologue or epilogue: on
// the SME-only parts this variant exists for (Apple M4 and later), the
// first such instruction traps. clang 21 does exactly that for a locally
// streaming function in a TU that has SVE available (cntd to save the
// vector granule for unwinding); LLVM 22 no longer does, GCC never did.
#include <cstddef>
#include <cstdint>

#include "kernel/kernel.hpp"

#ifndef BLAKE3PP_ARCH_NS
#error "sme_entry.cpp is kernel-TU-internal; compile with -DBLAKE3PP_ARCH_NS=<variant>"
#endif
#ifndef BLAKE3PP_KERNEL_LANES
#error "sme_entry.cpp needs -DBLAKE3PP_KERNEL_LANES=<streaming vector length / 32>"
#endif

namespace blake3pp::kern::BLAKE3PP_ARCH_NS {

namespace streaming {
void compress_in_place(std::uint32_t cv[8],
                                 const std::uint8_t block[block_len],
                                 std::uint32_t len, std::uint64_t counter,
                                 std::uint32_t flags) noexcept __arm_streaming;
void compress_xof(const std::uint32_t cv[8],
                            const std::uint8_t block[block_len],
                            std::uint32_t len, std::uint64_t counter,
                            std::uint32_t flags,
                            std::uint8_t out[64]) noexcept __arm_streaming;
void xof_many(const std::uint32_t cv[8],
                        const std::uint8_t block[block_len], std::uint32_t len,
                        std::uint64_t counter, std::uint32_t flags,
                        std::uint8_t* out,
                        std::size_t num_blocks) noexcept __arm_streaming;
void hash_many(const std::uint8_t* const* inputs,
                         std::size_t num_inputs, std::size_t blocks,
                         const std::uint32_t key[8], std::uint64_t counter,
                         bool increment_counter, std::uint32_t flags,
                         std::uint32_t flags_start, std::uint32_t flags_end,
                         std::uint8_t* out) noexcept __arm_streaming;
}  // namespace streaming

namespace {

__arm_locally_streaming void compress_in_place_entry(
    std::uint32_t cv[8], const std::uint8_t block[block_len],
    std::uint32_t len, std::uint64_t counter, std::uint32_t flags) noexcept {
  streaming::compress_in_place(cv, block, len, counter, flags);
}

__arm_locally_streaming void compress_xof_entry(
    const std::uint32_t cv[8], const std::uint8_t block[block_len],
    std::uint32_t len, std::uint64_t counter, std::uint32_t flags,
    std::uint8_t out[64]) noexcept {
  streaming::compress_xof(cv, block, len, counter, flags, out);
}

__arm_locally_streaming void xof_many_entry(const std::uint32_t cv[8],
                                      const std::uint8_t block[block_len],
                                      std::uint32_t len, std::uint64_t counter,
                                      std::uint32_t flags, std::uint8_t* out,
                                      std::size_t num_blocks) noexcept {
  streaming::xof_many(cv, block, len, counter, flags, out, num_blocks);
}

__arm_locally_streaming void hash_many_entry(
    const std::uint8_t* const* inputs, std::size_t num_inputs,
    std::size_t blocks, const std::uint32_t key[8], std::uint64_t counter,
    bool increment_counter, std::uint32_t flags, std::uint32_t flags_start,
    std::uint32_t flags_end, std::uint8_t* out) noexcept {
  streaming::hash_many(inputs, num_inputs, blocks, key, counter,
                      increment_counter, flags, flags_start, flags_end, out);
}

}  // namespace

extern const kernel_ops ops;
const kernel_ops ops = {
    arch::BLAKE3PP_ARCH_NS,
    /*simd_degree=*/BLAKE3PP_KERNEL_LANES,
    &compress_in_place_entry,
    &compress_xof_entry,
    &xof_many_entry,
    &hash_many_entry,
};

}  // namespace blake3pp::kern::BLAKE3PP_ARCH_NS
