// blake3pp, amalgamated from 9c43def-dirty by tools/amalgamate.py.
//
// Two kernels: the scalar fallback, and BLAKE3PP_AMALGAM_NS, built for whatever
// this compiler was told to target. The shipped library compiles one
// translation unit per variant and dispatches between all of them at run
// time; one file cannot, so this carries one. Everything else is the real
// library, unmodified.
//
// Compiler Explorer: x86-64 gcc 16.1, 16.2 or trunk (older GCCs have
// no <simd>; the file detects that and carries the scalar kernel only)
//   -std=c++26 -O2 -msse4.2        on x86
//   -std=c++26 -O2                 on aarch64, where NEON is mandatory
// and, for the multi-core example only, the beman.execution library.
//
// Which vector kernel this carries is decided by those flags, not by the
// generator. Choose them for the machine that will RUN it: one file means
// one baseline, so everything here including dispatch is emitted for the
// target you name, and a binary built for a wider one dies with an
// illegal instruction on a narrower CPU before dispatch can choose.
//
//   -msse4.2   on every x86-64 part since roughly 2009, and part of the
//              x86-64-v2 baseline: safe essentially everywhere
//   (none)     on aarch64 NEON is in the architecture, so it is selected
//              and is always runnable
//   -mavx2     Haswell and Zen onward: common, not universal
//   -mavx512f  narrow availability, absent from most desktop parts
//
// The shipped library carries no such constraint. It compiles one
// translation unit per variant, leaves the program baseline alone, and
// chooses between them at run time.
#include <cstddef>
#include <cstdint>
#include <span>

#include <version>

#if defined(__AVX512F__)
#define BLAKE3PP_AMALGAM_NS avx512
#elif defined(__AVX2__)
#define BLAKE3PP_AMALGAM_NS avx2
#elif defined(__SSE4_2__)
#define BLAKE3PP_AMALGAM_NS sse42
#elif defined(__ARM_NEON) || defined(__aarch64__)
#define BLAKE3PP_AMALGAM_NS neon
#elif defined(__wasm_simd128__)
#define BLAKE3PP_AMALGAM_NS simd128
#endif

#if defined(__has_include) && __has_include(<simd>)
#define BLAKE3PP_HAS_STD_SIMD 1
#else
#define BLAKE3PP_AMALGAM_SCALAR_ONLY 1   // no <simd> here
#endif



/// @file
/// Instruction-set variants and runtime dispatch: which kernels this
/// binary carries, which the running CPU can use, pinning, and the build
/// introspection the tools print. Standard library only.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace blake3pp {

namespace kern {
struct kernel_ops;
}

/// The instruction-set variants a binary may carry.
///
/// Which ones a binary actually carries is a build-time property
/// (compiled_arches(), cmake/ArchKernels.cmake); which ones are usable is
/// a runtime property of the CPU (available_arches()). auto_detect
/// resolves to the best usable variant. The enumerators, their canonical
/// names and the dispatch preference are generated from one list,
/// detail/arch.def, which documents each entry; its line order is the
/// ABI order and append-only.
enum class arch : std::uint8_t {
#define BLAKE3PP_ARCH(enumerator, name, rank) enumerator,
// The one canonical list of instruction-set variants: the single source
// the enum, the name strings, and the dispatch preference order are all
// generated from (same include-me-under-a-macro pattern as the build-time
// kernel registry). Consumers define BLAKE3PP_ARCH(enumerator, "name",
// rank) and include this file; no include guard on purpose.
//
// Two orders live here, deliberately separated:
//   * LINE ORDER is ABI order. The enum's underlying values follow it, so
//     entries are APPEND-ONLY: never reorder, never insert, never remove.
//   * RANK is the dispatch preference, lower wins, unique (asserted).
//     auto_detect must hold the lowest rank; scalar the highest. Ranks are
//     spaced by 10 so a measured reordering is a one-number edit with no
//     ABI consequence.
//
// The enumerator doubles as the kernel namespace token and the CMake
// registration name (see cmake/ArchKernels.cmake); they must all agree.
// The display name is separate only where convention demands it ("auto").
// The /// comments are the enumerators' documentation (they attach to the
// enum's members through the macro expansion).

/// Resolves to the best usable variant at runtime.
BLAKE3PP_ARCH(auto_detect, "auto", 0)
/// Portable C++ without SIMD; always compiled, the final fallback.
BLAKE3PP_ARCH(scalar, "scalar", 190)
/// x86-64 SSE4.2, 128-bit lanes.
BLAKE3PP_ARCH(sse42, "sse42", 30)
/// x86-64 AVX2, 256-bit lanes.
BLAKE3PP_ARCH(avx2, "avx2", 20)
/// x86-64 AVX-512 (F, CD, VL, BW, DQ), 512-bit lanes.
BLAKE3PP_ARCH(avx512, "avx512", 10)
/// AArch64 NEON, 128-bit lanes. Architecturally mandatory on AArch64:
/// presence of the kernel implies availability.
BLAKE3PP_ARCH(neon, "neon", 90)
/// WebAssembly SIMD128. A module-level feature: an engine that lacks it
/// rejects the whole module at load, so if this code is running at all,
/// the variant is available.
BLAKE3PP_ARCH(simd128, "simd128", 180)
// Fixed-length SVE variants. Vector-length-specific code is only valid
// when the runtime vector length EQUALS the compiled one (GCC/Arm
// document exact-match only), so each VL is its own variant and at most
// one of each SVE generation is ever available on a given machine. The
// sve2_* variants additionally use SVE2's XAR fused xor-rotate: a
// measured +19% over neon on Neoverse V2, entirely from XAR, which is
// why sve2_128 outranks neon while sve128 (same width, no XAR,
// measured parity) sits below it. sve128 / sve2_256 /
// sve2_512 match no shipping silicon and are compiled only when
// BLAKE3PP_SVE_ALL_VARIANTS=ON (emulators make them runnable anyway).
/// AArch64 SVE at a vector length of exactly 128 bits (opt-in build).
BLAKE3PP_ARCH(sve128, "sve128", 100)
/// AArch64 SVE at exactly 256 bits (Neoverse V1 / Graviton3 class).
BLAKE3PP_ARCH(sve256, "sve256", 70)
/// AArch64 SVE at exactly 512 bits (Fujitsu A64FX class).
BLAKE3PP_ARCH(sve512, "sve512", 50)
/// AArch64 SVE2 at exactly 128 bits, with the XAR fused xor-rotate
/// (Neoverse N2/V2, Grace, Graviton4).
BLAKE3PP_ARCH(sve2_128, "sve2_128", 80)
/// AArch64 SVE2 at exactly 256 bits (opt-in build).
BLAKE3PP_ARCH(sve2_256, "sve2_256", 60)
/// AArch64 SVE2 at exactly 512 bits (opt-in build).
BLAKE3PP_ARCH(sve2_512, "sve2_512", 40)
// Fixed-VLEN RVV 1.0 variants: the same exact-match rule as SVE
// (-mrvv-vector-bits=zvl pins vscale, min AND max, and whole-register
// moves assume it), keyed on the runtime vlenb. VLEN>512 hardware falls
// back to scalar (documented gap; a scalable kernel would be new work).
/// RISC-V RVV 1.0 at a vector length of exactly 128 bits.
BLAKE3PP_ARCH(rvv128, "rvv128", 160)
/// RISC-V RVV 1.0 at exactly 256 bits.
BLAKE3PP_ARCH(rvv256, "rvv256", 140)
/// RISC-V RVV 1.0 at exactly 512 bits.
BLAKE3PP_ARCH(rvv512, "rvv512", 120)
/// T-Head XTheadVector, the draft-RVV-0.7.1 encoding of C906/C910
/// silicon (Allwinner D1, SG2042, TH1520). Hand-written kernel, opt-in
/// build (BLAKE3PP_XTHEAD_KERNEL=ON); detected through the hwprobe
/// vendor-extension key (Linux 6.13+) or the trap-guarded probes behind
/// run_trap_probes().
BLAKE3PP_ARCH(xthead, "xthead", 170)
// RVV 1.0 + Zvbb: the same fixed-VLEN kernels with the 3-op shift-or
// rotate replaced by Zvbb's single vror (base RVV has no rotate); the
// RVV analog of SVE2's XAR. Zvbb is detected via hwprobe (it has no
// HWCAP letter), VLEN exact-match as ever.
/// RISC-V RVV 1.0 plus Zvbb (vector rotate) at exactly 128 bits.
BLAKE3PP_ARCH(rvv128_zvbb, "rvv128_zvbb", 150)
/// RISC-V RVV 1.0 plus Zvbb at exactly 256 bits.
BLAKE3PP_ARCH(rvv256_zvbb, "rvv256_zvbb", 130)
/// RISC-V RVV 1.0 plus Zvbb at exactly 512 bits.
BLAKE3PP_ARCH(rvv512_zvbb, "rvv512_zvbb", 110)
/// POWER VSX, 128-bit lanes (the ppc64le baseline is POWER8 with VSX;
/// the kernel probes at power9). Detected through the HWCAP VSX bit,
/// checked rather than assumed.
BLAKE3PP_ARCH(vsx, "vsx", 175)
/// IBM z Vector-Enhancements-1 (z14+), 128-bit lanes: the big-endian
/// SIMD target, every word load and store byteswapped (BLAKE3 is defined
/// little-endian). Detected through HWCAP_S390_VXRS_EXT.
BLAKE3PP_ARCH(vxe, "vxe", 176)
/// MIPS MSA, 128-bit lanes (MIPS32r5/MIPS64r5 and later). Built through
/// the vector-extension provider: no std provider deduces a width on this
/// target and xsimd has no MSA backend. Detected through HWCAP_MIPS_MSA.
/// EMULATOR-ONLY: validated under qemu against the golden vectors, never
/// on MSA silicon.
BLAKE3PP_ARCH(msa, "msa", 177)

#undef BLAKE3PP_ARCH
};

/// True if the variant is compiled into this binary and the running CPU
/// supports it.
/// @param a  Any variant; auto_detect is always available.
[[nodiscard]] bool is_available(arch a) noexcept;

/// True if the running CPU and OS could execute the variant, whether or
/// not this binary carries it.
///
/// is_available(a) == (compiled_arches() contains a) && cpu_supports(a).
/// Lets tools report "the machine supports X, this build does not carry
/// it" (blake3ppsum --version does).
/// @param a  Any variant.
[[nodiscard]] bool cpu_supports(arch a) noexcept;

/// The variant auto_detect resolves to on this machine.
[[nodiscard]] arch best_available() noexcept;

/// The variants compiled into this binary, best-first, always ending with
/// scalar.
///
/// The table is generated by the build from the registered kernels, so a
/// variant appears iff its code is really in the binary.
[[nodiscard]] std::span<const arch> compiled_arches() noexcept;

/// The compiled variants this CPU can run, best-first; never empty, and
/// available_arches().front() == best_available().
[[nodiscard]] std::span<const arch> available_arches() noexcept;

/// Runs the deferred, trap-guarded detection probes once and upgrades the
/// dispatch verdict with what they learn.
///
/// On most machines the default detection (auxv, hwprobe, CPUID reads)
/// tells the whole story and this is a no-op. On RISC-V vendor-kernel
/// shapes (T-Head boards whose kernel predates the hwprobe vendor key,
/// and pre-6.4 kernels without hwprobe) part of the answer can only be
/// learned by executing an instruction that may trap, under a scoped
/// SIGILL guard. Default detection never does that, because swapping a
/// signal disposition is briefly process-global; it reports scalar on
/// those shapes instead. This call is the explicit opt-in: thread-safe
/// and idempotent, it upgrades cpu_supports(), available_arches() and
/// auto-dispatch. Call it where the application is not concurrently
/// manipulating SIGILL handling; the shipped CLI tools call it at
/// startup, since a standalone binary owns its process. The
/// BLAKE3PP_ASSUME_XTHEADVECTOR environment hook is independent of this
/// and needs no probe.
/// @return true if anything new was learned.
bool run_trap_probes() noexcept;

/// The canonical lowercase name of a variant ("auto", "sse42", "avx2").
/// @param a  Any variant.
[[nodiscard]] const char* to_string(arch a) noexcept;

/// The inverse of to_string(): parses a canonical (lowercase) name.
/// @param name  A variant name, as to_string() spells it.
/// @return The variant, or std::nullopt for any other string.
[[nodiscard]] std::optional<arch> arch_from_string(
    std::string_view name) noexcept;

/// Every enumerator (auto_detect first, then best-first) regardless of
/// what is compiled or runnable: the list to build CLIs and menus from,
/// so consumers never hand-enumerate the enum.
[[nodiscard]] std::span<const arch> all_arches() noexcept;

/// The library version, as the library was built (the git-derived
/// stamp; see blake3ppsum --version).
[[nodiscard]] std::string_view version() noexcept;
/// Which SIMD facade backs this build of the library: "std::simd",
/// "std::experimental::simd" or "xsimd".
[[nodiscard]] std::string_view simd_provider() noexcept;
/// Which sender/receiver implementation backs this build of the library:
/// "std::execution", "beman.execution" or "stdexec".
[[nodiscard]] std::string_view execution_provider() noexcept;

/// The strategy for the 16-lane message transpose used by every width-16
/// kernel (avx512, sve512/sve2_512, rvv512).
///
/// The right choice depends on the execution datapath (full-width or
/// double-pumped), which no CPUID bit reports, and on where the input
/// lives; it moves about 10% and is not predictable from the CPU alone.
/// Two AMD parts with identical feature flags rank the strategies in
/// opposite order at the same input size (Strix Point: quartered 17% over
/// staging; Strix Halo: staging 8% over quartered), and on one machine
/// the winner changes with the input (Strix Halo: quartered for an 8 MiB
/// input that fits in cache, staging for a 512 MiB one streaming from
/// DRAM). All numbers are AVX-512 measurements, the only width-16
/// hardware measured so far.
///
/// The three cost policies, none of them implicit:
///   - Do nothing: quartered, the default, measured best on most parts
///     tested and never worse than about 10% off.
///   - Tune per start: tune_transpose16(bytes) races the strategies on
///     this CPU over a working set the size of the typical input,
///     applies the winner process-wide and returns it.
///   - Tune once ever: persist to_string(tune_transpose16(bytes)) and on
///     later starts restore with
///     set_transpose16(transpose16_from_string(saved).value_or(
///         transpose16::quartered)).
/// Workloads at the edge measure themselves with blake3pp_bench (the
/// t16-* rows and --t16-sweep) and pin the winner with set_transpose16().
/// On machines without a width-16 kernel the setting is inert. Thread-safe;
/// switching mid-hash is benign, since every strategy is correct.
enum class transpose16 : std::uint8_t {
  staging = 0,    ///< Scalar gather through a staging array.
  tree = 1,       ///< Radix-2 register shuffle network.
  quartered = 2,  ///< 128-bit insert-loads plus in-lane unpacks (the default).
};
/// Pins the transpose strategy process-wide.
/// @param strategy  The strategy every width-16 kernel uses from now on.
void set_transpose16(transpose16 strategy) noexcept;
/// The strategy currently in effect.
[[nodiscard]] transpose16 active_transpose16() noexcept;

/// The working set tune_transpose16() races over when given no size:
/// large enough to stream past the last-level cache of current parts,
/// the regime of this library's headline workload (hashing files).
/// Callers who hash something smaller should say so.
inline constexpr std::size_t default_tune_bytes = 128u << 20;
/// Races the transpose strategies on this CPU and applies the winner
/// process-wide.
///
/// Never called implicitly. Costs one streaming pass per strategy (tens of
/// milliseconds for small inputs, a few hundred at the 256 MiB cap) and
/// allocates a buffer of that size. A no-op returning the active strategy
/// where no width-16 kernel is available.
/// @param typical_input_bytes  The size the application actually hashes;
///                             the answer depends on it.
/// @return The winning strategy, now active.
transpose16 tune_transpose16(std::size_t typical_input_bytes) noexcept;
/// Races the transpose strategies over default_tune_bytes and applies the
/// winner process-wide.
/// @return The winning strategy, now active.
transpose16 tune_transpose16() noexcept;
/// The canonical name of a strategy ("staging", "tree", "quartered").
/// @param strategy  Any strategy.
[[nodiscard]] std::string_view to_string(transpose16 strategy) noexcept;
/// The inverse of to_string(): parses a strategy name.
/// @param name  A name as to_string() spells it.
/// @return The strategy, or std::nullopt for any other string.
[[nodiscard]] std::optional<transpose16> transpose16_from_string(
    std::string_view name) noexcept;

namespace detail {
// Maps an arch to its kernel table; unavailable variants fall back to the
// best available one. Never returns null.
[[nodiscard]] const kern::kernel_ops* resolve(arch a) noexcept;
}

}  // namespace blake3pp



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
/// @param out    Receives 2 * bytes.size() characters; must be that large.
void to_hex(std::span<const std::byte> bytes, std::span<char> out) noexcept;
/// Returns the lowercase hex of any byte sequence as a string.
/// @param bytes  The bytes to encode.
/// @return 2 * bytes.size() lowercase hex characters.
[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes);

namespace detail {

// One chunk (up to 1024 bytes) in flight. The final block of a chunk is kept
// buffered rather than compressed eagerly: its flags (CHUNK_END, possibly
// ROOT) are only known once we see whether more input arrives.
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
  /// The comparison is constant-time, like operator==. Malformed hex is
  /// simply no match.
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
  /// Writes the next out.size() bytes of the output stream and advances
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
/// @param out  Receives the next out.size() bytes of r's stream.
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

  /// Extended output: fills out with the first out.size() bytes of the
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
  /// @pre subtree_chunks is a power of two.
  /// @pre The hasher sits on a chunk boundary: count() is a multiple of
  ///      chunk_size. A full chunk still open from update() is closed out
  ///      here, since the subtree proves it is not the last.
  /// @pre The current chunk position is subtree_chunks-aligned.
  /// @pre At least one byte of the message follows the subtree; it must
  ///      not contain the final chunk.
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



// The portable boundary over OS-native file reading. One interface, the
// fastest backend the platform and filesystem allow, decided at runtime:
//
//   Linux:  io_uring + O_DIRECT (async, page-cache-bypassing) with graceful
//           per-feature fallback (no O_DIRECT support -> buffered io_uring;
//           no io_uring -> synchronous pread)
//   Windows: IOCP + FILE_FLAG_NO_BUFFERING (async, page-cache-bypassing)
//           with the same per-feature fallback (no port -> sync ReadFile)
//   macOS:  GCD (libdispatch pool) + F_NOCACHE (async, page-cache-
//           bypassing for uncached data; already-cached pages still come
//           from RAM) with the same fallback (no async -> sync pread)
//   POSIX:  synchronous pread
//   other:  buffered stdio
//
// The model: the file is a sequence of fixed-size windows. queue_depth
// buffers are allocated once at construction (the only allocation);
// windows are delivered strictly in file order while later windows stream
// in behind them. release() recycles a buffer, which is what creates
// backpressure: at most queue_depth windows are ever in flight or held.
// Not thread-safe; drive it from one pipeline thread.

#include <cstddef>
#include <string_view>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace blake3pp::detail {

struct file_reader_options {
  // Rounded down to a power-of-2 multiple of the chunk size, min 64 KiB.
  std::size_t window_bytes = 8 * 1024 * 1024;
  unsigned queue_depth = 4;  // clamped to [2, 32]
  bool direct_io = true;     // try O_DIRECT; silently degrade if refused
  bool async = true;         // try io_uring; silently degrade if refused
  // Issue each read on the kernel's worker threads (io_uring: IOSQE_ASYNC)
  // rather than inline in the submit call. Issuing a large direct read is
  // real CPU work (pinning pages, building and queueing the bios) that
  // otherwise lands on the thread that also waits for the hash; see
  // src/io/uring_backend.hpp. Ignored by backends without the notion.
  bool offload_submit = true;
};

class file_reader {
 public:
  // Throws std::system_error if the file cannot be opened or statted.
  // std::filesystem::path is the canonical currency: it carries the
  // platform's native encoding, which is what makes the Windows backend
  // implementable without an API break.
  file_reader(const std::filesystem::path& path,
              const file_reader_options& opts);
  ~file_reader();
  file_reader(const file_reader&) = delete;
  file_reader& operator=(const file_reader&) = delete;

  struct window {
    const std::byte* data;
    std::size_t bytes;     // == window_bytes for all but possibly the last
    std::uint64_t offset;  // byte offset within the file
    bool last;             // reaches end of file
    unsigned slot;         // buffer slot; hand back via release()
  };

  [[nodiscard]] std::uint64_t file_size() const noexcept;

  // Next window in file order; blocks until its read completes. Empty at
  // EOF. Throws std::system_error on read failure. The data stays valid
  // until release() of this window (or destruction).
  std::optional<window> next();

  // Recycles the buffer slot, allowing the next pending window's read to
  // be issued into it.
  void release(const window& w) noexcept;

  // Which mechanism was actually engaged, e.g. "io_uring+direct",
  // "iocp+direct", "gcd+nocache", "pread", "readfile", "stdio".
  [[nodiscard]] std::string_view backend() const noexcept;

 private:
  struct impl;
  // unique_ptr over an incomplete type: legal because the destructor is
  // only DECLARED here and defined in the TU where impl is complete. That
  // also keeps this class non-movable by default, which is deliberate:
  // outstanding windows/buffers hold the slot indices this object owns.
  std::unique_ptr<impl> impl_;
};

}  // namespace blake3pp::detail



// The write-side mirror of file_reader: sequential file output through the
// fastest mechanism the platform allows, decided at runtime:
//
//   Linux:  io_uring + O_DIRECT (async, page-cache-bypassing) with graceful
//           per-feature fallback (no O_DIRECT -> buffered io_uring;
//           no io_uring -> synchronous pwrite)
//   Windows: IOCP + FILE_FLAG_NO_BUFFERING, preallocation via
//           SetEndOfFile + best-effort SetFileValidData (waives NTFS's
//           synchronous zero-fill to the valid-data length)
//   macOS:  GCD (libdispatch pool) + F_NOCACHE, preallocation via
//           F_PREALLOCATE + ftruncate
//   POSIX:  synchronous pwrite
//   other:  buffered stdio
//
// The model inverts the reader's: acquire() hands out one of queue_depth
// fixed-size buffers (allocated once at construction, the only
// allocation), the caller fills it, submit() queues the write at the next
// sequential offset and immediately returns so the producer can fill the
// next buffer while the device drains this one. acquire() blocking on a
// still-in-flight slot is the backpressure. O_DIRECT demands 4 KiB-aligned
// lengths, so only the final submit() may be partial or unaligned; it is
// written through a plain fd, the same trick the reader uses for its tail.
// finish() drains all in-flight writes. Not thread-safe; drive it from one
// producer thread (the buffers it hands out may of course be filled by
// many).

#include <cstddef>
#include <string_view>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace blake3pp::detail {

struct file_writer_options {
  // Rounded up to a multiple of 4 KiB, min 64 KiB.
  std::size_t buffer_bytes = 8 * 1024 * 1024;
  unsigned queue_depth = 4;  // clamped to [2, 32]
  bool direct_io = true;     // try O_DIRECT; silently degrade if refused
  bool async = true;         // try io_uring; silently degrade if refused
  // Issue each write on the kernel's worker threads (io_uring: IOSQE_ASYNC)
  // rather than inline in the submit call, the reader's offload_submit for
  // the producer side; see src/io/uring_backend.hpp.
  bool offload_submit = true;
  // Preallocate this many bytes at construction when the total is known.
  // This matters enormously for async direct I/O: writes that EXTEND the
  // file serialize on the inode lock (each waits out journal + allocation),
  // while writes into preallocated extents overlap freely. On Windows the
  // same role is played by SetEndOfFile plus SetFileValidData (privilege
  // permitting). finish() trims the file back to the bytes actually
  // written.
  std::uint64_t preallocate_bytes = 0;
};

class file_writer {
 public:
  // Creates or truncates the file. Throws std::system_error on failure.
  file_writer(const std::filesystem::path& path,
              const file_writer_options& opts);
  ~file_writer();
  file_writer(const file_writer&) = delete;
  file_writer& operator=(const file_writer&) = delete;

  struct buffer {
    std::byte* data;
    std::size_t capacity;  // == buffer_bytes (rounded)
    unsigned slot;
  };

  // Next free buffer; blocks until the slot's previous write completes.
  // Throws std::system_error if that write failed.
  buffer acquire();

  // Queues `bytes` from the buffer at the next sequential file offset and
  // returns without waiting. `bytes` must be a multiple of 4 KiB except on
  // the final submit before finish(). Throws std::system_error on
  // submission failure.
  void submit(const buffer& b, std::size_t bytes);

  // Blocks until every queued write has hit the file; surfaces any
  // deferred write error. Implicit in the destructor, but only finish()
  // can report failure, so call it.
  void finish();

  [[nodiscard]] std::uint64_t bytes_written() const noexcept;

  // Which mechanism was actually engaged, e.g. "io_uring+direct",
  // "iocp+direct+vdl", "gcd+nocache", "pwrite", "writefile", "stdio".
  [[nodiscard]] std::string_view backend() const noexcept;

 private:
  struct impl;
  // unique_ptr over an incomplete type: legal because the destructor is
  // only DECLARED here and defined in the TU where impl is complete. That
  // also keeps this class non-movable by default, which is deliberate:
  // outstanding windows/buffers hold the slot indices this object owns.
  std::unique_ptr<impl> impl_;
};

}  // namespace blake3pp::detail



/// @file
/// File hashing at storage speed: the windowed pipeline that joins the
/// file_reader (io_uring + O_DIRECT, IOCP + NO_BUFFERING, or GCD +
/// F_NOCACHE, whatever the platform allows) to the compute engine. While
/// window i is being hashed, windows i+1..i+depth-1 are already streaming
/// in; the queue-depth buffer ring is the backpressure mechanism, so the
/// pipeline never allocates past setup and never lets the device idle
/// waiting for compute (or vice versa).
///
/// The primitive is update_file(): hasher::update() with a file as the
/// source. It streams into a caller-owned hasher and returns, so the
/// hasher's mode (plain, keyed, derive_key) and every finalize form
/// (digest, extended output, the seekable reader) compose with file input
/// without this header knowing about them. hash_file() is the one-shot
/// convenience on top: construct, update_file, finalize.
///
/// This header is free of any execution-provider dependency: core.hpp,
/// dispatch.hpp and io.hpp compile against the standard library alone.
/// Reads still overlap hashing here (the reader is asynchronous); what is
/// sequential is the compute. The scheduler-taking overloads, which fan
/// each window out over cores, live in parallel_io.hpp, the one public
/// header that needs stdexec/beman/std::execution.
///
/// std::filesystem::path is the path currency throughout (string literals
/// and std::string convert implicitly). Each entry point follows the
/// standard library's dual-overload idiom: the plain form throws
/// std::system_error on I/O failure, the std::error_code& form reports
/// through ec instead.

#include <array>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>


namespace blake3pp {

/// The pipeline's knobs: what update_file() takes. The hasher it streams
/// into already carries the SIMD variant and the mode.
struct file_io_options {
  /// Bytes per window; rounded down to a power-of-2 multiple of chunk_size,
  /// minimum 64 KiB.
  std::size_t window_bytes = 8 * 1024 * 1024;
  /// Windows in flight at once; clamped to [2, 32].
  unsigned queue_depth = 4;
  /// Bypass the page cache where the platform supports it; degrades to
  /// buffered reads where it does not.
  bool direct_io = true;
  /// Issue each read on the kernel's I/O worker threads instead of inline
  /// in the submitting thread (Linux: io_uring's IOSQE_ASYNC). Issuing a
  /// large direct read costs real CPU time, and inline it is paid by the
  /// thread that also drives the hash; kernels since 6.x issue inline
  /// whenever they can, so this asks for the hand-off explicitly. Ignored
  /// where the platform has no such notion.
  bool offload_submit = true;
};

/// hash_file()'s knobs: the pipeline's, plus what shapes the hasher it
/// constructs internally.
///
/// Both structs spell the shared fields the same way so designated
/// initializers read alike, and this one converts to file_io_options so a
/// single options object can drive both entry points (a tool's
/// --window/--qd/--no-direct flags land in one place).
struct hash_file_options {
  /// The SIMD variant of the hasher.
  arch a = arch::auto_detect;
  /// Bytes per window; rounded down to a power-of-2 multiple of chunk_size,
  /// minimum 64 KiB.
  std::size_t window_bytes = 8 * 1024 * 1024;
  /// Windows in flight at once; clamped to [2, 32].
  unsigned queue_depth = 4;
  /// Bypass the page cache where the platform supports it.
  bool direct_io = true;
  /// Issue reads on the kernel's I/O worker threads; see file_io_options.
  bool offload_submit = true;
  /// Keyed (MAC/PRF) mode when set, e.g. for authenticated file manifests.
  /// derive_key and extended output have no shortcut here: build the
  /// hasher yourself and use update_file().
  std::optional<std::array<std::byte, key_size>> key = std::nullopt;

  /// The pipeline knobs alone, so one options object drives update_file()
  /// too.
  constexpr operator file_io_options() const noexcept {
    return {window_bytes, queue_depth, direct_io, offload_submit};
  }
};

namespace detail {

inline hasher make_hasher(const hash_file_options& opts,
                          const kern::kernel_ops* ops) noexcept {
  if (opts.key.has_value()) {
    return hasher::keyed(std::span<const std::byte, key_size>{opts.key.value()},
                         ops);
  }
  return hasher{ops};
}

// The body of every std::error_code overload: ec is cleared, then set
// from the std::system_error the throwing form raises. Anything else
// escaping the pipeline is an allocation failure at setup (the buffer
// ring, the queue), reported as not_enough_memory. Value-returning
// callers get a default-constructed result on failure.
template <class F>
auto with_error_code(std::error_code& ec, F&& fn) noexcept
    -> std::invoke_result_t<F> {
  using result = std::invoke_result_t<F>;
  try {
    ec.clear();
    return std::forward<F>(fn)();
  } catch (const std::system_error& e) {
    ec = e.code();
  } catch (...) {
    ec = std::make_error_code(std::errc::not_enough_memory);
  }
  if constexpr (!std::is_void_v<result>) {
    return result{};
  }
}

}  // namespace detail

/// Streams a file's bytes into a hasher, as h.update() would, and returns
/// with h open for more input or any finalize form.
///
/// Files hash in sequence: after update_file(h, a); update_file(h, b);
/// h holds the hash of a's bytes followed by b's. The hasher's mode
/// (plain, keyed, derive_key) applies unchanged.
/// @param h     The hasher to stream into.
/// @param path  The file to read.
/// @param opts  The pipeline knobs.
/// @throws std::system_error on I/O failure; h is then in an unspecified
///         but valid state (reset() or discard it).
///
/// @code
/// blake3pp::hasher h = blake3pp::hasher::derive_key("fixture v3 2026-09");
/// blake3pp::update_file(h, "seed.bin");
/// auto stream = h.finalize_xof();
/// @endcode
void update_file(hasher& h, const std::filesystem::path& path,
                 const file_io_options& opts = {});
/// Streams a file's bytes into a hasher, reporting failure through ec
/// instead of throwing.
/// @param h     The hasher to stream into.
/// @param path  The file to read.
/// @param ec    Cleared on success; the I/O error otherwise (allocation
///              failure at setup reads as not_enough_memory).
/// @param opts  The pipeline knobs.
void update_file(hasher& h, const std::filesystem::path& path,
                 std::error_code& ec,
                 const file_io_options& opts = {}) noexcept;

/// One-shot digest of a file: a hasher shaped by opts (SIMD variant,
/// optional key), the file streamed through it, finalized.
/// @param path  The file to hash.
/// @param opts  The hasher's variant and key, and the pipeline knobs.
/// @throws std::system_error on I/O failure.
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               const hash_file_options& opts = {});
/// One-shot digest of a file, reporting failure through ec instead of
/// throwing.
/// @param path  The file to hash.
/// @param ec    Cleared on success; the I/O error otherwise.
/// @param opts  The hasher's variant and key, and the pipeline knobs.
/// @return The digest, or an all-zero digest when ec is set.
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept;

namespace detail {

// Path types from other filesystem libraries (boost::filesystem::path is
// the motivating case): anything exposing a native() character sequence
// that std::filesystem::path accepts as a Source. Bridging through
// native() preserves the platform encoding exactly (no lossy transcoding,
// unlike .string() on Windows). Structural, so no third-party dependency
// or naming enters this library.
template <class P>
concept foreign_path =
    !std::same_as<std::remove_cvref_t<P>, std::filesystem::path> &&
    requires(const P& p) { std::filesystem::path(p.native()); };

}  // namespace detail

/// update_file() for a path type from another filesystem library (e.g.
/// boost::filesystem::path): anything with a native() the standard path
/// accepts, bridged without transcoding.
/// @param h     The hasher to stream into.
/// @param path  The file to read.
/// @param opts  The pipeline knobs.
template <detail::foreign_path P>
void update_file(hasher& h, const P& path, const file_io_options& opts = {}) {
  update_file(h, std::filesystem::path(path.native()), opts);
}

/// update_file() for a foreign path type, reporting through ec.
/// @param h     The hasher to stream into.
/// @param path  The file to read.
/// @param ec    Cleared on success; the error otherwise.
/// @param opts  The pipeline knobs.
template <detail::foreign_path P>
void update_file(hasher& h, const P& path, std::error_code& ec,
                 const file_io_options& opts = {}) noexcept {
  // The path conversion allocates, so it belongs inside the guard too.
  detail::with_error_code(ec, [&] {
    update_file(h, std::filesystem::path(path.native()), opts);
  });
}

/// hash_file() for a foreign path type.
/// @param path  The file to hash.
/// @param opts  The hasher's variant and key, and the pipeline knobs.
template <detail::foreign_path P>
[[nodiscard]] digest hash_file(const P& path,
                               const hash_file_options& opts = {}) {
  return hash_file(std::filesystem::path(path.native()), opts);
}

/// hash_file() for a foreign path type, reporting through ec.
/// @param path  The file to hash.
/// @param ec    Cleared on success; the error otherwise.
/// @param opts  The hasher's variant and key, and the pipeline knobs.
/// @return The digest, or an all-zero digest when ec is set.
template <detail::foreign_path P>
[[nodiscard]] digest hash_file(const P& path, std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept {
  return detail::with_error_code(ec, [&] {
    return hash_file(std::filesystem::path(path.native()), opts);
  });
}

}  // namespace blake3pp



// Inlining enforcement for the kernel's call chains.
//
// Plain `inline` is a suggestion compilers decline for the facade's call
// chains, which defeats the one-TU-full-inlining design this library is
// built on. MSVC at /O2 /Ob3 compiled the whole kernel as a call graph
// (hash_batch with 9 calls, rounds as functions); __forceinline is honored.
// Clang 22 on aarch64 outlines the ~900-instruction all_rounds<u32v> at
// -O3, and an outlined round core keeps v[] and m[] IN MEMORY, every
// micro-step a load/compute/store round-trip (measured: 15% behind
// upstream's NEON kernel from this alone). always_inline restores the
// design.
//
// It is NOT a no-op on x86-64, contrary to what this comment claimed until
// the A/B below was actually run (256 MiB, best of 3, interleaved, Zen 3+):
// GCC 16 scalar +26% / sse42 +9% / avx2 +40%, Clang 22 scalar +18% /
// sse42 +12% / avx2 +6%. Both frontends outline without it: clang the
// whole all_rounds<u32v>, GCC the index_sequence lambda inside it. The
// reference-kernel bench rows moved <3% across the same runs, which is the
// noise floor those numbers stand above.
//
// Both spellings must be applied: always_inline/__forceinline on a function
// does NOT propagate into a lambda it defines, and every compiler measured
// here was observed outlining exactly the round-fold lambda while inlining
// its enclosing function.

// Set by cmake/ArchKernels.cmake from -DBLAKE3PP_KERNEL_INLINE_ENFORCEMENT=
// auto|on|off; the fallback below repeats that default for tooling and for
// consumers building these sources outside our CMake. Turning it off
// restores the pre-enforcement spelling so one tree can be built both ways
// and the numbers above re-measured on other hardware.
#ifndef BLAKE3PP_KERNEL_INLINE_ENFORCEMENT
#define BLAKE3PP_KERNEL_INLINE_ENFORCEMENT 1
#endif

#if !BLAKE3PP_KERNEL_INLINE_ENFORCEMENT
#define BLAKE3PP_FORCE_INLINE inline
#define BLAKE3PP_LAMBDA_FORCE_INLINE
#elif defined(_MSC_VER) && !defined(__clang__)
#define BLAKE3PP_FORCE_INLINE __forceinline
// Lambdas have no keyword position for __forceinline; MSVC accepts the
// [[msvc::forceinline]] attribute after the parameter list instead.
#define BLAKE3PP_LAMBDA_FORCE_INLINE [[msvc::forceinline]]
#else
#define BLAKE3PP_FORCE_INLINE __attribute__((always_inline)) inline
#define BLAKE3PP_LAMBDA_FORCE_INLINE __attribute__((always_inline))
#endif



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
extern std::atomic<transpose16_mode> transpose16_active;  // transpose16.cpp


inline constexpr std::uint32_t block_len = 64;
inline constexpr std::size_t chunk_len = 1024;
inline constexpr std::size_t out_len = 32;

