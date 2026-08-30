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
#endif

namespace blake3pp::detail {

#if defined(BLAKE3PP_HAS_CPU_DETECT)
// Can the running CPU execute this variant? Only the platform's own
// enumerators need answering; dispatch.cpp resolves auto_detect, scalar
// and simd128 before calling here, and anything foreign returns false.
[[nodiscard]] bool platform_cpu_supports(arch a) noexcept;
#endif

}  // namespace blake3pp::detail
