// Runtime architecture routing. Selection stays a plain pointer to a
// constexpr-initialized POD table: no heap, no vtable, no ifunc.
//
// Which variants exist in this binary is not hand-maintained here: the
// build system generates blake3pp_kernel_registry.inc from the
// blake3pp_add_kernel() calls, and the X-macro expansions below turn it
// into the extern declarations and the dispatch/query tables. Register a
// kernel in CMake and it shows up everywhere; there is no second list to
// forget to update.
//
// On x86 the CPU check is a self-contained cpuid + xgetbv probe with the
// OSXSAVE/XCR0 check: "avx2" is only reported when the OS actually saves
// YMM state, not merely when the CPU has the silicon. (The
// __builtin_cpu_supports shortcut was retired: it drags libgcc/
// compiler-rt's __cpu_model machinery into the link, which is absent
// under lld-link on Windows AND under zig/musl static linking.) On
// AArch64, NEON is architecturally mandatory, so presence of the kernel
// implies availability.

#include <blake3pp/dispatch.hpp>

#include "blake3pp_version_stamp.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>

#include "kernel/kernel.hpp"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || \
    defined(_M_IX86)
// One manual cpuid/xgetbv probe for ALL x86 toolchains. The tempting
// alternative, __builtin_cpu_supports, references compiler-rt/libgcc's
// __cpu_model support machinery, a link-time dependency that failed us
// twice (clang-cl with lld-link on Windows; zig/musl static linking):
// the builtin is only as portable as the runtime library du jour. The
// manual probe is self-contained and does the same OSXSAVE/XCR0 dance.
#define BLAKE3PP_X86_CPUID 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#if defined(__aarch64__) && defined(__linux__)
// SVE probing: auxv HWCAP bits for presence, prctl for the runtime vector
// length. Both are plain syscall surfaces, usable from this flag-neutral
// TU (no SVE codegen needed, unlike an rdvl/svcntb read) and emulated
// faithfully by qemu-user. Constants carry #ifndef fallbacks because musl
// and older glibc auxv/prctl headers do not spell them all.
#define BLAKE3PP_AARCH64_LINUX_SVE 1
#include <sys/auxv.h>
#include <sys/prctl.h>
#ifndef HWCAP_SVE
#define HWCAP_SVE (1UL << 22)
#endif
#ifndef HWCAP2_SVE2
#define HWCAP2_SVE2 (1UL << 1)
#endif
#ifndef PR_SVE_GET_VL
#define PR_SVE_GET_VL 51
#endif
#ifndef PR_SVE_VL_LEN_MASK
#define PR_SVE_VL_LEN_MASK 0xffff
#endif
#endif