// Upper bound on any variant's simd_degree in THIS build (AVX-512: 16 u32
// lanes); sizes the caller-side staging buffers for hash_many batches, and
// with them the subtree recursion's per-level buffers (core/subtree.hpp).
// The build system defines it from BLAKE3PP_MAX_SIMD_DEGREE on
// blake3pp::features, which the kernel objects link and the library
// propagates, so every TU in a build agrees on the value; a target whose
// widest kernel is narrower lowers it and pays smaller buffers, and each
// kernel TU static_asserts its own degree against it.
#if defined(BLAKE3PP_MAX_SIMD_DEGREE)
inline constexpr std::size_t max_simd_degree = BLAKE3PP_MAX_SIMD_DEGREE;
#else
inline constexpr std::size_t max_simd_degree = 16;
#endif

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

#define BLAKE3PP_FORCE_SCALAR 1
// The one kernel translation unit, compiled once per architecture variant by
// cmake/ArchKernels.cmake. The compression core is written once over a wide
// word type W: instantiated with plain uint32_t it is the scalar compress;
// instantiated with the simd facade's u32v it hashes width-many independent
// inputs at once, one per lane (BLAKE3's hash_many strategy).



#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>




// The wide word's compile-time-amount rotate, and the per-ISA escape
// hatches that make it fast. BLAKE3's g function is rotate-bound: every
// rotate sits on the serial critical path in the shape rot<N>(x ^ y), so
// each ISA's best SPELLING of that pair is worth real percentages and is
// collected here, behind one pair of functions:
//
//   rot<N>(a)        rotate right by a compile-time amount
//   xor_rot<N>(x,y)  the fused form g actually uses
//
// Every escape below defends a measurement and answers to a tuning switch
// (cmake/ArchKernels.cmake) so it can be re-measured on hardware it has
// not been measured on. The scalar word overloads live in kernel.cpp.

#include <bit>
#include <cstddef>
#include <type_traits>

#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR)
#include <arm_neon.h>
#endif

// SVE2 fixed-length TUs get the fused xor+rotate (XAR); see xor_rot.
#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR) && \
    defined(__ARM_FEATURE_SVE2) && defined(__ARM_FEATURE_SVE_BITS)
#include <arm_sve.h>
#define BLAKE3PP_HAVE_SVE2_XAR 1
#endif

// RVV+Zvbb fixed-vlen TUs get the single-instruction rotate; see xor_rot.
#if defined(__riscv) && !defined(BLAKE3PP_FORCE_SCALAR) && \
    defined(__riscv_zvbb) && defined(__riscv_v_fixed_vlen)
#include <riscv_vector.h>
#define BLAKE3PP_HAVE_ZVBB_VROR 1
#endif




// Selects the TU's shuffle backend: the one place in the kernel where a
// preprocessor conditional decides anything about shuffles.
//
// Two backends implement the same tiny interface (reg<W>, supports<W>,
// shuf<I...>, load/store, to_word/from_word, rot_bytes<RB,W>), so the
// networks in shuffle/networks.hpp and the transposes in transpose.hpp are
// written exactly once and neither contains a backend conditional.
//
// Preference is vext-then-xsimd because vext works for EVERY provider (it
// bit_casts the provider's register), whereas the xsimd backend needs the
// xsimd provider. In practice that means: every GNU-frontend build uses
// vector extensions, and MSVC (the only frontend without
// __builtin_shufflevector) uses xsimd's portable shuffle. On GCC/Clang the
// two lower to identical instructions anyway (xsimd's shuffle IS
// __builtin_shufflevector there), so the preference costs nothing.
//
// When neither is available (BLAKE3PP_FORCE_SCALAR, or MSVC-on-arm64 below)
// BLAKE3PP_HAVE_SHUFFLE stays undefined and every transpose falls back to
// the scalar staging gather. The gate must be a preprocessor one, not just
// `if constexpr`: a discarded constexpr branch still name-looks-up its
// non-dependent identifiers, so machinery that does not exist in this TU
// must not even be NAMED.

// Testing override: -DBLAKE3PP_SHUFFLE_FORCE_XSIMD picks the xsimd backend
// on a GNU frontend too, so the path MSVC actually takes can be compiled
// and run anywhere. Worth the three lines: twice already, kernel bugs have
// hidden in a platform path nobody could execute locally.



// The u32-vector facade: one minimal wide-word type, four providers, chosen
// here and nowhere else.
//
//   BLAKE3PP_FORCE_SCALAR   width-1 plain uint32_t (the scalar kernel, and
//                           the correctness oracle for everything else)
//   BLAKE3PP_FORCE_VEXT     per-TU override to GNU vector extensions at a
//                           width the build names. For ISAs no provider
//                           covers: the std providers deduce width 1 on a
//                           target their ISA list does not know, and xsimd
//                           has no backend at all (MSA is the customer).
//   BLAKE3PP_FORCE_XSIMD    per-TU override to xsimd even when a std
//                           provider exists. The SVE kernels need it:
//                           libstdc++'s experimental::simd SVE backend
//                           keeps guarded inline-variable index tables
//                           whose dynamic initializers are SVE code that
//                           runs at LOAD TIME: an instant SIGILL on any
//                           non-SVE machine, which defeats the whole
//                           fat-binary premise. xsimd's SVE backend is
//                           initializer-free.
//   BLAKE3PP_HAS_STD_SIMD   native C++26 std::simd (GCC 16's <simd>)
//   BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD
//                           Parallelism TS v2 <experimental/simd>
//   BLAKE3PP_HAS_XSIMD      xsimd polyfill (libc++, MSVC, anything else)
//
// Each provider defines the same struct u32v in simd/<provider>.hpp
// (broadcast/load/store, operator+ / operator^ / rotr), and none of them
// contains a single #ifdef. Adding a provider means adding a file and one
// arm below; the kernel source never learns which one it got.
//
// The width is whatever the TU's -m flags make native (SSE: 4, AVX2: 8,
// AVX-512: 16, NEON: 4), so the same kernel source vectorizes differently in
// every arch variant. The type lives INSIDE the arch namespace on purpose:
// its layout depends on the TU's flags, so a shared-namespace definition
// would be an ODR lie. It must never cross the kernel boundary.




#if defined(BLAKE3PP_FORCE_SCALAR)


// Provider: none. Width-1 plain uint32_t: the scalar kernel, and the
// correctness oracle every vector variant is checked against.
// Included by simd_facade.hpp, which selects exactly one provider.

#include <bit>
#include <cstddef>
#include <cstdint>



namespace blake3pp::kern::scalar {

struct u32v {
  using impl = std::uint32_t;
  static constexpr std::size_t width = 1;
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {x};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    return {p[0]};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    p[0] = v;
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    return {std::rotr(a.v, n)};
  }
};

}  // namespace blake3pp::kern::scalar

#elif defined(BLAKE3PP_FORCE_VEXT)


// Provider: GNU/Clang vector extensions, for targets that neither xsimd
// nor a std provider covers. Included by simd_facade.hpp, which selects
// exactly one provider.
//
// The width is named by the build here rather than deduced, and that is
// the whole reason this provider exists. The std providers pick a native
// width from an ISA list, and on a target that list has never heard of,
// the width they pick is 1. Measured on mips64el with -mmsa, GCC 14:
// std::experimental::native_simd<uint32_t> reports width 1 and emits no
// vector instruction, while the same xor-then-rotate over an explicit
// 16-byte vector compiles to six MSA instructions. The compiler reaches
// the ISA perfectly; only the library's width guess does not.
//
// Everything below is ordinary C++ over a vector-extension type, so the
// instruction selection is the compiler's job on every target it knows.

#include <cstddef>
#include <cstdint>
#include <cstring>



#ifndef BLAKE3PP_VEXT_BYTES
#error "the vext provider needs -DBLAKE3PP_VEXT_BYTES=<register bytes>"
#endif

namespace blake3pp::kern::scalar {

struct u32v {
  using impl [[gnu::vector_size(BLAKE3PP_VEXT_BYTES)]] = std::uint32_t;
  static constexpr std::size_t width =
      BLAKE3PP_VEXT_BYTES / sizeof(std::uint32_t);
  impl v;

  // Vector-scalar arithmetic broadcasts the scalar, so this is a splat
  // and folds to one instruction.
  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl{} + x};
  }
  // memcpy rather than a cast: the callers' buffers carry no vector
  // alignment, and every compiler folds a sizeof-register memcpy into
  // the target's unaligned load.
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    impl x;
    std::memcpy(&x, p, sizeof(x));
    return {x};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    std::memcpy(p, &v, sizeof(v));
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    return {(a.v >> n) | (a.v << (32 - n))};
  }
};

}  // namespace blake3pp::kern::scalar

#elif defined(BLAKE3PP_FORCE_XSIMD)


// Provider: the xsimd polyfill, for libc++, MSVC, and anything else without
// a usable standard simd. Included by simd_facade.hpp, which selects
// exactly one provider.

#include <cstddef>
#include <cstdint>
#include <xsimd/xsimd.hpp>



namespace blake3pp::kern::scalar {

struct u32v {
  using impl = xsimd::batch<std::uint32_t>;
  static constexpr std::size_t width = impl::size;
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl(x)};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    return {impl::load_unaligned(p)};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    v.store_unaligned(p);
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    return {xsimd::rotr(a.v, n)};
  }
};

}  // namespace blake3pp::kern::scalar

#elif defined(BLAKE3PP_HAS_STD_SIMD)


// Provider: native C++26 std::simd (GCC 16's <simd>, spelled
// std::simd::vec<T>). Included by simd_facade.hpp, which selects exactly
// one provider.

#include <cstddef>
#include <cstdint>
#include <simd>
#include <span>



namespace blake3pp::kern::scalar {

struct u32v {
  using impl = std::simd::vec<std::uint32_t>;
  static constexpr std::size_t width = impl::size();
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl(x)};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    return {std::simd::unchecked_load<impl>(
        std::span<const std::uint32_t>(p, width))};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    std::simd::unchecked_store(v, std::span<std::uint32_t>(p, width));
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    // No simd rotate in the MVP; the shift-or idiom pattern-matches to
    // native rotates where they exist (AVX-512 vprord).
    return {(a.v >> n) | (a.v << (32 - n))};
  }
};

}  // namespace blake3pp::kern::scalar

#elif defined(BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD)


// Provider: Parallelism TS v2 <experimental/simd> (libstdc++ from GCC 11
// on, including Clang against a correctly pinned libstdc++; libc++'s is too
// incomplete and fails the configure probe). Included by simd_facade.hpp,
// which selects exactly one provider.

#include <cstddef>
#include <cstdint>
#include <experimental/simd>



namespace blake3pp::kern::scalar {

struct u32v {
  using impl = std::experimental::native_simd<std::uint32_t>;
  static constexpr std::size_t width = impl::size();
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl(x)};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    impl x;
    x.copy_from(p, std::experimental::element_aligned);
    return {x};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    v.copy_to(p, std::experimental::element_aligned);
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    return {(a.v >> n) | (a.v << (32 - n))};
  }
};

}  // namespace blake3pp::kern::scalar

#elif defined(BLAKE3PP_HAS_XSIMD)

#else
#error "No simd provider: expected BLAKE3PP_FORCE_SCALAR, BLAKE3PP_FORCE_VEXT, BLAKE3PP_HAS_STD_SIMD, BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD or BLAKE3PP_HAS_XSIMD"
#endif


// BLAKE3PP_KERNEL_SHUFFLE_TREE (cmake/ArchKernels.cmake) off leaves the
// kernel without a shuffle backend: transposes go through the scalar
// staging gather and the byte-granular rotates through shift-or, the
// spelling before the bypass, kept buildable so the pair can be measured.
#ifndef BLAKE3PP_KERNEL_SHUFFLE_TREE
#define BLAKE3PP_KERNEL_SHUFFLE_TREE 1
#endif

#if defined(BLAKE3PP_FORCE_SCALAR) || !BLAKE3PP_KERNEL_SHUFFLE_TREE
// Width 1: nothing to transpose. Or the bypass switched off.

#elif (defined(__GNUC__) || defined(__clang__)) && \
    !defined(BLAKE3PP_SHUFFLE_FORCE_XSIMD)
#define BLAKE3PP_HAVE_SHUFFLE 1


// Shuffle backend: GNU/Clang vector extensions.
//
// GCC and Clang both provide __builtin_shufflevector over vector-extension
// types with compile-time indices, and both instruction-select a shuffle
// whose indices exactly match a hardware macro into that single instruction.
// This backend works for EVERY simd provider: the provider's register is
// bit_cast in and out, which is free because all of them are register-sized
// and trivially copyable.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>




namespace blake3pp::kern::scalar::shuffle_detail {

// GCC raises -Wpsabi for vector types wider than the TU's -m flags allow
// natively. These types never appear in any cross-TU signature (that is the
// entire point of the kernel's design), so the ABI concern is moot; GCC's
// own <simd> internals suppress the warning on the same reasoning.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"

template <std::size_t W>
struct vext;
template <>
struct vext<4> {
  typedef std::uint32_t type __attribute__((vector_size(16)));
};
template <>
struct vext<8> {
  typedef std::uint32_t type __attribute__((vector_size(32)));
};
template <>
struct vext<16> {
  typedef std::uint32_t type __attribute__((vector_size(64)));
};

// Byte view of the same register, for the byte-granular rotates.
template <std::size_t W>
struct bext;
template <>
struct bext<4> {
  typedef std::uint8_t type __attribute__((vector_size(16)));
};
template <>
struct bext<8> {
  typedef std::uint8_t type __attribute__((vector_size(32)));
};

struct vext_backend {
  template <std::size_t W>
  using reg = typename vext<W>::type;

  template <std::size_t W>
  static constexpr bool supports =
      (W == 4 || W == 8 || W == 16) &&
      std::endian::native == std::endian::little &&
      sizeof(typename u32v::impl) == 4 * W &&
      std::is_trivially_copyable_v<typename u32v::impl>;

  // Byte-granular rotate: rotr by a multiple of 8 bits is a byte
  // permutation within each 32-bit element, and the lane-local constexpr
  // pattern is exactly vpshufb (NEON: tbl, and clang recognizes the 16-bit
  // case as rev32): one uop instead of the three (shift, shift, or) a
  // generic rotate costs on ISAs without a native rotate instruction.
  template <std::size_t W>
  static constexpr bool supports_byte_rot = supports<W> && (W == 4 || W == 8);

