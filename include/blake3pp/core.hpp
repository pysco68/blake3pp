#pragma once

/// @file
/// The sequential core: the digest value type, the incremental hasher in
/// its three modes (plain, keyed, derive_key), extended output, and the
/// one-shot functions. Standard library only; never allocates except in
/// the std::string-returning conveniences.

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
/// The compression block in bytes: the granularity of extended output
/// (each block of the XOF stream is one compression with its own counter).
inline constexpr std::size_t block_size = 64;
/// The default digest length in bytes; extended output continues past it.
inline constexpr std::size_t digest_size = 32;
/// The key length of keyed mode in bytes (hasher::keyed, keyed_hash,
/// hash_file_options::key take exactly this many).
inline constexpr std::size_t key_size = 32;

/// Writes the lowercase hex of any byte sequence into a caller's buffer.
///
/// Neither allocates nor NUL-terminates.
/// @param bytes  The bytes to encode (extended output, a key, a digest).
/// @param out    Receives 2 * `bytes.size()` characters; must be that large.
void to_hex(std::span<const std::byte> bytes, std::span<char> out) noexcept;
/// Returns the lowercase hex of any byte sequence as a string.
/// @param bytes  The bytes to encode.
/// @return 2 * `bytes.size()` lowercase hex characters.
[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes);

namespace detail {

// One chunk (up to 1024 bytes) in flight. The final block of a chunk is kept
// buffered rather than compressed eagerly. Its flags (CHUNK_END, possibly
// ROOT) are known only once more input has arrived, or has not.
struct chunk_state {
  std::array<std::uint32_t, 8> cv;
  std::uint64_t chunk_counter;
  std::array<std::uint8_t, 64> block;
  std::uint8_t block_len;
  std::uint8_t blocks_compressed;
};

}  // namespace detail

/// A 32-byte BLAKE3 digest: a regular value type with constant-time
/// equality and hex round-tripping.
///
/// The digest is the first 32 bytes of the hash's extended output stream.
/// It formats with std::format ("{}" prints the lowercase hex) where the
/// standard library provides `<format>`.
struct digest {
  /// The digest bytes.
  std::array<std::byte, digest_size> bytes;

  /// Constant-time equality (matching the Rust reference): the safe
  /// default for a value that is compared in security-sensitive contexts,
  /// at a cost that is irrelevant.
  friend bool operator==(const digest& lhs, const digest& rhs) noexcept;

  /// Returns the digest as 64 lowercase hex characters.
  [[nodiscard]] std::string to_hex() const;

  /// Returns the digest as 64 lowercase hex characters plus a terminating
  /// NUL, without allocating.
  [[nodiscard]] std::array<char, 65> to_hex_chars() const noexcept;

  /// Parses a digest from 64 hex characters of either case.
  /// @param hex  Exactly 64 hex characters.
  /// @return The digest, or std::nullopt if hex has any other shape.
  [[nodiscard]] static std::optional<digest> from_hex(
      std::string_view hex) noexcept;

  /// Verifies a hex string against this digest in one step.
  ///
  /// The comparison is constant-time, like operator==. Malformed hex
  /// counts as no match.
  /// @param hex  The hex to check, 64 characters of either case.
  /// @return true iff hex parses and denotes exactly this digest.
  [[nodiscard]] bool matches(std::string_view hex) const noexcept;
};

/// Streams BLAKE3's unbounded extended output (XOF).
///
/// Obtained from hasher::finalize_xof(). A small value type capturing the
/// root node; copyable, and independent of the hasher afterwards. The
/// stream is seekable in O(1): output block t is one compression with
/// counter t, so positioning to byte 10 GiB costs the same as byte 0. The
/// 32-byte digest is exactly the stream's first 32 bytes.
class output_reader {
 public:
  /// Writes the next `out.size()` bytes of the output stream and advances
  /// past them.
  /// @param out  Any length; the stream is unbounded.
  void fill(std::span<std::byte> out) noexcept;

  /// Returns the next N bytes of the stream by value and advances past
  /// them.
  ///
  /// The length is a template argument because it shapes the return type;
  /// runtime lengths use fill().
  /// @tparam N  The number of bytes to take.
  template <std::size_t N>
  [[nodiscard]] std::array<std::byte, N> take() noexcept {
    std::array<std::byte, N> out;
    fill(std::span<std::byte>{out});
    return out;
  }

  /// Positions the stream at an absolute byte offset, in constant time.
  /// @param byte_offset  The offset of the next byte fill() will produce.
  void seek(std::uint64_t byte_offset) noexcept {
    position_ = byte_offset;
    cache_valid_ = false;
  }

  /// The byte offset of the next byte fill() will produce.
  [[nodiscard]] std::uint64_t position() const noexcept { return position_; }

 private:
  friend class hasher;
  output_reader() = default;

