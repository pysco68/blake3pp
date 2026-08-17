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

#include <array>
#include <chrono>
#include <cstddef>

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
constexpr arch all_enumerators[] = {arch::auto_detect, arch::avx512,
                                    arch::avx2,        arch::sse42,
                                    arch::neon,        arch::simd128,
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

std::string_view version() noexcept { return BLAKE3PP_VERSION; }

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
// only honest detector is a stopwatch. ~1 ms, once, opt-in.
transpose16 tune_transpose16() noexcept {
  if (!is_available(arch::avx512)) {
    return active_transpose16();  // dial is inert without a W=16 kernel
  }
  const kern::kernel_ops* ops = detail::resolve(arch::avx512);
  static constexpr std::size_t chunk = 1024;
  static constexpr std::size_t lanes = 16;
  std::array<std::uint8_t, lanes * chunk> data;
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::uint8_t>(i % 251);
  }
  const std::uint8_t* inputs[lanes];
  for (std::size_t i = 0; i < lanes; ++i) {
    inputs[i] = data.data() + i * chunk;
  }
  constexpr std::uint32_t key[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u,
                                    0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu,
                                    0x1F83D9ABu, 0x5BE0CD19u};
  std::array<std::uint8_t, lanes * 32> out;

  const transpose16 saved = active_transpose16();
  const auto run_batch = [&] {
    ops->hash_many(inputs, lanes, chunk / 64, key, 0, true, 0,
                   kern::flag_chunk_start, kern::flag_chunk_end, out.data());
  };
  // Race at STEADY-STATE scale. A ~300us in-cache micro-race mispicked on
  // real full-width AVX-512 hardware (Skylake-SP: chose tree while the
  // 512 MiB benchmark showed quartered ahead by 13%). Wide-vector
  // frequency licensing and cache-hot staging distort short samples. 256
  // batches x 16 KiB per rep (~4 MiB) with a longer warm-up tracks the
  // macro benchmark's verdict; whole tune stays in the tens of ms.
  const auto race = [&](transpose16 mode) {
    set_transpose16(mode);
    for (int i = 0; i < 64; ++i) {  // warm-up: frequency ramp + caches
      run_batch();
    }
    auto best = std::chrono::steady_clock::duration::max();
    for (int rep = 0; rep < 3; ++rep) {
      const auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < 256; ++i) {
        run_batch();
      }
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
  set_transpose16(winner);
  return winner;
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