  template <int... I, class V>
  static BLAKE3PP_FORCE_INLINE V shuf(V a, V b) noexcept {
    return __builtin_shufflevector(a, b, I...);
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE reg<W> load(const std::uint8_t* p) noexcept {
    reg<W> r;
    std::memcpy(&r, p, sizeof(r));
    return r;
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE void store(std::uint8_t* p, reg<W> v) noexcept {
    std::memcpy(p, &v, sizeof(v));
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE u32v to_word(reg<W> v) noexcept {
    return u32v{std::bit_cast<typename u32v::impl>(v)};
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE reg<W> from_word(u32v w) noexcept {
    return std::bit_cast<reg<W>>(w.v);
  }

  template <int RB, std::size_t W>
  static BLAKE3PP_FORCE_INLINE u32v rot_bytes(u32v w) noexcept {
    using B = typename bext<W>::type;
    const B b = std::bit_cast<B>(from_word<W>(w));
    const B r = [&]<std::size_t... I>(std::index_sequence<I...>) {
      // Little-endian: rotr by 8*RB bits moves source byte (j+RB)%4 into
      // destination byte j of each element.
      return __builtin_shufflevector(b, b,
                                     ((I / 4) * 4 + ((I % 4) + RB) % 4)...);
    }(std::make_index_sequence<4 * W>{});
    return to_word<W>(std::bit_cast<reg<W>>(r));
  }
};

#pragma GCC diagnostic pop

}  // namespace blake3pp::kern::scalar::shuffle_detail

namespace blake3pp::kern::scalar {
using shuffle_backend = shuffle_detail::vext_backend;
}

#elif defined(BLAKE3PP_HAS_XSIMD) && !(defined(_M_ARM64) && !defined(__clang__))
// The pure-MSVC-arm64 exclusion: xsimd 14.3's neon64 swizzle (which the
// shuffle decomposition instantiates on non-builtin frontends) returns
// through a vreinterpretq_* chain that MSVC's arm64_neon.h defines as
// no-ops over one shared __n128 type, so a batch<uint8_t> lands in a
// batch<uint32_t> return seat and C2440 follows. Until that is fixed
// upstream, cl-on-arm64 keeps NEON rounds but stages its transposes.
#define BLAKE3PP_HAVE_SHUFFLE 1


// Shuffle backend: xsimd's own portable two-input constant shuffle.
//
// The fallback for frontends without vector extensions; in practice MSVC,
// which has no __builtin_shufflevector. There xsimd decomposes each shuffle
// into swizzle(x)+swizzle(y)+select (~3 uops), still far ahead of the scalar
// staging gather. On GCC/Clang xsimd lowers it through
// __builtin_shufflevector, so this backend and vext are instruction-
// identical there (verified by object-histogram diff), which is why the
// selection in shuffle.hpp can prefer either without a performance cliff.
//
// Requires the xsimd provider: it shuffles u32v::impl directly, no cast.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>




namespace blake3pp::kern::scalar::shuffle_detail {

// Byte-rotate mask: dest byte i of each 32-bit element takes source byte
// ((i%4)+RB)%4, a little-endian rotr by 8*RB bits, the same pattern the
// vext backend's rot_bytes encodes. Lane-local by construction, which is what
// makes xsimd 14.3's constant u8 swizzle emit a single vpshufb (its
// is_cross_lane check) instead of a cross-lane fixup.
template <int RB>
struct rot_bytes_gen {
  static constexpr std::uint8_t get(std::size_t i, std::size_t) noexcept {
    return static_cast<std::uint8_t>((i / 4) * 4 + ((i % 4) + RB) % 4);
  }
};

struct xsimd_backend {
  // The batch's width IS W; there is no separate register type to name.
  template <std::size_t W>
  using reg = typename u32v::impl;

  template <std::size_t W>
  static constexpr bool supports = (W == 4 || W == 8 || W == 16) &&
                                   W == u32v::width &&
                                   std::endian::native == std::endian::little;

  // CRITICAL gate for the byte-rotate: the hardware must actually HAVE a
  // byte shuffle. Below ssse3, xsimd's sse2 u8 swizzle is a per-byte
  // scalar loop that measured 4x WORSE than shift-or, so any pre-ssse3
  // instantiation must stay on shift-or. (Every shipped x86 kernel is
  // sse4.2 or wider today, MSVC included: KernelVariants.cmake passes
  // /arch:SSE4.2 plus the __SSSE3__/__SSE4_1__/__SSE4_2__ defines xsimd
  // keys on, so the gate is open there; it remains the guard for any
  // narrower build.) NEON and wasm carry their byte shuffles natively.
  static constexpr bool is_x86 =
      std::is_base_of_v<xsimd::sse2, typename u32v::impl::arch_type>;
  static constexpr bool has_byte_shuffle =
      !is_x86 ||
      std::is_base_of_v<xsimd::ssse3, typename u32v::impl::arch_type>;

  // W==16 stays on shift-or: avx512bw has no constant u8 swizzle in
  // xsimd 14.3 (and GNU compilers fuse shift-or into vprold there anyway).
  template <std::size_t W>
  static constexpr bool supports_byte_rot =
      supports<W> && (W == 4 || W == 8) && has_byte_shuffle;

  template <int... I, class V>
  static BLAKE3PP_FORCE_INLINE V shuf(V a, V b) noexcept {
    return xsimd::shuffle(
        a, b,
        xsimd::batch_constant<std::uint32_t, typename V::arch_type,
                              static_cast<std::uint32_t>(I)...>{});
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE reg<W> load(const std::uint8_t* p) noexcept {
    reg<W> r;
    std::memcpy(&r, p, sizeof(r));
    return r;
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE void store(std::uint8_t* p, reg<W> v) noexcept {
    std::memcpy(p, &v, sizeof(v));
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE u32v to_word(reg<W> v) noexcept {
    return u32v{v};
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE reg<W> from_word(u32v w) noexcept {
    return w.v;
  }

  template <int RB, std::size_t W>
  static BLAKE3PP_FORCE_INLINE u32v rot_bytes(u32v w) noexcept {
    using B = typename u32v::impl;
    const auto bytes = xsimd::bitwise_cast<std::uint8_t>(w.v);
    constexpr auto mask =
        xsimd::make_batch_constant<std::uint8_t, rot_bytes_gen<RB>,
                                   typename B::arch_type>();
    return u32v{
        xsimd::bitwise_cast<std::uint32_t>(xsimd::swizzle(bytes, mask))};
  }
};

}  // namespace blake3pp::kern::scalar::shuffle_detail

namespace blake3pp::kern::scalar {
using shuffle_backend = shuffle_detail::xsimd_backend;
}
#endif

#if defined(BLAKE3PP_HAVE_SHUFFLE)


// The radix-2 shuffle networks, written ONCE, over any backend supplying a
// two-input constant shuffle.
//
// A W x W word transpose decomposes into log2(W) radix-2 stages whose index
// patterns are precisely the in-lane 32-bit unpacks (vpunpckl/hdq), the
// in-lane 64-bit unpacks (vpunpckl/hqdq), and the 128-bit lane merges
// (vperm2i128 / vinserti128): 24 single-uop shuffles for the AVX2 8x8 case,
// verified on GCC 16 (integer domain) and Clang 22 (same network, float
// domain: vunpcklps/vunpcklpd/vperm2f128).
//
// The index lists below are the whole point of this file: they are hardware
// macro-ops spelled as permutations, they were derived by simulation, and
// they must never drift between backends. Op::shuf<I...> is the only thing
// a backend has to provide to run them.

#include <cstddef>



namespace blake3pp::kern::scalar::shuffle_detail {

// 4x4: two radix-2 stages (32-bit unpacks, then 64-bit unpacks).
template <class Op, class V>
BLAKE3PP_FORCE_INLINE void transpose_net(const V (&r)[4], V (&out)[4]) noexcept {
  const V a0 = Op::template shuf<0, 4, 1, 5>(r[0], r[1]);
  const V a1 = Op::template shuf<2, 6, 3, 7>(r[0], r[1]);
  const V a2 = Op::template shuf<0, 4, 1, 5>(r[2], r[3]);
  const V a3 = Op::template shuf<2, 6, 3, 7>(r[2], r[3]);
  out[0] = Op::template shuf<0, 1, 4, 5>(a0, a2);
  out[1] = Op::template shuf<2, 3, 6, 7>(a0, a2);
  out[2] = Op::template shuf<0, 1, 4, 5>(a1, a3);
  out[3] = Op::template shuf<2, 3, 6, 7>(a1, a3);
}

// 8x8: three radix-2 stages. The index sets are lane-local on purpose:
// they are exactly vpunpckl/hdq, vpunpckl/hqdq, and the final cross-lane
// merge vperm2i128/vinserti128.
template <class Op, class V>
BLAKE3PP_FORCE_INLINE void transpose_net(const V (&r)[8], V (&out)[8]) noexcept {
  const V a0 = Op::template shuf<0,  8,  1,  9, 4, 12,  5, 13>(r[0], r[1]);
  const V a1 = Op::template shuf<2, 10,  3, 11, 6, 14,  7, 15>(r[0], r[1]);
  const V a2 = Op::template shuf<0,  8,  1,  9, 4, 12,  5, 13>(r[2], r[3]);
  const V a3 = Op::template shuf<2, 10,  3, 11, 6, 14,  7, 15>(r[2], r[3]);
  const V a4 = Op::template shuf<0,  8,  1,  9, 4, 12,  5, 13>(r[4], r[5]);
  const V a5 = Op::template shuf<2, 10,  3, 11, 6, 14,  7, 15>(r[4], r[5]);
  const V a6 = Op::template shuf<0,  8,  1,  9, 4, 12,  5, 13>(r[6], r[7]);
  const V a7 = Op::template shuf<2, 10,  3, 11, 6, 14,  7, 15>(r[6], r[7]);

  const V b0 = Op::template shuf<0,  1,  8,  9, 4,  5, 12, 13>(a0, a2);
  const V b1 = Op::template shuf<2,  3, 10, 11, 6,  7, 14, 15>(a0, a2);
  const V b2 = Op::template shuf<0,  1,  8,  9, 4,  5, 12, 13>(a1, a3);
  const V b3 = Op::template shuf<2,  3, 10, 11, 6,  7, 14, 15>(a1, a3);
  const V b4 = Op::template shuf<0,  1,  8,  9, 4,  5, 12, 13>(a4, a6);
  const V b5 = Op::template shuf<2,  3, 10, 11, 6,  7, 14, 15>(a4, a6);
  const V b6 = Op::template shuf<0,  1,  8,  9, 4,  5, 12, 13>(a5, a7);
  const V b7 = Op::template shuf<2,  3, 10, 11, 6,  7, 14, 15>(a5, a7);
  
  out[0] = Op::template shuf<0, 1, 2, 3,  8,  9, 10, 11>(b0, b4);
  out[1] = Op::template shuf<0, 1, 2, 3,  8,  9, 10, 11>(b1, b5);
  out[2] = Op::template shuf<0, 1, 2, 3,  8,  9, 10, 11>(b2, b6);
  out[3] = Op::template shuf<0, 1, 2, 3,  8,  9, 10, 11>(b3, b7);
  out[4] = Op::template shuf<4, 5, 6, 7, 12, 13, 14, 15>(b0, b4);
  out[5] = Op::template shuf<4, 5, 6, 7, 12, 13, 14, 15>(b1, b5);
  out[6] = Op::template shuf<4, 5, 6, 7, 12, 13, 14, 15>(b2, b6);
  out[7] = Op::template shuf<4, 5, 6, 7, 12, 13, 14, 15>(b3, b7);
}

// 16x16: four radix-2 stages. Index lists derived from the same recursive
// construction (verified by simulation): in-lane dword unpacks
// (vpunpckl/hdq), in-lane qword unpacks (vpunpckl/hqdq), then two levels
// of 128-bit-block merges, AVX-512's vshufi32x4 territory; even a
// generic lowering lands on vpermt2d (any two-source dword permute, one
// uop). 64 two-register shuffles replace the 256 scalar load/stores of
// the staging gather.
template <class Op, class V>
BLAKE3PP_FORCE_INLINE void transpose_net(const V (&r)[16],
                                         V (&out)[16]) noexcept {
  V a[16];
  for (std::size_t g = 0; g < 8; ++g) {
    a[2 * g] =     Op::template shuf<0, 16, 1, 17, 4, 20, 5, 21,  8, 24,  9, 25, 12, 28, 13, 29>(r[2 * g], r[2 * g + 1]);
    a[2 * g + 1] = Op::template shuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15, 31>(r[2 * g], r[2 * g + 1]);
  }
  V b[16];
  for (std::size_t q = 0; q < 4; ++q) {
    const std::size_t k = 4 * q;
    b[k + 0] = Op::template shuf<0, 1, 16, 17, 4, 5, 20, 21,  8,  9, 24, 25, 12, 13, 28, 29>(a[k + 0], a[k + 2]);
    b[k + 1] = Op::template shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(a[k + 0], a[k + 2]);
    b[k + 2] = Op::template shuf<0, 1, 16, 17, 4, 5, 20, 21,  8,  9, 24, 25, 12, 13, 28, 29>(a[k + 1], a[k + 3]);
    b[k + 3] = Op::template shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(a[k + 1], a[k + 3]);
  }
  V c[16];
  for (std::size_t h = 0; h < 2; ++h) {
    const std::size_t k = 8 * h;
    for (std::size_t j = 0; j < 4; ++j) {
      c[k + j] =     Op::template shuf<0, 1, 2, 3, 16, 17, 18, 19,  8,  9, 10, 11, 24, 25, 26, 27>(b[k + j], b[k + j + 4]);
      c[k + j + 4] = Op::template shuf<4, 5, 6, 7, 20, 21, 22, 23, 12, 13, 14, 15, 28, 29, 30, 31>(b[k + j], b[k + j + 4]);
    }
  }
  for (std::size_t j = 0; j < 8; ++j) {
    out[j] =     Op::template shuf<0, 1,  2,  3,  4,  5,  6,  7, 16, 17, 18, 19, 20, 21, 22, 23>(c[j], c[j + 8]);
    out[j + 8] = Op::template shuf<8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31>(c[j], c[j + 8]);
  }
}

// The two in-lane stages alone, factored for the quartered W==16 form: with
// block-level transposition already done by 128-bit addressing, a 4x4
// transpose per 128-bit lane finishes the job (same s1/s2 index lists as
// the full tree; all vpunpck, no cross-lane traffic).
template <class Op, class V>
BLAKE3PP_FORCE_INLINE void inlane_4x4(const V (&r)[4], V (&t)[4]) noexcept {
  const V a0 = Op::template shuf<0, 16, 1, 17, 4, 20, 5, 21,  8, 24,  9, 25, 12, 28, 13, 29>(r[0], r[1]);
  const V a1 = Op::template shuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15, 31>(r[0], r[1]);
  const V a2 = Op::template shuf<0, 16, 1, 17, 4, 20, 5, 21,  8, 24,  9, 25, 12, 28, 13, 29>(r[2], r[3]);
  const V a3 = Op::template shuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15, 31>(r[2], r[3]);
  t[0] = Op::template shuf<0, 1, 16, 17, 4, 5, 20, 21,  8,  9, 24, 25, 12, 13, 28, 29>(a0, a2);
  t[1] = Op::template shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(a0, a2);
  t[2] = Op::template shuf<0, 1, 16, 17, 4, 5, 20, 21,  8,  9, 24, 25, 12, 13, 28, 29>(a1, a3);
  t[3] = Op::template shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(a1, a3);
}

}  // namespace blake3pp::kern::scalar::shuffle_detail

#endif




// The tuning switches, set by cmake/ArchKernels.cmake from
// -DBLAKE3PP_KERNEL_*_ROTATE=auto|on|off; each fallback repeats that
// default (the headers are also parsed by tooling that does not pass our
// flags). Off restores the generic spelling for re-measurement.
#ifndef BLAKE3PP_KERNEL_SRI_ROTATE
#define BLAKE3PP_KERNEL_SRI_ROTATE 1
#endif
#ifndef BLAKE3PP_KERNEL_XAR_ROTATE
#define BLAKE3PP_KERNEL_XAR_ROTATE 1
#endif
#ifndef BLAKE3PP_KERNEL_VROR_ROTATE
#define BLAKE3PP_KERNEL_VROR_ROTATE 1
#endif

namespace blake3pp::kern::scalar {

#if defined(BLAKE3PP_HAVE_SVE2_XAR)
typedef svuint32_t sve_fixed_u32
    __attribute__((arm_sve_vector_bits(__ARM_FEATURE_SVE_BITS)));
#endif

#if defined(BLAKE3PP_HAVE_ZVBB_VROR)
typedef vuint32m1_t rvv_fixed_u32
    __attribute__((riscv_rvv_vector_bits(__riscv_v_fixed_vlen)));
#endif

// Which SOURCE spelling reaches the one-instruction rotate is a property of
// the compiler, not of the hardware. Measured for rot(d ^ a, N), the shape
// g actually uses, at both 128- and 256-bit width:
//
//                       clang 22        GCC 14/16
//   generic shift-or    1x pshufb       pslld+psrld+por
//   byte shuffle        N=16: pshuflw+pshufhw    1x pshufb
//                       N=8:  1x pshufb          1x pshufb
//
// Mirror images: LLVM canonicalizes the shift-or rotate idiom straight to
// pshufb, but lowers OUR byte-shuffle to the 16-bit-lane pair for N==16
// (both are legal; its cost model dislikes materializing the mask, even
// though upstream's asm hoists exactly that mask out of the loop). GCC
// never forms a shuffle from shift-or and needs the byte spelling. Neither
// form is portable-optimal, so the choice is made here per compiler.
// Forcing it with _mm256_shuffle_epi8 does NOT work: InstCombine folds a
// constant-mask pshufb intrinsic back into a generic shuffle and lowers it
// the same way. The split is stable across the clang range this project
// builds with (18.1, 20.1 and 22 all lower both spellings identically),
// so the predicate is a compiler-family choice, not a version workaround.
//
// Worth +4.0% avx2 and +4.2% sse42 on clang 22 / Zen 3+ (256 MiB, best of
// 3, interleaved; the reference-kernel rows moved 0.8% over the same runs).
// Not because it shrinks the loop; it does not: 1489 -> 1486 instructions,
// because the freed pshuflw/pshufhw pair comes back as spills once the mask
// occupies a register all loop long. What shortens is g's SERIAL chain, two
// dependent shuffles down to one. Same lesson as the aarch64 sri escape:
// on this kernel the metric is critical-path length, not instruction count.
// BLAKE3PP_KERNEL_ROT16_PER_COMPILER (cmake/ArchKernels.cmake) off gives
// clang the byte shuffle for 16 like every other compiler: the spelling
// before the split, kept buildable so the pair can be re-measured.
#ifndef BLAKE3PP_KERNEL_ROT16_PER_COMPILER
#define BLAKE3PP_KERNEL_ROT16_PER_COMPILER 1
#endif
template <int N>
constexpr bool prefer_byte_rot() noexcept {
#if defined(__clang__) && BLAKE3PP_KERNEL_ROT16_PER_COMPILER && \
    (defined(__x86_64__) || defined(__i386__) || defined(_M_X64))
  return N == 8;  // N == 16 is one instruction cheaper as generic shift-or
#else
  return N == 16 || N == 8;
#endif
}

// Compile-time-amount rotate for the wide word: a single byte shuffle for
// the 16- and 8-bit amounts where that is the better spelling (above), then
// the aarch64 shl+sri pair, then the generic shift-or.
// W is a defaulted template parameter (not read directly off u32v) so the
// discarded constexpr branches stay dependent; non-dependent constructs in
// a discarded branch are still instantiated.
template <int N, std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE u32v rot(u32v a) noexcept {
#if defined(BLAKE3PP_HAVE_SHUFFLE)
  if constexpr (prefer_byte_rot<N>() && shuffle_backend::supports_byte_rot<W>) {
    return shuffle_backend::rot_bytes<N / 8, W>(a);
  }
#endif
#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR) && \
    BLAKE3PP_KERNEL_SRI_ROTATE
  // The rotate amounts with no byte-granular shuffle (12 and 7): shl+sri
  // instead of the shl+usra clang selects for the generic shift-or. SRI and
  // USRA cost the same two instructions, but SRI is a cycle faster on Apple
  // cores, and these rotates sit on g's serial critical path. This was the
  // entire residual against upstream's blake3_neon.c, whose explicit
  // intrinsics reach sri directly (their PR #319 measured the same):
  // 1.61 -> 1.71 GiB/s on Apple M2 / clang 22, exactly upstream's number;
  // the two hash loops are otherwise instruction-for-instruction identical.
  // (sri also wins on Neoverse V2, +12%, and N1, +8%, in alternating A/B
  // pairs of the static clang build; the SRI_ROTATE switch exists to
  // re-measure. GCC 15 selects usra without it exactly as clang does.)
  if constexpr (W == 4 && sizeof(typename u32v::impl) == 16 &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    // Immediately-invoked generic lambda: `if constexpr` only shields
    // DEPENDENT constructs from the discarded branch, and everything here
    // is concrete: a wider-than-NEON aarch64 TU (fixed-length SVE) would
    // hard-error on the 16-byte bit_cast at template definition time even
    // though the branch is never taken. Routing a.v through a deduced
    // parameter restores the dependency. (At SVE VL=128 the branch IS
    // taken, and validly: Z0-Z31 alias V0-V31, so the NEON sri applies.)
    return [](auto impl) BLAKE3PP_LAMBDA_FORCE_INLINE {
      const uint32x4_t x = std::bit_cast<uint32x4_t>(impl);
      return u32v{std::bit_cast<decltype(impl)>(
          vsriq_n_u32(vshlq_n_u32(x, 32 - N), x, N))};
    }(a.v);
  }
#endif
  return rotr(a, N);
}

// The fused xor-then-rotate the kernel's g function is made of:
// every rotate in BLAKE3 has the shape rot<N>(x ^ y). SVE2's XAR does the
// pair in ONE instruction (rotate right of the exclusive-or), replacing
// either eor+tbl (N=16/8, byte-granular) or eor+shl+sri (N=12/7). It is
// the whole reason an SVE2 variant beats the NEON kernel at the same
// 128-bit width (+18% on Neoverse V2, entirely this). Everywhere else
// this is exactly rot<N>(x ^ y).
template <int N>
BLAKE3PP_FORCE_INLINE u32v xor_rot(u32v x, u32v y) noexcept {
#if defined(BLAKE3PP_HAVE_SVE2_XAR) && BLAKE3PP_KERNEL_XAR_ROTATE
  if constexpr (sizeof(typename u32v::impl) * 8 == __ARM_FEATURE_SVE_BITS &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    // Same dependent-lambda shield as rot's sri escape above: keeps the
    // bit_casts out of TUs whose impl is not the fixed-length SVE size.
    return [](auto ix, auto iy) BLAKE3PP_LAMBDA_FORCE_INLINE {
      // The provider's register itself where it exposes one, rather than
      // a reinterpret of the object holding it. Both spellings name the
      // same fixed-length SVE type, but clang under the MSVC ABI
      // declines to inline a reinterpret of the batch CLASS even with
      // always_inline: it emitted an out-of-line `ldr q0, [x0]; ret` and
      // spilled a live vector at every use, 448 calls and 224 spills in
      // hash_many, which is two per rotate. The same clang targeting
      // Linux, and GCC, fold it away. The SVE kernels are pinned to
      // xsimd (FORCE_XSIMD in cmake/KernelVariants.cmake), so the member
      // is always there; the reinterpret stays as the general spelling.
      if constexpr (requires { ix.data; }) {
        const sve_fixed_u32 r = svxar_n_u32(ix.data, iy.data, N);
        return u32v{decltype(ix){r}};
      } else {
        const sve_fixed_u32 r = svxar_n_u32(std::bit_cast<sve_fixed_u32>(ix),
                                            std::bit_cast<sve_fixed_u32>(iy),
                                            N);
        return u32v{std::bit_cast<decltype(ix)>(r)};
      }
    }(x.v, y.v);
  }
#endif
#if defined(BLAKE3PP_HAVE_ZVBB_VROR) && BLAKE3PP_KERNEL_VROR_ROTATE
  if constexpr (sizeof(typename u32v::impl) * 8 == __riscv_v_fixed_vlen &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    // Zvbb's vror is XAR minus the folded eor: base RVV has no rotate at
    // all, so the generic fallback is FOUR ops (vxor+vsll+vsrl+vor); this
    // is vxor+vror. Same dependent-lambda shield as the branches above.
    const u32v e = x ^ y;
    return [](auto impl) BLAKE3PP_LAMBDA_FORCE_INLINE {
      const rvv_fixed_u32 v = std::bit_cast<rvv_fixed_u32>(impl);
      const rvv_fixed_u32 r =
          __riscv_vror_vx_u32m1(v, N, __riscv_v_fixed_vlen / 32);
      return u32v{std::bit_cast<decltype(impl)>(r)};
    }(e.v);
  }
#endif
  return rot<N>(x ^ y);
}

}  // namespace blake3pp::kern::scalar




// The message transpose. (The wide word's rotate and its per-ISA escape
// hatches live in rotate.hpp.)
//
// hash_batch needs the 16 message words of a block gathered ACROSS lanes
// (word j of every input in one vector). No simd provider exposes a portable
// permute for that (the std::simd MVP has no shuffle API at all), so the
// naive route stages through a scalar array, and it costs ~40% of the whole
// hash (upstream's SSE4.1 assembly matches our AVX2 because of it).
//
// The bypass is a radix-2 shuffle tree whose index patterns ARE hardware
// macro-ops; it lives in shuffle/networks.hpp, written once over whichever
// backend shuffle.hpp selected for this TU. What remains here is the part
// that is genuinely about transposing a message: which registers to load
// from where, and where their transposed results go. Widths other than
// 4/8/16, and TUs with no shuffle backend at all, fall back to the scalar
// staging gather at the bottom of each function.

#include <cstddef>
#include <cstdint>
#include <cstring>






namespace blake3pp::kern::scalar {
namespace transpose_detail {

// The W==16 strategy is a RUNTIME dial (kern::transpose16_active, set via
// blake3pp::set_transpose16 / tune_transpose16): on double-pumped AVX-512
// (Strix Point) the register tree measured 20% slower than the scalar
// staging gather while the quartered form measured 17% faster, and no
// CPUID bit distinguishes those microarchitectures, so the winner is
// raced, not detected. All three paths compile into the W==16 kernel; the
// relaxed load deciding between them amortizes over a >=16 KiB batch.
BLAKE3PP_FORCE_INLINE transpose16_mode t16_mode() noexcept {
  return transpose16_active.load(std::memory_order_relaxed);
}

BLAKE3PP_FORCE_INLINE std::uint32_t ld32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

#if defined(BLAKE3PP_HAVE_SHUFFLE)

// Gathers the block's 16 message words across W lanes through the shuffle
// networks. Returns false only for the W==16 `staging` dial setting, whose
// whole point is to use the scalar gather instead.
template <class B, std::size_t W>
BLAKE3PP_FORCE_INLINE bool shuffle_load(const std::uint8_t* const* inputs,
                                        std::size_t offset, u32v m[16],
                                        [[maybe_unused]]
                                        transpose16_mode mode) noexcept {
  using V = typename B::template reg<W>;
  if constexpr (W == 16) {
    if (mode == transpose16_mode::quartered) {
      // Quartered: 128-bit pieces land block-transposed by ADDRESSING;
      // registers only run the two in-lane stages.
      for (std::size_t q = 0; q < 4; ++q) {
        V r[4];
        for (std::size_t k = 0; k < 4; ++k) {
          std::uint8_t quad[64];
          for (std::size_t l = 0; l < 4; ++l) {
            std::memcpy(quad + 16 * l, inputs[4 * l + k] + offset + 16 * q,
                        16);
          }
          r[k] = B::template load<W>(quad);
        }
        V t[4];
        shuffle_detail::inlane_4x4<B>(r, t);
        for (std::size_t j = 0; j < 4; ++j) {
          m[4 * q + j] = B::template to_word<W>(t[j]);
        }
      }
      return true;
    }
    if (mode == transpose16_mode::tree) {
      V r[16];
      for (std::size_t lane = 0; lane < 16; ++lane) {
        r[lane] = B::template load<W>(inputs[lane] + offset);
      }
      V t[16];
      shuffle_detail::transpose_net<B>(r, t);
      for (std::size_t j = 0; j < 16; ++j) {
        m[j] = B::template to_word<W>(t[j]);
      }
      return true;
    }
    return false;  // staging
  } else {
    constexpr std::size_t groups = 16 / W;
    for (std::size_t g = 0; g < groups; ++g) {
      V r[W];
      for (std::size_t lane = 0; lane < W; ++lane) {
        r[lane] = B::template load<W>(inputs[lane] + offset + g * W * 4);
      }
      V t[W];
      shuffle_detail::transpose_net<B>(r, t);
      for (std::size_t j = 0; j < W; ++j) {
        m[g * W + j] = B::template to_word<W>(t[j]);
      }
    }
    return true;
  }
}

// The mirror image: 16 wide words out to W lane-major 64-byte blocks.
template <class B, std::size_t W>
BLAKE3PP_FORCE_INLINE bool shuffle_store(const u32v (&w)[16],
                                         std::uint8_t* out,
                                         [[maybe_unused]]
                                         transpose16_mode mode) noexcept {
  using V = typename B::template reg<W>;
  if constexpr (W == 16) {
    if (mode == transpose16_mode::quartered) {
      // Quartered mirror: two in-lane stages, then 128-bit pieces go to
      // their destinations by addressing (extract-stores).
      for (std::size_t q = 0; q < 4; ++q) {
        V r[4];
        for (std::size_t j = 0; j < 4; ++j) {
          r[j] = B::template from_word<W>(w[4 * q + j]);
        }
        V t[4];
        shuffle_detail::inlane_4x4<B>(r, t);
        for (std::size_t k = 0; k < 4; ++k) {
          std::uint8_t quad[64];
          B::template store<W>(quad, t[k]);
          for (std::size_t l = 0; l < 4; ++l) {
            std::memcpy(out + (4 * l + k) * 64 + 16 * q, quad + 16 * l, 16);
          }
        }
      }
      return true;
    }
    if (mode == transpose16_mode::tree) {
      V r[16];
      for (std::size_t j = 0; j < 16; ++j) {
        r[j] = B::template from_word<W>(w[j]);
      }
      V t[16];
      shuffle_detail::transpose_net<B>(r, t);
      for (std::size_t lane = 0; lane < 16; ++lane) {
        B::template store<W>(out + lane * 64, t[lane]);
      }
      return true;
    }
    return false;  // staging
  } else {
    constexpr std::size_t groups = 16 / W;
    for (std::size_t g = 0; g < groups; ++g) {
      V r[W];
      for (std::size_t j = 0; j < W; ++j) {
        r[j] = B::template from_word<W>(w[g * W + j]);
      }
      V t[W];
      shuffle_detail::transpose_net<B>(r, t);
      for (std::size_t lane = 0; lane < W; ++lane) {
        B::template store<W>(out + lane * 64 + g * W * 4, t[lane]);
      }
    }
    return true;
  }
}

#endif  // BLAKE3PP_HAVE_SHUFFLE

}  // namespace transpose_detail

// Fills m[0..15] with the block's message words transposed across W lanes:
// m[j][lane] = word j of inputs[lane] at byte offset `offset`.
template <std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE void load_transposed(const std::uint8_t* const* inputs,
                                           std::size_t offset, u32v m[16],
                                           [[maybe_unused]]
                                           transpose16_mode mode) noexcept {
  namespace td = transpose_detail;
#if defined(BLAKE3PP_HAVE_SHUFFLE)
  if constexpr (shuffle_backend::supports<W>) {
    if (td::shuffle_load<shuffle_backend, W>(inputs, offset, m, mode)) {
      return;
    }
  }
#endif
  std::uint32_t lanes[W];
  for (std::size_t j = 0; j < 16; ++j) {
    for (std::size_t lane = 0; lane < W; ++lane) {
      lanes[lane] = td::ld32(inputs[lane] + offset + 4 * j);
    }
    m[j] = u32v::load(lanes);
  }
}

// Writes 16 wide words lane-major: lane l receives words w[0..15][l] as
// 64 little-endian bytes at out + l*64, the mirror of load_transposed.
template <std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE void store_transposed(const u32v (&w)[16],
                                            std::uint8_t* out,
                                            [[maybe_unused]]
                                            transpose16_mode mode) noexcept {
  namespace td = transpose_detail;
#if defined(BLAKE3PP_HAVE_SHUFFLE)
  if constexpr (shuffle_backend::supports<W>) {
    if (td::shuffle_store<shuffle_backend, W>(w, out, mode)) {
      return;
    }
  }
#endif
  std::uint32_t lanes[W];
  for (std::size_t j = 0; j < 16; ++j) {
    w[j].store(lanes);
    for (std::size_t lane = 0; lane < W; ++lane) {
      const std::uint32_t v = lanes[lane];
      std::uint8_t* p = out + lane * 64 + 4 * j;
      p[0] = static_cast<std::uint8_t>(v);
      p[1] = static_cast<std::uint8_t>(v >> 8);
      p[2] = static_cast<std::uint8_t>(v >> 16);
      p[3] = static_cast<std::uint8_t>(v >> 24);
    }
  }
}

}  // namespace blake3pp::kern::scalar


namespace blake3pp::kern::scalar {
namespace {

static_assert(u32v::width <= max_simd_degree,
              "BLAKE3PP_MAX_SIMD_DEGREE is narrower than this kernel's "
              "simd_degree: it sizes the staging buffers this kernel fills");

// Little-endian load/store, spelled memcpy + byteswap rather than the
// byte-wise shift-or idiom: GCC and Clang fold both spellings to a single
// mov on LE targets, but MSVC 19.51 does not recognize the shift-or idiom
// at all: it emitted the four movzx/shl/or per word verbatim, 10
// instructions per message word in the scalar kernel's block loop. The
// memcpy folds to one mov on every compiler; the byteswap arm keeps the
// endian independence the old spelling had.
//
// std::byteswap is C++23; the C++20 presets get the shift-mask spelling,
// which every compiler folds to a single bswap. Feature-tested rather
// than __cplusplus-gated, and needed even though the call sits in a
// discarded if-constexpr branch: non-dependent names in discarded
// branches must still exist (found by the C++20 CI leg, not by review).
BLAKE3PP_FORCE_INLINE constexpr std::uint32_t bswap32(std::uint32_t v) noexcept {
#if defined(__cpp_lib_byteswap)
  return std::byteswap(v);
#else
  return (v >> 24) | ((v >> 8) & 0x0000ff00u) | ((v << 8) & 0x00ff0000u) |
         (v << 24);
#endif
}

// The clang that zig 0.16 bundles, 21.1.0, merges the byte-wise word
// loads of the message below into element-width vector loads reading the
// caller's pointer directly. Where a misaligned vector access faults that
// turns a legal load into a bus error: RISC-V lets an implementation
// refuse such an access and the SpacemiT X60 (RVV 1.0) does, while x86
// and AArch64 perform them in hardware.
//
// Measured on one preprocessed source, counting loads whose base is the
// block parameter in compress_in_place: zig 0.16 (clang 21.1.0) three,
// Ubuntu clang 21.1.8 none, zig 0.17-dev (clang 22.1.8) none, Ubuntu
// clang 22.1.8 none, GCC 15 none. So a 21.1.x patch release or zig's own
// build of it is the dividing line, and the version bound below is
// deliberately conservative: it also covers clang 21 builds that do not
// need it, and lifts itself when the musl builds move to zig 0.17.
#if defined(__riscv) && defined(__riscv_v) && defined(__clang__) && __clang_major__ < 22
#define BLAKE3PP_ALIGN_MESSAGE_BLOCK 1
#else
#define BLAKE3PP_ALIGN_MESSAGE_BLOCK 0
#endif

BLAKE3PP_FORCE_INLINE std::uint32_t load32(const std::uint8_t* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof v);
  if constexpr (std::endian::native == std::endian::big) {
    v = bswap32(v);
  }
  return v;
}

BLAKE3PP_FORCE_INLINE void store32(std::uint8_t* p, std::uint32_t v) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    v = bswap32(v);
  }
  std::memcpy(p, &v, sizeof v);
}

template <int N>
BLAKE3PP_FORCE_INLINE std::uint32_t rot(std::uint32_t x) noexcept {
  return std::rotr(x, N);
}

// Scalar twin of the wide xor_rot in transpose.hpp: the rounds are written
// against the fused form so SVE2's XAR can claim it; everywhere else the
// compilers fold this back to exactly the old eor + ror pair.
template <int N>
BLAKE3PP_FORCE_INLINE std::uint32_t xor_rot(std::uint32_t x,
                                            std::uint32_t y) noexcept {
  return std::rotr(x ^ y, N);
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
  // See BLAKE3PP_ALIGN_MESSAGE_BLOCK: where the merged load would fault,
  // an unaligned block is copied once so that the address really is
  // aligned. Staging it unconditionally does not work, since the
  // optimizer forwards the copy and rebuilds the loads from the original
  // pointer; the aligned case, which is the common one, pays one
  // predictable branch and no copy.
  const std::uint8_t* src = block;
#if BLAKE3PP_ALIGN_MESSAGE_BLOCK
  alignas(std::uint32_t) std::uint8_t staged[block_len];
  if ((reinterpret_cast<std::uintptr_t>(src) &
       (alignof(std::uint32_t) - 1)) != 0) {
    std::memcpy(staged, src, block_len);
    src = staged;
  }
  src = static_cast<const std::uint8_t*>(
      __builtin_assume_aligned(src, alignof(std::uint32_t)));
#endif

  std::uint32_t m[16];
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
  store_transposed(wide, out, transpose_detail::t16_mode());
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

  // Read the W==16 transpose dial ONCE per batch, not once per block. MSVC
  // emits std::atomic<transpose16_mode>::load out of line, and a call in
  // the block loop is an optimisation barrier that spills the whole wide
  // state every iteration, measured as the entire AVX-512 width advantage.
  const transpose16_mode t16 = transpose_detail::t16_mode();

  for (std::size_t b = 0; b < blocks; ++b) {
    std::uint32_t block_flags = flags;
    if (b == 0) {
      block_flags |= flags_start;
    }
    if (b == blocks - 1) {
      block_flags |= flags_end;
    }

    u32v m[16];
    load_transposed(inputs, b * block_len, m, t16);

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
    arch::scalar,
    /*simd_degree=*/u32v::width,
    &compress_in_place,
    &compress_xof,
    &xof_many,
    &hash_many,
};

}  // namespace blake3pp::kern::scalar

#if defined(BLAKE3PP_AMALGAM_NS) && !defined(BLAKE3PP_AMALGAM_SCALAR_ONLY)
#undef BLAKE3PP_FORCE_SCALAR
// The one kernel translation unit, compiled once per architecture variant by
// cmake/ArchKernels.cmake. The compression core is written once over a wide
// word type W: instantiated with plain uint32_t it is the scalar compress;
// instantiated with the simd facade's u32v it hashes width-many independent
// inputs at once, one per lane (BLAKE3's hash_many strategy).



#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>




// The wide word's compile-time-amount rotate, and the per-ISA escape
// hatches that make it fast. BLAKE3's g function is rotate-bound: every
// rotate sits on the serial critical path in the shape rot<N>(x ^ y), so
// each ISA's best SPELLING of that pair is worth real percentages and is
// collected here, behind one pair of functions:
//
//   rot<N>(a)        rotate right by a compile-time amount
//   xor_rot<N>(x,y)  the fused form g actually uses
//
// Every escape below defends a measurement and answers to a tuning switch
// (cmake/ArchKernels.cmake) so it can be re-measured on hardware it has
// not been measured on. The scalar word overloads live in kernel.cpp.

#include <bit>
#include <cstddef>
#include <type_traits>

#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR)
#include <arm_neon.h>
#endif

// SVE2 fixed-length TUs get the fused xor+rotate (XAR); see xor_rot.
#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR) && \
    defined(__ARM_FEATURE_SVE2) && defined(__ARM_FEATURE_SVE_BITS)
#include <arm_sve.h>
#define BLAKE3PP_HAVE_SVE2_XAR 1
#endif

// RVV+Zvbb fixed-vlen TUs get the single-instruction rotate; see xor_rot.
#if defined(__riscv) && !defined(BLAKE3PP_FORCE_SCALAR) && \
    defined(__riscv_zvbb) && defined(__riscv_v_fixed_vlen)
#include <riscv_vector.h>
#define BLAKE3PP_HAVE_ZVBB_VROR 1
#endif




// Selects the TU's shuffle backend: the one place in the kernel where a
// preprocessor conditional decides anything about shuffles.
//
// Two backends implement the same tiny interface (reg<W>, supports<W>,
// shuf<I...>, load/store, to_word/from_word, rot_bytes<RB,W>), so the
// networks in shuffle/networks.hpp and the transposes in transpose.hpp are
// written exactly once and neither contains a backend conditional.
//
// Preference is vext-then-xsimd because vext works for EVERY provider (it
// bit_casts the provider's register), whereas the xsimd backend needs the
// xsimd provider. In practice that means: every GNU-frontend build uses
// vector extensions, and MSVC (the only frontend without
// __builtin_shufflevector) uses xsimd's portable shuffle. On GCC/Clang the
// two lower to identical instructions anyway (xsimd's shuffle IS
// __builtin_shufflevector there), so the preference costs nothing.
//
// When neither is available (BLAKE3PP_FORCE_SCALAR, or MSVC-on-arm64 below)
// BLAKE3PP_HAVE_SHUFFLE stays undefined and every transpose falls back to
// the scalar staging gather. The gate must be a preprocessor one, not just
// `if constexpr`: a discarded constexpr branch still name-looks-up its
// non-dependent identifiers, so machinery that does not exist in this TU
// must not even be NAMED.

// Testing override: -DBLAKE3PP_SHUFFLE_FORCE_XSIMD picks the xsimd backend
// on a GNU frontend too, so the path MSVC actually takes can be compiled
// and run anywhere. Worth the three lines: twice already, kernel bugs have
// hidden in a platform path nobody could execute locally.



// The u32-vector facade: one minimal wide-word type, four providers, chosen
// here and nowhere else.
//
//   BLAKE3PP_FORCE_SCALAR   width-1 plain uint32_t (the scalar kernel, and
//                           the correctness oracle for everything else)
//   BLAKE3PP_FORCE_VEXT     per-TU override to GNU vector extensions at a
//                           width the build names. For ISAs no provider
//                           covers: the std providers deduce width 1 on a
//                           target their ISA list does not know, and xsimd
//                           has no backend at all (MSA is the customer).
//   BLAKE3PP_FORCE_XSIMD    per-TU override to xsimd even when a std
//                           provider exists. The SVE kernels need it:
//                           libstdc++'s experimental::simd SVE backend
//                           keeps guarded inline-variable index tables
//                           whose dynamic initializers are SVE code that
//                           runs at LOAD TIME: an instant SIGILL on any
//                           non-SVE machine, which defeats the whole
//                           fat-binary premise. xsimd's SVE backend is
//                           initializer-free.
//   BLAKE3PP_HAS_STD_SIMD   native C++26 std::simd (GCC 16's <simd>)
//   BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD
//                           Parallelism TS v2 <experimental/simd>
//   BLAKE3PP_HAS_XSIMD      xsimd polyfill (libc++, MSVC, anything else)
//
// Each provider defines the same struct u32v in simd/<provider>.hpp
// (broadcast/load/store, operator+ / operator^ / rotr), and none of them
// contains a single #ifdef. Adding a provider means adding a file and one
// arm below; the kernel source never learns which one it got.
//
// The width is whatever the TU's -m flags make native (SSE: 4, AVX2: 8,
// AVX-512: 16, NEON: 4), so the same kernel source vectorizes differently in
// every arch variant. The type lives INSIDE the arch namespace on purpose:
// its layout depends on the TU's flags, so a shared-namespace definition
// would be an ODR lie. It must never cross the kernel boundary.




#if defined(BLAKE3PP_FORCE_SCALAR)


// Provider: none. Width-1 plain uint32_t: the scalar kernel, and the
// correctness oracle every vector variant is checked against.
// Included by simd_facade.hpp, which selects exactly one provider.

#include <bit>
#include <cstddef>
#include <cstdint>



namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS {

struct u32v {
  using impl = std::uint32_t;
  static constexpr std::size_t width = 1;
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {x};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    return {p[0]};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    p[0] = v;
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    return {std::rotr(a.v, n)};
  }
};

}  // namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS

#elif defined(BLAKE3PP_FORCE_VEXT)


// Provider: GNU/Clang vector extensions, for targets that neither xsimd
// nor a std provider covers. Included by simd_facade.hpp, which selects
// exactly one provider.
//
// The width is named by the build here rather than deduced, and that is
// the whole reason this provider exists. The std providers pick a native
// width from an ISA list, and on a target that list has never heard of,
// the width they pick is 1. Measured on mips64el with -mmsa, GCC 14:
// std::experimental::native_simd<uint32_t> reports width 1 and emits no
// vector instruction, while the same xor-then-rotate over an explicit
// 16-byte vector compiles to six MSA instructions. The compiler reaches
// the ISA perfectly; only the library's width guess does not.
//
// Everything below is ordinary C++ over a vector-extension type, so the
// instruction selection is the compiler's job on every target it knows.

#include <cstddef>
#include <cstdint>
#include <cstring>



#ifndef BLAKE3PP_VEXT_BYTES
#error "the vext provider needs -DBLAKE3PP_VEXT_BYTES=<register bytes>"
#endif

namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS {

struct u32v {
  using impl [[gnu::vector_size(BLAKE3PP_VEXT_BYTES)]] = std::uint32_t;
  static constexpr std::size_t width =
      BLAKE3PP_VEXT_BYTES / sizeof(std::uint32_t);
  impl v;

  // Vector-scalar arithmetic broadcasts the scalar, so this is a splat
  // and folds to one instruction.
  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl{} + x};
  }
  // memcpy rather than a cast: the callers' buffers carry no vector
  // alignment, and every compiler folds a sizeof-register memcpy into
  // the target's unaligned load.
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    impl x;
    std::memcpy(&x, p, sizeof(x));
    return {x};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    std::memcpy(p, &v, sizeof(v));
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    return {(a.v >> n) | (a.v << (32 - n))};
  }
};

}  // namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS

#elif defined(BLAKE3PP_FORCE_XSIMD)


// Provider: the xsimd polyfill, for libc++, MSVC, and anything else without
// a usable standard simd. Included by simd_facade.hpp, which selects
// exactly one provider.

#include <cstddef>
#include <cstdint>
#include <xsimd/xsimd.hpp>



namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS {

struct u32v {
  using impl = xsimd::batch<std::uint32_t>;
  static constexpr std::size_t width = impl::size;
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl(x)};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    return {impl::load_unaligned(p)};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    v.store_unaligned(p);
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    return {xsimd::rotr(a.v, n)};
  }
};

}  // namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS

#elif defined(BLAKE3PP_HAS_STD_SIMD)


// Provider: native C++26 std::simd (GCC 16's <simd>, spelled
// std::simd::vec<T>). Included by simd_facade.hpp, which selects exactly
// one provider.

#include <cstddef>
#include <cstdint>
#include <simd>
#include <span>



namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS {

struct u32v {
  using impl = std::simd::vec<std::uint32_t>;
  static constexpr std::size_t width = impl::size();
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl(x)};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    return {std::simd::unchecked_load<impl>(
        std::span<const std::uint32_t>(p, width))};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    std::simd::unchecked_store(v, std::span<std::uint32_t>(p, width));
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    // No simd rotate in the MVP; the shift-or idiom pattern-matches to
    // native rotates where they exist (AVX-512 vprord).
    return {(a.v >> n) | (a.v << (32 - n))};
  }
};

}  // namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS

#elif defined(BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD)


// Provider: Parallelism TS v2 <experimental/simd> (libstdc++ from GCC 11
// on, including Clang against a correctly pinned libstdc++; libc++'s is too
// incomplete and fails the configure probe). Included by simd_facade.hpp,
// which selects exactly one provider.

#include <cstddef>
#include <cstdint>
#include <experimental/simd>



namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS {

struct u32v {
  using impl = std::experimental::native_simd<std::uint32_t>;
  static constexpr std::size_t width = impl::size();
  impl v;

  static BLAKE3PP_FORCE_INLINE u32v broadcast(std::uint32_t x) noexcept {
    return {impl(x)};
  }
  static BLAKE3PP_FORCE_INLINE u32v load(const std::uint32_t* p) noexcept {
    impl x;
    x.copy_from(p, std::experimental::element_aligned);
    return {x};
  }
  BLAKE3PP_FORCE_INLINE void store(std::uint32_t* p) const noexcept {
    v.copy_to(p, std::experimental::element_aligned);
  }

  friend BLAKE3PP_FORCE_INLINE u32v operator+(u32v a, u32v b) noexcept {
    return {a.v + b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v operator^(u32v a, u32v b) noexcept {
    return {a.v ^ b.v};
  }
  friend BLAKE3PP_FORCE_INLINE u32v rotr(u32v a, int n) noexcept {
    return {(a.v >> n) | (a.v << (32 - n))};
  }
};

}  // namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS

#elif defined(BLAKE3PP_HAS_XSIMD)

#else
#error "No simd provider: expected BLAKE3PP_FORCE_SCALAR, BLAKE3PP_FORCE_VEXT, BLAKE3PP_HAS_STD_SIMD, BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD or BLAKE3PP_HAS_XSIMD"
#endif


// BLAKE3PP_KERNEL_SHUFFLE_TREE (cmake/ArchKernels.cmake) off leaves the
// kernel without a shuffle backend: transposes go through the scalar
// staging gather and the byte-granular rotates through shift-or, the
// spelling before the bypass, kept buildable so the pair can be measured.
#ifndef BLAKE3PP_KERNEL_SHUFFLE_TREE
#define BLAKE3PP_KERNEL_SHUFFLE_TREE 1
#endif

#if defined(BLAKE3PP_FORCE_SCALAR) || !BLAKE3PP_KERNEL_SHUFFLE_TREE
// Width 1: nothing to transpose. Or the bypass switched off.

#elif (defined(__GNUC__) || defined(__clang__)) && \
    !defined(BLAKE3PP_SHUFFLE_FORCE_XSIMD)
#define BLAKE3PP_HAVE_SHUFFLE 1


// Shuffle backend: GNU/Clang vector extensions.
//
// GCC and Clang both provide __builtin_shufflevector over vector-extension
// types with compile-time indices, and both instruction-select a shuffle
// whose indices exactly match a hardware macro into that single instruction.
// This backend works for EVERY simd provider: the provider's register is
// bit_cast in and out, which is free because all of them are register-sized
// and trivially copyable.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>




namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS::shuffle_detail {

// GCC raises -Wpsabi for vector types wider than the TU's -m flags allow
// natively. These types never appear in any cross-TU signature (that is the
// entire point of the kernel's design), so the ABI concern is moot; GCC's
// own <simd> internals suppress the warning on the same reasoning.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"

template <std::size_t W>
struct vext;
template <>
struct vext<4> {
  typedef std::uint32_t type __attribute__((vector_size(16)));
};
template <>
struct vext<8> {
  typedef std::uint32_t type __attribute__((vector_size(32)));
};
template <>
struct vext<16> {
  typedef std::uint32_t type __attribute__((vector_size(64)));
};

// Byte view of the same register, for the byte-granular rotates.
template <std::size_t W>
struct bext;
template <>
struct bext<4> {
  typedef std::uint8_t type __attribute__((vector_size(16)));
};
template <>
struct bext<8> {
  typedef std::uint8_t type __attribute__((vector_size(32)));
};

struct vext_backend {
  template <std::size_t W>
  using reg = typename vext<W>::type;

  template <std::size_t W>
  static constexpr bool supports =
      (W == 4 || W == 8 || W == 16) &&
      std::endian::native == std::endian::little &&
      sizeof(typename u32v::impl) == 4 * W &&
      std::is_trivially_copyable_v<typename u32v::impl>;

  // Byte-granular rotate: rotr by a multiple of 8 bits is a byte
  // permutation within each 32-bit element, and the lane-local constexpr
  // pattern is exactly vpshufb (NEON: tbl, and clang recognizes the 16-bit
  // case as rev32): one uop instead of the three (shift, shift, or) a
  // generic rotate costs on ISAs without a native rotate instruction.
  template <std::size_t W>
  static constexpr bool supports_byte_rot = supports<W> && (W == 4 || W == 8);

  template <int... I, class V>
  static BLAKE3PP_FORCE_INLINE V shuf(V a, V b) noexcept {
    return __builtin_shufflevector(a, b, I...);
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE reg<W> load(const std::uint8_t* p) noexcept {
    reg<W> r;
    std::memcpy(&r, p, sizeof(r));
    return r;
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE void store(std::uint8_t* p, reg<W> v) noexcept {
    std::memcpy(p, &v, sizeof(v));
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE u32v to_word(reg<W> v) noexcept {
    return u32v{std::bit_cast<typename u32v::impl>(v)};
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE reg<W> from_word(u32v w) noexcept {
    return std::bit_cast<reg<W>>(w.v);
  }

  template <int RB, std::size_t W>
  static BLAKE3PP_FORCE_INLINE u32v rot_bytes(u32v w) noexcept {
    using B = typename bext<W>::type;
    const B b = std::bit_cast<B>(from_word<W>(w));
    const B r = [&]<std::size_t... I>(std::index_sequence<I...>) {
      // Little-endian: rotr by 8*RB bits moves source byte (j+RB)%4 into
      // destination byte j of each element.
      return __builtin_shufflevector(b, b,
                                     ((I / 4) * 4 + ((I % 4) + RB) % 4)...);
    }(std::make_index_sequence<4 * W>{});
    return to_word<W>(std::bit_cast<reg<W>>(r));
  }
};

#pragma GCC diagnostic pop

}  // namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS::shuffle_detail

namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS {
using shuffle_backend = shuffle_detail::vext_backend;
}

#elif defined(BLAKE3PP_HAS_XSIMD) && !(defined(_M_ARM64) && !defined(__clang__))
// The pure-MSVC-arm64 exclusion: xsimd 14.3's neon64 swizzle (which the
// shuffle decomposition instantiates on non-builtin frontends) returns
// through a vreinterpretq_* chain that MSVC's arm64_neon.h defines as
// no-ops over one shared __n128 type, so a batch<uint8_t> lands in a
// batch<uint32_t> return seat and C2440 follows. Until that is fixed
// upstream, cl-on-arm64 keeps NEON rounds but stages its transposes.
#define BLAKE3PP_HAVE_SHUFFLE 1


// Shuffle backend: xsimd's own portable two-input constant shuffle.
//
// The fallback for frontends without vector extensions; in practice MSVC,
// which has no __builtin_shufflevector. There xsimd decomposes each shuffle
// into swizzle(x)+swizzle(y)+select (~3 uops), still far ahead of the scalar
// staging gather. On GCC/Clang xsimd lowers it through
// __builtin_shufflevector, so this backend and vext are instruction-
// identical there (verified by object-histogram diff), which is why the
// selection in shuffle.hpp can prefer either without a performance cliff.
//
// Requires the xsimd provider: it shuffles u32v::impl directly, no cast.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>




namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS::shuffle_detail {

// Byte-rotate mask: dest byte i of each 32-bit element takes source byte
// ((i%4)+RB)%4, a little-endian rotr by 8*RB bits, the same pattern the
// vext backend's rot_bytes encodes. Lane-local by construction, which is what
// makes xsimd 14.3's constant u8 swizzle emit a single vpshufb (its
// is_cross_lane check) instead of a cross-lane fixup.
template <int RB>
struct rot_bytes_gen {
  static constexpr std::uint8_t get(std::size_t i, std::size_t) noexcept {
    return static_cast<std::uint8_t>((i / 4) * 4 + ((i % 4) + RB) % 4);
  }
};

struct xsimd_backend {
  // The batch's width IS W; there is no separate register type to name.
  template <std::size_t W>
  using reg = typename u32v::impl;

  template <std::size_t W>
  static constexpr bool supports = (W == 4 || W == 8 || W == 16) &&
                                   W == u32v::width &&
                                   std::endian::native == std::endian::little;

  // CRITICAL gate for the byte-rotate: the hardware must actually HAVE a
  // byte shuffle. Below ssse3, xsimd's sse2 u8 swizzle is a per-byte
  // scalar loop that measured 4x WORSE than shift-or, so any pre-ssse3
  // instantiation must stay on shift-or. (Every shipped x86 kernel is
  // sse4.2 or wider today, MSVC included: KernelVariants.cmake passes
  // /arch:SSE4.2 plus the __SSSE3__/__SSE4_1__/__SSE4_2__ defines xsimd
  // keys on, so the gate is open there; it remains the guard for any
  // narrower build.) NEON and wasm carry their byte shuffles natively.
  static constexpr bool is_x86 =
      std::is_base_of_v<xsimd::sse2, typename u32v::impl::arch_type>;
  static constexpr bool has_byte_shuffle =
      !is_x86 ||
      std::is_base_of_v<xsimd::ssse3, typename u32v::impl::arch_type>;

  // W==16 stays on shift-or: avx512bw has no constant u8 swizzle in
  // xsimd 14.3 (and GNU compilers fuse shift-or into vprold there anyway).
  template <std::size_t W>
  static constexpr bool supports_byte_rot =
      supports<W> && (W == 4 || W == 8) && has_byte_shuffle;

  template <int... I, class V>
  static BLAKE3PP_FORCE_INLINE V shuf(V a, V b) noexcept {
    return xsimd::shuffle(
        a, b,
        xsimd::batch_constant<std::uint32_t, typename V::arch_type,
                              static_cast<std::uint32_t>(I)...>{});
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE reg<W> load(const std::uint8_t* p) noexcept {
    reg<W> r;
    std::memcpy(&r, p, sizeof(r));
    return r;
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE void store(std::uint8_t* p, reg<W> v) noexcept {
    std::memcpy(p, &v, sizeof(v));
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE u32v to_word(reg<W> v) noexcept {
    return u32v{v};
  }

  template <std::size_t W>
  static BLAKE3PP_FORCE_INLINE reg<W> from_word(u32v w) noexcept {
    return w.v;
  }

  template <int RB, std::size_t W>
  static BLAKE3PP_FORCE_INLINE u32v rot_bytes(u32v w) noexcept {
    using B = typename u32v::impl;
    const auto bytes = xsimd::bitwise_cast<std::uint8_t>(w.v);
    constexpr auto mask =
        xsimd::make_batch_constant<std::uint8_t, rot_bytes_gen<RB>,
                                   typename B::arch_type>();
    return u32v{
        xsimd::bitwise_cast<std::uint32_t>(xsimd::swizzle(bytes, mask))};
  }
};

}  // namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS::shuffle_detail

namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS {
using shuffle_backend = shuffle_detail::xsimd_backend;
}
#endif

#if defined(BLAKE3PP_HAVE_SHUFFLE)


// The radix-2 shuffle networks, written ONCE, over any backend supplying a
// two-input constant shuffle.
//
// A W x W word transpose decomposes into log2(W) radix-2 stages whose index
// patterns are precisely the in-lane 32-bit unpacks (vpunpckl/hdq), the
// in-lane 64-bit unpacks (vpunpckl/hqdq), and the 128-bit lane merges
// (vperm2i128 / vinserti128): 24 single-uop shuffles for the AVX2 8x8 case,
// verified on GCC 16 (integer domain) and Clang 22 (same network, float
// domain: vunpcklps/vunpcklpd/vperm2f128).
//
// The index lists below are the whole point of this file: they are hardware
// macro-ops spelled as permutations, they were derived by simulation, and
// they must never drift between backends. Op::shuf<I...> is the only thing
// a backend has to provide to run them.

#include <cstddef>



namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS::shuffle_detail {

// 4x4: two radix-2 stages (32-bit unpacks, then 64-bit unpacks).
template <class Op, class V>
BLAKE3PP_FORCE_INLINE void transpose_net(const V (&r)[4], V (&out)[4]) noexcept {
  const V a0 = Op::template shuf<0, 4, 1, 5>(r[0], r[1]);
  const V a1 = Op::template shuf<2, 6, 3, 7>(r[0], r[1]);
  const V a2 = Op::template shuf<0, 4, 1, 5>(r[2], r[3]);
  const V a3 = Op::template shuf<2, 6, 3, 7>(r[2], r[3]);
  out[0] = Op::template shuf<0, 1, 4, 5>(a0, a2);
  out[1] = Op::template shuf<2, 3, 6, 7>(a0, a2);
  out[2] = Op::template shuf<0, 1, 4, 5>(a1, a3);
  out[3] = Op::template shuf<2, 3, 6, 7>(a1, a3);
}

// 8x8: three radix-2 stages. The index sets are lane-local on purpose:
// they are exactly vpunpckl/hdq, vpunpckl/hqdq, and the final cross-lane
// merge vperm2i128/vinserti128.
template <class Op, class V>
BLAKE3PP_FORCE_INLINE void transpose_net(const V (&r)[8], V (&out)[8]) noexcept {
  const V a0 = Op::template shuf<0,  8,  1,  9, 4, 12,  5, 13>(r[0], r[1]);
  const V a1 = Op::template shuf<2, 10,  3, 11, 6, 14,  7, 15>(r[0], r[1]);
  const V a2 = Op::template shuf<0,  8,  1,  9, 4, 12,  5, 13>(r[2], r[3]);
  const V a3 = Op::template shuf<2, 10,  3, 11, 6, 14,  7, 15>(r[2], r[3]);
  const V a4 = Op::template shuf<0,  8,  1,  9, 4, 12,  5, 13>(r[4], r[5]);
  const V a5 = Op::template shuf<2, 10,  3, 11, 6, 14,  7, 15>(r[4], r[5]);
  const V a6 = Op::template shuf<0,  8,  1,  9, 4, 12,  5, 13>(r[6], r[7]);
  const V a7 = Op::template shuf<2, 10,  3, 11, 6, 14,  7, 15>(r[6], r[7]);

  const V b0 = Op::template shuf<0,  1,  8,  9, 4,  5, 12, 13>(a0, a2);
  const V b1 = Op::template shuf<2,  3, 10, 11, 6,  7, 14, 15>(a0, a2);
  const V b2 = Op::template shuf<0,  1,  8,  9, 4,  5, 12, 13>(a1, a3);
  const V b3 = Op::template shuf<2,  3, 10, 11, 6,  7, 14, 15>(a1, a3);
  const V b4 = Op::template shuf<0,  1,  8,  9, 4,  5, 12, 13>(a4, a6);
  const V b5 = Op::template shuf<2,  3, 10, 11, 6,  7, 14, 15>(a4, a6);
  const V b6 = Op::template shuf<0,  1,  8,  9, 4,  5, 12, 13>(a5, a7);
  const V b7 = Op::template shuf<2,  3, 10, 11, 6,  7, 14, 15>(a5, a7);
  
  out[0] = Op::template shuf<0, 1, 2, 3,  8,  9, 10, 11>(b0, b4);
  out[1] = Op::template shuf<0, 1, 2, 3,  8,  9, 10, 11>(b1, b5);
  out[2] = Op::template shuf<0, 1, 2, 3,  8,  9, 10, 11>(b2, b6);
  out[3] = Op::template shuf<0, 1, 2, 3,  8,  9, 10, 11>(b3, b7);
  out[4] = Op::template shuf<4, 5, 6, 7, 12, 13, 14, 15>(b0, b4);
  out[5] = Op::template shuf<4, 5, 6, 7, 12, 13, 14, 15>(b1, b5);
  out[6] = Op::template shuf<4, 5, 6, 7, 12, 13, 14, 15>(b2, b6);
  out[7] = Op::template shuf<4, 5, 6, 7, 12, 13, 14, 15>(b3, b7);
}

// 16x16: four radix-2 stages. Index lists derived from the same recursive
// construction (verified by simulation): in-lane dword unpacks
// (vpunpckl/hdq), in-lane qword unpacks (vpunpckl/hqdq), then two levels
// of 128-bit-block merges, AVX-512's vshufi32x4 territory; even a
// generic lowering lands on vpermt2d (any two-source dword permute, one
// uop). 64 two-register shuffles replace the 256 scalar load/stores of
// the staging gather.
template <class Op, class V>
BLAKE3PP_FORCE_INLINE void transpose_net(const V (&r)[16],
                                         V (&out)[16]) noexcept {
  V a[16];
  for (std::size_t g = 0; g < 8; ++g) {
    a[2 * g] =     Op::template shuf<0, 16, 1, 17, 4, 20, 5, 21,  8, 24,  9, 25, 12, 28, 13, 29>(r[2 * g], r[2 * g + 1]);
    a[2 * g + 1] = Op::template shuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15, 31>(r[2 * g], r[2 * g + 1]);
  }
  V b[16];
  for (std::size_t q = 0; q < 4; ++q) {
    const std::size_t k = 4 * q;
    b[k + 0] = Op::template shuf<0, 1, 16, 17, 4, 5, 20, 21,  8,  9, 24, 25, 12, 13, 28, 29>(a[k + 0], a[k + 2]);
    b[k + 1] = Op::template shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(a[k + 0], a[k + 2]);
    b[k + 2] = Op::template shuf<0, 1, 16, 17, 4, 5, 20, 21,  8,  9, 24, 25, 12, 13, 28, 29>(a[k + 1], a[k + 3]);
    b[k + 3] = Op::template shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(a[k + 1], a[k + 3]);
  }
  V c[16];
  for (std::size_t h = 0; h < 2; ++h) {
    const std::size_t k = 8 * h;
    for (std::size_t j = 0; j < 4; ++j) {
      c[k + j] =     Op::template shuf<0, 1, 2, 3, 16, 17, 18, 19,  8,  9, 10, 11, 24, 25, 26, 27>(b[k + j], b[k + j + 4]);
      c[k + j + 4] = Op::template shuf<4, 5, 6, 7, 20, 21, 22, 23, 12, 13, 14, 15, 28, 29, 30, 31>(b[k + j], b[k + j + 4]);
    }
  }
  for (std::size_t j = 0; j < 8; ++j) {
    out[j] =     Op::template shuf<0, 1,  2,  3,  4,  5,  6,  7, 16, 17, 18, 19, 20, 21, 22, 23>(c[j], c[j + 8]);
    out[j + 8] = Op::template shuf<8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31>(c[j], c[j + 8]);
  }
}

// The two in-lane stages alone, factored for the quartered W==16 form: with
// block-level transposition already done by 128-bit addressing, a 4x4
// transpose per 128-bit lane finishes the job (same s1/s2 index lists as
// the full tree; all vpunpck, no cross-lane traffic).
template <class Op, class V>
BLAKE3PP_FORCE_INLINE void inlane_4x4(const V (&r)[4], V (&t)[4]) noexcept {
  const V a0 = Op::template shuf<0, 16, 1, 17, 4, 20, 5, 21,  8, 24,  9, 25, 12, 28, 13, 29>(r[0], r[1]);
  const V a1 = Op::template shuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15, 31>(r[0], r[1]);
  const V a2 = Op::template shuf<0, 16, 1, 17, 4, 20, 5, 21,  8, 24,  9, 25, 12, 28, 13, 29>(r[2], r[3]);
  const V a3 = Op::template shuf<2, 18, 3, 19, 6, 22, 7, 23, 10, 26, 11, 27, 14, 30, 15, 31>(r[2], r[3]);
  t[0] = Op::template shuf<0, 1, 16, 17, 4, 5, 20, 21,  8,  9, 24, 25, 12, 13, 28, 29>(a0, a2);
  t[1] = Op::template shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(a0, a2);
  t[2] = Op::template shuf<0, 1, 16, 17, 4, 5, 20, 21,  8,  9, 24, 25, 12, 13, 28, 29>(a1, a3);
  t[3] = Op::template shuf<2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31>(a1, a3);
}

}  // namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS::shuffle_detail

#endif




// The tuning switches, set by cmake/ArchKernels.cmake from
// -DBLAKE3PP_KERNEL_*_ROTATE=auto|on|off; each fallback repeats that
// default (the headers are also parsed by tooling that does not pass our
// flags). Off restores the generic spelling for re-measurement.
#ifndef BLAKE3PP_KERNEL_SRI_ROTATE
#define BLAKE3PP_KERNEL_SRI_ROTATE 1
#endif
#ifndef BLAKE3PP_KERNEL_XAR_ROTATE
#define BLAKE3PP_KERNEL_XAR_ROTATE 1
#endif
#ifndef BLAKE3PP_KERNEL_VROR_ROTATE
#define BLAKE3PP_KERNEL_VROR_ROTATE 1
#endif

namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS {

#if defined(BLAKE3PP_HAVE_SVE2_XAR)
typedef svuint32_t sve_fixed_u32
    __attribute__((arm_sve_vector_bits(__ARM_FEATURE_SVE_BITS)));
#endif

#if defined(BLAKE3PP_HAVE_ZVBB_VROR)
typedef vuint32m1_t rvv_fixed_u32
    __attribute__((riscv_rvv_vector_bits(__riscv_v_fixed_vlen)));
#endif

// Which SOURCE spelling reaches the one-instruction rotate is a property of
// the compiler, not of the hardware. Measured for rot(d ^ a, N), the shape
// g actually uses, at both 128- and 256-bit width:
//
//                       clang 22        GCC 14/16
//   generic shift-or    1x pshufb       pslld+psrld+por
//   byte shuffle        N=16: pshuflw+pshufhw    1x pshufb
//                       N=8:  1x pshufb          1x pshufb
//
// Mirror images: LLVM canonicalizes the shift-or rotate idiom straight to
// pshufb, but lowers OUR byte-shuffle to the 16-bit-lane pair for N==16
// (both are legal; its cost model dislikes materializing the mask, even
// though upstream's asm hoists exactly that mask out of the loop). GCC
// never forms a shuffle from shift-or and needs the byte spelling. Neither
// form is portable-optimal, so the choice is made here per compiler.
// Forcing it with _mm256_shuffle_epi8 does NOT work: InstCombine folds a
// constant-mask pshufb intrinsic back into a generic shuffle and lowers it
// the same way. The split is stable across the clang range this project
// builds with (18.1, 20.1 and 22 all lower both spellings identically),
// so the predicate is a compiler-family choice, not a version workaround.
//
// Worth +4.0% avx2 and +4.2% sse42 on clang 22 / Zen 3+ (256 MiB, best of
// 3, interleaved; the reference-kernel rows moved 0.8% over the same runs).
// Not because it shrinks the loop; it does not: 1489 -> 1486 instructions,
// because the freed pshuflw/pshufhw pair comes back as spills once the mask
// occupies a register all loop long. What shortens is g's SERIAL chain, two
// dependent shuffles down to one. Same lesson as the aarch64 sri escape:
// on this kernel the metric is critical-path length, not instruction count.
// BLAKE3PP_KERNEL_ROT16_PER_COMPILER (cmake/ArchKernels.cmake) off gives
// clang the byte shuffle for 16 like every other compiler: the spelling
// before the split, kept buildable so the pair can be re-measured.
#ifndef BLAKE3PP_KERNEL_ROT16_PER_COMPILER
#define BLAKE3PP_KERNEL_ROT16_PER_COMPILER 1
#endif
template <int N>
constexpr bool prefer_byte_rot() noexcept {
#if defined(__clang__) && BLAKE3PP_KERNEL_ROT16_PER_COMPILER && \
    (defined(__x86_64__) || defined(__i386__) || defined(_M_X64))
  return N == 8;  // N == 16 is one instruction cheaper as generic shift-or
#else
  return N == 16 || N == 8;
#endif
}

// Compile-time-amount rotate for the wide word: a single byte shuffle for
// the 16- and 8-bit amounts where that is the better spelling (above), then
// the aarch64 shl+sri pair, then the generic shift-or.
// W is a defaulted template parameter (not read directly off u32v) so the
// discarded constexpr branches stay dependent; non-dependent constructs in
// a discarded branch are still instantiated.
template <int N, std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE u32v rot(u32v a) noexcept {
#if defined(BLAKE3PP_HAVE_SHUFFLE)
  if constexpr (prefer_byte_rot<N>() && shuffle_backend::supports_byte_rot<W>) {
    return shuffle_backend::rot_bytes<N / 8, W>(a);
  }
#endif
#if defined(__aarch64__) && !defined(BLAKE3PP_FORCE_SCALAR) && \
    BLAKE3PP_KERNEL_SRI_ROTATE
  // The rotate amounts with no byte-granular shuffle (12 and 7): shl+sri
  // instead of the shl+usra clang selects for the generic shift-or. SRI and
  // USRA cost the same two instructions, but SRI is a cycle faster on Apple
  // cores, and these rotates sit on g's serial critical path. This was the
  // entire residual against upstream's blake3_neon.c, whose explicit
  // intrinsics reach sri directly (their PR #319 measured the same):
  // 1.61 -> 1.71 GiB/s on Apple M2 / clang 22, exactly upstream's number;
  // the two hash loops are otherwise instruction-for-instruction identical.
  // (sri also wins on Neoverse V2, +12%, and N1, +8%, in alternating A/B
  // pairs of the static clang build; the SRI_ROTATE switch exists to
  // re-measure. GCC 15 selects usra without it exactly as clang does.)
  if constexpr (W == 4 && sizeof(typename u32v::impl) == 16 &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    // Immediately-invoked generic lambda: `if constexpr` only shields
    // DEPENDENT constructs from the discarded branch, and everything here
    // is concrete: a wider-than-NEON aarch64 TU (fixed-length SVE) would
    // hard-error on the 16-byte bit_cast at template definition time even
    // though the branch is never taken. Routing a.v through a deduced
    // parameter restores the dependency. (At SVE VL=128 the branch IS
    // taken, and validly: Z0-Z31 alias V0-V31, so the NEON sri applies.)
    return [](auto impl) BLAKE3PP_LAMBDA_FORCE_INLINE {
      const uint32x4_t x = std::bit_cast<uint32x4_t>(impl);
      return u32v{std::bit_cast<decltype(impl)>(
          vsriq_n_u32(vshlq_n_u32(x, 32 - N), x, N))};
    }(a.v);
  }
#endif
  return rotr(a, N);
}

// The fused xor-then-rotate the kernel's g function is made of:
// every rotate in BLAKE3 has the shape rot<N>(x ^ y). SVE2's XAR does the
// pair in ONE instruction (rotate right of the exclusive-or), replacing
// either eor+tbl (N=16/8, byte-granular) or eor+shl+sri (N=12/7). It is
// the whole reason an SVE2 variant beats the NEON kernel at the same
// 128-bit width (+18% on Neoverse V2, entirely this). Everywhere else
// this is exactly rot<N>(x ^ y).
template <int N>
BLAKE3PP_FORCE_INLINE u32v xor_rot(u32v x, u32v y) noexcept {
#if defined(BLAKE3PP_HAVE_SVE2_XAR) && BLAKE3PP_KERNEL_XAR_ROTATE
  if constexpr (sizeof(typename u32v::impl) * 8 == __ARM_FEATURE_SVE_BITS &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    // Same dependent-lambda shield as rot's sri escape above: keeps the
    // bit_casts out of TUs whose impl is not the fixed-length SVE size.
    return [](auto ix, auto iy) BLAKE3PP_LAMBDA_FORCE_INLINE {
      // The provider's register itself where it exposes one, rather than
      // a reinterpret of the object holding it. Both spellings name the
      // same fixed-length SVE type, but clang under the MSVC ABI
      // declines to inline a reinterpret of the batch CLASS even with
      // always_inline: it emitted an out-of-line `ldr q0, [x0]; ret` and
      // spilled a live vector at every use, 448 calls and 224 spills in
      // hash_many, which is two per rotate. The same clang targeting
      // Linux, and GCC, fold it away. The SVE kernels are pinned to
      // xsimd (FORCE_XSIMD in cmake/KernelVariants.cmake), so the member
      // is always there; the reinterpret stays as the general spelling.
      if constexpr (requires { ix.data; }) {
        const sve_fixed_u32 r = svxar_n_u32(ix.data, iy.data, N);
        return u32v{decltype(ix){r}};
      } else {
        const sve_fixed_u32 r = svxar_n_u32(std::bit_cast<sve_fixed_u32>(ix),
                                            std::bit_cast<sve_fixed_u32>(iy),
                                            N);
        return u32v{std::bit_cast<decltype(ix)>(r)};
      }
    }(x.v, y.v);
  }
#endif
#if defined(BLAKE3PP_HAVE_ZVBB_VROR) && BLAKE3PP_KERNEL_VROR_ROTATE
  if constexpr (sizeof(typename u32v::impl) * 8 == __riscv_v_fixed_vlen &&
                std::is_trivially_copyable_v<typename u32v::impl>) {
    // Zvbb's vror is XAR minus the folded eor: base RVV has no rotate at
    // all, so the generic fallback is FOUR ops (vxor+vsll+vsrl+vor); this
    // is vxor+vror. Same dependent-lambda shield as the branches above.
    const u32v e = x ^ y;
    return [](auto impl) BLAKE3PP_LAMBDA_FORCE_INLINE {
      const rvv_fixed_u32 v = std::bit_cast<rvv_fixed_u32>(impl);
      const rvv_fixed_u32 r =
          __riscv_vror_vx_u32m1(v, N, __riscv_v_fixed_vlen / 32);
      return u32v{std::bit_cast<decltype(impl)>(r)};
    }(e.v);
  }
#endif
  return rot<N>(x ^ y);
}

}  // namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS




// The message transpose. (The wide word's rotate and its per-ISA escape
// hatches live in rotate.hpp.)
//
// hash_batch needs the 16 message words of a block gathered ACROSS lanes
// (word j of every input in one vector). No simd provider exposes a portable
// permute for that (the std::simd MVP has no shuffle API at all), so the
// naive route stages through a scalar array, and it costs ~40% of the whole
// hash (upstream's SSE4.1 assembly matches our AVX2 because of it).
//
// The bypass is a radix-2 shuffle tree whose index patterns ARE hardware
// macro-ops; it lives in shuffle/networks.hpp, written once over whichever
// backend shuffle.hpp selected for this TU. What remains here is the part
// that is genuinely about transposing a message: which registers to load
// from where, and where their transposed results go. Widths other than
// 4/8/16, and TUs with no shuffle backend at all, fall back to the scalar
// staging gather at the bottom of each function.

#include <cstddef>
#include <cstdint>
#include <cstring>






namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS {
namespace transpose_detail {

// The W==16 strategy is a RUNTIME dial (kern::transpose16_active, set via
// blake3pp::set_transpose16 / tune_transpose16): on double-pumped AVX-512
// (Strix Point) the register tree measured 20% slower than the scalar
// staging gather while the quartered form measured 17% faster, and no
// CPUID bit distinguishes those microarchitectures, so the winner is
// raced, not detected. All three paths compile into the W==16 kernel; the
// relaxed load deciding between them amortizes over a >=16 KiB batch.
BLAKE3PP_FORCE_INLINE transpose16_mode t16_mode() noexcept {
  return transpose16_active.load(std::memory_order_relaxed);
}

BLAKE3PP_FORCE_INLINE std::uint32_t ld32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

#if defined(BLAKE3PP_HAVE_SHUFFLE)

// Gathers the block's 16 message words across W lanes through the shuffle
// networks. Returns false only for the W==16 `staging` dial setting, whose
// whole point is to use the scalar gather instead.
template <class B, std::size_t W>
BLAKE3PP_FORCE_INLINE bool shuffle_load(const std::uint8_t* const* inputs,
                                        std::size_t offset, u32v m[16],
                                        [[maybe_unused]]
                                        transpose16_mode mode) noexcept {
  using V = typename B::template reg<W>;
  if constexpr (W == 16) {
    if (mode == transpose16_mode::quartered) {
      // Quartered: 128-bit pieces land block-transposed by ADDRESSING;
      // registers only run the two in-lane stages.
      for (std::size_t q = 0; q < 4; ++q) {
        V r[4];
        for (std::size_t k = 0; k < 4; ++k) {
          std::uint8_t quad[64];
          for (std::size_t l = 0; l < 4; ++l) {
            std::memcpy(quad + 16 * l, inputs[4 * l + k] + offset + 16 * q,
                        16);
          }
          r[k] = B::template load<W>(quad);
        }
        V t[4];
        shuffle_detail::inlane_4x4<B>(r, t);
        for (std::size_t j = 0; j < 4; ++j) {
          m[4 * q + j] = B::template to_word<W>(t[j]);
        }
      }
      return true;
    }
    if (mode == transpose16_mode::tree) {
      V r[16];
      for (std::size_t lane = 0; lane < 16; ++lane) {
        r[lane] = B::template load<W>(inputs[lane] + offset);
      }
      V t[16];
      shuffle_detail::transpose_net<B>(r, t);
      for (std::size_t j = 0; j < 16; ++j) {
        m[j] = B::template to_word<W>(t[j]);
      }
      return true;
    }
    return false;  // staging
  } else {
    constexpr std::size_t groups = 16 / W;
    for (std::size_t g = 0; g < groups; ++g) {
      V r[W];
      for (std::size_t lane = 0; lane < W; ++lane) {
        r[lane] = B::template load<W>(inputs[lane] + offset + g * W * 4);
      }
      V t[W];
      shuffle_detail::transpose_net<B>(r, t);
      for (std::size_t j = 0; j < W; ++j) {
        m[g * W + j] = B::template to_word<W>(t[j]);
      }
    }
    return true;
  }
}

// The mirror image: 16 wide words out to W lane-major 64-byte blocks.
template <class B, std::size_t W>
BLAKE3PP_FORCE_INLINE bool shuffle_store(const u32v (&w)[16],
                                         std::uint8_t* out,
                                         [[maybe_unused]]
                                         transpose16_mode mode) noexcept {
  using V = typename B::template reg<W>;
  if constexpr (W == 16) {
    if (mode == transpose16_mode::quartered) {
      // Quartered mirror: two in-lane stages, then 128-bit pieces go to
      // their destinations by addressing (extract-stores).
      for (std::size_t q = 0; q < 4; ++q) {
        V r[4];
        for (std::size_t j = 0; j < 4; ++j) {
          r[j] = B::template from_word<W>(w[4 * q + j]);
        }
        V t[4];
        shuffle_detail::inlane_4x4<B>(r, t);
        for (std::size_t k = 0; k < 4; ++k) {
          std::uint8_t quad[64];
          B::template store<W>(quad, t[k]);
          for (std::size_t l = 0; l < 4; ++l) {
            std::memcpy(out + (4 * l + k) * 64 + 16 * q, quad + 16 * l, 16);
          }
        }
      }
      return true;
    }
    if (mode == transpose16_mode::tree) {
      V r[16];
      for (std::size_t j = 0; j < 16; ++j) {
        r[j] = B::template from_word<W>(w[j]);
      }
      V t[16];
      shuffle_detail::transpose_net<B>(r, t);
      for (std::size_t lane = 0; lane < 16; ++lane) {
        B::template store<W>(out + lane * 64, t[lane]);
      }
      return true;
    }
    return false;  // staging
  } else {
    constexpr std::size_t groups = 16 / W;
    for (std::size_t g = 0; g < groups; ++g) {
      V r[W];
      for (std::size_t j = 0; j < W; ++j) {
        r[j] = B::template from_word<W>(w[g * W + j]);
      }
      V t[W];
      shuffle_detail::transpose_net<B>(r, t);
      for (std::size_t lane = 0; lane < W; ++lane) {
        B::template store<W>(out + lane * 64 + g * W * 4, t[lane]);
      }
    }
    return true;
  }
}

#endif  // BLAKE3PP_HAVE_SHUFFLE

}  // namespace transpose_detail

// Fills m[0..15] with the block's message words transposed across W lanes:
// m[j][lane] = word j of inputs[lane] at byte offset `offset`.
template <std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE void load_transposed(const std::uint8_t* const* inputs,
                                           std::size_t offset, u32v m[16],
                                           [[maybe_unused]]
                                           transpose16_mode mode) noexcept {
  namespace td = transpose_detail;
#if defined(BLAKE3PP_HAVE_SHUFFLE)
  if constexpr (shuffle_backend::supports<W>) {
    if (td::shuffle_load<shuffle_backend, W>(inputs, offset, m, mode)) {
      return;
    }
  }
#endif
  std::uint32_t lanes[W];
  for (std::size_t j = 0; j < 16; ++j) {
    for (std::size_t lane = 0; lane < W; ++lane) {
      lanes[lane] = td::ld32(inputs[lane] + offset + 4 * j);
    }
    m[j] = u32v::load(lanes);
  }
}

// Writes 16 wide words lane-major: lane l receives words w[0..15][l] as
// 64 little-endian bytes at out + l*64, the mirror of load_transposed.
template <std::size_t W = u32v::width>
BLAKE3PP_FORCE_INLINE void store_transposed(const u32v (&w)[16],
                                            std::uint8_t* out,
                                            [[maybe_unused]]
                                            transpose16_mode mode) noexcept {
  namespace td = transpose_detail;
#if defined(BLAKE3PP_HAVE_SHUFFLE)
  if constexpr (shuffle_backend::supports<W>) {
    if (td::shuffle_store<shuffle_backend, W>(w, out, mode)) {
      return;
    }
  }
#endif
  std::uint32_t lanes[W];
  for (std::size_t j = 0; j < 16; ++j) {
    w[j].store(lanes);
    for (std::size_t lane = 0; lane < W; ++lane) {
      const std::uint32_t v = lanes[lane];
      std::uint8_t* p = out + lane * 64 + 4 * j;
      p[0] = static_cast<std::uint8_t>(v);
      p[1] = static_cast<std::uint8_t>(v >> 8);
      p[2] = static_cast<std::uint8_t>(v >> 16);
      p[3] = static_cast<std::uint8_t>(v >> 24);
    }
  }
}

}  // namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS


namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS {
namespace {

static_assert(u32v::width <= max_simd_degree,
              "BLAKE3PP_MAX_SIMD_DEGREE is narrower than this kernel's "
              "simd_degree: it sizes the staging buffers this kernel fills");

// Little-endian load/store, spelled memcpy + byteswap rather than the
// byte-wise shift-or idiom: GCC and Clang fold both spellings to a single
// mov on LE targets, but MSVC 19.51 does not recognize the shift-or idiom
// at all: it emitted the four movzx/shl/or per word verbatim, 10
// instructions per message word in the scalar kernel's block loop. The
// memcpy folds to one mov on every compiler; the byteswap arm keeps the
// endian independence the old spelling had.
//
// std::byteswap is C++23; the C++20 presets get the shift-mask spelling,
// which every compiler folds to a single bswap. Feature-tested rather
// than __cplusplus-gated, and needed even though the call sits in a
// discarded if-constexpr branch: non-dependent names in discarded
// branches must still exist (found by the C++20 CI leg, not by review).
BLAKE3PP_FORCE_INLINE constexpr std::uint32_t bswap32(std::uint32_t v) noexcept {
#if defined(__cpp_lib_byteswap)
  return std::byteswap(v);
#else
  return (v >> 24) | ((v >> 8) & 0x0000ff00u) | ((v << 8) & 0x00ff0000u) |
         (v << 24);
#endif
}

// The clang that zig 0.16 bundles, 21.1.0, merges the byte-wise word
// loads of the message below into element-width vector loads reading the
// caller's pointer directly. Where a misaligned vector access faults that
// turns a legal load into a bus error: RISC-V lets an implementation
// refuse such an access and the SpacemiT X60 (RVV 1.0) does, while x86
// and AArch64 perform them in hardware.
//
// Measured on one preprocessed source, counting loads whose base is the
// block parameter in compress_in_place: zig 0.16 (clang 21.1.0) three,
// Ubuntu clang 21.1.8 none, zig 0.17-dev (clang 22.1.8) none, Ubuntu
// clang 22.1.8 none, GCC 15 none. So a 21.1.x patch release or zig's own
// build of it is the dividing line, and the version bound below is
// deliberately conservative: it also covers clang 21 builds that do not
// need it, and lifts itself when the musl builds move to zig 0.17.
#if defined(__riscv) && defined(__riscv_v) && defined(__clang__) && __clang_major__ < 22
#define BLAKE3PP_ALIGN_MESSAGE_BLOCK 1
#else
#define BLAKE3PP_ALIGN_MESSAGE_BLOCK 0
#endif

BLAKE3PP_FORCE_INLINE std::uint32_t load32(const std::uint8_t* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof v);
  if constexpr (std::endian::native == std::endian::big) {
    v = bswap32(v);
  }
  return v;
}

BLAKE3PP_FORCE_INLINE void store32(std::uint8_t* p, std::uint32_t v) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    v = bswap32(v);
  }
  std::memcpy(p, &v, sizeof v);
}

template <int N>
BLAKE3PP_FORCE_INLINE std::uint32_t rot(std::uint32_t x) noexcept {
  return std::rotr(x, N);
}

// Scalar twin of the wide xor_rot in transpose.hpp: the rounds are written
// against the fused form so SVE2's XAR can claim it; everywhere else the
// compilers fold this back to exactly the old eor + ror pair.
template <int N>
BLAKE3PP_FORCE_INLINE std::uint32_t xor_rot(std::uint32_t x,
                                            std::uint32_t y) noexcept {
  return std::rotr(x ^ y, N);
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
  // See BLAKE3PP_ALIGN_MESSAGE_BLOCK: where the merged load would fault,
  // an unaligned block is copied once so that the address really is
  // aligned. Staging it unconditionally does not work, since the
  // optimizer forwards the copy and rebuilds the loads from the original
  // pointer; the aligned case, which is the common one, pays one
  // predictable branch and no copy.
  const std::uint8_t* src = block;
#if BLAKE3PP_ALIGN_MESSAGE_BLOCK
  alignas(std::uint32_t) std::uint8_t staged[block_len];
  if ((reinterpret_cast<std::uintptr_t>(src) &
       (alignof(std::uint32_t) - 1)) != 0) {
    std::memcpy(staged, src, block_len);
    src = staged;
  }
  src = static_cast<const std::uint8_t*>(
      __builtin_assume_aligned(src, alignof(std::uint32_t)));
#endif

  std::uint32_t m[16];
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
  store_transposed(wide, out, transpose_detail::t16_mode());
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

  // Read the W==16 transpose dial ONCE per batch, not once per block. MSVC
  // emits std::atomic<transpose16_mode>::load out of line, and a call in
  // the block loop is an optimisation barrier that spills the whole wide
  // state every iteration, measured as the entire AVX-512 width advantage.
  const transpose16_mode t16 = transpose_detail::t16_mode();

  for (std::size_t b = 0; b < blocks; ++b) {
    std::uint32_t block_flags = flags;
    if (b == 0) {
      block_flags |= flags_start;
    }
    if (b == blocks - 1) {
      block_flags |= flags_end;
    }

    u32v m[16];
    load_transposed(inputs, b * block_len, m, t16);

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
    arch::BLAKE3PP_AMALGAM_NS,
    /*simd_degree=*/u32v::width,
    &compress_in_place,
    &compress_xof,
    &xof_many,
    &hash_many,
};

}  // namespace blake3pp::kern::BLAKE3PP_AMALGAM_NS

#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>



// Arch-agnostic BLAKE3 structure: chunk states, the chaining-value stack
// discipline, parent nodes, and root finalization. All compression is routed
// through a kernel_ops table so the same logic drives every architecture
// variant. Header-only and allocation-free; the parallel engine (M3) reuses
// these pieces for subtree hashing.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>




namespace blake3pp::core {

// A node whose chaining value has not been computed yet. Keeping the inputs
// around (rather than eagerly compressing) is what makes the ROOT flag
// possible: the last node's compression must wait until we know it is last.
struct output {
  std::array<std::uint32_t, 8> input_cv;
  std::array<std::uint8_t, kern::block_len> block;
  std::uint32_t block_len;
  std::uint64_t counter;
  std::uint32_t flags;
};

inline void chaining_value(const kern::kernel_ops& k, const output& o,
                           std::array<std::uint32_t, 8>& out_cv) noexcept {
  out_cv = o.input_cv;
  k.compress_in_place(out_cv.data(), o.block.data(), o.block_len, o.counter,
                      o.flags);
}

inline void chunk_init(detail::chunk_state& cs,
                       std::span<const std::uint32_t, 8> key,
                       std::uint64_t chunk_counter) noexcept {
  std::ranges::copy(key, cs.cv.begin());
  cs.chunk_counter = chunk_counter;
  cs.block.fill(0);
  cs.block_len = 0;
  cs.blocks_compressed = 0;
}

inline std::size_t chunk_len(const detail::chunk_state& cs) noexcept {
  return kern::block_len * cs.blocks_compressed + cs.block_len;
}

inline std::uint32_t chunk_start_flag(const detail::chunk_state& cs) noexcept {
  return cs.blocks_compressed == 0 ? kern::flag_chunk_start : 0;
}

// Feeds up to (chunk_len - len) bytes; caller ensures the chunk has room.
// base_flags carries the hashing mode (0, KEYED_HASH, DERIVE_KEY_*).
inline void chunk_update(const kern::kernel_ops& k, detail::chunk_state& cs,
                         const std::uint8_t* input, std::size_t len,
                         std::uint32_t base_flags) noexcept {
  while (len > 0) {
    // A full buffered block is only compressed once more input shows up: if
    // it turned out to be the chunk's last block it needs CHUNK_END later.
    if (cs.block_len == kern::block_len) {
      k.compress_in_place(cs.cv.data(), cs.block.data(), kern::block_len,
                          cs.chunk_counter, chunk_start_flag(cs) | base_flags);
      cs.blocks_compressed++;
      cs.block_len = 0;
      cs.block.fill(0);
    }
    const std::size_t take =
        len < kern::block_len - cs.block_len ? len
                                             : kern::block_len - cs.block_len;
    std::copy_n(input, take, cs.block.begin() + cs.block_len);
    cs.block_len = static_cast<std::uint8_t>(cs.block_len + take);
    input += take;
    len -= take;
  }
}

inline output chunk_output(const detail::chunk_state& cs,
                           std::uint32_t base_flags) noexcept {
  output o;
  o.input_cv = cs.cv;
  o.block = cs.block;
  o.block_len = cs.block_len;
  o.counter = cs.chunk_counter;
  o.flags = chunk_start_flag(cs) | kern::flag_chunk_end | base_flags;
  return o;
}

// Parent nodes always compress one full block (left CV || right CV) with
// counter 0 (spec section 2.4).
inline output parent_output(std::span<const std::uint32_t, 8> left_cv,
                            std::span<const std::uint32_t, 8> right_cv,
                            std::span<const std::uint32_t, 8> key,
                            std::uint32_t base_flags) noexcept {
  output o;
  std::ranges::copy(key, o.input_cv.begin());
  for (std::size_t w = 0; w < 8; ++w) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      o.block[4 * w + byte] =
          static_cast<std::uint8_t>(left_cv[w] >> (8 * byte));
      o.block[32 + 4 * w + byte] =
          static_cast<std::uint8_t>(right_cv[w] >> (8 * byte));
    }
  }
  o.block_len = kern::block_len;
  o.counter = 0;
  o.flags = kern::flag_parent | base_flags;
  return o;
}

}  // namespace blake3pp::core



// Wide subtree compression: reduces a chunk-aligned power-of-2 subtree to a
// single chaining value with BOTH levels of work batched through hash_many:
// chunks across SIMD lanes, and parent nodes across SIMD lanes too. This is
// what closes the gap to hand-tuned implementations: with parents compressed
// one at a time (scalar), a binary tree spends ~1 scalar block per chunk on
// interior nodes, a measured ~1.5x drag at AVX2 chunk speeds.
//
// Everything here is allocation-free. The recursion holds one
// 4*max_simd_degree CV buffer per level (2 KiB while a 16-wide kernel is
// compiled in), sized for the widest variant compiled rather than the one
// that runs, and it stops at twice the RUNNING variant's degree, so a
// scalar kernel recurses deepest: a 2^54-chunk maximum-size subtree would
// take over 100 KiB of stack. The parallel engine's stack_budget bounds its
// own part table, not this. M3's parallel engine reuses these pieces per
// subtree.

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>



namespace blake3pp::core {

// One generation of parents: pairs of child CVs (contiguous, LE bytes)
// become one-block parent nodes, hashed lanes-wide. An odd trailing child
// passes through unchanged (spec 2.4). Returns the new generation's count.
inline std::size_t compress_parents_wide(const kern::kernel_ops& k,
                                         const std::uint8_t* child_cvs,
                                         std::size_t num_children,
                                         std::span<const std::uint32_t, 8> key,
                                         std::uint32_t base_flags,
                                         std::uint8_t* out) noexcept {
  const std::size_t num_parents = num_children / 2;
  const std::uint8_t* parent_blocks[kern::max_batch_inputs];
  for (std::size_t i = 0; i < num_parents; ++i) {
    parent_blocks[i] = child_cvs + 2 * i * kern::out_len;
  }
  k.hash_many(parent_blocks, num_parents, 1, key.data(), 0,
              /*increment_counter=*/false, kern::flag_parent | base_flags, 0,
              0, out);
  if (num_children % 2 != 0) {
    // kern::out_len is a compile-time constant; a counted loop keeps this
    // inline on MSVC, which turns the algorithm call into memcpy.
    const std::uint8_t* odd = child_cvs + (num_children - 1) * kern::out_len;
    std::uint8_t* dst = out + num_parents * kern::out_len;
    for (std::size_t i = 0; i < kern::out_len; ++i) {
      dst[i] = odd[i];
    }
    return num_parents + 1;
  }
  return num_parents;
}

// num_chunks is a power of two; input holds num_chunks complete chunks.
// Returns min(num_chunks, 2 * simd_degree) CVs in out_cvs. The leaf spans
// TWO SIMD batches so hash_many can run its dual-batch interleaved path.
inline std::size_t compress_subtree_wide(const kern::kernel_ops& k,
                                         const std::uint8_t* input,
                                         std::size_t num_chunks,
                                         std::uint64_t chunk_counter,
                                         std::span<const std::uint32_t, 8> key,
                                         std::uint32_t base_flags,
                                         std::uint8_t* out_cvs) noexcept {
  if (num_chunks <= 2 * k.simd_degree) {
    const std::uint8_t* chunks[kern::max_batch_inputs];
    for (std::size_t i = 0; i < num_chunks; ++i) {
      chunks[i] = input + i * kern::chunk_len;
    }
    k.hash_many(chunks, num_chunks, kern::chunk_len / kern::block_len,
                key.data(),
                chunk_counter, /*increment_counter=*/true, base_flags,
                kern::flag_chunk_start, kern::flag_chunk_end, out_cvs);
    return num_chunks;
  }

  const std::size_t half = num_chunks / 2;
  std::uint8_t child_cvs[2 * kern::max_batch_inputs * kern::out_len];
  const std::size_t nl = compress_subtree_wide(k, input, half, chunk_counter,
                                               key, base_flags, child_cvs);
  const std::size_t nr = compress_subtree_wide(
      k, input + half * kern::chunk_len, half, chunk_counter + half, key,
      base_flags, child_cvs + nl * kern::out_len);
  return compress_parents_wide(k, child_cvs, nl + nr, key, base_flags,
                               out_cvs);
}

// Full reduction of a power-of-2 subtree (>= 2 chunks) to one CV. The final
// log2(degree) generations run below full lane occupancy, but that tail is
// O(log degree) blocks per subtree and amortizes to noise.
inline void compress_subtree_to_cv_recursive(
    const kern::kernel_ops& k, const std::uint8_t* input,
    std::size_t num_chunks, std::uint64_t chunk_counter,
    std::span<const std::uint32_t, 8> key, std::uint32_t base_flags,
    std::span<std::uint32_t, 8> out_cv) noexcept {
  std::uint8_t cvs[kern::max_batch_inputs * kern::out_len];
  std::uint8_t next[kern::max_batch_inputs * kern::out_len];
  std::size_t n = compress_subtree_wide(k, input, num_chunks, chunk_counter,
                                        key, base_flags, cvs);
  while (n > 1) {
    n = compress_parents_wide(k, cvs, n, key, base_flags, next);
    std::copy_n(next, n * kern::out_len, cvs);
  }
  for (std::size_t w = 0; w < 8; ++w) {
    const std::uint8_t* b = cvs + 4 * w;
    out_cv[w] = static_cast<std::uint32_t>(b[0]) |
                (static_cast<std::uint32_t>(b[1]) << 8) |
                (static_cast<std::uint32_t>(b[2]) << 16) |
                (static_cast<std::uint32_t>(b[3]) << 24);
  }
}

// The same reduction, folding as it goes instead of holding the tree: one
// group of 2*simd_degree chunks at a time through hash_many, reduced to one
// CV by wide parent passes, then merged into a binary-counter stack (the
// shape hasher::push_cv uses, src/blake3pp.cpp). Parents stay batched across
// lanes inside a group; only the one parent that joins a group to the stack
// is compressed alone, once per 2*simd_degree chunks. The working set is two
// group buffers plus the stack, so it does not grow with the subtree's
// depth, where the recursion above costs one buffer per level.
//
// MaxStack bounds the stack in CVs and so the subtree this can reduce:
// log2(num_chunks / group) + 1 entries are needed, 54 covering the largest
// subtree BLAKE3 defines. It is also the working set, so a target picks the
// smallest bound its parts need.
//
// Measured against the recursion (tests/subtree_fold.cpp pins the outputs
// equal): the same 1023 parent blocks for a 1024-chunk subtree, but spread
// over 319 hash_many calls instead of 67 on avx2, none of them at full lane
// occupancy, which costs 11% on sse42 and avx2. On a scalar kernel there is
// no occupancy to lose and throughput is unchanged, which is why this is
// opt-in for narrow targets (BLAKE3PP_SUBTREE_FOLD) rather than a default:
// there it replaces one buffer per level with a fixed working set, 720 bytes
// at 12 levels against 480 + 272 per level.
template <std::size_t MaxStack = 54>
inline void compress_subtree_to_cv_folded(
    const kern::kernel_ops& k, const std::uint8_t* input,
    std::size_t num_chunks, std::uint64_t chunk_counter,
    std::span<const std::uint32_t, 8> key, std::uint32_t base_flags,
    std::span<std::uint32_t, 8> out_cv) noexcept {
  std::uint8_t cvs[kern::max_batch_inputs * kern::out_len];
  std::uint8_t next[kern::max_batch_inputs * kern::out_len];
  std::uint8_t stack[MaxStack * kern::out_len];
  std::size_t depth = 0;

  // Both are powers of two, so every group is a subtree-aligned unit and the
  // last group is full whenever the subtree spans more than one.
  const std::size_t group = 2 * k.simd_degree;
  std::uint64_t groups = 0;
  for (std::size_t done = 0; done < num_chunks; done += group) {
    const std::size_t n = std::min(group, num_chunks - done);
    const std::uint8_t* chunks[kern::max_batch_inputs];
    for (std::size_t i = 0; i < n; ++i) {
      chunks[i] = input + (done + i) * kern::chunk_len;
    }
    k.hash_many(chunks, n, kern::chunk_len / kern::block_len, key.data(),
                chunk_counter + done, /*increment_counter=*/true, base_flags,
                kern::flag_chunk_start, kern::flag_chunk_end, cvs);
    // The group's own parents, lanes-wide, down to its single root CV.
    for (std::size_t m = n; m > 1;) {
      m = compress_parents_wide(k, cvs, m, key, base_flags, next);
      std::copy_n(next, m * kern::out_len, cvs);
    }
    assert(depth < MaxStack);
    std::copy_n(cvs, kern::out_len, stack + depth * kern::out_len);
    depth++;
    // Two subtrees of equal size on top merge; as many times as the group
    // count has trailing zeros, which is the binary counter's carry.
    ++groups;
    for (int carries = std::countr_zero(groups); carries > 0; --carries) {
      assert(depth >= 2);
      depth -= 2;
      compress_parents_wide(k, stack + depth * kern::out_len, 2, key,
                            base_flags, next);
      std::copy_n(next, kern::out_len, stack + depth * kern::out_len);
      depth++;
    }
  }
  assert(depth == 1);
  for (std::size_t w = 0; w < 8; ++w) {
    const std::uint8_t* b = stack + 4 * w;
    out_cv[w] = static_cast<std::uint32_t>(b[0]) |
                (static_cast<std::uint32_t>(b[1]) << 8) |
                (static_cast<std::uint32_t>(b[2]) << 16) |
                (static_cast<std::uint32_t>(b[3]) << 24);
  }
}

// Which of the two the library uses: the recursion unless a build opts into
// the fold, which only narrow targets should. Both stay compiled so the
// tests can compare them on every machine.
inline void compress_subtree_to_cv(const kern::kernel_ops& k,
                                   const std::uint8_t* input,
                                   std::size_t num_chunks,
                                   std::uint64_t chunk_counter,
                                   std::span<const std::uint32_t, 8> key,
                                   std::uint32_t base_flags,
                                   std::span<std::uint32_t, 8> out_cv) noexcept {
#if defined(BLAKE3PP_SUBTREE_FOLD)
  // The macro's value is the stack bound in levels; see ArchKernels.cmake.
  compress_subtree_to_cv_folded<BLAKE3PP_SUBTREE_FOLD>(
      k, input, num_chunks, chunk_counter, key, base_flags, out_cv);
#else
  compress_subtree_to_cv_recursive(k, input, num_chunks, chunk_counter, key,
                                   base_flags, out_cv);
#endif
}

}  // namespace blake3pp::core



namespace blake3pp {

namespace {

// BLAKE3 keys enter as 32 little-endian bytes and live as 8 words.
std::array<std::uint32_t, 8> key_bytes_to_words(
    std::span<const std::byte, 32> key) noexcept {
  std::array<std::uint32_t, 8> words;
  for (std::size_t w = 0; w < 8; ++w) {
    const auto b = [&](std::size_t i) {
      return std::to_integer<std::uint32_t>(key[4 * w + i]);
    };
    words[w] = b(0) | (b(1) << 8) | (b(2) << 16) | (b(3) << 24);
  }
  return words;
}

}  // namespace

hasher::hasher(arch a) noexcept : hasher(detail::resolve(a)) {}

hasher::hasher(const kern::kernel_ops* custom_ops) noexcept
    : hasher(custom_ops, kern::iv, 0) {}

hasher::hasher(const kern::kernel_ops* ops,
               std::span<const std::uint32_t, 8> key,
               std::uint32_t base_flags) noexcept
    : ops_(ops), base_flags_(base_flags), cv_stack_len_(0) {
  std::ranges::copy(key, key_words_.begin());
  core::chunk_init(chunk_, key_words_, 0);
}

hasher hasher::keyed(std::span<const std::byte, 32> key, arch a) noexcept {
  return keyed(key, detail::resolve(a));
}

hasher hasher::keyed(std::span<const std::byte, 32> key,
                     const kern::kernel_ops* ops) noexcept {
  return hasher(ops, key_bytes_to_words(key), kern::flag_keyed_hash);
}

hasher hasher::derive_key(std::string_view context, arch a) noexcept {
  return derive_key(context, detail::resolve(a));
}

hasher hasher::derive_key(std::string_view context,
                          const kern::kernel_ops* ops) noexcept {
  // Stage 1: hash the context string in DERIVE_KEY_CONTEXT mode...
  hasher ctx(ops, kern::iv, kern::flag_derive_key_context);
  ctx.update(context);
  const digest context_key = ctx.finalize();
  // ...stage 2: the returned hasher consumes key material keyed by it.
  return hasher(ops, key_bytes_to_words(context_key.bytes),
                kern::flag_derive_key_material);
}

void hasher::reset() noexcept {
  core::chunk_init(chunk_, key_words_, 0);
  cv_stack_len_ = 0;
}

arch hasher::selected_arch() const noexcept { return ops_->variant; }

std::uint64_t hasher::count() const noexcept {
  return chunk_.chunk_counter * kern::chunk_len + core::chunk_len(chunk_);
}

// Merge completed subtrees, then push. The pushed CV may itself be the root
// of a subtree_chunks-sized (power-of-2, aligned) subtree: counting in
// subtree units, each trailing zero bit of total_chunks/subtree_chunks is a
// full sibling subtree waiting on the stack (spec 5.1.2). The invariant
// afterwards is stack_len == popcount(total_chunks).
void hasher::push_cv(std::span<const std::uint32_t, 8> cv,
                     std::uint64_t total_chunks,
                     std::uint64_t subtree_chunks) noexcept {
  // push_subtree_cv is an expert entry point whose preconditions are
  // documented but unenforceable at run time without cost. Violating them
  // is not a graceful failure: subtree_chunks == 0 divides by zero here,
  // and a misaligned or non-power-of-two size produces more trailing zero
  // bits than there are stacked siblings, wrapping the uint8_t length to
  // 255 and indexing a 54-entry array out of bounds. Debug builds say so.
  assert(subtree_chunks > 0 && std::has_single_bit(subtree_chunks));
  assert(total_chunks % subtree_chunks == 0);
  std::array<std::uint32_t, 8> new_cv;
  // Counted loop rather than ranges::copy: the extent is 8 and MSVC still
  // lowers the algorithm to an out-of-line memmove call here.
  for (std::size_t i = 0; i < 8; ++i) {
    new_cv[i] = cv[i];
  }
  std::uint64_t chunks = total_chunks / subtree_chunks;
  while ((chunks & 1) == 0) {
    assert(cv_stack_len_ > 0);  // a sibling must be waiting for each merge
    cv_stack_len_--;
    core::chaining_value(*ops_,
                         core::parent_output(cv_stack_[cv_stack_len_], new_cv,
                                             key_words_, base_flags_),
                         new_cv);
    chunks >>= 1;
  }
  assert(cv_stack_len_ < cv_stack_.size());
  cv_stack_[cv_stack_len_] = new_cv;
  cv_stack_len_++;
}

// A full chunk is only closed out when more input arrives: the final
// chunk of the message must stay open for possible ROOT finalization. So
// "on a chunk boundary" has two shapes, an empty chunk state or a full
// one, and every path that appends after the boundary (update() and
// push_subtree_cv()) starts by collapsing the second into the first.
void hasher::close_full_chunk() noexcept {
  if (core::chunk_len(chunk_) == kern::chunk_len) {
    std::array<std::uint32_t, 8> chunk_cv;
    core::chaining_value(*ops_, core::chunk_output(chunk_, base_flags_),
                         chunk_cv);
    const std::uint64_t total_chunks = chunk_.chunk_counter + 1;
    push_cv(chunk_cv, total_chunks, 1);
    core::chunk_init(chunk_, key_words_, total_chunks);
  }
}

void hasher::update(std::span<const std::byte> input) noexcept {
  const auto* p = reinterpret_cast<const std::uint8_t*>(input.data());
  std::size_t len = input.size();

  while (len > 0) {
    close_full_chunk();

    // Subtree fast path: aligned on a chunk boundary with more than one
    // whole chunk ahead, reduce the largest power-of-2, position-aligned
    // subtree in one wide pass (chunks AND parents lanes-parallel). The
    // strict > keeps at least one byte in reserve, so no offloaded chunk
    // can turn out to be the message's last (which would need
    // CHUNK_END-with-ROOT handling).
    if (core::chunk_len(chunk_) == 0 && len > kern::chunk_len) {
      const std::size_t safe_chunks = (len - 1) / kern::chunk_len;
      std::size_t subtree = std::bit_floor(safe_chunks);
      // A subtree merged as one CV must sit on a subtree-aligned position
      // in the overall tree.
      while (((subtree - 1) & chunk_.chunk_counter) != 0) {
        subtree /= 2;
      }
      if (subtree >= 2) {
        std::array<std::uint32_t, 8> cv;
        core::compress_subtree_to_cv(*ops_, p, subtree, chunk_.chunk_counter,
                                     key_words_, base_flags_, cv);
        push_cv(cv, chunk_.chunk_counter + subtree, subtree);
        chunk_.chunk_counter += subtree;
        p += subtree * kern::chunk_len;
        len -= subtree * kern::chunk_len;
        continue;
      }
      // A single (or misaligned) chunk falls through to the buffered path.
    }

    const std::size_t room = kern::chunk_len - core::chunk_len(chunk_);
    const std::size_t take = len < room ? len : room;
    core::chunk_update(*ops_, chunk_, p, take, base_flags_);
    p += take;
    len -= take;
  }
}

void hasher::update(std::string_view input) noexcept {
  update(std::as_bytes(std::span{input.data(), input.size()}));
}

output_reader hasher::finalize_xof() const noexcept {
  // Collapse the stack from the top down, then capture the ROOT node
  // instead of compressing it: the reader re-compresses it with varying
  // output counters. No member state is modified, so finalization can
  // happen at any point and hashing may continue afterwards.
  core::output o = core::chunk_output(chunk_, base_flags_);
  std::size_t parents = cv_stack_len_;
  while (parents > 0) {
    parents--;
    std::array<std::uint32_t, 8> right_cv;
    core::chaining_value(*ops_, o, right_cv);
    o = core::parent_output(cv_stack_[parents], right_cv, key_words_,
                            base_flags_);
  }

  output_reader r;
  r.ops_ = ops_;
  r.input_cv_ = o.input_cv;
  r.block_ = o.block;
  r.block_len_ = o.block_len;
  r.flags_ = o.flags | kern::flag_root;
  return r;
}

// The 32-byte digest is, by definition, the first 32 bytes of the output
// stream.
digest hasher::finalize() const noexcept {
  digest d;
  finalize_xof().fill(std::span<std::byte>{d.bytes});
  return d;
}

void hasher::finalize(std::span<std::byte> out) const noexcept {
  finalize_xof().fill(out);
}

void output_reader::fill(std::span<std::byte> out) noexcept {
  std::size_t done = 0;
  while (done < out.size()) {
    const std::uint64_t block_index = position_ / kern::block_len;
    const std::size_t in_block =
        static_cast<std::size_t>(position_ % kern::block_len);
    // Block-aligned bulk of the request goes lanes-wide: output counters
    // map to SIMD lanes, so long fills run at hash_many-class speed.
    if (in_block == 0 && out.size() - done >= kern::block_len) {
      const std::size_t nblocks = (out.size() - done) / kern::block_len;
      ops_->xof_many(input_cv_.data(), block_.data(), block_len_,
                     block_index, flags_,
                     reinterpret_cast<std::uint8_t*>(out.data() + done),
                     nblocks);
      done += nblocks * kern::block_len;
      position_ += nblocks * kern::block_len;
      continue;
    }
    if (!cache_valid_ || cached_block_ != block_index) {
      // uint8_t view of the byte cache at the flat kernel ABI boundary.
      ops_->compress_xof(input_cv_.data(), block_.data(), block_len_,
                         block_index, flags_,
                         reinterpret_cast<std::uint8_t*>(cache_.data()));
      cached_block_ = block_index;
      cache_valid_ = true;
    }
    // Explicit std::size_t: kern::block_len is uint32_t, so on a 32-bit
    // target the two arguments deduce to different types and the call is
    // ambiguous.  Both values are bounded by out.size() and 64.
    const std::size_t take =
        std::min<std::size_t>(out.size() - done, kern::block_len - in_block);
    std::copy_n(cache_.data() + in_block, take, out.data() + done);
    done += take;
    position_ += take;
  }
}

void hasher::push_subtree_cv(std::span<const std::uint32_t, 8> cv,
                             std::uint64_t subtree_chunks) noexcept {
  close_full_chunk();
  push_cv(cv, chunk_.chunk_counter + subtree_chunks, subtree_chunks);
  chunk_.chunk_counter += subtree_chunks;
}

namespace detail {
void compress_subtree_cv(const kern::kernel_ops* ops, const std::byte* data,
                         std::size_t num_chunks, std::uint64_t chunk_counter,
                         std::span<const std::uint32_t, 8> key,
                         std::uint32_t base_flags,
                         std::span<std::uint32_t, 8> out_cv) noexcept {
  core::compress_subtree_to_cv(*ops,
                               reinterpret_cast<const std::uint8_t*>(data),
                               num_chunks, chunk_counter, key, base_flags,
                               out_cv);
}
}  // namespace detail

digest hash(std::span<const std::byte> input) noexcept {
  hasher h;
  h.update(input);
  return h.finalize();
}

digest hash(std::string_view input) noexcept {
  hasher h;
  h.update(input);
  return h.finalize();
}

digest keyed_hash(std::span<const std::byte, 32> key,
                  std::span<const std::byte> input) noexcept {
  hasher h = hasher::keyed(key);
  h.update(input);
  return h.finalize();
}

digest keyed_hash(std::span<const std::byte, 32> key,
                  std::string_view input) noexcept {
  return keyed_hash(key, std::as_bytes(std::span{input.data(), input.size()}));
}

digest derive_key(std::string_view context,
                  std::span<const std::byte> key_material) noexcept {
  hasher h = hasher::derive_key(context);
  h.update(key_material);
  return h.finalize();
}

digest derive_key(std::string_view context,
                  std::string_view key_material) noexcept {
  return derive_key(context, std::as_bytes(std::span{key_material.data(),
                                                     key_material.size()}));
}

std::optional<digest> digest::from_hex(std::string_view hex) noexcept {
  if (hex.size() != 64) {
    return std::nullopt;
  }
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  digest d;
  for (std::size_t i = 0; i < 32; ++i) {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    d.bytes[i] = static_cast<std::byte>((hi << 4) | lo);
  }
  return d;
}

bool digest::matches(std::string_view hex) const noexcept {
  return from_hex(hex) == *this;  // optional's heterogeneous, CT inner ==
}

void to_hex(std::span<const std::byte> bytes, std::span<char> out) noexcept {
  static constexpr char alphabet[] = "0123456789abcdef";
  assert(out.size() >= 2 * bytes.size());
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const auto v = std::to_integer<unsigned>(bytes[i]);
    out[2 * i] = alphabet[v >> 4];
    out[2 * i + 1] = alphabet[v & 0xF];
  }
}

std::string to_hex(std::span<const std::byte> bytes) {
  std::string s(2 * bytes.size(), '\0');
  to_hex(bytes, s);
  return s;
}

std::array<char, 65> digest::to_hex_chars() const noexcept {
  std::array<char, 65> out;
  blake3pp::to_hex(bytes, out);
  out[64] = '\0';
  return out;
}

std::string digest::to_hex() const { return blake3pp::to_hex(bytes); }

bool operator==(const digest& lhs, const digest& rhs) noexcept {
  // Accumulate the whole difference before deciding: no data-dependent
  // early exit, so comparison time is independent of where bytes differ.
  unsigned acc = 0;
  for (std::size_t i = 0; i < lhs.bytes.size(); ++i) {
    acc |= std::to_integer<unsigned>(lhs.bytes[i] ^ rhs.bytes[i]);
  }
  return acc == 0;
}

}  // namespace blake3pp

// Runtime architecture routing. Selection stays a plain pointer to a
// constexpr-initialized POD table: no heap, no vtable, no ifunc.
//
// Nothing here is hand-maintained in parallel with anything else:
//  * WHICH variants exist in this binary comes from the build-generated
//    blake3pp_kernel_registry.inc (one entry per blake3pp_add_kernel()
//    call), expanded below into extern declarations and the registry.
//  * WHAT variants exist at all, their names, and the preference ranking
//    come from include/blake3pp/detail/arch.def, the same list the
//    public enum is generated from.
//  * WHETHER this CPU can run one lives in the per-platform probe TUs,
//    cpu_detect_{x86,arm,riscv}.cpp, behind cpu_detect.hpp.
// Register a kernel in CMake, add its line to arch.def, answer for it in
// the right probe: each fact is stated exactly once.


#define BLAKE3PP_STAMPED_VERSION "9c43def-dirty-amalgamated"


#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>



// The seam between generic dispatch and per-platform CPU capability
// probing. Exactly one cpu_detect_<platform>.cpp defines
// platform_cpu_supports() per target (the others compile empty); targets
// with no probe file at all (wasm) simply never define
// BLAKE3PP_HAS_CPU_DETECT and dispatch.cpp answers without it.
//
// THE RULE every probe TU inherits: these files are compiled with the
// library's baseline flags and must stay FLAG-NEUTRAL: no intrinsics
// that require an -m/-march above the baseline, no
// __builtin_cpu_supports (it drags compiler-runtime machinery into the
// link that lld-link and musl static linking do not have). Probing is
// syscalls, CPUID-style instructions available at baseline, and
// carefully scoped inline asm only.


#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || \
    defined(_M_IX86)
#define BLAKE3PP_CPU_DETECT_X86 1
#define BLAKE3PP_HAS_CPU_DETECT 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define BLAKE3PP_CPU_DETECT_ARM 1
#define BLAKE3PP_HAS_CPU_DETECT 1
#elif defined(__riscv) && __riscv_xlen == 64
#define BLAKE3PP_CPU_DETECT_RISCV 1
#define BLAKE3PP_HAS_CPU_DETECT 1
#elif defined(__powerpc64__)
#define BLAKE3PP_CPU_DETECT_PPC 1
#define BLAKE3PP_HAS_CPU_DETECT 1
#elif defined(__s390x__)
#define BLAKE3PP_CPU_DETECT_S390 1
#define BLAKE3PP_HAS_CPU_DETECT 1
#elif defined(__mips__)
#define BLAKE3PP_CPU_DETECT_MIPS 1
#define BLAKE3PP_HAS_CPU_DETECT 1
#endif

#if defined(BLAKE3PP_TEST_PROBE_SHAPES)
#include <cstdint>
// The riscv detection ladder's test seam (see cpu_detect_riscv.cpp): a
// machine description standing in for HWCAP and hwprobe, compiled only
// into tests/riscv_probe_shapes.
namespace blake3pp::detail::test {
struct machine {
  bool active = false;
  bool hwcap_v = false;
  bool hwprobe = true;                   // false: pre-6.4 kernel (ENOSYS)
  bool vendor_key = false;               // hwprobe knows VENDOR_EXT_THEAD_0
  std::uint64_t vendor_ext_thead_0 = 0;
  std::uint64_t ima_ext_0 = 0;
  std::uint64_t mvendorid = 0;
  bool force_trap = false;               // the guarded probes hit an illegal instruction
  bool dialect_071 = false;              // the vsetvli probe answers as 0.7.1 hardware
};
void set_machine(const machine& m) noexcept;
}  // namespace blake3pp::detail::test
#endif

namespace blake3pp::detail {

#if defined(BLAKE3PP_HAS_CPU_DETECT)
// Can the running CPU execute this variant? Only the platform's own
// enumerators need answering; dispatch.cpp resolves auto_detect, scalar
// and simd128 before calling here, and anything foreign returns false.
[[nodiscard]] bool platform_cpu_supports(arch a) noexcept;

// Backs blake3pp::run_trap_probes(): runs any detection rungs the
// platform deferred because they need a trap-guarded probe (a scoped
// signal-handler swap), and upgrades the state platform_cpu_supports()
// reads. Thread-safe and idempotent; returns whether anything new was
// learned. Every platform TU defines it; only riscv64 has deferred
// rungs today, the rest return false.
bool platform_run_trap_probes() noexcept;
#endif

}  // namespace blake3pp::detail



namespace blake3pp {

namespace kern {
#define BLAKE3PP_KERNEL(ns) \
  namespace ns {            \
  extern const kernel_ops ops; \
  }
BLAKE3PP_KERNEL(scalar)
#ifdef BLAKE3PP_AMALGAM_NS
BLAKE3PP_KERNEL(BLAKE3PP_AMALGAM_NS)
#endif

#undef BLAKE3PP_KERNEL
}  // namespace kern

namespace {

struct registry_entry {
  arch a;
  const kern::kernel_ops* ops;
};

constexpr registry_entry registry[] = {
#define BLAKE3PP_KERNEL(ns) {arch::ns, &kern::ns::ops},
BLAKE3PP_KERNEL(scalar)
#ifdef BLAKE3PP_AMALGAM_NS
BLAKE3PP_KERNEL(BLAKE3PP_AMALGAM_NS)
#endif

#undef BLAKE3PP_KERNEL
};

constexpr std::size_t num_kernels = std::size(registry);

// The arch.def rows: enum value, canonical name, preference rank.
struct arch_row {
  arch a;
  const char* name;
  unsigned rank;
};

constexpr arch_row arch_table[] = {
#define BLAKE3PP_ARCH(enumerator, name, rank) {arch::enumerator, name, rank},
// The one canonical list of instruction-set variants: the single source
// the enum, the name strings, and the dispatch preference order are all
// generated from (same include-me-under-a-macro pattern as the build-time
// kernel registry). Consumers define BLAKE3PP_ARCH(enumerator, "name",
// rank) and include this file; no include guard on purpose.
//
// Two orders live here, deliberately separated:
//   * LINE ORDER is ABI order. The enum's underlying values follow it, so
//     entries are APPEND-ONLY: never reorder, never insert, never remove.
//   * RANK is the dispatch preference, lower wins, unique (asserted).
//     auto_detect must hold the lowest rank; scalar the highest. Ranks are
//     spaced by 10 so a measured reordering is a one-number edit with no
//     ABI consequence.
//
// The enumerator doubles as the kernel namespace token and the CMake
// registration name (see cmake/ArchKernels.cmake); they must all agree.
// The display name is separate only where convention demands it ("auto").
// The /// comments are the enumerators' documentation (they attach to the
// enum's members through the macro expansion).

/// Resolves to the best usable variant at runtime.
BLAKE3PP_ARCH(auto_detect, "auto", 0)
/// Portable C++ without SIMD; always compiled, the final fallback.
BLAKE3PP_ARCH(scalar, "scalar", 190)
/// x86-64 SSE4.2, 128-bit lanes.
BLAKE3PP_ARCH(sse42, "sse42", 30)
/// x86-64 AVX2, 256-bit lanes.
BLAKE3PP_ARCH(avx2, "avx2", 20)
/// x86-64 AVX-512 (F, CD, VL, BW, DQ), 512-bit lanes.
BLAKE3PP_ARCH(avx512, "avx512", 10)
/// AArch64 NEON, 128-bit lanes. Architecturally mandatory on AArch64:
/// presence of the kernel implies availability.
BLAKE3PP_ARCH(neon, "neon", 90)
/// WebAssembly SIMD128. A module-level feature: an engine that lacks it
/// rejects the whole module at load, so if this code is running at all,
/// the variant is available.
BLAKE3PP_ARCH(simd128, "simd128", 180)
// Fixed-length SVE variants. Vector-length-specific code is only valid
// when the runtime vector length EQUALS the compiled one (GCC/Arm
// document exact-match only), so each VL is its own variant and at most
// one of each SVE generation is ever available on a given machine. The
// sve2_* variants additionally use SVE2's XAR fused xor-rotate: a
// measured +19% over neon on Neoverse V2, entirely from XAR, which is
// why sve2_128 outranks neon while sve128 (same width, no XAR,
// measured parity) sits below it. sve128 / sve2_256 /
// sve2_512 match no shipping silicon and are compiled only when
// BLAKE3PP_SVE_ALL_VARIANTS=ON (emulators make them runnable anyway).
/// AArch64 SVE at a vector length of exactly 128 bits (opt-in build).
BLAKE3PP_ARCH(sve128, "sve128", 100)
/// AArch64 SVE at exactly 256 bits (Neoverse V1 / Graviton3 class).
BLAKE3PP_ARCH(sve256, "sve256", 70)
/// AArch64 SVE at exactly 512 bits (Fujitsu A64FX class).
BLAKE3PP_ARCH(sve512, "sve512", 50)
/// AArch64 SVE2 at exactly 128 bits, with the XAR fused xor-rotate
/// (Neoverse N2/V2, Grace, Graviton4).
BLAKE3PP_ARCH(sve2_128, "sve2_128", 80)
/// AArch64 SVE2 at exactly 256 bits (opt-in build).
BLAKE3PP_ARCH(sve2_256, "sve2_256", 60)
/// AArch64 SVE2 at exactly 512 bits (opt-in build).
BLAKE3PP_ARCH(sve2_512, "sve2_512", 40)
// Fixed-VLEN RVV 1.0 variants: the same exact-match rule as SVE
// (-mrvv-vector-bits=zvl pins vscale, min AND max, and whole-register
// moves assume it), keyed on the runtime vlenb. VLEN>512 hardware falls
// back to scalar (documented gap; a scalable kernel would be new work).
/// RISC-V RVV 1.0 at a vector length of exactly 128 bits.
BLAKE3PP_ARCH(rvv128, "rvv128", 160)
/// RISC-V RVV 1.0 at exactly 256 bits.
BLAKE3PP_ARCH(rvv256, "rvv256", 140)
/// RISC-V RVV 1.0 at exactly 512 bits.
BLAKE3PP_ARCH(rvv512, "rvv512", 120)
/// T-Head XTheadVector, the draft-RVV-0.7.1 encoding of C906/C910
/// silicon (Allwinner D1, SG2042, TH1520). Hand-written kernel, opt-in
/// build (BLAKE3PP_XTHEAD_KERNEL=ON); detected through the hwprobe
/// vendor-extension key (Linux 6.13+) or the trap-guarded probes behind
/// run_trap_probes().
BLAKE3PP_ARCH(xthead, "xthead", 170)
// RVV 1.0 + Zvbb: the same fixed-VLEN kernels with the 3-op shift-or
// rotate replaced by Zvbb's single vror (base RVV has no rotate); the
// RVV analog of SVE2's XAR. Zvbb is detected via hwprobe (it has no
// HWCAP letter), VLEN exact-match as ever.
/// RISC-V RVV 1.0 plus Zvbb (vector rotate) at exactly 128 bits.
BLAKE3PP_ARCH(rvv128_zvbb, "rvv128_zvbb", 150)
/// RISC-V RVV 1.0 plus Zvbb at exactly 256 bits.
BLAKE3PP_ARCH(rvv256_zvbb, "rvv256_zvbb", 130)
/// RISC-V RVV 1.0 plus Zvbb at exactly 512 bits.
BLAKE3PP_ARCH(rvv512_zvbb, "rvv512_zvbb", 110)
/// POWER VSX, 128-bit lanes (the ppc64le baseline is POWER8 with VSX;
/// the kernel probes at power9). Detected through the HWCAP VSX bit,
/// checked rather than assumed.
BLAKE3PP_ARCH(vsx, "vsx", 175)
/// IBM z Vector-Enhancements-1 (z14+), 128-bit lanes: the big-endian
/// SIMD target, every word load and store byteswapped (BLAKE3 is defined
/// little-endian). Detected through HWCAP_S390_VXRS_EXT.
BLAKE3PP_ARCH(vxe, "vxe", 176)
/// MIPS MSA, 128-bit lanes (MIPS32r5/MIPS64r5 and later). Built through
/// the vector-extension provider: no std provider deduces a width on this
/// target and xsimd has no MSA backend. Detected through HWCAP_MIPS_MSA.
/// EMULATOR-ONLY: validated under qemu against the golden vectors, never
/// on MSA silicon.
BLAKE3PP_ARCH(msa, "msa", 177)

#undef BLAKE3PP_ARCH
};

constexpr std::size_t num_arches = std::size(arch_table);

consteval bool ranks_unique() {
  for (std::size_t i = 0; i < num_arches; ++i) {
    for (std::size_t j = i + 1; j < num_arches; ++j) {
      if (arch_table[i].rank == arch_table[j].rank) {
        return false;
      }
    }
  }
  return true;
}
static_assert(ranks_unique(), "arch.def ranks must be unique");

// Every enumerator sorted by rank: auto_detect first (rank 0), then
// best-first. This is all_arches() verbatim, and the dispatch preference
// order is simply its tail. One list, two roles.
constexpr std::array<arch, num_arches> all_enumerators = [] {
  std::array<arch, num_arches> out{};
  std::size_t n = 0;
  // Selection by ascending rank; ranks are unique (asserted above).
  for (unsigned last = 0; n < num_arches;) {
    const arch_row* next = nullptr;
    for (const arch_row& r : arch_table) {
      if ((n == 0 || r.rank > last) &&
          (next == nullptr || r.rank < next->rank)) {
        next = &r;
      }
    }
    out[n++] = next->a;
    last = next->rank;
  }
  return out;
}();
static_assert(all_enumerators.front() == arch::auto_detect,
              "auto_detect must hold the lowest rank");
static_assert(all_enumerators.back() == arch::scalar,
              "scalar must hold the highest rank");

constexpr std::span<const arch> preference =
    std::span{all_enumerators}.subspan(1);

// The compiled variants, sorted best-first, computed at compile time.
constexpr std::array<arch, num_kernels> compiled_sorted = [] {
  std::array<arch, num_kernels> out{};
  std::size_t i = 0;
  for (const arch p : preference) {
    for (const registry_entry& e : registry) {
      if (e.a == p) {
        out[i++] = p;
      }
    }
  }
  return out;
}();
static_assert(compiled_sorted.back() == arch::scalar,
              "the scalar fallback must always be registered");

bool compiled_in(arch a) noexcept {
  for (const registry_entry& e : registry) {
    if (e.a == a) {
      return true;
    }
  }
  return false;
}

bool cpu_supports_impl(arch a) noexcept {
  switch (a) {
    case arch::auto_detect:
    case arch::scalar:
      return true;
#if defined(__wasm__)
    case arch::simd128:
      // Module-level feature: SIMD opcodes in a module make load-time
      // validation the availability check; running code proves support.
      return true;
#endif
    default:
#if defined(BLAKE3PP_HAS_CPU_DETECT)
      return detail::platform_cpu_supports(a);
#else
      return false;
#endif
  }
}

struct avail_table {
  std::array<arch, num_kernels> entries{};
  std::size_t count = 0;
};

avail_table build_available() noexcept {
  avail_table t;
  for (const arch a : compiled_sorted) {
    if (cpu_supports_impl(a)) {
      t.entries[t.count++] = a;
    }
  }
  return t;
}

// The base table is built once; run_trap_probes() may publish an
// upgraded rebuild through the atomic pointer. Both live in static
// storage, so the returned span never dangles, and readers see either
// snapshot as one consistent unit.
avail_table g_avail_upgraded;
std::atomic<const avail_table*> g_avail_active{nullptr};

std::span<const arch> available_impl() noexcept {
  static const avail_table base = build_available();
  const avail_table* p = g_avail_active.load(std::memory_order_acquire);
  const avail_table& t = p != nullptr ? *p : base;
  return {t.entries.data(), t.count};
}

}  // namespace

bool is_available(arch a) noexcept {
  return compiled_in(a) && cpu_supports_impl(a);
}

bool run_trap_probes() noexcept {
#if defined(BLAKE3PP_HAS_CPU_DETECT)
  // The magic static serializes concurrent callers and makes the call
  // idempotent; the platform hook itself is once-guarded too.
  static const bool changed = [] {
    if (!detail::platform_run_trap_probes()) {
      return false;
    }
    g_avail_upgraded = build_available();
    g_avail_active.store(&g_avail_upgraded, std::memory_order_release);
    return true;
  }();
  return changed;
#else
  return false;
#endif
}

bool cpu_supports(arch a) noexcept { return cpu_supports_impl(a); }

arch best_available() noexcept { return available_impl().front(); }

std::span<const arch> compiled_arches() noexcept { return compiled_sorted; }

std::span<const arch> available_arches() noexcept { return available_impl(); }

std::span<const arch> all_arches() noexcept { return all_enumerators; }

std::string_view version() noexcept { return BLAKE3PP_STAMPED_VERSION; }

std::string_view simd_provider() noexcept {
#if defined(BLAKE3PP_HAS_STD_SIMD)
  return "std::simd";
#elif defined(BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD)
  return "std::experimental::simd";
#else
  return "xsimd";
#endif
}

std::string_view execution_provider() noexcept {
#if defined(BLAKE3PP_EXECUTION_STD)
  return "std::execution";
#elif defined(BLAKE3PP_EXECUTION_BEMAN)
  return "beman.execution";
#else
  return "stdexec";
#endif
}

const char* to_string(arch a) noexcept {
  for (const arch_row& r : arch_table) {
    if (r.a == a) {
      return r.name;
    }
  }
  return "unknown";
}

std::optional<arch> arch_from_string(std::string_view name) noexcept {
  for (const arch_row& r : arch_table) {
    if (name == r.name) {
      return r.a;
    }
  }
  return std::nullopt;
}

namespace detail {

const kern::kernel_ops* resolve(arch a) noexcept {
  if (a == arch::auto_detect || !cpu_supports_impl(a)) {
    a = best_available();
  }
  for (const registry_entry& e : registry) {
    if (e.a == a) {
      return e.ops;
    }
  }
  // Requested variant not compiled in: fall back to the best one that is.
  for (const arch b : available_impl()) {
    for (const registry_entry& e : registry) {
      if (e.a == b) {
        return e.ops;
      }
    }
  }
  return &kern::scalar::ops;  // unreachable: scalar is always registered
}

}  // namespace detail
}  // namespace blake3pp

// The width-16 transpose dial and its stopwatch tuner: runtime TUNING
// policy, deliberately separate from dispatch (which only ever answers
// "can this CPU run that kernel"; this file answers "which of three
// correct strategies is fastest HERE", a question no feature bit can).


#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>



namespace blake3pp {

namespace kern {
// Definition of the dial declared in kernel.hpp; every kernel TU reads it
// with a relaxed load. Quartered is the measured-best default.
std::atomic<transpose16_mode> transpose16_active{
    transpose16_mode::quartered};
}  // namespace kern

void set_transpose16(transpose16 strategy) noexcept {
  kern::transpose16_active.store(
      static_cast<kern::transpose16_mode>(strategy),
      std::memory_order_relaxed);
}

transpose16 active_transpose16() noexcept {
  return static_cast<transpose16>(
      kern::transpose16_active.load(std::memory_order_relaxed));
}

std::optional<transpose16> transpose16_from_string(
    std::string_view name) noexcept {
  for (const transpose16 t : {transpose16::staging, transpose16::tree,
                              transpose16::quartered}) {
    if (name == to_string(t)) {
      return t;
    }
  }
  return std::nullopt;
}

std::string_view to_string(transpose16 strategy) noexcept {
  switch (strategy) {
    case transpose16::staging:   return "staging";
    case transpose16::tree:      return "tree";
    case transpose16::quartered: return "quartered";
  }
  return "unknown";
}

// Races the three strategies on this CPU and applies the winner. No CPUID
// bit distinguishes a double-pumped from a full-width AVX-512 datapath
// (Strix Point and Granite Ridge report identical feature flags), so the
// only honest detector is a stopwatch.
//
// The race walks a buffer sized like the caller's inputs, ONE pass per
// timed repetition, because the winner depends on where the data lives as
// much as on the CPU. Measured on a Ryzen AI Max 395: quartered wins by
// 18% over staging when the input fits in last-level cache (8 MiB) and
// LOSES to it by 8% when the input streams from DRAM (512 MiB), both
// reproducible. An earlier version of this race hashed the same 16 KiB
// ~800 times (L1-resident throughout) and duly picked the cache-resident
// winner for every caller, including ones hashing gigabyte files, where it
// selected the slowest of the three.
//
// This is a heuristic, not an oracle: it samples one size on one machine
// while it is not doing the caller's real work. Callers who need the last
// few percent should measure with blake3pp_bench and pin the result with
// set_transpose16().
transpose16 tune_transpose16(std::size_t typical_input_bytes) noexcept {
  // Race whichever width-16 kernel this machine would actually dispatch to
  // (avx512, sve512/sve2_512, rvv512...): available_arches() is best-first,
  // so the first width-16 entry is the one auto_detect would pick.
  const kern::kernel_ops* ops = nullptr;
  for (const arch a : available_arches()) {
    const kern::kernel_ops* k = detail::resolve(a);
    if (k->simd_degree == 16) {
      ops = k;
      break;
    }
  }
  if (ops == nullptr) {
    return active_transpose16();  // dial is inert without a W=16 kernel
  }
  static constexpr std::size_t chunk = 1024;  // BLAKE3 chunk
  static constexpr std::size_t lanes = 16;    // the W=16 kernel's batch
  static constexpr std::size_t batch = lanes * chunk;   // 16 KiB per step
  static constexpr std::size_t cap = 256u << 20;

  // Whole batches, at least one, and never more than the cap: a caller
  // hashing 40 GiB files does not get a 40 GiB race.
  std::size_t bytes = typical_input_bytes > cap ? cap : typical_input_bytes;
  bytes = (bytes / batch) * batch;
  if (bytes == 0) {
    bytes = batch;
  }

  // Heap, not stack: the point is a working set that does not fit in
  // cache. Tuning must not fail the program, so an allocation failure
  // simply leaves the current setting alone.
  auto* data = static_cast<std::uint8_t*>(std::malloc(bytes));
  if (data == nullptr) {
    return active_transpose16();
  }
  for (std::size_t i = 0; i < bytes; ++i) {
    data[i] = static_cast<std::uint8_t>(i % 251);
  }
  constexpr std::uint32_t key[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u,
                                    0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu,
                                    0x1F83D9ABu, 0x5BE0CD19u};
  std::array<std::uint8_t, lanes * 32> out;

  const transpose16 saved = active_transpose16();
  const std::size_t batches = bytes / batch;
  const auto one_pass = [&] {
    for (std::size_t b = 0; b < batches; ++b) {
      const std::uint8_t* inputs[lanes];
      for (std::size_t i = 0; i < lanes; ++i) {
        inputs[i] = data + b * batch + i * chunk;
      }
      ops->hash_many(inputs, lanes, chunk / 64, key, 0, true, 0,
                     kern::flag_chunk_start, kern::flag_chunk_end,
                     out.data());
    }
  };
  // INTERLEAVED repetitions, not per-strategy blocks: under drifting
  // conditions (laptop boost droop, a power-clamped OS profile, noisy
  // cloud neighbors) a sequential race hands later strategies a worse
  // environment; a power-clamped Windows box measurably picked the
  // second-raced strategy over a 37%-faster one that raced last. Round-
  // robin spreads the drift evenly; best-of per strategy still filters
  // one-off stalls. (The bench's --t16-sweep guards against the same
  // effect with its order-reversed 'rev' rows.)
  constexpr transpose16 modes[] = {transpose16::staging, transpose16::tree,
                                   transpose16::quartered};
  std::chrono::steady_clock::duration best[3] = {
      std::chrono::steady_clock::duration::max(),
      std::chrono::steady_clock::duration::max(),
      std::chrono::steady_clock::duration::max()};
  for (const transpose16 mode : modes) {
    set_transpose16(mode);
    one_pass();  // warm-up: frequency ramp, page faults, TLB, per mode
  }
  for (int rep = 0; rep < 3; ++rep) {
    for (std::size_t i = 0; i < 3; ++i) {
      set_transpose16(modes[i]);
      const auto t0 = std::chrono::steady_clock::now();
      one_pass();
      const auto dt = std::chrono::steady_clock::now() - t0;
      if (dt < best[i]) {
        best[i] = dt;
      }
    }
  }
  transpose16 winner = saved;
  auto winner_time = std::chrono::steady_clock::duration::max();
  for (std::size_t i = 0; i < 3; ++i) {
    if (best[i] < winner_time) {
      winner_time = best[i];
      winner = modes[i];
    }
  }
  std::free(data);
  set_transpose16(winner);
  return winner;
}

transpose16 tune_transpose16() noexcept {
  return tune_transpose16(default_tune_bytes);
}

}  // namespace blake3pp

// x86 capability probe: one manual cpuid/xgetbv sequence for ALL x86
// toolchains. The tempting alternative, __builtin_cpu_supports, references
// compiler-rt/libgcc's __cpu_model support machinery, a link-time
// dependency that failed us twice (clang-cl with lld-link on Windows;
// zig/musl static linking): the builtin is only as portable as the runtime
// library du jour. The manual probe is self-contained and does the same
// OSXSAVE/XCR0 dance: "avx2" is only reported when the OS actually saves
// YMM state, not merely when the CPU has the silicon.



#if defined(BLAKE3PP_CPU_DETECT_X86)

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace blake3pp::detail {
namespace {

void x86_cpuid(unsigned leaf, unsigned subleaf, unsigned out[4]) noexcept {
#if defined(_MSC_VER)
  int r[4];
  __cpuidex(r, static_cast<int>(leaf), static_cast<int>(subleaf));
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<unsigned>(r[i]);
  }
#else
  // Returns 0 WITHOUT writing the outputs when the CPU's max basic
  // leaf is below the request; zero them so feature tests read a
  // deterministic "absent" instead of stale registers.
  if (__get_cpuid_count(leaf, subleaf, &out[0], &out[1], &out[2],
                        &out[3]) == 0) {
    out[0] = out[1] = out[2] = out[3] = 0;
  }
#endif
}

unsigned x86_xgetbv0() noexcept {
#if defined(_MSC_VER)
  return static_cast<unsigned>(_xgetbv(0));
#else
  // The intrinsic needs -mxsave on GNU compilers (unavailable in this
  // flag-neutral TU); the two-byte encoding is the portable spelling.
  unsigned eax = 0;
  unsigned edx = 0;
  asm volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0u));
  return eax;
#endif
}

}  // namespace

bool platform_cpu_supports(arch a) noexcept {
  unsigned r[4];
  x86_cpuid(1, 0, r);
  const unsigned ecx1 = r[2];
  if (a == arch::sse42) {
    return (ecx1 >> 20) & 1u;  // SSE4.2; XMM state is OS baseline
  }
  const bool osxsave = (ecx1 >> 27) & 1u;
  if (!osxsave) {
    return false;
  }
  const unsigned xcr0 = x86_xgetbv0();
  x86_cpuid(7, 0, r);
  const unsigned ebx7 = r[1];
  if (a == arch::avx2) {
    return (xcr0 & 0x6u) == 0x6u &&  // XMM + YMM saved
           ((ebx7 >> 5) & 1u);
  }
  if (a == arch::avx512) {
    return (xcr0 & 0xE6u) == 0xE6u &&  // + opmask/ZMM state saved
           ((ebx7 >> 16) & 1u) &&      // F
           ((ebx7 >> 28) & 1u) &&      // CD
           ((ebx7 >> 31) & 1u) &&      // VL
           ((ebx7 >> 30) & 1u) &&      // BW
           ((ebx7 >> 17) & 1u);        // DQ
  }
  return false;
}

// No detection rung on this platform needs a trap-guarded probe; the
// syscall/CPUID rungs tell the whole story (see cpu_detect.hpp).
bool platform_run_trap_probes() noexcept { return false; }

}  // namespace blake3pp::detail

#endif  // BLAKE3PP_CPU_DETECT_X86

// file_reader implementation: a pimpl shell around the portable engine.
// This TU contains no platform code and no engine logic: reader_engine
// (io/engine.hpp) is the window/slot machine, written once against the
// reader_backend concept, and io/backend_select.hpp decides which OS
// backend it is instantiated with here: io_uring (Linux), IOCP
// (Windows), GCD (macOS), plain pread (other POSIX), stdio (everything
// else). Runtime degradation (O_DIRECT refused -> buffered, async engine
// refused -> sync) happens inside the backends.


#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>



// The one platform decision left in the I/O layer. Each branch names the
// backend pair the OS gets; everything else (the engines and the backends
// themselves) is straight-line C++. The engine TUs static_assert the
// concepts (io/backend.hpp) against these aliases, so a backend drifting
// from the contract fails loudly at this seam, not somewhere inside the
// engine. Internal to src/io/, never installed.

#if defined(__linux__)


// The Linux backend: io_uring driven through raw syscalls (three of them:
// setup, enter, and mmap for the rings), with no liburing dependency, so
// every moving part is visible. Degrades per-feature at RUNTIME inside
// this class: O_DIRECT refused by the filesystem -> buffered io_uring;
// io_uring refused (seccomp, old kernel) -> synchronous pread. Internal
// to src/io/, never installed.

#if defined(__linux__)

#include <linux/io_uring.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>



// The portable contract between the file_reader/file_writer engines and
// the per-OS I/O backends. 
//
// Two contract rules that span every operation, so they live here rather
// than on any one requirement below:
//  - Slot exclusivity: start()/start_write() may only be called for a
//    slot the backend claimed via wants_async(...), and only while
//    nothing else is outstanding on that slot.
//  - Teardown drain: a backend's destructor drains every in-flight
//    operation. The engines declare their buffer pool member BEFORE the
//    backend member precisely so the drain runs before the pool is
//    freed.
// Internal to src/io/, never installed.

#include <cerrno>
#include <string_view>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <new>
#include <span>
#include <system_error>


namespace blake3pp::detail::io_impl {

[[noreturn]] inline void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

// The O_DIRECT / FILE_FLAG_NO_BUFFERING buffer-and-length granule.
constexpr std::size_t direct_align = 4096;

// The engines' buffer arena: one direct-I/O-aligned allocation, RAII so
// member declaration order alone sequences teardown against the backend.
struct aligned_pool {
  std::byte* data = nullptr;

  explicit aligned_pool(std::size_t bytes)
      : data(static_cast<std::byte*>(
            ::operator new(bytes, std::align_val_t{direct_align}))) {}
  ~aligned_pool() { ::operator delete(data, std::align_val_t{direct_align}); }
  aligned_pool(const aligned_pool&) = delete;
  aligned_pool& operator=(const aligned_pool&) = delete;
};

template <class B>
concept reader_backend =
    // Opens the file and decides (at runtime, per feature) how much of
    // the requested fast path (direct I/O, async engine) it can actually
    // deliver. The unsigned is the engine's queue depth: the most slots
    // that can ever be outstanding at once.
    std::constructible_from<B, const std::filesystem::path&,
                            const file_reader_options&, unsigned> &&
    requires(B b, const B cb, unsigned slot, std::uint64_t off,
             std::span<std::byte> buf) {
      { cb.size() } noexcept -> std::same_as<std::uint64_t>;
      { cb.name() } noexcept -> std::convertible_to<std::string_view>;
      // The whole runtime-degradation ladder folded into one question the
      // engine asks per window: "may THIS (offset, length) ride your
      // async path?" uring/IOCP answer engaged && length aligned
      // (O_DIRECT / NO_BUFFERING reject unaligned lengths), GCD answers
      // engaged (F_NOCACHE has no alignment contract, so the tail rides
      // too), sync backends answer never. The engine doesn't learn why:
      // false just routes the window to read_sync at delivery time.
      // (Current backends ignore the offset, since engine windows start
      // at 64 KiB multiples and it is therefore always granule-aligned,
      // but it is part of the question because O_DIRECT constrains offset
      // alignment too, and a future engine might not guarantee it.)
      { cb.wants_async(off, std::size_t{}) } noexcept -> std::same_as<bool>;
      // Begins an async read of buf at off, owned by `slot`; legal only
      // after wants_async() said yes for exactly this window.
      { b.start(slot, off, buf) };
      // Blocks until `slot`'s read fully completes, reissuing short
      // reads and absorbing OTHER slots' completions when the OS delivers
      // them out of order. Throws std::system_error on failure, including
      // failures a worker thread captured earlier.
      { b.wait(slot) };
      // Positional synchronous read: completes fully or throws. Picks the
      // right handle internally (direct vs buffered) for the length's
      // alignment; the unaligned-tail dance is backend business.
      { b.read_sync(off, buf) };
    };

template <class B>
concept writer_backend =
    // Creates/truncates the file, preallocates if asked (fallocate /
    // SetEndOfFile+VDL / F_PREALLOCATE), and engages what it can of the
    // fast path. The unsigned is the engine's queue depth.
    std::constructible_from<B, const std::filesystem::path&,
                            const file_writer_options&, unsigned> &&
    requires(B b, const B cb, unsigned slot, std::uint64_t off,
             std::span<const std::byte> buf, std::uint64_t written) {
      { cb.name() } noexcept -> std::convertible_to<std::string_view>;
      // The reader's degradation question, minus the offset: the engine
      // writes strictly sequentially and only the final submit may be
      // unaligned, so the length alone decides. False routes the buffer
      // to write_sync.
      { cb.wants_async(std::size_t{}) } noexcept -> std::same_as<bool>;
      // Begins an async write of buf at off, owned by `slot`; legal only
      // after wants_async() said yes for this length. Returns without
      // waiting: the device drains while the producer fills the next
      // buffer.
      { b.start_write(slot, off, buf) };
      // Blocks until `slot` is idle (trivially so on sync backends).
      // This is the engine's backpressure point: acquire() calls it
      // before recycling the slot's buffer. Throws the slot's deferred
      // write error as std::system_error.
      { b.wait_slot(slot) };
      // Positional synchronous write: completes fully or throws. Handle
      // choice (direct vs buffered) for the length's alignment is
      // backend business, same as read_sync.
      { b.write_sync(off, buf) };
      // End of stream: drain every in-flight write, trim the
      // preallocation back to `written` bytes, flush what needs
      // flushing. May be called more than once; the destructor is the
      // error-swallowing fallback for what finish() didn't get to.
      { b.finish(written) };
    };

}  // namespace blake3pp::detail::io_impl



// Shared POSIX file plumbing for the I/O backends: the buffered fd that
// always exists, the optional O_DIRECT reopen next to it (a per-open flag,
// hence a second fd; Darwin's F_NOCACHE backend instead flips `direct` on
// the one fd), and the EINTR-looping positional read/write primitives.
// Internal to src/io/, never installed.

#if defined(__unix__) || defined(__APPLE__)

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <system_error>



namespace blake3pp::detail::io_impl {

struct posix_file {
  int fd_plain = -1;  // always-buffered fd (unaligned tails, fallback)
  int fd = -1;        // == fd_plain unless the O_DIRECT reopen engaged
  bool direct = false;

  posix_file() = default;
  posix_file(const posix_file&) = delete;
  posix_file& operator=(const posix_file&) = delete;
  ~posix_file() {
    if (fd != fd_plain && fd >= 0) {
      ::close(fd);
    }
    if (fd_plain >= 0) {
      ::close(fd_plain);
    }
  }

  void open(const char* path, int flags, ::mode_t mode = 0) {
    fd_plain = ::open(path, flags, mode);
    if (fd_plain < 0) {
      throw_errno("open");
    }
    fd = fd_plain;
  }

  [[nodiscard]] std::uint64_t stat_size() const {
    struct stat st;
    if (::fstat(fd_plain, &st) != 0) {
      throw_errno("fstat");
    }
    return static_cast<std::uint64_t>(st.st_size);
  }

  // O_DIRECT is a per-open flag: engage by reopening, keeping the plain fd
  // for unaligned lengths. Silently declines where the platform (Darwin)
  // lacks the flag or the filesystem refuses it.
  void try_odirect(const char* path, int flags) noexcept {
#if defined(O_DIRECT)
    const int dfd = ::open(path, flags | O_DIRECT);
    if (dfd >= 0) {
      fd = dfd;
      direct = true;
    }
#else
    (void)path;
    (void)flags;
#endif
  }

  // Picks the fd for a synchronous positional transfer: direct only when
  // engaged AND the length keeps O_DIRECT's alignment contract.
  [[nodiscard]] int sync_fd(std::size_t len) const noexcept {
    return direct && len % direct_align == 0 ? fd : fd_plain;
  }

  void pread_all(int use_fd, std::byte* dst, std::size_t len,
                 std::uint64_t off) const {
    std::size_t got = 0;
    while (got < len) {
      const ssize_t n = ::pread(use_fd, dst + got, len - got,
                                static_cast<off_t>(off + got));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw_errno("pread");
      }
      if (n == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF");
      }
      got += static_cast<std::size_t>(n);
    }
  }

  void pwrite_all(int use_fd, const std::byte* src, std::size_t len,
                  std::uint64_t off) const {
    std::size_t put = 0;
    while (put < len) {
      const ssize_t n = ::pwrite(use_fd, src + put, len - put,
                                 static_cast<off_t>(off + put));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw_errno("pwrite");
      }
      put += static_cast<std::size_t>(n);
    }
  }
};

}  // namespace blake3pp::detail::io_impl

