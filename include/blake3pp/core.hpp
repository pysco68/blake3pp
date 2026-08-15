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

/// The BLAKE3 chunk granularity in bytes; subtree offloading
/// (hasher::push_subtree_cv, <blake3pp/parallel.hpp>) is expressed in
/// units of this.
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

/// A 32-byte BLAKE3 digest: a regular value type with constant-time
/// equality and hex round-tripping.
///
/// The digest is the first 32 bytes of the hash's extended output stream.
/// It formats with std::format ("{}" prints the lowercase hex) where the
/// standard library provides <format>.
struct digest {
  /// The digest bytes.
  std::array<std::byte, 32> bytes;

  /// Constant-time equality (matching the Rust reference): the safe
  /// default for a value that is compared in security-sensitive contexts,
  /// at a cost that is irrelevant.
  friend bool operator==(const digest&, const digest&) = default;

  /// Returns the digest as 64 lowercase hex characters.
  [[nodiscard]] std::string to_hex() const;

  /// Parses a digest from 64 hex characters of either case.
  /// @param hex  Exactly 64 hex characters.
  /// @return The digest, or std::nullopt if hex has any other shape.
  [[nodiscard]] static std::optional<digest> from_hex(
      std::string_view hex) noexcept;

  /// Verifies a hex string against this digest in one step.
  ///
  /// The comparison is constant-time, like operator==. Malformed hex is
  /// simply no match.
  /// @param hex  The hex to check, 64 characters of either case.
  /// @return true iff hex parses and denotes exactly this digest.
  [[nodiscard]] bool matches(std::string_view hex) const noexcept;
};

/// The incremental BLAKE3 hasher, with a non-destructive finalize.
///
/// A fixed-size, trivially relocatable value; never allocates. finalize()
/// leaves the hasher usable, so a digest can be taken mid-stream and
/// feeding can continue. An instance is not thread-safe; distinct
/// instances are independent. The chaining-value stack is sized for the
/// spec's maximum input of 2^64 bytes.
///
/// @code
/// blake3pp::hasher h;
/// h.update(header);
/// h.update(body);                      // std::span<const std::byte> or string_view
/// blake3pp::digest d = h.finalize();   // non-destructive
/// @endcode
class hasher {
 public:
  /// A plain-mode hasher on the best variant the running CPU supports.
  hasher() noexcept : hasher(arch::auto_detect) {}
  /// A plain-mode hasher pinned to a variant.
  /// @param a  The variant to run on; an unavailable one falls back to the
  ///           best available (see selected_arch()).
  explicit hasher(arch a) noexcept;

  /// Expert: a hasher running on a caller-supplied kernel table.
  ///
  /// This is how external kernels (hand-written assembly, for instance)
  /// plug into the dispatch seam for comparison; see bench/throughput.cpp.
  /// @param custom_ops  The kernel table; must outlive the hasher.
  explicit hasher(const kern::kernel_ops* custom_ops) noexcept;

  /// Absorbs the next bytes of the message.
  /// @param input  Any length, including zero.
  void update(std::span<const std::byte> input) noexcept;
  /// Absorbs the next bytes of the message, given as text.
  /// @param input  The bytes of the string, not including any terminator.
  void update(std::string_view input) noexcept;

  /// The digest of everything absorbed so far; the hasher stays usable.
  [[nodiscard]] digest finalize() const noexcept;

  /// Returns the hasher to its just-constructed state, keeping its mode,
  /// key and variant.
  void reset() noexcept;

  /// The variant this hasher actually runs on (auto_detect resolved).
  [[nodiscard]] arch selected_arch() const noexcept;

  /// Expert seam for externally computed subtrees, used by the parallel
  /// engine and the I/O pipeline: absorbs the root chaining value of a
  /// subtree covering subtree_chunks complete chunks.
  ///
  /// @pre subtree_chunks is a power of two.
  /// @pre The hasher sits on a chunk boundary: the bytes absorbed so far
  ///      are a multiple of chunk_size.
  /// @pre The current chunk position is subtree_chunks-aligned.
  /// @pre At least one byte of the message follows the subtree; it must
  ///      not contain the final chunk.
  /// @param cv              The subtree's root chaining value.
  /// @param subtree_chunks  The number of complete chunks it covers.
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

/// One-shot hash of a byte sequence on the best available variant.
/// @param input  Any length.
[[nodiscard]] digest hash(std::span<const std::byte> input) noexcept;
/// One-shot hash of a string's bytes.
/// @param input  The bytes of the string, not including any terminator.
[[nodiscard]] digest hash(std::string_view input) noexcept;

namespace detail {
// Reduces a power-of-2 subtree (>= 2 complete chunks) to its root CV using
// the given kernel table, key schedule and mode flags (take them from the
// destination hasher's key_words()/mode_flags()). Thread-safe and
// allocation-free; the bridge the parallel engine schedules over.
void compress_subtree_cv(const kern::kernel_ops* ops, const std::byte* data,
                         std::size_t num_chunks, std::uint64_t chunk_counter,
                         std::uint32_t out_cv[8]) noexcept;
}  // namespace detail

}  // namespace blake3pp

#if defined(__cpp_lib_format)
/// std::format support: "{}" prints the lowercase hex digest.
template <>
struct std::formatter<blake3pp::digest> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(const blake3pp::digest& d, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(d.to_hex(), ctx);
  }
};
#endif
