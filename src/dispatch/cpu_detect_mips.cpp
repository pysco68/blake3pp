// MIPS capability probe, Linux-only. MSA arrived with MIPS32r5/MIPS64r5,
// well after the r2-era cores most MIPS Linux systems run, so unlike the
// POWER case the answer here is usually no and the check earns its keep.
//
// One auxv bit settles it. The constant carries an #ifndef fallback
// because a cross sysroot's asm/hwcap.h is not guaranteed to be on the
// include path, and the value is ABI-stable: it is the same bit the
// kernel sets and the same one `ASEs implemented` in /proc/cpuinfo is
// derived from.
#include "dispatch/cpu_detect.hpp"

#if defined(BLAKE3PP_CPU_DETECT_MIPS)

#if defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_MIPS_MSA
#define HWCAP_MIPS_MSA (1 << 1)
#endif
#endif

namespace blake3pp::detail {

bool platform_cpu_supports(arch a) noexcept {
#if defined(__linux__)
  if (a == arch::msa) {
    static const bool s = (getauxval(AT_HWCAP) & HWCAP_MIPS_MSA) != 0;
    return s;
  }
#endif
  (void)a;
  return false;
}

// No detection rung on this platform needs a trap-guarded probe; the
// syscall/CPUID rungs tell the whole story (see cpu_detect.hpp).
bool platform_run_trap_probes() noexcept { return false; }

}  // namespace blake3pp::detail

#endif  // BLAKE3PP_CPU_DETECT_MIPS