#endif  // __unix__ || __APPLE__


// MemorySanitizer cannot see io_uring completions: the kernel fills read
// buffers without any libc call MSan intercepts, so the bytes stay
// "uninitialized" in its shadow. Read completions therefore unpoison the
// range they filled, stating a fact MSan has no other way to learn.
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#define BLAKE3PP_MSAN_UNPOISON(ptr, len) __msan_unpoison(ptr, len)
#endif
#endif
#if !defined(BLAKE3PP_MSAN_UNPOISON)
#define BLAKE3PP_MSAN_UNPOISON(ptr, len) ((void)0)
#endif

namespace blake3pp::detail::io_impl {

inline int sys_io_uring_setup(unsigned entries, io_uring_params* p) noexcept {
  return static_cast<int>(::syscall(__NR_io_uring_setup, entries, p));
}

inline int sys_io_uring_enter(int ring_fd, unsigned to_submit,
                              unsigned min_complete, unsigned flags) noexcept {
  return static_cast<int>(::syscall(__NR_io_uring_enter, ring_fd, to_submit,
                                    min_complete, flags, nullptr, 0));
}

// Names the reason the ring is absent, for the fallback backend name the
// tools print. Without this, "blocked by policy" (Android's seccomp
// filter, EPERM) and "kernel too old" (ENOSYS) are indistinguishable in
// bench output: a fourth kind of quiet degradation the backend line
// otherwise wouldn't confess to, in the same spirit as --version's "cpu
// also supports X (not compiled in)".
inline std::string no_uring_suffix(int err) {
  switch (err) {
    case EPERM:
      return " [io_uring: EPERM, blocked by policy]";
    case ENOSYS:
      return " [io_uring: ENOSYS, kernel too old]";
    default:
      return " [io_uring: errno " + std::to_string(err) + "]";
  }
}

// The mapped rings, reduced to what this pipeline needs. Kernel-shared
// integers are accessed through atomic_ref with acquire/release, per the
// io_uring memory-ordering contract.
struct uring {
  int fd = -1;
  // errno from a failed io_uring_setup. EPERM (a seccomp policy forbids
  // the syscall; Android does) and ENOSYS (kernel predates io_uring) are
  // the same observable with entirely different causes, so the fallback
  // name below reports which one fired.
  int setup_errno = 0;
  unsigned sq_entries = 0;
  unsigned cq_entries = 0;
  void* sq_ring = nullptr;
  std::size_t sq_ring_sz = 0;
  void* cq_ring = nullptr;
  std::size_t cq_ring_sz = 0;
  io_uring_sqe* sqes = nullptr;
  std::size_t sqes_sz = 0;
  unsigned* sq_tail = nullptr;
  unsigned* sq_mask = nullptr;
  unsigned* sq_array = nullptr;
  unsigned* cq_head = nullptr;
  unsigned* cq_tail = nullptr;
  unsigned* cq_mask = nullptr;
  io_uring_cqe* cqes = nullptr;