#if defined(__riscv) && __riscv_xlen == 64 && defined(__linux__)
// RVV probing: the auxv HWCAP bit for the single-letter 'V' extension
// (bits 0-25 map A-Z), then the vlenb CSR for the vector length in bytes.
// Reading a vector CSR needs V enabled at the ASSEMBLER level; the
// .option push/arch/pop dance scopes that to one instruction so this
// flag-neutral TU still builds as plain rv64gc, and the read only ever
// EXECUTES behind the hwcap check, so a V-less CPU never sees it.
#define BLAKE3PP_RISCV64_LINUX_RVV 1
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace blake3pp {

namespace kern {
#define BLAKE3PP_KERNEL(ns) \
  namespace ns {            \
  extern const kernel_ops ops; \
  }
#include "blake3pp_kernel_registry.inc"
#undef BLAKE3PP_KERNEL
}  // namespace kern

namespace {

struct registry_entry {
  arch a;
  const kern::kernel_ops* ops;
};

constexpr registry_entry registry[] = {
#define BLAKE3PP_KERNEL(ns) {arch::ns, &kern::ns::ops},
#include "blake3pp_kernel_registry.inc"
#undef BLAKE3PP_KERNEL
};

constexpr std::size_t num_kernels = std::size(registry);

// The one canonical enumerator list: auto_detect first, then best-first.
// The dispatch preference order is simply its tail. One list, two roles.
// Ranking notes: the fixed-length SVE variants are exact-VL matches,
// so at most one SVE1 and one SVE2 entry can ever be runtime-available at
// once; their relative order encodes generation (SVE2's XAR) and width.
// sve2_128 above neon is MEASURED: 1.89 vs 1.59 GiB/s single-thread on
// Neoverse V2 (GCP Axion, 2026-08-29), the whole margin being XAR (the
// XAR_ROTATE=off build lands exactly on neon parity). sve128 sits BELOW
// neon: same width, no XAR, measured parity (1.62), so no reason to
// displace the tuned NEON kernel.
constexpr arch all_enumerators[] = {
    arch::auto_detect, arch::avx512, arch::avx2,     arch::sse42,
    arch::sve2_512,    arch::sve512, arch::sve2_256, arch::sve256,
    arch::sve2_128,    arch::neon,   arch::sve128,   arch::rvv512,
    arch::rvv256,      arch::rvv128, arch::xthead,   arch::simd128,
    arch::scalar};
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

#if defined(BLAKE3PP_X86_CPUID)
// The probe: CPUID feature bits AND the OSXSAVE/XCR0 check; the OS must
// actually save the YMM/ZMM state; silicon alone is not availability.
void x86_cpuid(unsigned leaf, unsigned subleaf, unsigned out[4]) noexcept {
#if defined(_MSC_VER)
  int r[4];
  __cpuidex(r, static_cast<int>(leaf), static_cast<int>(subleaf));
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<unsigned>(r[i]);
  }
#else
  __get_cpuid_count(leaf, subleaf, &out[0], &out[1], &out[2], &out[3]);
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

bool x86_cpu_supports(arch a) noexcept {
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
#endif

#if defined(BLAKE3PP_AARCH64_LINUX_SVE)
struct sve_state {
  bool sve = false;
  bool sve2 = false;
  unsigned long vl_bytes = 0;
};

const sve_state& sve_probe() noexcept {
  static const sve_state s = [] {
    sve_state st{};
    if ((getauxval(AT_HWCAP) & HWCAP_SVE) != 0) {
      // The auxv bit says CPU and kernel both speak SVE; the prctl reports
      // the vector length this thread actually runs at (process-wide
      // unless somebody lowers it). A negative return means a kernel
      // without SVE state handling after all; treat as absent.
      const int vl = prctl(PR_SVE_GET_VL);
      if (vl >= 0) {
        st.sve = true;
        st.vl_bytes = static_cast<unsigned long>(vl) & PR_SVE_VL_LEN_MASK;
        st.sve2 = (getauxval(AT_HWCAP2) & HWCAP2_SVE2) != 0;
      }
    }
    return st;
  }();
  return s;
}

// Exact-match on the runtime VL: vector-length-specific code is only
// guaranteed on hardware whose VL equals the compiled -msve-vector-bits
// (GCC and Arm both document exact-match only), so a 256-bit kernel on a
// 512-bit machine is not a degraded option: it is not an option at all.
bool sve_cpu_supports(arch a) noexcept {
  const sve_state& s = sve_probe();
  if (!s.sve) {
    return false;
  }
  switch (a) {
    case arch::sve128:   return s.vl_bytes == 16;
    case arch::sve256:   return s.vl_bytes == 32;
    case arch::sve512:   return s.vl_bytes == 64;
    case arch::sve2_128: return s.sve2 && s.vl_bytes == 16;
    case arch::sve2_256: return s.sve2 && s.vl_bytes == 32;
    case arch::sve2_512: return s.sve2 && s.vl_bytes == 64;
    default:             return false;
  }
}
#endif

#if defined(BLAKE3PP_RISCV64_LINUX_RVV)
struct rvv_state {
  bool v = false;
  unsigned long vlenb = 0;
};

const rvv_state& rvv_probe() noexcept {
  static const rvv_state s = [] {
    rvv_state st{};
    // Single-letter ISA extensions occupy HWCAP bits 0-25 (A-Z).
    if ((getauxval(AT_HWCAP) & (1UL << ('V' - 'A'))) != 0) {
      unsigned long vlenb = 0;
      asm(".option push\n\t"
          ".option arch, +v\n\t"
          "csrr %0, vlenb\n\t"
          ".option pop"
          : "=r"(vlenb));
      st.v = vlenb != 0;
      st.vlenb = vlenb;
    }
    return st;
  }();
  return s;
}

// XTheadVector (draft RVV 0.7.1, T-Head encoding): Linux 6.13+ reports it
// through the hwprobe vendor-extension key. Raw syscall: glibc grew a
// wrapper only recently and musl has none; ENOSYS (pre-6.4 kernels) and
// missing-key (pre-6.13) both degrade to "absent". Never parse
// /proc/cpuinfo: old vendor kernels print a bare "v" for 0.7.1. The
// BLAKE3PP_ASSUME_XTHEADVECTOR=1 env hook exists for emulator testing
// (T-Head's qemu fork predates the hwprobe key) and is honored for this
// one arch only.
#ifndef BLAKE3PP_NR_riscv_hwprobe
#define BLAKE3PP_NR_riscv_hwprobe 258
#endif
#ifndef RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0
#define RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0 11
#endif
#ifndef RISCV_HWPROBE_VENDOR_EXT_XTHEADVECTOR
#define RISCV_HWPROBE_VENDOR_EXT_XTHEADVECTOR (1 << 0)
#endif

bool xthead_cpu_supports() noexcept {
  static const bool s = [] {
    const char* assume = std::getenv("BLAKE3PP_ASSUME_XTHEADVECTOR");
    if (assume != nullptr && assume[0] == '1') {
      return true;
    }
    struct {
      std::int64_t key;
      std::uint64_t value;
    } pair = {RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0, 0};
    const long rc = syscall(BLAKE3PP_NR_riscv_hwprobe, &pair, 1UL, 0UL,
                            nullptr, 0U);
    // An unknown key comes back as key=-1 with value=0, an old kernel as
    // ENOSYS; both mean "not detectable" and therefore "absent".
    return rc == 0 && pair.key == RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0 &&
           (pair.value & RISCV_HWPROBE_VENDOR_EXT_XTHEADVECTOR) != 0;
  }();
  return s;
}

// Exact-match on the runtime vlenb, same reasoning as SVE: fixed-vlen
// code pins vscale min AND max, and its whole-register moves are only
// correct at exactly the compiled VLEN.
bool rvv_cpu_supports(arch a) noexcept {
  const rvv_state& s = rvv_probe();
  if (!s.v) {
    return false;
  }
  switch (a) {
    case arch::rvv128: return s.vlenb == 16;
    case arch::rvv256: return s.vlenb == 32;
    case arch::rvv512: return s.vlenb == 64;
    default:           return false;
  }
}
#endif

bool cpu_supports(arch a) noexcept {
  switch (a) {
    case arch::auto_detect:
    case arch::scalar:
      return true;
#if defined(BLAKE3PP_X86_CPUID)
    case arch::sse42:
    case arch::avx2:
    case arch::avx512:
      return x86_cpu_supports(a);
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    // GCC/Clang spell it __aarch64__, MSVC _M_ARM64; NEON is
    // architecturally mandatory on AArch64 either way.
    case arch::neon:
      return true;
#if defined(BLAKE3PP_AARCH64_LINUX_SVE)
    case arch::sve128:
    case arch::sve256:
    case arch::sve512:
    case arch::sve2_128:
    case arch::sve2_256:
    case arch::sve2_512:
      return sve_cpu_supports(a);
#endif
#endif
#if defined(BLAKE3PP_RISCV64_LINUX_RVV)
    case arch::rvv128:
    case arch::rvv256:
    case arch::rvv512:
      return rvv_cpu_supports(a);
    case arch::xthead:
      return xthead_cpu_supports();
#endif
#if defined(__wasm__)
    case arch::simd128:
      // Module-level feature: SIMD opcodes in a module make load-time
      // validation the availability check; running code proves support.
      return true;
#endif
    default:
      return false;
  }
}

// Built once; storage is static, so the returned span never dangles.
std::span<const arch> available_impl() noexcept {
  static const auto table = [] {
    struct {
      std::array<arch, num_kernels> entries{};
      std::size_t count = 0;
    } t;
    for (const arch a : compiled_sorted) {
      if (cpu_supports(a)) {
        t.entries[t.count++] = a;
      }
    }
    return t;
  }();
  return {table.entries.data(), table.count};
}

}  // namespace

bool is_available(arch a) noexcept {
  return compiled_in(a) && cpu_supports(a);
}

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

std::optional<arch> arch_from_string(std::string_view name) noexcept {
  for (const arch a : all_enumerators) {
    if (name == to_string(a)) {
      return a;
    }
  }
  return std::nullopt;
}

const char* to_string(arch a) noexcept {
  switch (a) {
    case arch::auto_detect:
      return "auto";
    case arch::scalar:
      return "scalar";
    case arch::sse42:
      return "sse42";
    case arch::avx2:
      return "avx2";
    case arch::avx512:
      return "avx512";
    case arch::neon:
      return "neon";
    case arch::simd128:
      return "simd128";
    case arch::sve128:
      return "sve128";
    case arch::sve256:
      return "sve256";
    case arch::sve512:
      return "sve512";
    case arch::sve2_128:
      return "sve2_128";
    case arch::sve2_256:
      return "sve2_256";
    case arch::sve2_512:
      return "sve2_512";
    case arch::rvv128:
      return "rvv128";
    case arch::rvv256:
      return "rvv256";
    case arch::rvv512:
      return "rvv512";
    case arch::xthead:
      return "xthead";
  }
  return "unknown";
}


// ---- transpose16: the runtime dial for the AVX-512 message transpose ----

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
  // (avx512, sve512/sve2_512): available_arches() is best-first, so the
  // first width-16 entry is the one auto_detect would pick.
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
  const auto race = [&](transpose16 mode) {
    set_transpose16(mode);
    one_pass();  // warm-up: frequency ramp, page faults, TLB
    auto best = std::chrono::steady_clock::duration::max();
    for (int rep = 0; rep < 3; ++rep) {
      const auto t0 = std::chrono::steady_clock::now();
      one_pass();
      const auto dt = std::chrono::steady_clock::now() - t0;
      if (dt < best) {
        best = dt;
      }
    }
    return best;
  };

  transpose16 winner = saved;
  auto winner_time = std::chrono::steady_clock::duration::max();
  for (const transpose16 mode : {transpose16::staging, transpose16::tree,
                                 transpose16::quartered}) {
    const auto t = race(mode);
    if (t < winner_time) {
      winner_time = t;
      winner = mode;
    }
  }
  std::free(data);
  set_transpose16(winner);
  return winner;
}

transpose16 tune_transpose16() noexcept {
  return tune_transpose16(default_tune_bytes);
}

namespace detail {

const kern::kernel_ops* resolve(arch a) noexcept {
  if (a == arch::auto_detect || !cpu_supports(a)) {
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
