#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <version>

#if defined(__cpp_lib_format)
#include <format>
#endif

#include <blake3pp/dispatch.hpp>

namespace blake3pp {

// The BLAKE3 chunk granularity; subtree offloading (push_subtree_cv,
// <blake3pp/parallel.hpp>) is expressed in units of this.
inline constexpr std::size_t chunk_size = 1024;

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

  // Parses 64 hex characters (either case); nullopt on any malformation.
  [[nodiscard]] static std::optional<digest> from_hex(
      std::string_view hex) noexcept;

  // The verification entry point: true iff hex parses and denotes exactly
  // this digest. The byte comparison is constant-time (unlike operator==,
  // which is for ordinary value semantics), so this is the right call when
  // the expected digest comes from an untrusted or security-relevant
  // source. Malformed hex is simply no match.
  [[nodiscard]] bool matches(std::string_view hex) const noexcept;
};

// Incremental BLAKE3 hasher. Fixed-size, trivially relocatable state; never
// allocates. The chaining-value stack is sized for the spec's maximum input
// of 2^64 bytes, hence the 54 entries.
class hasher {
 public:
  hasher() noexcept : hasher(arch::auto_detect) {}
  explicit hasher(arch a) noexcept;

  // Expert: run on a caller-supplied kernel table (must outlive the
  // hasher). This is how external kernels (e.g. hand-written assembly)
  // plug into the dispatch seam for comparison; see bench/throughput.cpp.
  explicit hasher(const kern::kernel_ops* custom_ops) noexcept;

  void update(std::span<const std::byte> input) noexcept;
  void update(std::string_view input) noexcept;

  [[nodiscard]] digest finalize() const noexcept;

  void reset() noexcept;

  [[nodiscard]] arch selected_arch() const noexcept;

  // Expert seam for external subtree computation (the parallel engine and,
  // later, the I/O pipeline): absorbs the root CV of a subtree covering
  // subtree_chunks complete chunks. Preconditions: subtree_chunks is a
  // power of two; the hasher sits exactly on a chunk boundary (bytes
  // consumed so far are a multiple of chunk_size); the current chunk
  // position is subtree_chunks-aligned; and at least one byte of the
  // message follows the subtree (it must not contain the final chunk).
  void push_subtree_cv(const std::uint32_t cv[8],
                       std::uint64_t subtree_chunks) noexcept;

 private:
  void push_cv(const std::uint32_t cv[8], std::uint64_t total_chunks,
               std::uint64_t subtree_chunks) noexcept;

  const kern::kernel_ops* ops_;
  detail::chunk_state chunk_;
  std::uint32_t cv_stack_[54][8];
  std::uint8_t cv_stack_len_;
};

[[nodiscard]] digest hash(std::span<const std::byte> input) noexcept;
[[nodiscard]] digest hash(std::string_view input) noexcept;

namespace detail {
// Reduces a power-of-2 subtree (>= 2 complete chunks) to its root CV using
// the given kernel table. Thread-safe and allocation-free; the bridge the
// parallel engine schedules over.
void compress_subtree_cv(const kern::kernel_ops* ops, const std::byte* data,
                         std::size_t num_chunks, std::uint64_t chunk_counter,
                         std::uint32_t out_cv[8]) noexcept;
}  // namespace detail

}  // namespace blake3pp

#if defined(__cpp_lib_format)
// std::format support: "{}" prints the lowercase hex digest.
template <>
struct std::formatter<blake3pp::digest> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(const blake3pp::digest& d, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(d.to_hex(), ctx);
  }
};
#endif