  uring() = default;
  uring(const uring&) = delete;
  uring& operator=(const uring&) = delete;
  ~uring() { destroy(); }

  bool init(unsigned entries) noexcept {
    io_uring_params p;
    std::memset(&p, 0, sizeof(p));
    fd = sys_io_uring_setup(entries, &p);
    if (fd < 0) {
      setup_errno = errno;
      return false;
    }
    sq_entries = p.sq_entries;
    cq_entries = p.cq_entries;
    sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
      sq_ring_sz = cq_ring_sz = std::max(sq_ring_sz, cq_ring_sz);
    }
    sq_ring = ::mmap(nullptr, sq_ring_sz, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (sq_ring == MAP_FAILED) {
      sq_ring = nullptr;  // destroy() tests for null, and MAP_FAILED is -1
      return fail();
    }
    cq_ring = (p.features & IORING_FEAT_SINGLE_MMAP)
                  ? sq_ring
                  : ::mmap(nullptr, cq_ring_sz, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    if (cq_ring == MAP_FAILED) {
      cq_ring = nullptr;
      return fail();
    }
    sqes_sz = p.sq_entries * sizeof(io_uring_sqe);
    sqes = static_cast<io_uring_sqe*>(
        ::mmap(nullptr, sqes_sz, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES));
    if (sqes == MAP_FAILED) {
      sqes = nullptr;
      return fail();
    }
    auto* sqb = static_cast<unsigned char*>(sq_ring);
    auto* cqb = static_cast<unsigned char*>(cq_ring);
    sq_tail = reinterpret_cast<unsigned*>(sqb + p.sq_off.tail);
    sq_mask = reinterpret_cast<unsigned*>(sqb + p.sq_off.ring_mask);
    sq_array = reinterpret_cast<unsigned*>(sqb + p.sq_off.array);
    cq_head = reinterpret_cast<unsigned*>(cqb + p.cq_off.head);
    cq_tail = reinterpret_cast<unsigned*>(cqb + p.cq_off.tail);
    cq_mask = reinterpret_cast<unsigned*>(cqb + p.cq_off.ring_mask);
    cqes = reinterpret_cast<io_uring_cqe*>(cqb + p.cq_off.cqes);
    return true;
  }

  bool fail() noexcept {
    destroy();
    return false;
  }

  void destroy() noexcept {
    if (sqes != nullptr) {
      ::munmap(sqes, sqes_sz);
      sqes = nullptr;
    }
    if (cq_ring != nullptr && cq_ring != sq_ring) {
      ::munmap(cq_ring, cq_ring_sz);
    }
    cq_ring = nullptr;
    if (sq_ring != nullptr) {
      ::munmap(sq_ring, sq_ring_sz);
      sq_ring = nullptr;
    }
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  // Queues one READ or WRITE; the sole submitter, so sq_tail needs no CAS.
  void submit_rw(std::uint8_t opcode, int file_fd, const void* buf,
                 unsigned len, std::uint64_t off, std::uint64_t user_data,
                 bool offload = false) {
    const unsigned tail = *sq_tail;  // we are the only writer
    const unsigned idx = tail & *sq_mask;
    io_uring_sqe& sqe = sqes[idx];
    std::memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = opcode;
    if (offload) {
      // Issue on io-wq rather than inline. Kernels before ~6.x bailed out
      // of the inline attempt on a large O_DIRECT read and punted anyway;
      // newer ones complete it inline, 1.5-1.9 ms of pinning, splitting
      // and queueing per 64 MiB on the submitting thread (four PCIe 5
      // drives, kernel 7.0). Serialized with the hash that thread also
      // waits for, that halved the pipeline; asking for the hand-off
      // restored it (22 -> 41 GiB/s).
      sqe.flags |= IOSQE_ASYNC;
    }
    sqe.fd = file_fd;
    sqe.addr = reinterpret_cast<std::uint64_t>(buf);
    sqe.len = len;
    sqe.off = off;
    sqe.user_data = user_data;
    sq_array[idx] = idx;
    std::atomic_ref<unsigned>(*sq_tail).store(tail + 1,
                                              std::memory_order_release);
    if (sys_io_uring_enter(fd, 1, 0, 0) < 0) {
      throw_errno("io_uring_enter(submit)");
    }
  }

  // Blocks for one completion and returns (user_data, result).
  std::pair<std::uint64_t, int> wait_one() {
    for (;;) {
      const unsigned head = *cq_head;  // we are the only consumer
      const unsigned tail =
          std::atomic_ref<unsigned>(*cq_tail).load(std::memory_order_acquire);
      if (head != tail) {
        const io_uring_cqe& cqe = cqes[head & *cq_mask];
        const std::pair<std::uint64_t, int> out{cqe.user_data, cqe.res};
        std::atomic_ref<unsigned>(*cq_head).store(head + 1,
                                                  std::memory_order_release);
        return out;
      }
      if (sys_io_uring_enter(fd, 0, 1, IORING_ENTER_GETEVENTS) < 0 &&
          errno != EINTR) {
        throw_errno("io_uring_enter(wait)");
      }
    }
  }
};

class uring_reader {
 public:
  uring_reader(const std::filesystem::path& path,
               const file_reader_options& opts, unsigned nslots)
      : slots_(nslots) {
    f_.open(path.c_str(), O_RDONLY | O_CLOEXEC);
    size_ = f_.stat_size();
    if (opts.direct_io) {
      f_.try_odirect(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
    if (opts.async && ring_.init(2 * nslots)) {
      use_uring_ = true;
      offload_ = opts.offload_submit;
    }
    name_ = use_uring_ ? (f_.direct ? "io_uring+direct" : "io_uring")
                       : (f_.direct ? "pread+direct" : "pread");
    if (use_uring_ && !offload_) {
      name_ += " (inline submit)";
    }
    if (opts.async && !use_uring_ && ring_.setup_errno != 0) {
      name_ += no_uring_suffix(ring_.setup_errno);
    }
  }

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  // Only fully-aligned windows may ride the io_uring path (O_DIRECT
  // rejects unaligned lengths); the tail goes through read_sync.
  [[nodiscard]] bool wants_async(std::uint64_t, std::size_t len) const
      noexcept {
    return use_uring_ && len % direct_align == 0;
  }

  void start(unsigned s, std::uint64_t off, std::span<std::byte> buf) {
    slots_[s] = {buf, off, 0, false};
    ring_.submit_rw(IORING_OP_READ, f_.fd, buf.data(),
                    static_cast<unsigned>(buf.size()), off, s, offload_);
  }

  // Reaps completions (issuing continuations for short reads) until slot
  // `s` is fully read; completions for other slots are absorbed into
  // their state along the way.
  void wait(unsigned s) {
    while (!slots_[s].ready) {
      const auto [ud, res] = ring_.wait_one();
      const unsigned c = static_cast<unsigned>(ud);
      slot& st = slots_[c];
      if (res < 0) {
        throw std::system_error(-res, std::generic_category(),
                                "io_uring read");
      }
      if (res == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF (io_uring)");
      }
      BLAKE3PP_MSAN_UNPOISON(st.buf.data() + st.filled,
                             static_cast<std::size_t>(res));
      st.filled += static_cast<std::size_t>(res);
      if (st.filled < st.buf.size()) {
        ring_.submit_rw(IORING_OP_READ, f_.fd, st.buf.data() + st.filled,
                        static_cast<unsigned>(st.buf.size() - st.filled),
                        st.off + st.filled, c, offload_);
      } else {
        st.ready = true;
      }
    }
  }

  void read_sync(std::uint64_t off, std::span<std::byte> buf) {
    f_.pread_all(f_.sync_fd(buf.size()), buf.data(), buf.size(), off);
  }

 private:
  struct slot {
    std::span<std::byte> buf{};
    std::uint64_t off = 0;
    std::size_t filled = 0;
    bool ready = false;
  };

  posix_file f_;
  uring ring_;
  std::vector<slot> slots_;
  std::uint64_t size_ = 0;
  bool use_uring_ = false;
  bool offload_ = false;
  std::string name_ = "pread";
};

class uring_writer {
 public:
  uring_writer(const std::filesystem::path& path,
               const file_writer_options& opts, unsigned nslots)
      : slots_(nslots) {
    f_.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    // Preallocating turns every write into an overwrite of existing
    // extents. Extending writes serialize on the inode lock; with async
    // direct I/O that collapses the whole queue to one stalled write at a
    // time.
    if (opts.preallocate_bytes > 0 &&
        ::fallocate(f_.fd_plain, 0, 0,
                    static_cast<off_t>(opts.preallocate_bytes)) == 0) {
      prealloc_ = opts.preallocate_bytes;
    }
    if (opts.direct_io) {
      // Reopen (not TRUNC, already truncated) so the tail keeps a plain fd.
      f_.try_odirect(path.c_str(), O_WRONLY | O_CLOEXEC);
    }
    if (opts.async && ring_.init(2 * nslots)) {
      use_uring_ = true;
      offload_ = opts.offload_submit;
    }
    name_ = use_uring_ ? (f_.direct ? "io_uring+direct" : "io_uring")
                       : (f_.direct ? "pwrite+direct" : "pwrite");
    if (use_uring_ && !offload_) {
      name_ += " (inline submit)";
    }
    if (opts.async && !use_uring_ && ring_.setup_errno != 0) {
      name_ += no_uring_suffix(ring_.setup_errno);
    }
  }

  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  [[nodiscard]] bool wants_async(std::size_t len) const noexcept {
    return use_uring_ && len % direct_align == 0;
  }

  void start_write(unsigned s, std::uint64_t off,
                   std::span<const std::byte> buf) {
    slots_[s] = {buf, off, 0, true};
    ring_.submit_rw(IORING_OP_WRITE, f_.fd, buf.data(),
                    static_cast<unsigned>(buf.size()), off, s, offload_);
  }

  void wait_slot(unsigned s) {
    while (slots_[s].busy) {
      reap_one();
    }
  }

  void write_sync(std::uint64_t off, std::span<const std::byte> buf) {
    f_.pwrite_all(f_.sync_fd(buf.size()), buf.data(), buf.size(), off);
  }

  void finish(std::uint64_t written) {
    for (unsigned s = 0; s < slots_.size(); ++s) {
      wait_slot(s);
    }
    // fallocate set the file size up front; trim if less was written.
    if (prealloc_ > written &&
        ::ftruncate(f_.fd_plain, static_cast<off_t>(written)) != 0) {
      throw_errno("ftruncate");
    }
    prealloc_ = 0;
  }

 private:
  struct slot {
    std::span<const std::byte> buf{};
    std::uint64_t off = 0;
    std::size_t done = 0;
    bool busy = false;
  };

  // Reaps one completion, issuing a continuation on a short write. The
  // device may complete slots in any order; each carries its slot index.
  void reap_one() {
    const auto [ud, res] = ring_.wait_one();
    const unsigned s = static_cast<unsigned>(ud);
    slot& st = slots_[s];
    if (res <= 0) {
      throw std::system_error(res < 0 ? -res : EIO, std::generic_category(),
                              "io_uring write");
    }
    st.done += static_cast<std::size_t>(res);
    if (st.done < st.buf.size()) {
      ring_.submit_rw(IORING_OP_WRITE, f_.fd, st.buf.data() + st.done,
                      static_cast<unsigned>(st.buf.size() - st.done),
                      st.off + st.done, s, offload_);
    } else {
      st.busy = false;
    }
  }

  posix_file f_;
  uring ring_;
  std::vector<slot> slots_;
  std::uint64_t prealloc_ = 0;
  bool use_uring_ = false;
  bool offload_ = false;
  std::string name_ = "pwrite";
};

// Definition-site conformance check. Concepts only verify use-sites, so
// without this a drifting backend wouldn't be diagnosed until an engine
// instantiation in some other TU; this makes the header self-checking.
static_assert(reader_backend<uring_reader>);
static_assert(writer_backend<uring_writer>);

}  // namespace blake3pp::detail::io_impl

#endif  // __linux__

namespace blake3pp::detail::io_impl {
using native_reader = uring_reader;
using native_writer = uring_writer;
}  // namespace blake3pp::detail::io_impl
#elif defined(_WIN32)


// The Windows backend: CreateFileW handles (std::filesystem::path's native
// wide string is the whole reason the public API trades in paths), an I/O
// completion port as the completion queue, and the SeManageVolumePrivilege
// dance for SetFileValidData. The mapping to the io_uring backend is
// nearly 1:1: one OVERLAPPED per buffer slot plays the SQE,
// GetQueuedCompletionStatus plays wait_one, and FILE_FLAG_NO_BUFFERING is
// O_DIRECT (same sector-alignment demands, same buffered-handle escape
// hatch for the unaligned tail). Degrades per-feature at RUNTIME: no port
// -> sync ReadFile/WriteFile, NO_BUFFERING refused -> buffered. Internal
// to src/io/, never installed.

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string_view>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <system_error>
#include <utility>
#include <vector>



namespace blake3pp::detail::io_impl {

[[noreturn]] inline void throw_winerr(const char* what) {
  throw std::system_error(static_cast<int>(::GetLastError()),
                          std::system_category(), what);
}

// Writes that land beyond the file's valid data length force NTFS to
// zero-fill the gap synchronously, the Windows twin of ext4's
// extending-write serialization. SetFileValidData waives the zero-fill,
// but only for callers holding SeManageVolumePrivilege (admins, usually,
// and only if the privilege is enabled in the token). Best-effort by
// design: returns whether it actually took.
inline bool try_set_valid_data(HANDLE file, std::int64_t size) noexcept {
  HANDLE token = nullptr;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES,
                         &token) == 0) {
    return false;
  }
  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  bool ok = ::LookupPrivilegeValueW(nullptr, L"SeManageVolumePrivilege",
                                    &tp.Privileges[0].Luid) != 0;
  ok = ok &&
       ::AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) != 0 &&
       ::GetLastError() == ERROR_SUCCESS;
  ::CloseHandle(token);
  if (!ok) {
    return false;
  }
  LARGE_INTEGER n;
  n.QuadPart = size;
  return ::SetFileValidData(file, n.QuadPart) != 0;
}

// Owns one Win32 HANDLE and closes it exactly once. Win32 spells "no
// handle" two ways (CreateFileW yields INVALID_HANDLE_VALUE, the IOCP
// calls yield nullptr), so both count as empty here and neither is ever
// handed to CloseHandle. Move-only, so a handle can never be owned twice.
class unique_handle {
 public:
  unique_handle() = default;
  explicit unique_handle(HANDLE h) noexcept : h_(h) {}
  unique_handle(const unique_handle&) = delete;
  unique_handle& operator=(const unique_handle&) = delete;
  unique_handle(unique_handle&& other) noexcept
      : h_(std::exchange(other.h_, INVALID_HANDLE_VALUE)) {}
  unique_handle& operator=(unique_handle&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.h_, INVALID_HANDLE_VALUE));
    }
    return *this;
  }
  ~unique_handle() { reset(); }

  [[nodiscard]] HANDLE get() const noexcept { return h_; }
  explicit operator bool() const noexcept {
    return h_ != nullptr && h_ != INVALID_HANDLE_VALUE;
  }
  void reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept {
    if (*this) {
      ::CloseHandle(h_);
    }
    h_ = h;
  }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

