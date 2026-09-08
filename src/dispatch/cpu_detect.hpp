#pragma once

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

#include <blake3pp/dispatch.hpp>

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
