#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <blake3pp/dispatch.hpp>

namespace blake3pp {

namespace detail {

// One chunk (up to 1024 bytes) in flight. The final block of a chunk is kept
// buffered rather than compressed eagerly: its flags (CHUNK_END, possibly
// ROOT) are only known once we see whether more input arrives.
struct chunk_state {
  std::uint32_t cv[8];
  std::uint64_t chunk_counter;
  std::uint8_t block[64];
  std::uint8_t block_len;
  std::uint8_t blocks_compressed;
};

}  // namespace detail

struct digest {
  std::array<std::byte, 32> bytes;

  friend bool operator==(const digest&, const digest&) = default;

  [[nodiscard]] std::string to_hex() const;
};

// Incremental BLAKE3 hasher. Fixed-size, trivially relocatable state; never
// allocates. The chaining-value stack is sized for the spec's maximum input
// of 2^64 bytes, hence the 54 entries.
class hasher {
 public:
  hasher() noexcept : hasher(arch::auto_detect) {}
  explicit hasher(arch a) noexcept;

  void update(std::span<const std::byte> input) noexcept;
  void update(std::string_view input) noexcept;

  [[nodiscard]] digest finalize() const noexcept;

  void reset() noexcept;

  [[nodiscard]] arch selected_arch() const noexcept;

 private:
  void push_chunk_cv(const std::uint32_t cv[8],
                     std::uint64_t total_chunks) noexcept;

  const kern::kernel_ops* ops_;
  detail::chunk_state chunk_;
  std::uint32_t cv_stack_[54][8];
  std::uint8_t cv_stack_len_;
};

[[nodiscard]] digest hash(std::span<const std::byte> input) noexcept;
[[nodiscard]] digest hash(std::string_view input) noexcept;

}  // namespace blake3pp
