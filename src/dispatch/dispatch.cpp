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
// On x86 the CPU check is __builtin_cpu_supports, which goes through
// libgcc / compiler-rt's cpu-model probe; that includes the OSXSAVE/XCR0
// check, so "avx2" is only reported when the OS actually saves YMM state,
// not merely when the CPU has the silicon. On AArch64, NEON is
// architecturally mandatory, so presence of the kernel implies
// availability.

#include <blake3pp/dispatch.hpp>

#include <array>
#include <cstddef>

#include "kernel/kernel.hpp"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
// Covers clang-cl too: its __builtin_cpu_supports references compiler-rt's
// __cpu_model global, which is not part of what lld-link pulls in by
// default on Windows; the manual probe avoids the linker dependency.
#define BLAKE3PP_MSVC_CPUID 1
#include <intrin.h>
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
                                    arch::neon,        arch::scalar};
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

#if defined(BLAKE3PP_MSVC_CPUID)
// MSVC-frontend toolchains (cl and clang-cl) get the cpu-model probe
// spelled out: CPUID feature bits AND the OSXSAVE/XCR0 check; the OS
// must actually save the YMM/ZMM state, silicon alone is not
// availability.
bool msvc_cpu_supports(arch a) noexcept {
  int r[4];
  __cpuid(r, 1);
  const unsigned ecx1 = static_cast<unsigned>(r[2]);
  if (a == arch::sse42) {
    return (ecx1 >> 20) & 1u;  // SSE4.2; XMM state is baseline on Windows
  }
  const bool osxsave = (ecx1 >> 27) & 1u;
  if (!osxsave) {
    return false;
  }
  const unsigned xcr0 = static_cast<unsigned>(_xgetbv(0));
  __cpuidex(r, 7, 0);
  const unsigned ebx7 = static_cast<unsigned>(r[1]);
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
#if defined(BLAKE3PP_MSVC_CPUID)
    case arch::sse42:
    case arch::avx2:
    case arch::avx512:
      return msvc_cpu_supports(a);
#elif defined(__x86_64__) || defined(__i386__)
    case arch::sse42:
      return __builtin_cpu_supports("sse4.2");
    case arch::avx2:
      return __builtin_cpu_supports("avx2");
    case arch::avx512:
      return __builtin_cpu_supports("avx512f") &&
             __builtin_cpu_supports("avx512cd") &&
             __builtin_cpu_supports("avx512vl") &&
             __builtin_cpu_supports("avx512bw") &&
             __builtin_cpu_supports("avx512dq");
#endif
#if defined(__aarch64__)
    case arch::neon:
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
#if defined(BLAKE3PP_HAS_STD_SENDERS)
  return "std::execution";
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
  }
  return "unknown";
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