// Shared Windows file plumbing, the twin of posix_file: the buffered
// handle that always exists, the optional NO_BUFFERING/OVERLAPPED reopen
// next to it (both are per-open flags, hence a second handle), and the
// completion port when async engages. Every handle lives in a
// unique_handle, which makes the type non-copyable by construction and
// unwinds the whole set when a constructor throws part-way through.
//
// `fast` is held ONLY when the reopen genuinely engaged, so it never
// aliases `plain` and no destructor has to test for that.
// Shared verbatim between reader and writer; only access/creation differ.
struct win_file {
  unique_handle plain;  // always-buffered+sync: unaligned tails, fallback
  unique_handle fast;   // the reopened handle, when one engaged
  unique_handle port;   // IOCP, when async engaged
  bool direct = false;
  bool use_iocp = false;

  // The handle the fast path should use: the reopened one when it
  // engaged, else the plain one. Borrowed: the caller never closes it.
  [[nodiscard]] HANDLE h() const noexcept {
    return fast ? fast.get() : plain.get();
  }

  void open(const wchar_t* path, DWORD access, DWORD share, DWORD creation,
            DWORD flags) {
    plain.reset(::CreateFileW(path, access, share, nullptr, creation, flags,
                              nullptr));
    if (!plain) {
      throw_winerr("CreateFileW");
    }
  }

  [[nodiscard]] std::uint64_t stat_size() const {
    LARGE_INTEGER sz;
    if (::GetFileSizeEx(plain.get(), &sz) == 0) {
      throw_winerr("GetFileSizeEx");
    }
    return static_cast<std::uint64_t>(sz.QuadPart);
  }

  // NO_BUFFERING and OVERLAPPED are per-open flags: engage by reopening,
  // keeping the plain handle for unaligned lengths. Best-effort: a
  // refused reopen, or a port that will not attach, simply leaves the
  // plain handle in charge with direct/use_iocp still false.
  void engage(const wchar_t* path, DWORD access, DWORD share,
              bool want_direct, bool want_async) noexcept {
    if (!want_direct && !want_async) {
      return;
    }
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (want_direct) {
      flags |= FILE_FLAG_NO_BUFFERING;
    }
    if (want_async) {
      flags |= FILE_FLAG_OVERLAPPED;
    }
    unique_handle cand(::CreateFileW(path, access, share, nullptr,
                                     OPEN_EXISTING, flags, nullptr));
    if (!cand) {
      return;
    }
    if (want_async) {
      unique_handle p(
          ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
      // An unattachable port takes the fast handle down with it: a
      // FILE_FLAG_OVERLAPPED handle cannot serve the synchronous path.
      // Both candidates close on the way out of this branch.
      if (!p ||
          ::CreateIoCompletionPort(cand.get(), p.get(), 0, 0) == nullptr) {
        return;
      }
      port = std::move(p);
      use_iocp = true;
    }
    fast = std::move(cand);
    direct = want_direct;
  }
};

class iocp_reader {
 public:
  iocp_reader(const std::filesystem::path& path,
              const file_reader_options& opts, unsigned nslots)
      : slots_(nslots), ovs_(nslots) {
    file_.open(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN);
    size_ = file_.stat_size();
    file_.engage(path.c_str(), GENERIC_READ, FILE_SHARE_READ, opts.direct_io,
                 opts.async);
    name_ = file_.use_iocp ? (file_.direct ? "iocp+direct" : "iocp")
            : file_.direct ? "readfile+direct"
                           : "readfile";
  }


  // Cancels every in-flight request and waits for ALL of them to report.
  // The wait is INFINITE on purpose. These requests target the engine's
  // buffer pool, which is declared before this backend and therefore freed
  // AFTER it, so returning while one is still pending hands the kernel a
  // window to write into freed memory. CancelIoEx makes that wait bounded
  // in practice: once it returns, every outstanding request is guaranteed
  // to complete, successfully or with ERROR_OPERATION_ABORTED. A null
  // OVERLAPPED here therefore means the port itself has failed, not that a
  // request is merely slow: no completion can ever arrive, so breaking is
  // the only option left.
  void drain_cancelled() noexcept {
    ::CancelIoEx(file_.h(), nullptr);
    while (outstanding_ > 0) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* pov = nullptr;
      ::GetQueuedCompletionStatus(file_.port.get(), &bytes, &key, &pov,
                                  INFINITE);
      if (pov == nullptr) {
        break;  // port unusable: no completion will ever arrive
      }
      --outstanding_;
    }
  }

  // Only the drain is hand-written now: every handle belongs to file_,
  // whose destructor runs after this body, that is, after the last
  // request has reported, which is the ordering the drain exists for.
  ~iocp_reader() {
    if (file_.use_iocp && outstanding_ > 0) {
      drain_cancelled();
    }
  }
  iocp_reader(const iocp_reader&) = delete;
  iocp_reader& operator=(const iocp_reader&) = delete;

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  // Only fully-aligned windows may ride the IOCP path (NO_BUFFERING
  // rejects unaligned lengths); the tail goes through read_sync.
  [[nodiscard]] bool wants_async(std::uint64_t, std::size_t len) const
      noexcept {
    return file_.use_iocp && len % direct_align == 0;
  }

  void start(unsigned s, std::uint64_t off, std::span<std::byte> buf) {
    slots_[s] = {buf.data(), buf.size(), off, 0, false};
    submit_read(s, 0);
  }

  // Reaps completions (issuing continuations for short reads) until slot
  // `s` is fully read. GetQueuedCompletionStatus is wait_one: the
  // OVERLAPPED pointer identifies the slot.
  void wait(unsigned s) {
    while (!slots_[s].ready) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* pov = nullptr;
      const BOOL ok = ::GetQueuedCompletionStatus(file_.port.get(), &bytes,
                                                  &key, &pov, INFINITE);
      if (pov == nullptr) {
        throw_winerr("GetQueuedCompletionStatus");
      }
      --outstanding_;
      const unsigned c = static_cast<unsigned>(pov - ovs_.data());
      slot& st = slots_[c];
      if (ok == 0) {
        throw_winerr("iocp read");
      }
      if (bytes == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF (iocp)");
      }
      st.filled += bytes;
      if (st.filled < st.len) {
        submit_read(c, st.filled);
      } else {
        st.ready = true;
      }
    }
  }

  // Positional synchronous read: a non-OVERLAPPED handle plus an
  // OVERLAPPED offset blocks until complete. In iocp mode only unaligned
  // tails reach this path; the !use_iocp guard makes it structural that
  // the overlapped handle is never used synchronously.
  void read_sync(std::uint64_t off, std::span<std::byte> buf) {
    const HANDLE use_h = file_.direct && !file_.use_iocp &&
                                 buf.size() % direct_align == 0
                             ? file_.h()
                             : file_.plain.get();
    std::size_t got = 0;
    while (got < buf.size()) {
      OVERLAPPED ov{};
      const std::uint64_t o = off + got;
      ov.Offset = static_cast<DWORD>(o);
      ov.OffsetHigh = static_cast<DWORD>(o >> 32);
      DWORD n = 0;
      if (::ReadFile(use_h, buf.data() + got,
                     static_cast<DWORD>(buf.size() - got), &n, &ov) == 0) {
        throw_winerr("ReadFile");
      }
      if (n == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF");
      }
      got += n;
    }
  }

 private:
  struct slot {
    std::byte* dst = nullptr;
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t filled = 0;
    bool ready = false;
  };

  // Queues one async read (or a short-read continuation from `from`).
  void submit_read(unsigned s, std::size_t from) {
    slot& st = slots_[s];
    OVERLAPPED& ov = ovs_[s];
    std::memset(&ov, 0, sizeof(ov));
    const std::uint64_t off = st.off + from;
    ov.Offset = static_cast<DWORD>(off);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    if (::ReadFile(file_.h(), st.dst + from,
                   static_cast<DWORD>(st.len - from), nullptr, &ov) == 0 &&
        ::GetLastError() != ERROR_IO_PENDING) {
      throw_winerr("ReadFile(async)");
    }
    ++outstanding_;
  }

  std::vector<slot> slots_;
  std::vector<OVERLAPPED> ovs_;  // one per slot; the SQE equivalent
  std::uint64_t size_ = 0;
  unsigned outstanding_ = 0;  // async reads in flight
  std::string_view name_ = "readfile";
  // Declared LAST so it is destroyed FIRST: the handles must close before
  // ovs_ goes away. drain_cancelled() normally guarantees nothing is in
  // flight by then, but it gives up early if the port itself has failed,
  // and closing the handles is what cancels any request still holding an
  // OVERLAPPED in that path.
  win_file file_;  // plain (tail, fallback) + fast reopen + IOCP port
};

class iocp_writer {
 public:
  iocp_writer(const std::filesystem::path& path,
              const file_writer_options& opts, unsigned nslots)
      : slots_(nslots), ovs_(nslots) {
    // Two opens of one file need explicit sharing on Windows.
    constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    file_.open(path.c_str(), GENERIC_WRITE, share, CREATE_ALWAYS,
               FILE_ATTRIBUTE_NORMAL);
    bool vdl = false;
    if (opts.preallocate_bytes > 0) {
      // SetEndOfFile is the fallocate twin: writes become overwrites of an
      // existing region. NTFS adds a second lock beyond ext4's, the valid
      // data length: any write landing past VDL zero-fills the gap
      // synchronously. SetFileValidData waives that, privilege permitting.
      LARGE_INTEGER target;
      target.QuadPart = static_cast<std::int64_t>(opts.preallocate_bytes);
      if (::SetFilePointerEx(file_.plain.get(), target, nullptr,
                             FILE_BEGIN) != 0 &&
          ::SetEndOfFile(file_.plain.get()) != 0) {
        prealloc_ = opts.preallocate_bytes;
        vdl = try_set_valid_data(file_.plain.get(), target.QuadPart);
      }
      LARGE_INTEGER zero{};
      ::SetFilePointerEx(file_.plain.get(), zero, nullptr, FILE_BEGIN);
    }
    file_.engage(path.c_str(), GENERIC_WRITE, share, opts.direct_io,
                 opts.async);
    name_ = file_.use_iocp
                ? (file_.direct ? (vdl ? "iocp+direct+vdl" : "iocp+direct")
                                : "iocp")
                : file_.direct
                    ? (vdl ? "writefile+direct+vdl" : "writefile+direct")
                    : "writefile";
  }


  // Cancels every in-flight request and waits for ALL of them to report.
  // The wait is INFINITE on purpose. These requests target the engine's
  // buffer pool, which is declared before this backend and therefore freed
  // AFTER it, so returning while one is still pending hands the kernel a
  // window to write into freed memory. CancelIoEx makes that wait bounded
  // in practice: once it returns, every outstanding request is guaranteed
  // to complete, successfully or with ERROR_OPERATION_ABORTED. A null
  // OVERLAPPED here therefore means the port itself has failed, not that a
  // request is merely slow: no completion can ever arrive, so breaking is
  // the only option left.
  void drain_cancelled() noexcept {
    ::CancelIoEx(file_.h(), nullptr);
    while (outstanding_ > 0) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* pov = nullptr;
      ::GetQueuedCompletionStatus(file_.port.get(), &bytes, &key, &pov,
                                  INFINITE);
      if (pov == nullptr) {
        break;  // port unusable: no completion will ever arrive
      }
      --outstanding_;
    }
  }

  // Only the drain is hand-written now: every handle belongs to file_,
  // whose destructor runs after this body, that is, after the last
  // request has reported, which is the ordering the drain exists for.
  ~iocp_writer() {
    // finish() may have thrown or been skipped, leaving writes in flight.
    if (file_.use_iocp && outstanding_ > 0) {
      drain_cancelled();
    }
  }
  iocp_writer(const iocp_writer&) = delete;
  iocp_writer& operator=(const iocp_writer&) = delete;

  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  [[nodiscard]] bool wants_async(std::size_t len) const noexcept {
    return file_.use_iocp && len % direct_align == 0;
  }

  void start_write(unsigned s, std::uint64_t off,
                   std::span<const std::byte> buf) {
    slots_[s] = {buf.data(), buf.size(), off, 0, true};
    submit_async(s, 0);
  }

  void wait_slot(unsigned s) {
    while (slots_[s].busy) {
      reap_one();
    }
  }

  // Positional synchronous write on a non-OVERLAPPED handle; the
  // unaligned tail always takes the buffered handle (NO_BUFFERING rejects
  // unaligned lengths, same story as O_DIRECT).
  void write_sync(std::uint64_t off, std::span<const std::byte> buf) {
    const HANDLE use_h = file_.direct && !file_.use_iocp &&
                                 buf.size() % direct_align == 0
                             ? file_.h()
                             : file_.plain.get();
    std::size_t put = 0;
    while (put < buf.size()) {
      OVERLAPPED ov{};
      const std::uint64_t o = off + put;
      ov.Offset = static_cast<DWORD>(o);
      ov.OffsetHigh = static_cast<DWORD>(o >> 32);
      DWORD n = 0;
      if (::WriteFile(use_h, buf.data() + put,
                      static_cast<DWORD>(buf.size() - put), &n, &ov) == 0) {
        throw_winerr("WriteFile");
      }
      put += n;
    }
  }

  void finish(std::uint64_t written) {
    for (unsigned s = 0; s < slots_.size(); ++s) {
      wait_slot(s);
    }
    // SetEndOfFile set the size up front; trim back if less was written.
    if (prealloc_ > written) {
      LARGE_INTEGER n;
      n.QuadPart = static_cast<std::int64_t>(written);
      if (::SetFilePointerEx(file_.plain.get(), n, nullptr, FILE_BEGIN) == 0 ||
          ::SetEndOfFile(file_.plain.get()) == 0) {
        throw_winerr("SetEndOfFile(trim)");
      }
    }
    prealloc_ = 0;
  }

 private:
  struct slot {
    const std::byte* src = nullptr;
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t done = 0;
    bool busy = false;
  };

  void submit_async(unsigned s, std::size_t from) {
    slot& st = slots_[s];
    OVERLAPPED& ov = ovs_[s];
    std::memset(&ov, 0, sizeof(ov));
    const std::uint64_t o = st.off + from;
    ov.Offset = static_cast<DWORD>(o);
    ov.OffsetHigh = static_cast<DWORD>(o >> 32);
    if (::WriteFile(file_.h(), st.src + from,
                    static_cast<DWORD>(st.len - from), nullptr, &ov) == 0 &&
        ::GetLastError() != ERROR_IO_PENDING) {
      throw_winerr("WriteFile(async)");
    }
    ++outstanding_;
  }

  void reap_one() {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* pov = nullptr;
    const BOOL ok = ::GetQueuedCompletionStatus(file_.port.get(), &bytes, &key,
                                                &pov, INFINITE);
    if (pov == nullptr) {
      throw_winerr("GetQueuedCompletionStatus");
    }
    --outstanding_;
    const unsigned s = static_cast<unsigned>(pov - ovs_.data());
    slot& st = slots_[s];
    if (ok == 0 || bytes == 0) {
      throw_winerr("iocp write");
    }
    st.done += bytes;
    if (st.done < st.len) {
      submit_async(s, st.done);
    } else {
      st.busy = false;
    }
  }

  std::vector<slot> slots_;
  std::vector<OVERLAPPED> ovs_;  // one per slot
  std::uint64_t prealloc_ = 0;
  unsigned outstanding_ = 0;
  std::string_view name_ = "writefile";
  // Declared LAST so it is destroyed FIRST: the handles must close before
  // ovs_ goes away. drain_cancelled() normally guarantees nothing is in
  // flight by then, but it gives up early if the port itself has failed,
  // and closing the handles is what cancels any request still holding an
  // OVERLAPPED in that path.
  win_file file_;  // plain (tail, trim) + fast reopen + IOCP port
};

// Definition-site conformance check (see uring_backend.hpp): fails here,
// with the missed requirement named, the first time MSVC compiles this
// header, before any engine instantiation exists.
static_assert(reader_backend<iocp_reader>);
static_assert(writer_backend<iocp_writer>);

}  // namespace blake3pp::detail::io_impl

#endif  // _WIN32

namespace blake3pp::detail::io_impl {
using native_reader = iocp_reader;
using native_writer = iocp_writer;
}  // namespace blake3pp::detail::io_impl
#elif defined(__APPLE__)


// The Darwin backend. macOS has no io_uring; the platform's async story IS
// libdispatch (GCD), and its page-cache bypass is fcntl(F_NOCACHE),
// per-fd rather than per-open, with no alignment contract: unaligned edges
// are silently served through the cache instead of being rejected, so the
// dual-fd tail trick the O_DIRECT and NO_BUFFERING backends need
// disappears here. The backend runs positional pread/pwrite loops on GCD's
// global concurrent pool, straight into the engine's buffer ring
// (dispatch_io was considered and rejected: it delivers dispatch_data_t
// chunks it allocated itself, an extra copy the zero-copy pipeline exists
// to avoid). A dispatch_group is the teardown drain and a mutex/condvar
// pair the completion queue. Internal to src/io/, never installed.

#if defined(__APPLE__)

#include <dispatch/dispatch.h>
#include <string_view>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <system_error>
#include <vector>




namespace blake3pp::detail::io_impl {

// Turns the page cache off for this fd: Darwin's O_DIRECT analogue.
inline bool set_nocache(int fd) noexcept {
  return ::fcntl(fd, F_NOCACHE, 1) != -1;
}

// fallocate's Darwin twin: reserve the extents (contiguous if the volume
// can, scattered otherwise), then give the file its final logical size,
// so queued writes land as overwrites instead of size-extending appends.
inline bool preallocate(int fd, std::uint64_t len) noexcept {
  fstore_t st{};
  st.fst_flags = F_ALLOCATECONTIG;
  st.fst_posmode = F_PEOFPOSMODE;
  st.fst_offset = 0;
  st.fst_length = static_cast<off_t>(len);
  if (::fcntl(fd, F_PREALLOCATE, &st) == -1) {
    st.fst_flags = F_ALLOCATEALL;
    if (::fcntl(fd, F_PREALLOCATE, &st) == -1) {
      return false;
    }
  }
  return ::ftruncate(fd, static_cast<off_t>(len)) == 0;
}

// The submission/completion machinery, playing the role the uring and the
// completion port play elsewhere: submit() is fire-and-forget onto GCD's
// global pool, workers publish per-slot completion under m and signal cv,
// and the pipeline thread blocks on cv for the slot it needs next. The
// group exists for teardown: in-flight workers touch the engine's buffer
// pool, so destroy() must wait them out before the pool is freed.
struct gcd_pump {
  dispatch_group_t group = nullptr;
  std::mutex m;
  std::condition_variable cv;

  bool init() noexcept {
    group = dispatch_group_create();
    return group != nullptr;
  }

  // Owning the group means owning its release. The backends below also
  // call destroy() explicitly (it is idempotent), but they can only do so
  // once their constructor has COMPLETED: both allocate slot vectors after
  // init() succeeds, and a throw there destroys members without ever
  // running the backend destructor. This is the net under that window.
  ~gcd_pump() { destroy(); }
  gcd_pump() = default;
  gcd_pump(const gcd_pump&) = delete;
  gcd_pump& operator=(const gcd_pump&) = delete;

  void destroy() noexcept {
    if (group != nullptr) {
      dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
      dispatch_release(group);
      group = nullptr;
    }
  }

  void submit(void (*fn)(void*), void* ctx) noexcept {
    dispatch_group_async_f(
        group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ctx,
        fn);
  }
};

class gcd_reader {
 public:
  gcd_reader(const std::filesystem::path& path,
             const file_reader_options& opts, unsigned nslots) {
    f_.open(path.c_str(), O_RDONLY | O_CLOEXEC);
    size_ = f_.stat_size();
    // Darwin's cache bypass is per-fd, not per-open, and tolerates any
    // alignment, so one fd serves every window, tail included.
    if (opts.direct_io && set_nocache(f_.fd_plain)) {
      f_.direct = true;
    }
    name_ = f_.direct ? "pread+nocache" : "pread";
    if (opts.async && pump_.init()) {
      use_gcd_ = true;
      slots_.resize(nslots);
      tasks_.resize(nslots);
      for (unsigned s = 0; s < nslots; ++s) {
        tasks_[s] = {this, s};
      }
      name_ = f_.direct ? "gcd+nocache" : "gcd";
    }
  }

  // In-flight workers write into the engine's buffer pool: wait them out
  // here, before the pool member (declared before this backend in the
  // engine) is freed. This is the GCD flavor of the IOCP cancel-and-drain
  // rule.
  ~gcd_reader() { pump_.destroy(); }
  gcd_reader(const gcd_reader&) = delete;
  gcd_reader& operator=(const gcd_reader&) = delete;

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  // Every window, the unaligned tail included, takes the async path:
  // F_NOCACHE has no alignment contract, the kernel just serves unaligned
  // edges through the cache.
  [[nodiscard]] bool wants_async(std::uint64_t, std::size_t) const noexcept {
    return use_gcd_;
  }

  void start(unsigned s, std::uint64_t off, std::span<std::byte> buf) {
    slot& st = slots_[s];
    st.dst = buf.data();
    st.len = buf.size();
    st.off = off;
    st.filled = 0;
    st.error = 0;
    st.ready = false;
    pump_.submit(&gcd_reader::run_read, &tasks_[s]);
  }

  // Blocks until slot s completes; throws the worker's deferred errno.
  // ready/error are written under the pump lock, so even the "is it done
  // already" check lives here; an unlocked peek would be a data race.
  void wait(unsigned s) {
    slot& st = slots_[s];
    std::unique_lock<std::mutex> lk(pump_.m);
    pump_.cv.wait(lk, [&] { return st.ready; });
    if (st.error != 0) {
      throw std::system_error(st.error, std::generic_category(),
                              "gcd pread");
    }
  }

  void read_sync(std::uint64_t off, std::span<std::byte> buf) {
    f_.pread_all(f_.fd_plain, buf.data(), buf.size(), off);
  }

 private:
  struct slot {
    std::byte* dst = nullptr;
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t filled = 0;
    int error = 0;  // errno captured by the worker; thrown at wait()
    bool ready = false;
  };
  struct task {
    gcd_reader* self = nullptr;
    unsigned s = 0;
  };

  // Runs on a GCD worker: fills the slot's buffer with one positional
  // read loop, then publishes completion under the pump lock. noexcept:
  // errors travel through slot::error to the waiting thread.
  static void run_read(void* ctx) noexcept {
    const task t = *static_cast<task*>(ctx);
    gcd_reader& r = *t.self;
    slot& st = r.slots_[t.s];
    int err = 0;
    std::size_t got = 0;
    while (got < st.len) {
      const ssize_t n = ::pread(r.f_.fd_plain, st.dst + got, st.len - got,
                                static_cast<off_t>(st.off + got));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        err = errno;
        break;
      }
      if (n == 0) {
        err = EIO;  // unexpected EOF
        break;
      }
      got += static_cast<std::size_t>(n);
    }
    {
      const std::lock_guard<std::mutex> lk(r.pump_.m);
      st.filled = got;
      st.error = err;
      st.ready = true;
    }
    r.pump_.cv.notify_all();
  }

  posix_file f_;
  gcd_pump pump_;
  std::vector<slot> slots_;
  std::vector<task> tasks_;
  std::uint64_t size_ = 0;
  bool use_gcd_ = false;
  std::string_view name_ = "pread";
};

class gcd_writer {
 public:
  gcd_writer(const std::filesystem::path& path,
             const file_writer_options& opts, unsigned nslots) {
    f_.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    // Same story as Linux's fallocate, via Darwin's spelling: queued
    // writes should land as overwrites, not size-extending appends.
    if (opts.preallocate_bytes > 0 &&
        preallocate(f_.fd_plain, opts.preallocate_bytes)) {
      prealloc_ = opts.preallocate_bytes;
    }
    if (opts.direct_io && set_nocache(f_.fd_plain)) {
      f_.direct = true;
    }
    name_ = f_.direct ? "pwrite+nocache" : "pwrite";
    if (opts.async && pump_.init()) {
      use_gcd_ = true;
      slots_.resize(nslots);
      tasks_.resize(nslots);
      for (unsigned s = 0; s < nslots; ++s) {
        tasks_[s] = {this, s};
      }
      name_ = f_.direct ? "gcd+nocache" : "gcd";
    }
  }

  // If finish() threw (or was skipped), workers may still be writing from
  // the engine's pool: wait them all out before it is freed.
  ~gcd_writer() { pump_.destroy(); }
  gcd_writer(const gcd_writer&) = delete;
  gcd_writer& operator=(const gcd_writer&) = delete;

  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  [[nodiscard]] bool wants_async(std::size_t len) const noexcept {
    return use_gcd_ && len % direct_align == 0;
  }

  void start_write(unsigned s, std::uint64_t off,
                   std::span<const std::byte> buf) {
    slot& st = slots_[s];
    st.src = buf.data();
    st.len = buf.size();
    st.off = off;
    st.done = 0;
    st.busy = true;
    pump_.submit(&gcd_writer::run_write, &tasks_[s]);
  }

  // Blocks until slot s is idle; surfaces its deferred write error once
  // (cleared after the throw so the slot stays reusable).
  void wait_slot(unsigned s) {
    if (!use_gcd_) {
      return;
    }
    slot& st = slots_[s];
    std::unique_lock<std::mutex> lk(pump_.m);
    pump_.cv.wait(lk, [&] { return !st.busy; });
    if (st.error != 0) {
      const int e = st.error;
      st.error = 0;
      throw std::system_error(e, std::generic_category(), "gcd pwrite");
    }
  }

  void write_sync(std::uint64_t off, std::span<const std::byte> buf) {
    f_.pwrite_all(f_.fd_plain, buf.data(), buf.size(), off);
  }

  void finish(std::uint64_t written) {
    if (use_gcd_) {
      for (unsigned s = 0; s < slots_.size(); ++s) {
        wait_slot(s);
      }
    }
    // F_PREALLOCATE/ftruncate set the size up front; trim if less was
    // written.
    if (prealloc_ > written &&
        ::ftruncate(f_.fd_plain, static_cast<off_t>(written)) != 0) {
      throw_errno("ftruncate");
    }
    prealloc_ = 0;
  }

 private:
  struct slot {
    const std::byte* src = nullptr;
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t done = 0;
    int error = 0;  // errno captured by the worker; thrown at wait_slot()
    bool busy = false;
  };
  struct task {
    gcd_writer* self = nullptr;
    unsigned s = 0;
  };

  // Runs on a GCD worker: drains the slot's buffer with one positional
  // write loop, then publishes completion under the pump lock.
  static void run_write(void* ctx) noexcept {
    const task t = *static_cast<task*>(ctx);
    gcd_writer& w = *t.self;
    slot& st = w.slots_[t.s];
    int err = 0;
    std::size_t put = 0;
    while (put < st.len) {
      const ssize_t n = ::pwrite(w.f_.fd_plain, st.src + put, st.len - put,
                                 static_cast<off_t>(st.off + put));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        err = errno;
        break;
      }
      put += static_cast<std::size_t>(n);
    }
    {
      const std::lock_guard<std::mutex> lk(w.pump_.m);
      st.done = put;
      st.error = err;
      st.busy = false;
    }
    w.pump_.cv.notify_all();
  }

  posix_file f_;
  gcd_pump pump_;
  std::vector<slot> slots_;
  std::vector<task> tasks_;
  std::uint64_t prealloc_ = 0;
  bool use_gcd_ = false;
  std::string_view name_ = "pwrite";
};

// Definition-site conformance check (see uring_backend.hpp).
static_assert(reader_backend<gcd_reader>);
static_assert(writer_backend<gcd_writer>);

}  // namespace blake3pp::detail::io_impl

#endif  // __APPLE__

namespace blake3pp::detail::io_impl {
using native_reader = gcd_reader;
using native_writer = gcd_writer;
}  // namespace blake3pp::detail::io_impl
#elif defined(__unix__)


// The synchronous POSIX backend: plain positional pread/pwrite, with the
// O_DIRECT reopen where the platform offers it. This is the compile-time
// choice for POSIX systems with neither io_uring nor GCD; the equivalent
// RUNTIME floor on Linux lives inside uring_backend.hpp. Internal to
// src/io/, never installed.

#if defined(__unix__) || defined(__APPLE__)

#include <cassert>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <span>




namespace blake3pp::detail::io_impl {

class pread_reader {
 public:
  pread_reader(const std::filesystem::path& path,
               const file_reader_options& opts, unsigned) {
    f_.open(path.c_str(), O_RDONLY | O_CLOEXEC);
    size_ = f_.stat_size();
    if (opts.direct_io) {
      f_.try_odirect(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
  }

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::string_view name() const noexcept {
    return f_.direct ? "pread+direct" : "pread";
  }
  [[nodiscard]] bool wants_async(std::uint64_t, std::size_t) const noexcept {
    return false;
  }

  void start(unsigned, std::uint64_t, std::span<std::byte>) {
    assert(false && "pread backend has no async path");
  }
  void wait(unsigned) { assert(false && "pread backend has no async path"); }

  void read_sync(std::uint64_t off, std::span<std::byte> buf) {
    f_.pread_all(f_.sync_fd(buf.size()), buf.data(), buf.size(), off);
  }

 private:
  posix_file f_;
  std::uint64_t size_ = 0;
};

class pread_writer {
 public:
  pread_writer(const std::filesystem::path& path,
               const file_writer_options& opts, unsigned) {
    f_.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    // No preallocation here: fallocate is Linux, F_PREALLOCATE is Darwin,
    // and each lives in its platform's backend. Synchronous writes don't
    // suffer the extending-write serialization anyway.
    (void)opts;
    if (opts.direct_io) {
      f_.try_odirect(path.c_str(), O_WRONLY | O_CLOEXEC);
    }
  }

  [[nodiscard]] std::string_view name() const noexcept {
    return f_.direct ? "pwrite+direct" : "pwrite";
  }
  [[nodiscard]] bool wants_async(std::size_t) const noexcept { return false; }

  void start_write(unsigned, std::uint64_t, std::span<const std::byte>) {
    assert(false && "pread backend has no async path");
  }
  void wait_slot(unsigned) {}  // nothing is ever in flight

  void write_sync(std::uint64_t off, std::span<const std::byte> buf) {
    f_.pwrite_all(f_.sync_fd(buf.size()), buf.data(), buf.size(), off);
  }

  void finish(std::uint64_t) {}  // no queue to drain, no preallocation

 private:
  posix_file f_;
};

// Definition-site conformance check (see uring_backend.hpp).
static_assert(reader_backend<pread_reader>);
static_assert(writer_backend<pread_writer>);

}  // namespace blake3pp::detail::io_impl

#endif  // __unix__ || __APPLE__

namespace blake3pp::detail::io_impl {
using native_reader = pread_reader;
using native_writer = pread_writer;
}  // namespace blake3pp::detail::io_impl
#else


// The portable floor: buffered, fully synchronous stdio, for platforms
// with none of the native backends (e.g. wasm). Internal to src/io/,
// never installed.

#include <cassert>
#include <cerrno>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <system_error>



namespace blake3pp::detail::io_impl {

// std::fseek/std::ftell take and return `long`, which is 32 bits on
// Windows (LLP64) and on wasm32, precisely the platforms this fallback
// exists to serve. Truncating a file offset there does not fail: it seeks
// somewhere else and returns the wrong bytes, which for a hash function
// means a silently wrong digest. These wrappers keep the full 64-bit range
// where the platform offers it, and where it does not they REFUSE the
// offset rather than truncate it.
inline int seek64(std::FILE* f, std::uint64_t off) noexcept {
#if defined(_MSC_VER)
  return _fseeki64(f, static_cast<__int64>(off), SEEK_SET);
#elif defined(_WIN32)
  return fseeko64(f, static_cast<off64_t>(off), SEEK_SET);
#else
  // POSIX, including macOS and Emscripten (musl's off_t is always 64-bit).
  return ::fseeko(f, static_cast<::off_t>(off), SEEK_SET);
#endif
}

inline std::int64_t tell64(std::FILE* f) noexcept {
#if defined(_MSC_VER)
  return _ftelli64(f);
#elif defined(_WIN32)
  return ftello64(f);
#else
  return ::ftello(f);
#endif
}

class stdio_reader {
 public:
  stdio_reader(const std::filesystem::path& path, const file_reader_options&,
               unsigned) {
    stream_ = std::fopen(path.string().c_str(), "rb");
    if (stream_ == nullptr) {
      throw_errno("fopen");
    }
    // Unchecked, these silently produce a nonsense size: a failed seek
    // leaves ftell returning -1, which as an unsigned size is ~18 EiB, and
    // the engine then computes a huge window count that fails obscurely.
    if (std::fseek(stream_, 0, SEEK_END) != 0) {
      throw_errno("fseek(end)");
    }
    const std::int64_t end = tell64(stream_);
    if (end < 0) {
      throw_errno("ftell");
    }
    size_ = static_cast<std::uint64_t>(end);
  }
  ~stdio_reader() {
    if (stream_ != nullptr) {
      std::fclose(stream_);
    }
  }
  stdio_reader(const stdio_reader&) = delete;
  stdio_reader& operator=(const stdio_reader&) = delete;

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::string_view name() const noexcept { return "stdio"; }
  [[nodiscard]] bool wants_async(std::uint64_t, std::size_t) const noexcept {
    return false;
  }

  void start(unsigned, std::uint64_t, std::span<std::byte>) {
    assert(false && "stdio backend has no async path");
  }
  void wait(unsigned) { assert(false && "stdio backend has no async path"); }

  void read_sync(std::uint64_t off, std::span<std::byte> buf) {
    if (seek64(stream_, off) != 0) {
      throw_errno("fseek");
    }
    if (std::fread(buf.data(), 1, buf.size(), stream_) != buf.size()) {
      // Distinguish a real read error from a short read at EOF; the engine
      // never asks for more than the file holds, so EOF here means the file
      // was truncated underneath us.
      throw std::system_error(std::ferror(stream_) != 0 ? errno : EIO,
                              std::generic_category(), "fread");
    }
  }

 private:
  std::FILE* stream_ = nullptr;
  std::uint64_t size_ = 0;
};

class stdio_writer {
 public:
  stdio_writer(const std::filesystem::path& path, const file_writer_options&,
               unsigned) {
    stream_ = std::fopen(path.string().c_str(), "wb");
    if (stream_ == nullptr) {
      throw_errno("fopen");
    }
  }
  ~stdio_writer() {
    if (stream_ != nullptr) {
      std::fclose(stream_);
    }
  }
  stdio_writer(const stdio_writer&) = delete;
  stdio_writer& operator=(const stdio_writer&) = delete;

  [[nodiscard]] std::string_view name() const noexcept { return "stdio"; }
  [[nodiscard]] bool wants_async(std::size_t) const noexcept { return false; }

  void start_write(unsigned, std::uint64_t, std::span<const std::byte>) {
    assert(false && "stdio backend has no async path");
  }
  void wait_slot(unsigned) {}  // nothing is ever in flight

  // Writes are strictly sequential (the engine's offset only grows), so
  // the stream position is already `off` and no seek is needed.
  void write_sync(std::uint64_t, std::span<const std::byte> buf) {
    if (std::fwrite(buf.data(), 1, buf.size(), stream_) != buf.size()) {
      throw std::system_error(EIO, std::generic_category(), "fwrite");
    }
  }