  const kern::kernel_ops* ops_ = nullptr;
  std::array<std::uint32_t, 8> input_cv_ = {};
  std::array<std::uint8_t, 64> block_ = {};
  std::uint32_t block_len_ = 0;
  std::uint32_t flags_ = 0;
  std::uint64_t position_ = 0;
  std::uint64_t cached_block_ = 0;
  bool cache_valid_ = false;
  std::array<std::byte, 64> cache_ = {};
};

/// Free-function spelling of output_reader::fill(), so the sequential and
/// the scheduler-taking form (<blake3pp/parallel.hpp>) read alike:
/// fill(r, out) and fill(r, out, sched).
/// @param r    The reader to advance.
/// @param out  Receives the next `out.size()` bytes of r's stream.
inline void fill(output_reader& r, std::span<std::byte> out) noexcept {
  r.fill(out);
}

/// The incremental BLAKE3 hasher: plain, keyed (MAC/PRF) or key-derivation
/// mode, with a non-destructive finalize family.
///
/// A fixed-size, trivially relocatable value; never allocates. Every
/// finalize form leaves the hasher usable, so a digest can be taken
/// mid-stream and feeding can continue. An instance is not thread-safe;
/// distinct instances are independent. The chaining-value stack is sized
/// for the spec's maximum input of 2^64 bytes.
///
/// @code
/// blake3pp::hasher h;
/// h.update(header);
/// h.update(body);                      // std::span<const std::byte> or string_view
/// blake3pp::digest d = h.finalize();   // non-destructive
/// auto wide = h.finalize<64>();        // the first 64 bytes of the XOF stream
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

  /// A hasher in keyed mode: BLAKE3's MAC/PRF, its replacement for HMAC.
  /// @param key  Exactly key_size bytes, enforced by the span extent.
  /// @param a    The variant to run on.
  [[nodiscard]] static hasher keyed(std::span<const std::byte, key_size> key,
                                    arch a = arch::auto_detect) noexcept;
  /// Keyed mode on a caller-supplied kernel table (see the expert
  /// constructor).
  /// @param key  Exactly key_size bytes.
  /// @param ops  The kernel table; must outlive the hasher.
  [[nodiscard]] static hasher keyed(std::span<const std::byte, key_size> key,
                                    const kern::kernel_ops* ops) noexcept;

  /// A hasher in key-derivation mode: the input is the key material, and
  /// the output is a subkey bound to context.
  ///
  /// The context string should be hardcoded, globally unique and
  /// application-specific (see the BLAKE3 spec); it is not a secret.
  /// @param context  The domain-separation string.
  /// @param a        The variant to run on.
  [[nodiscard]] static hasher derive_key(std::string_view context,
                                         arch a = arch::auto_detect) noexcept;
  /// Key-derivation mode on a caller-supplied kernel table (see the expert
  /// constructor).
  /// @param context  The domain-separation string.
  /// @param ops      The kernel table; must outlive the hasher.
  [[nodiscard]] static hasher derive_key(std::string_view context,
                                         const kern::kernel_ops* ops) noexcept;

  /// Absorbs the next bytes of the message.
  /// @param input  Any length, including zero.
  void update(std::span<const std::byte> input) noexcept;
  /// Absorbs the next bytes of the message, given as text.
  /// @param input  The bytes of the string, not including any terminator.
  void update(std::string_view input) noexcept;

  /// The digest of everything absorbed so far; the hasher stays usable.
  [[nodiscard]] digest finalize() const noexcept;

  /// Extended output: fills out with the first `out.size()` bytes of the
  /// output stream, of which the digest is the first 32.
  /// @param out  Any length.
  void finalize(std::span<std::byte> out) const noexcept;
  /// Extended output as a seekable stream, independent of the hasher
  /// afterwards.
  [[nodiscard]] output_reader finalize_xof() const noexcept;

  /// Extended output by value: the first N bytes of the output stream.
  ///
  /// The length is a template argument because it shapes the return type;
  /// runtime lengths go through the span overload or finalize_xof().
  /// @tparam N  The number of bytes to return.
  template <std::size_t N>
  [[nodiscard]] std::array<std::byte, N> finalize() const noexcept {
    std::array<std::byte, N> out;
    finalize(std::span<std::byte>{out});
    return out;
  }

  /// Returns the hasher to its just-constructed state, keeping its mode,
  /// key and variant.
  void reset() noexcept;

  /// Total bytes absorbed since construction or reset, subtrees pushed
  /// through push_subtree_cv() included.
  [[nodiscard]] std::uint64_t count() const noexcept;

  /// The variant this hasher actually runs on (auto_detect resolved).
  [[nodiscard]] arch selected_arch() const noexcept;

  /// Expert seam for externally computed subtrees, used by the parallel
  /// engine and the I/O pipeline: absorbs the root chaining value of a
  /// subtree covering subtree_chunks complete chunks.
  ///
  /// @pre subtree_chunks is a power of two, at least 1.
  /// @pre The hasher sits on a chunk boundary: count() is a multiple of
  ///      chunk_size. A full chunk still open from update() is closed out
  ///      here, since the subtree proves it is not the last.
  /// @pre The current chunk position is subtree_chunks-aligned.
  /// @pre At least one byte of the message follows the subtree; it must
  ///      not contain the final chunk.
  /// @pre The message stays within BLAKE3's 2^64 bytes, at most 2^54
  ///      chunks in total, which is what the chaining-value stack holds.
  /// These preconditions are checked by assert() only. Violating them in
  /// a release build corrupts the hasher.
  /// The subtree must have been hashed under this hasher's key_words() and
  /// mode_flags() so keyed and derive_key modes propagate.
  /// @param cv              The subtree's root chaining value.
  /// @param subtree_chunks  The number of complete chunks it covers.
  void push_subtree_cv(std::span<const std::uint32_t, 8> cv,
                       std::uint64_t subtree_chunks) noexcept;

  /// Expert observer pairing with push_subtree_cv(): the key schedule
  /// external subtree computation must hash under.
  [[nodiscard]] std::span<const std::uint32_t, 8> key_words() const noexcept {
    return key_words_;
  }
  /// Expert observer pairing with push_subtree_cv(): the domain flags
  /// external subtree computation must hash under.
  [[nodiscard]] std::uint32_t mode_flags() const noexcept {
    return base_flags_;
  }

 private:
  hasher(const kern::kernel_ops* ops, std::span<const std::uint32_t, 8> key,
         std::uint32_t base_flags) noexcept;

  void push_cv(std::span<const std::uint32_t, 8> cv,
               std::uint64_t total_chunks,
               std::uint64_t subtree_chunks) noexcept;
  void close_full_chunk() noexcept;

  const kern::kernel_ops* ops_;
  std::array<std::uint32_t, 8> key_words_;
  std::uint32_t base_flags_;
  detail::chunk_state chunk_;
  std::array<std::array<std::uint32_t, 8>, 54> cv_stack_;
  std::uint8_t cv_stack_len_;
};

