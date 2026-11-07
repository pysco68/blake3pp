#pragma once

// A kernel table of the test's own: the scalar one with a hash_many that
// counts the chunks it is given, the way an expert caller's hand-written
// table would sit in front of the library's. An engine that compresses
// with the library's table instead of the hasher's leaves the count short.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <blake3pp/dispatch.hpp>

#include "kernel/kernel.hpp"

namespace b3test {

struct counting_kernels {
  static inline std::atomic<std::size_t> chunks{0};

  [[nodiscard]] static const blake3pp::kern::kernel_ops* table() noexcept {
    static const blake3pp::kern::kernel_ops t = [] {
      blake3pp::kern::kernel_ops copy = *base();
      copy.hash_many = &hash_many;
      return copy;
    }();
    return &t;
  }

 private:
  [[nodiscard]] static const blake3pp::kern::kernel_ops* base() noexcept {
    return blake3pp::detail::resolve(blake3pp::arch::scalar);
  }

  static void hash_many(const std::uint8_t* const* inputs,
                        std::size_t num_inputs, std::size_t blocks,
                        const std::uint32_t key[8], std::uint64_t counter,
                        bool increment_counter, std::uint32_t flags,
                        std::uint32_t flags_start, std::uint32_t flags_end,
                        std::uint8_t* out) noexcept {
    chunks.fetch_add(num_inputs, std::memory_order_relaxed);
    base()->hash_many(inputs, num_inputs, blocks, key, counter,
                      increment_counter, flags, flags_start, flags_end, out);
  }
};

}  // namespace b3test