  void finish(std::uint64_t) {
    if (std::fflush(stream_) != 0) {
      throw_errno("fflush");
    }
  }

 private:
  std::FILE* stream_ = nullptr;
};

// Definition-site conformance check (see uring_backend.hpp).
static_assert(reader_backend<stdio_reader>);
static_assert(writer_backend<stdio_writer>);

}  // namespace blake3pp::detail::io_impl

namespace blake3pp::detail::io_impl {
using native_reader = stdio_reader;
using native_writer = stdio_writer;
}  // namespace blake3pp::detail::io_impl
#endif



// The two portable I/O engines, written exactly once as class templates
// constrained by the backend concepts (io/backend.hpp). The public
// file_reader/file_writer TUs instantiate them with the platform backend
// backend_select.hpp picks; the tests instantiate them again with the
// off-platform POSIX/stdio backends, so those stay compiled AND executed
// on every platform even though the selector never chooses them there.
// The constraint is the contract: an engine can only speak the concept's
// vocabulary, and a backend drifting from it fails at the instantiation
// with a diagnostic naming the missed requirement. Internal to src/io/,
// never installed.

#include <algorithm>
#include <string_view>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <system_error>
#include <vector>




namespace blake3pp::detail::io_impl {

// The read-side window/slot engine: windows are delivered strictly in
// file order while later windows stream in behind them; release()
// recycles a buffer slot, which is what creates backpressure. Every
// window is just "async in flight" (wait) or "read lazily at delivery"
// (read_sync); the backend decided which via wants_async().
template <reader_backend B>
class reader_engine {
 public:
  using window = file_reader::window;

  reader_engine(const std::filesystem::path& path,
                const file_reader_options& opts)
      // Window: power-of-2 multiple of the chunk size so every full window
      // is a subtree-aligned unit; >= 64 KiB keeps O_DIRECT alignment
      // trivial.
      : window_(std::bit_floor(
            std::max<std::size_t>(opts.window_bytes, 64 * 1024))),
        qd_(std::min(32u, std::max(2u, opts.queue_depth))),
        slots_(qd_),
        pool_(std::size_t{qd_} * window_),
        backend_(path, opts, qd_) {
    num_windows_ = (backend_.size() + window_ - 1) / window_;
    const std::uint64_t initial = std::min<std::uint64_t>(qd_, num_windows_);
    for (unsigned s = 0; s < initial; ++s) {
      assign(s);
    }
  }

  [[nodiscard]] std::uint64_t file_size() const noexcept {
    return backend_.size();
  }
  [[nodiscard]] std::string_view backend_name() const noexcept {
    return backend_.name();
  }

  std::optional<window> next() {
    // A submission that fails inside release() cannot be reported there:
    // release() is noexcept because callers pair it with next() in a tight
    // loop, and letting it throw would call std::terminate, including
    // from hash_file(path, ec, opts), whose whole contract is to turn I/O
    // failures into an error_code. So the failure is latched here instead,
    // in the function already documented as throwing. The latch is
    // permanent: the slot whose submission failed holds a window that can
    // never be delivered, so there is no way to continue reading.
    if (submit_failed_) {
      std::rethrow_exception(submit_failed_);
    }
    if (next_deliver_ >= num_windows_) {
      return std::nullopt;
    }
    const std::uint64_t want = next_deliver_;
    unsigned s = 0;
    for (; s < qd_; ++s) {
      if (slots_[s].assigned && slots_[s].win == want) {
        break;
      }
    }
    assert(s < qd_);  // release() reassigns eagerly, so `want` has a slot
    slot_state& st = slots_[s];
    if (st.started) {
      backend_.wait(s);
    } else {
      backend_.read_sync(want * window_, {buf(s), st.target});
    }
    st.held = true;
    next_deliver_++;
    return window{buf(s), st.target, want * window_,
                  want + 1 == num_windows_, s};
  }

  void release(const window& w) noexcept {
    slot_state& st = slots_[w.slot];
    st.assigned = false;
    st.held = false;
    if (next_submit_ < num_windows_ && !submit_failed_) {
      // assign() submits real I/O (io_uring_enter / ReadFile), which can
      // fail. Latch it for next() to rethrow; see the note there.
      try {
        assign(w.slot);
      } catch (...) {
        submit_failed_ = std::current_exception();
      }
    }
  }

 private:
  struct slot_state {
    std::uint64_t win = 0;   // window index assigned to this slot
    std::size_t target = 0;  // bytes this window must read
    bool assigned = false;
    bool started = false;  // async read in flight (wait) vs lazy (read_sync)
    bool held = false;     // delivered, not yet released
  };

  std::size_t window_len(std::uint64_t w) const noexcept {
    const std::uint64_t off = w * window_;
    const std::uint64_t rest = backend_.size() - off;
    return rest < window_ ? static_cast<std::size_t>(rest) : window_;
  }

  std::byte* buf(unsigned slot) const noexcept {
    return pool_.data + static_cast<std::size_t>(slot) * window_;
  }

  void assign(unsigned s) {
    slot_state& st = slots_[s];
    st.win = next_submit_++;
    st.target = window_len(st.win);
    st.assigned = true;
    st.held = false;
    st.started = backend_.wants_async(st.win * window_, st.target);
    if (st.started) {
      backend_.start(s, st.win * window_, {buf(s), st.target});
    }
    // Slots the backend declined are read synchronously at delivery time.
  }

  std::size_t window_;
  unsigned qd_;
  std::uint64_t num_windows_ = 0;
  std::uint64_t next_submit_ = 0;   // next window index to assign to a slot
  std::uint64_t next_deliver_ = 0;  // next window index to hand out
  std::exception_ptr submit_failed_;  // latched by release(), thrown by next()
  std::vector<slot_state> slots_;

  // Declaration order is the teardown contract: the backend destructs
  // FIRST, draining any in-flight reads that target the pool, and the
  // pool is freed after. Do not reorder these two members.
  aligned_pool pool_;
  B backend_;
};

// The write-side slot engine, the reader's inverse: the producer fills
// buffers ahead of the device, and acquire() blocking on a slot whose
// write is still in flight is the entire backpressure story. The
// unaligned tail (only the final submit may be one) always goes through
// the backend's synchronous buffered path, because O_DIRECT and
// NO_BUFFERING both reject unaligned lengths; the backends that don't
// care route it the same way for uniformity.
template <writer_backend B>
class writer_engine {
 public:
  using buffer = file_writer::buffer;

  writer_engine(const std::filesystem::path& path,
                const file_writer_options& opts)
      : buffer_(rounded_buffer(opts.buffer_bytes)),
        qd_(std::min(32u, std::max(2u, opts.queue_depth))),
        pool_(std::size_t{qd_} * buffer_),
        backend_(path, opts, qd_) {}

  [[nodiscard]] std::uint64_t bytes_written() const noexcept {
    return written_;
  }
  [[nodiscard]] std::string_view backend_name() const noexcept {
    return backend_.name();
  }

  buffer acquire() {
    const unsigned s = next_slot_;
    backend_.wait_slot(s);
    return buffer{buf(s), buffer_, s};
  }

  void submit(const buffer& b, std::size_t bytes) {
    if (bytes == 0) {
      return;
    }
    if (tail_submitted_) {
      throw std::system_error(EINVAL, std::generic_category(),
                              "submit after partial write");
    }
    if (bytes % direct_align != 0) {
      tail_submitted_ = true;
    }
    const std::span<const std::byte> data{b.data, bytes};
    if (backend_.wants_async(bytes)) {
      backend_.start_write(b.slot, offset_, data);
    } else {
      backend_.write_sync(offset_, data);
    }
    offset_ += bytes;
    written_ += bytes;
    next_slot_ = (b.slot + 1) % qd_;
  }

  void finish() { backend_.finish(written_); }

 private:
  // Round up to the O_DIRECT length granule; >= 64 KiB so queued writes
  // are worth their submission cost.
  static std::size_t rounded_buffer(std::size_t bytes) noexcept {
    bytes = std::max<std::size_t>(bytes, 64 * 1024);
    return (bytes + direct_align - 1) / direct_align * direct_align;
  }

  std::byte* buf(unsigned slot) const noexcept {
    return pool_.data + static_cast<std::size_t>(slot) * buffer_;
  }

  std::size_t buffer_;
  unsigned qd_;
  unsigned next_slot_ = 0;       // round-robin acquire order
  std::uint64_t offset_ = 0;     // next sequential file offset
  std::uint64_t written_ = 0;    // total bytes accepted via submit()
  bool tail_submitted_ = false;  // a partial submit closes the stream

  // Declaration order is the teardown contract: the backend destructs
  // FIRST, draining any in-flight writes that read from the pool, and the
  // pool is freed after. Do not reorder these two members.
  aligned_pool pool_;
  B backend_;
};

}  // namespace blake3pp::detail::io_impl


namespace blake3pp::detail {

// Conformance is checked in the backend headers themselves (each ends
// with definition-site static_asserts) and again by the reader_engine
// constraint at this instantiation.
struct file_reader::impl : io_impl::reader_engine<io_impl::native_reader> {
  using reader_engine::reader_engine;
};

file_reader::file_reader(const std::filesystem::path& path,
                         const file_reader_options& opts)
    : impl_(std::make_unique<impl>(path, opts)) {}

file_reader::~file_reader() = default;

std::uint64_t file_reader::file_size() const noexcept {
  return impl_->file_size();
}

std::string_view file_reader::backend() const noexcept {
  return impl_->backend_name();
}

std::optional<file_reader::window> file_reader::next() {
  return impl_->next();
}

void file_reader::release(const window& w) noexcept { impl_->release(w); }

}  // namespace blake3pp::detail


#include <cstddef>
#include <filesystem>
#include <span>
#include <system_error>

namespace blake3pp {

void update_file(hasher& h, const std::filesystem::path& path,
                 const file_io_options& opts) {
  detail::file_reader reader(
      path, {opts.window_bytes, opts.queue_depth, opts.direct_io, true,
             opts.offload_submit});
  while (auto w = reader.next()) {
    h.update(std::span<const std::byte>{w->data, w->bytes});
    reader.release(*w);
  }
}

void update_file(hasher& h, const std::filesystem::path& path,
                 std::error_code& ec, const file_io_options& opts) noexcept {
  detail::with_error_code(ec, [&] { update_file(h, path, opts); });
}

digest hash_file(const std::filesystem::path& path,
                 const hash_file_options& opts) {
  hasher h = detail::make_hasher(opts, detail::resolve(opts.a));
  update_file(h, path, opts);
  return h.finalize();
}

digest hash_file(const std::filesystem::path& path, std::error_code& ec,
                 const hash_file_options& opts) noexcept {
  return detail::with_error_code(ec, [&] { return hash_file(path, opts); });
}

}  // namespace blake3pp


#if defined(__has_include) && __has_include(<beman/execution/execution.hpp>)
#define BLAKE3PP_EXECUTION_BEMAN 1
#define BLAKE3PP_HAS_STD_THREAD 1
#ifndef BEMAN_EXECUTION_WITH_DEFAULT_PARALLEL_SCHEDULER_BACKEND
#define BEMAN_EXECUTION_WITH_DEFAULT_PARALLEL_SCHEDULER_BACKEND 1
#endif
#define BLAKE3PP_AMALGAM_HAS_PARALLEL 1


/// @file
/// Parallel BLAKE3 over the sender/receiver model.
///
/// BLAKE3's binary Merkle tree makes the parallel decomposition exact, not
/// heuristic: any power-of-2, position-aligned run of chunks reduces to
/// one chaining value independently of everything else. So the engine
/// partitions the input into equal such subtrees, which the scheduler's
/// execution agents pull from a shared counter until none are left (a
/// slow agent takes fewer, rather than holding up the join), then absorbs
/// the CVs in order through the hasher's CV-stack discipline and finishes
/// the tail sequentially. The merge work after the parallel phase is
/// O(parts) scalar compressions, which is noise.
///
/// The provider is a build-time choice (BLAKE3PP_EXECUTION_PROVIDER):
/// std::execution where the standard library ships it, beman.execution as
/// the conformance-first polyfill, NVIDIA stdexec as the performance
/// workhorse (same source, same story as the simd providers). No heap
/// allocations in this header: the CV table lives on the caller's stack
/// and sender operation states live inside sync_wait's frame.

#include <algorithm>
#include <atomic>
#include <bit>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>


#if defined(BLAKE3PP_EXECUTION_STD)
#include <execution>
#elif defined(BLAKE3PP_EXECUTION_BEMAN)
#include <beman/execution/execution.hpp>
#else  // BLAKE3PP_EXECUTION_STDEXEC
#include <stdexec/execution.hpp>
#if BLAKE3PP_HAS_STD_THREAD
#include <exec/static_thread_pool.hpp>
#endif
#endif

namespace blake3pp {

/// The sender/receiver vocabulary this build uses (std::execution,
/// beman::execution or stdexec), so the library and its callers spell
/// schedule, bulk and sync_wait the same way whichever provider is built.
namespace ex {
#if defined(BLAKE3PP_EXECUTION_STD)
using namespace std::execution;
using std::this_thread::sync_wait;
#elif defined(BLAKE3PP_EXECUTION_BEMAN)
using namespace beman::execution;
#else
using namespace stdexec;
#endif
}  // namespace ex

// The process-wide parallel scheduler, in P2079's shape: the same call
// C++26 application code makes. Calling it is the explicit opt-in that may
// create the process pool (provider-dependent). The scheduler-taking hash
// overloads below stay the primary, caller-controlled API, and anyone who
// needs a sized or bounded pool constructs their provider's pool directly.
//
// parallel_scheduler_t is a per-build concrete type, not type-erased: the
// provider is fixed at configure time, so dispatch stays fully inlinable.
#if defined(BLAKE3PP_EXECUTION_STD)

/// The type of the process-wide scheduler: a per-build concrete type, not
/// type-erased, so dispatch stays fully inlinable.
using parallel_scheduler_t = decltype(std::execution::get_parallel_scheduler());
/// The process-wide parallel scheduler, in P2079's shape.
///
/// The same call C++26 application code makes. Calling it is the explicit
/// opt-in that may create the process pool. The scheduler-taking overloads
/// stay the primary, caller-controlled API; a sized or bounded pool is the
/// provider's own, passed in directly.
[[nodiscard]] inline parallel_scheduler_t get_parallel_scheduler() {
  return std::execution::get_parallel_scheduler();
}

#elif defined(BLAKE3PP_EXECUTION_BEMAN)

/// The type of the process-wide scheduler: a per-build concrete type, not
/// type-erased, so dispatch stays fully inlinable.
using parallel_scheduler_t = beman::execution::parallel_scheduler;
/// The process-wide parallel scheduler, in P2079's shape.
///
/// Backed by beman.execution's own parallel_scheduler backend. A program
/// may replace it by defining query_parallel_scheduler_backend() itself:
/// P2079 replaceability, one definition per program, like a global
/// allocator.
[[nodiscard]] inline parallel_scheduler_t get_parallel_scheduler() {
  return beman::execution::get_parallel_scheduler();
}

#elif BLAKE3PP_HAS_STD_THREAD  // BLAKE3PP_EXECUTION_STDEXEC

namespace detail {
// The size process_pool() will be built with, 0 meaning every core. Only
// <blake3pp/parallel_backend.hpp>'s size_parallel_scheduler() writes it,
// and only before the pool exists; the flag below is how it knows.
inline std::atomic<unsigned>& process_pool_threads() noexcept {
  static std::atomic<unsigned> threads{0};
  return threads;
}
inline std::atomic<bool>& process_pool_started() noexcept {
  static std::atomic<bool> started{false};
  return started;
}

// Function-local static: constructed on first use, threads joined during
// static destruction; the same lifetime the standard's parallel scheduler
// has.
inline exec::static_thread_pool& process_pool() {
  static exec::static_thread_pool pool{[] {
    process_pool_started().store(true, std::memory_order_relaxed);
    const unsigned n = process_pool_threads().load(std::memory_order_relaxed);
    return n != 0 ? n : std::thread::hardware_concurrency();
  }()};
  return pool;
}
}  // namespace detail

/// The type of the process-wide scheduler: a per-build concrete type, not
/// type-erased, so dispatch stays fully inlinable.
using parallel_scheduler_t =
    decltype(detail::process_pool().get_scheduler());
/// The process-wide parallel scheduler, in P2079's shape.
///
/// The same call C++26 application code makes. The first call creates the
/// process pool (one thread per hardware thread), joined during static
/// destruction. The scheduler-taking overloads stay the primary,
/// caller-controlled API; a sized or bounded pool is the provider's own
/// (exec::static_thread_pool pool(8); pool.get_scheduler()).
[[nodiscard]] inline parallel_scheduler_t get_parallel_scheduler() {
  return detail::process_pool().get_scheduler();
}

#endif  // no process pool without std::thread: see BLAKE3PP_HAS_STD_THREAD.
        // The scheduler-taking hash overloads below are unaffected, and are
        // the primary API in any case -- a freestanding caller brings its
        // own scheduler because only it knows what its agents should be.

namespace detail {
// Deliberately not constexpr and never defined: a stack_budget constructor
// that reaches one fails its constant evaluation, and the diagnostic names
// the violated rule.
void stack_budget_not_a_multiple_of_32_bytes();
void stack_budget_below_two_parts();
}  // namespace detail

/// The stack the multi-core functions may spend on their table of part
/// chaining values, passed as their first template argument:
///
/// @code
/// blake3pp::hash<blake3pp::stack_budget{1024}>(input, sched);   // 32 parts
/// @endcode
///
/// The input is split into at most parts() parts, each with a 32-byte
/// chaining value in a table on the calling thread's stack, whatever the
/// input's size. Agents pull parts from a shared counter, so a slow agent
/// takes fewer instead of holding up the join, and more parts balance more
/// finely; a smaller budget splits the input into fewer, larger parts,
/// which suits a machine with few cores.
///
/// A budget that is not a multiple of 32 bytes, or that holds fewer than
/// two parts, does not compile. There is no upper limit, because the stack
/// the calling thread has is not known where this header is compiled; the
/// platform, the linker, the thread's creator or the application's
/// configuration sets it. Code that knows its thread checks the budget
/// against that at compile time, leaving room for its own frames:
///
/// @code
/// constexpr blake3pp::stack_budget budget{1024};
/// static_assert(budget.bytes <= CONFIG_MAIN_STACK_SIZE / 4);   // e.g. on Zephyr
/// @endcode
///
/// The budget covers this table alone. Each part is reduced on the agent
/// that took it, by a recursion holding one chaining-value buffer per level
/// (2 KiB while a 16-wide kernel is compiled in: the buffer is sized for
/// the widest compiled variant, not the running one), as deep as halving
/// the part takes to reach twice the running variant's degree in chunks. A
/// smaller budget makes parts larger and that recursion deeper, so agent
/// threads need stack of their own.
struct stack_budget {
  /// The stack one part's chaining value takes.
  static constexpr std::size_t bytes_per_part = 32;

  /// The budget in bytes.
  std::size_t bytes;

  /// @param n  A multiple of bytes_per_part, at least two parts' worth.
  consteval explicit stack_budget(std::size_t n) : bytes{n} {
    if (n % bytes_per_part != 0) {
      detail::stack_budget_not_a_multiple_of_32_bytes();
    }
    if (n / bytes_per_part < 2) {
      detail::stack_budget_below_two_parts();
    }
  }

  /// The most parts the input is split into.
  [[nodiscard]] constexpr std::size_t parts() const noexcept {
    return bytes / bytes_per_part;
  }
};

/// The default budget: 32 KiB of stack, 1024 parts.
inline constexpr stack_budget default_stack_budget{32 * 1024};

namespace detail {

// A part is at least 16 chunks (16 KiB), which keeps small inputs in few
// tasks.
inline constexpr std::size_t min_part_chunks = 16;

// The part size in chunks for num_chunks: a power of two, so every part
// starting at a multiple of it is subtree-aligned, and large enough that
// num_chunks / part <= Budget.parts().
template <stack_budget Budget>
[[nodiscard]] constexpr std::size_t part_chunks(std::size_t num_chunks) noexcept {
  return std::bit_ceil(std::max(
      (num_chunks + Budget.parts() - 1) / Budget.parts(), min_part_chunks));
}

// One CV slot per part, on the caller's stack. The slots are unpadded:
// each is written once per part-sized task, so neighbours sharing a cache
// line cost nothing measurable.
template <stack_budget Budget>
using part_cvs = std::array<std::array<std::uint32_t, 8>, Budget.parts()>;
static_assert(sizeof(std::array<std::uint32_t, 8>) == stack_budget::bytes_per_part);

// Runs body(i) for every i in [0, n) on sched. The bulk shape only
// provides the agents: each call pulls indices from a shared counter until
// none are left, so the split follows each agent's actual speed rather
// than the fixed shares the provider's bulk may hand out. Every index runs
// exactly once however the implementation distributes the calls.
template <class Scheduler, class Body>
void for_each_part(Scheduler& sched, std::size_t n, Body body) {
  std::atomic<std::size_t> next{0};
  auto work = ex::schedule(sched) |
              ex::bulk(ex::par, n, [&](std::size_t) noexcept {
                for (std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                     i < n; i = next.fetch_add(1, std::memory_order_relaxed)) {
                  body(i);
                }
              });
  ex::sync_wait(std::move(work));
}

// The one-shot engine: partitions input into aligned subtrees, fans them
// out over sched, and finishes inside h, whose key_words() and
// mode_flags() drive the workers. Plain, keyed and derive_key hashers all
// work.
template <stack_budget Budget, class Scheduler>
[[nodiscard]] digest hash_into(hasher& h, std::span<const std::byte> input,
                               Scheduler&& sched,
                               const kern::kernel_ops* ops) {
  // Only chunks with at least one byte after them may be offloaded: the
  // message's final chunk must stay with the hasher for ROOT finalization.
  const std::size_t safe_chunks =
      input.size() > chunk_size ? (input.size() - 1) / chunk_size : 0;

  if (safe_chunks >= 2 * min_part_chunks) {
    const std::size_t part = part_chunks<Budget>(safe_chunks);
    const std::size_t n_parts = safe_chunks / part;  // <= Budget.parts()
    part_cvs<Budget> cvs;
    const std::byte* const base = input.data();

    // Starting from counter 0 in part-sized steps, every part is
    // automatically subtree-aligned.
    for_each_part(sched, n_parts, [&](std::size_t i) noexcept {
      detail::compress_subtree_cv(ops, base + i * part * chunk_size, part,
                                  static_cast<std::uint64_t>(i) * part,
                                  h.key_words(), h.mode_flags(), cvs[i]);
    });

    for (std::size_t i = 0; i < n_parts; ++i) {
      h.push_subtree_cv(cvs[i], part);
    }
    h.update(input.subspan(n_parts * part * chunk_size));
    return h.finalize();
  }

  h.update(input);
  return h.finalize();
}

}  // namespace detail

// The multi-core one-shots: core.hpp's hash / keyed_hash / derive_key
// family with a scheduler added, same spellings, same string_view
// conveniences. Every template is constrained on the scheduler concept
// so none of them can hijack a core overload (an arch enum or a
// string_view in the scheduler's position simply fails to match).

/// Expert: multi-core hash on a caller-supplied kernel table, the same
/// seam hasher's expert constructor exposes.
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param input  Any length.
/// @param sched  Where the subtree reductions run.
/// @param ops    The kernel table; must outlive the call.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash(std::span<const std::byte> input, Scheduler&& sched,
                          const kern::kernel_ops* ops) {
  hasher h{ops};
  return detail::hash_into<Budget>(h, input, std::forward<Scheduler>(sched),
                                   ops);
}

/// Multi-core one-shot hash: the subtree reductions of input run on sched,
/// and the digest is identical to the sequential hash(input).
///
/// Any std::execution-style scheduler works; inputs too small for
/// parallelism to pay for itself take the sequential path.
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param input  Any length.
/// @param sched  Where the subtree reductions run.
/// @param a      The variant to run on.
///
/// @code
/// auto sched = blake3pp::get_parallel_scheduler();
/// blake3pp::digest d = blake3pp::hash(big_buffer, sched);
/// @endcode
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash(std::span<const std::byte> input, Scheduler&& sched,
                          arch a = arch::auto_detect) {
  return hash<Budget>(input, std::forward<Scheduler>(sched),
                      detail::resolve(a));
}

/// Multi-core one-shot hash of a string's bytes.
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param input  The bytes of the string.
/// @param sched  Where the subtree reductions run.
/// @param a      The variant to run on.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest hash(std::string_view input, Scheduler&& sched,
                          arch a = arch::auto_detect) {
  return hash<Budget>(std::as_bytes(std::span{input.data(), input.size()}),
                      std::forward<Scheduler>(sched), a);
}

/// Multi-core keyed one-shot: the MAC/PRF of input under a 32-byte key,
/// same decomposition as hash().
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param key    Exactly key_size bytes, enforced by the span extent.
/// @param input  Any length.
/// @param sched  Where the subtree reductions run.
/// @param a      The variant to run on.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest keyed_hash(std::span<const std::byte, key_size> key,
                                std::span<const std::byte> input,
                                Scheduler&& sched,
                                arch a = arch::auto_detect) {
  // Reuse the ops overload's partitioning by seeding it with a keyed
  // hasher: the engine takes key material from the hasher itself.
  const kern::kernel_ops* const ops = detail::resolve(a);
  hasher h = hasher::keyed(key, ops);
  return detail::hash_into<Budget>(h, input, std::forward<Scheduler>(sched),
                                   ops);
}

/// Multi-core keyed one-shot of a string's bytes.
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param key    Exactly key_size bytes.
/// @param input  The bytes of the string.
/// @param sched  Where the subtree reductions run.
/// @param a      The variant to run on.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest keyed_hash(std::span<const std::byte, key_size> key,
                                std::string_view input, Scheduler&& sched,
                                arch a = arch::auto_detect) {
  return keyed_hash<Budget>(
      key, std::as_bytes(std::span{input.data(), input.size()}),
      std::forward<Scheduler>(sched), a);
}

/// Multi-core key derivation, for key material large enough to matter (a
/// file's worth of entropy, a whole seed image); see core.hpp's
/// derive_key() for the context contract.
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param context       The domain-separation string; not a secret.
/// @param key_material  The secret to derive from.
/// @param sched         Where the subtree reductions run.
/// @param a             The variant to run on.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest derive_key(std::string_view context,
                                std::span<const std::byte> key_material,
                                Scheduler&& sched,
                                arch a = arch::auto_detect) {
  const kern::kernel_ops* const ops = detail::resolve(a);
  hasher h = hasher::derive_key(context, ops);
  return detail::hash_into<Budget>(
      h, key_material, std::forward<Scheduler>(sched), ops);
}

/// Multi-core key derivation from a string's bytes.
/// @tparam Budget     The stack its part table may take; see stack_budget.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param context       The domain-separation string; not a secret.
/// @param key_material  The secret to derive from.
/// @param sched         Where the subtree reductions run.
/// @param a             The variant to run on.
template <stack_budget Budget = default_stack_budget, class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
[[nodiscard]] digest derive_key(std::string_view context,
                                std::string_view key_material,
                                Scheduler&& sched,
                                arch a = arch::auto_detect) {
  return derive_key<Budget>(
      context,
      std::as_bytes(std::span{key_material.data(), key_material.size()}),
      std::forward<Scheduler>(sched), a);
}

namespace detail {

// Fans one full window (num_chunks: power of two, counter-aligned) out
// over the scheduler and absorbs the part CVs in order.
template <stack_budget Budget, class Scheduler>
void hash_window_parallel(const kern::kernel_ops* ops, Scheduler& sched,
                          hasher& h, const std::byte* data,
                          std::size_t num_chunks,
                          std::uint64_t chunk_counter) {
  const std::size_t part = part_chunks<Budget>(num_chunks);
  if (part >= num_chunks) {
    // Window too small to fan out; hash it inline.
    h.update(std::span<const std::byte>{data, num_chunks * chunk_size});
    return;
  }
  const std::size_t n_parts = num_chunks / part;
  part_cvs<Budget> cvs;

  for_each_part(sched, n_parts, [&](std::size_t i) noexcept {
    compress_subtree_cv(ops, data + i * part * chunk_size, part,
                        chunk_counter + i * part, h.key_words(),
                        h.mode_flags(), cvs[i]);
  });
  for (std::size_t i = 0; i < n_parts; ++i) {
    h.push_subtree_cv(cvs[i], part);
  }
}

}  // namespace detail

/// parallel_hasher's knobs.
struct parallel_hasher_options {
  /// The SIMD variant of the internal hasher.
  arch a = arch::auto_detect;
  /// Bytes accumulated before a window is fanned out; rounded down to a
  /// power-of-2 multiple of chunk_size, minimum 64 KiB. Buffered input
  /// below one window hashes sequentially at finalize().
  std::size_t window_bytes = 8 * 1024 * 1024;
};

/// The incremental counterpart of the multi-core hash(). It offers the
/// same `update()`, `finalize()` and `reset()` interface as hasher, and
/// fans the subtree hashing out over a scheduler internally.
///
/// Input accumulates into an aligned window; a full window is fanned out
/// as soon as one more byte arrives, the "one byte in reserve" that keeps
/// BLAKE3's final chunk with the hasher for ROOT finalization. All
/// alignment and final-chunk discipline lives here, not with the caller,
/// and the digest equals the sequential one. The window buffer is the
/// type's one allocation, made at construction. finalize() is
/// non-destructive, like hasher's. Not thread-safe; the scheduler's
/// workers are used only inside update().
/// @tparam Scheduler  Any std::execution-style scheduler, held by value.
/// @tparam Budget     The stack a window's part table may take; see
///                    stack_budget.
///
/// @code
/// blake3pp::parallel_hasher ph{blake3pp::get_parallel_scheduler()};
/// while (auto block = source.next_block()) {
///   ph.update(*block);
/// }
/// blake3pp::digest d = ph.finalize();   // == the sequential digest
/// @endcode
template <class Scheduler, stack_budget Budget = default_stack_budget>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
class parallel_hasher {
 public:
  /// Plain mode.
  /// @param sched  Where the subtree reductions run.
  /// @param opts   The variant and the window size.
  explicit parallel_hasher(Scheduler sched,
                           const parallel_hasher_options& opts = {})
      : sched_(std::move(sched)),
        ops_(detail::resolve(opts.a)),
        h_(ops_),
        window_(std::bit_floor(
            std::max<std::size_t>(opts.window_bytes, 64 * 1024))) {}

  /// Keyed (MAC/PRF) mode.
  /// @param sched  Where the subtree reductions run.
  /// @param key    Exactly key_size bytes, enforced by the span extent.
  /// @param opts   The variant and the window size.
  parallel_hasher(Scheduler sched, std::span<const std::byte, key_size> key,
                  const parallel_hasher_options& opts = {})
      : sched_(std::move(sched)),
        ops_(detail::resolve(opts.a)),
        h_(hasher::keyed(key, ops_)),
        window_(std::bit_floor(
            std::max<std::size_t>(opts.window_bytes, 64 * 1024))) {}

  /// Key-derivation mode: the input is the key material, domain-separated
  /// by context (see hasher::derive_key).
  /// @param sched    Where the subtree reductions run.
  /// @param context  The domain-separation string; not a secret.
  /// @param opts     The variant and the window size.
  parallel_hasher(Scheduler sched, std::string_view context,
                  const parallel_hasher_options& opts = {})
      : sched_(std::move(sched)),
        ops_(detail::resolve(opts.a)),
        h_(hasher::derive_key(context, ops_)),
        window_(std::bit_floor(
            std::max<std::size_t>(opts.window_bytes, 64 * 1024))) {}

  /// Absorbs the next bytes of the message; complete windows are fanned
  /// out over the scheduler from here.
  /// @param input  Any length, including zero.
  void update(std::span<const std::byte> input) {
    const std::byte* p = input.data();
    std::size_t len = input.size();
    while (len > 0) {
      if (filled_ == window_.size()) {
        // More input exists, so the buffered window is provably not the
        // message's end: safe to offload.
        flush_window();
      }
      const std::size_t take = std::min(window_.size() - filled_, len);
      std::copy_n(p, take, window_.data() + filled_);
      filled_ += take;
      p += take;
      len -= take;
    }
  }

  /// Absorbs the next bytes of the message, given as text.
  /// @param input  The bytes of the string, not including any terminator.
  void update(std::string_view input) {
    update(std::as_bytes(std::span{input.data(), input.size()}));
  }

  /// The digest of everything absorbed so far; the hasher stays usable.
  [[nodiscard]] digest finalize() const { return drained().finalize(); }

  /// Extended output: fills out with the first out.size() bytes of the
  /// output stream.
  /// @param out  Any length.
  void finalize(std::span<std::byte> out) const { drained().finalize(out); }

  /// Extended output by value: the first N bytes of the output stream.
  /// @tparam N  The number of bytes to return.
  template <std::size_t N>
  [[nodiscard]] std::array<std::byte, N> finalize() const {
    return drained().template finalize<N>();
  }

  /// Extended output as a seekable stream, independent of this
  /// parallel_hasher afterwards.
  [[nodiscard]] output_reader finalize_xof() const {
    return drained().finalize_xof();
  }

  /// Returns the hasher to its just-constructed state, keeping its mode,
  /// key, variant and window.
  void reset() noexcept {
    h_.reset();
    filled_ = 0;
    chunk_counter_ = 0;
  }

  /// Total bytes absorbed since construction or reset.
  [[nodiscard]] std::uint64_t count() const noexcept {
    return h_.count() + filled_;
  }

 private:
  // The finalize seam: a copy of the flat internal hasher with the
  // buffered tail absorbed. Copying is what keeps every finalize form
  // const and non-destructive, exactly like hasher's.
  [[nodiscard]] hasher drained() const {
    hasher h = h_;
    h.update(std::span<const std::byte>{window_.data(), filled_});
    return h;
  }

  void flush_window() {
    const std::size_t chunks = window_.size() / chunk_size;
    detail::hash_window_parallel<Budget>(ops_, sched_, h_, window_.data(),
                                         chunks, chunk_counter_);
    chunk_counter_ += chunks;
    filled_ = 0;
  }

  Scheduler sched_;
  const kern::kernel_ops* ops_;
  hasher h_;
  std::vector<std::byte> window_;
  std::size_t filled_ = 0;
  std::uint64_t chunk_counter_ = 0;
};

/// Fills a buffer from an output reader on every core: the request is
/// split into segments, and each task copies the reader, seeks its own
/// segment and fills it straight into the caller's buffer.
///
/// Extended output is seekable in O(1), which makes this exact. r advances
/// past out afterwards exactly as r.fill(out) would have. Requests of one
/// segment or less take the sequential path.
/// @tparam Scheduler  Any std::execution-style scheduler.
/// @param r              The reader to advance.
/// @param out            Receives the next out.size() bytes of r's stream.
/// @param sched          Where the segments are filled.
/// @param segment_bytes  Bytes per task; rounded down to a multiple of
///                       block_size so every task starts on the wide path.
///                       The default matches a generator's natural write
///                       granularity.
template <class Scheduler>
  requires ex::scheduler<std::remove_cvref_t<Scheduler>>
void fill(output_reader& r, std::span<std::byte> out, Scheduler&& sched,
          std::size_t segment_bytes = 4 * 1024 * 1024) {
  const std::size_t segment = std::max(
      segment_bytes - segment_bytes % block_size, block_size);
  if (out.size() <= segment) {
    r.fill(out);
    return;
  }
  const std::uint64_t base = r.position();
  const std::size_t n_segs = (out.size() + segment - 1) / segment;
  auto work = ex::schedule(sched) |
              ex::bulk(ex::par, n_segs, [&](std::size_t i) noexcept {
                output_reader part = r;
                const std::size_t off = i * segment;
                part.seek(base + off);
                part.fill(out.subspan(off, std::min(segment, out.size() - off)));
              });
  ex::sync_wait(std::move(work));
  r.seek(base + out.size());
}

}  // namespace blake3pp

#endif  // beman.execution


// ---------------------------------------------------------------------
// The demo. Delete from here down to use the file as a library.
#include <array>
#include <print>
#include <string>
#include <vector>

int main() {
  std::string carried;
  for (auto a : blake3pp::compiled_arches()) {
    carried += blake3pp::to_string(a);
    carried += ' ';
  }
  std::println("blake3pp {} · simd {} · carries {}· auto picks {}",
               blake3pp::version(), blake3pp::simd_provider(), carried,
               blake3pp::to_string(blake3pp::hasher{}.selected_arch()));

  // One shot, and the digest as hex.
  std::println("\"abc\"      {}", blake3pp::hash("abc").to_hex());

  // Incremental: finalize() does not consume the hasher.
  blake3pp::hasher h;
  h.update("a");
  h.update("bc");
  std::println("streamed   {}", h.finalize().to_hex());

  // Keyed, for a MAC, and a context-separated subkey.
  std::array<std::byte, 32> key{};
  std::println("keyed      {}", blake3pp::keyed_hash(key, "abc").to_hex());
  std::println("derived    {}", blake3pp::derive_key("example 2026", "abc").to_hex());

  // Extended output, seekable in constant time.
  auto r = blake3pp::hasher{}.finalize_xof();
  r.seek(1'000'000'000);
  auto far = r.take<16>();
  std::println("byte 1e9   {}", blake3pp::to_hex(far));
}