/// One-shot hash of a byte sequence on the best available variant.
/// @param input  Any length.
[[nodiscard]] digest hash(std::span<const std::byte> input) noexcept;
/// One-shot hash of a string's bytes.
/// @param input  The bytes of the string, not including any terminator.
[[nodiscard]] digest hash(std::string_view input) noexcept;

/// One-shot keyed hash: the MAC/PRF of input under a 32-byte key.
/// @param key    Exactly key_size bytes, enforced by the span extent.
/// @param input  Any length.
[[nodiscard]] digest keyed_hash(std::span<const std::byte, key_size> key,
                                std::span<const std::byte> input) noexcept;
/// One-shot keyed hash of a string's bytes.
/// @param key    Exactly key_size bytes.
/// @param input  The bytes of the string.
[[nodiscard]] digest keyed_hash(std::span<const std::byte, key_size> key,
                                std::string_view input) noexcept;

/// One-shot key derivation: 32 bytes derived from key_material, bound to a
/// hardcoded, application-unique context (see hasher::derive_key).
/// @param context       The domain-separation string; not a secret.
/// @param key_material  The secret to derive from.
[[nodiscard]] digest derive_key(std::string_view context,
                                std::span<const std::byte> key_material) noexcept;
/// One-shot key derivation from a string's bytes.
/// @param context       The domain-separation string; not a secret.
/// @param key_material  The secret to derive from.
[[nodiscard]] digest derive_key(std::string_view context,
                                std::string_view key_material) noexcept;

namespace detail {
// Reduces a power-of-2 subtree (>= 2 complete chunks) to its root CV using
// the given kernel table, key schedule and mode flags (take them from the
// destination hasher's key_words() and mode_flags()). Thread-safe and
// allocation-free; the bridge the parallel engine schedules over.
void compress_subtree_cv(const kern::kernel_ops* ops, const std::byte* data,
                         std::size_t num_chunks, std::uint64_t chunk_counter,
                         std::span<const std::uint32_t, 8> key,
                         std::uint32_t base_flags,
                         std::span<std::uint32_t, 8> out_cv) noexcept;

// Reduces cvs.size() chaining values of equal-sized, adjacent subtrees
// (left to right) to the CV of their common ancestor, one generation at a
// time, lanes-wide. Never the root, so no ROOT flag. cvs is scratch and is
// clobbered. Same key and flags contract as compress_subtree_cv; take them
// from the destination hasher's key_words() and mode_flags(). Thread-safe
// and allocation-free.
// @pre cvs.size() is a power of two, at least 2.
void fold_sibling_cvs(const kern::kernel_ops* ops,
                      std::span<std::array<std::uint32_t, 8>> cvs,
                      std::span<const std::uint32_t, 8> key,
                      std::uint32_t base_flags,
                      std::span<std::uint32_t, 8> out_cv) noexcept;
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
