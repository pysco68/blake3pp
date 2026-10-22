// ppc64 capability probe, Linux-only. VSX has been architecturally
// present since POWER7 and every ppc64le distro baseline is POWER8+, so
// in practice the answer is always yes on the machines this binary can
// even load on. Still, the HWCAP bit exists, costs one auxv read, and
// checking beats assuming (big-endian ppc64 with an ancient CPU could
// run this binary too).
#include "dispatch/cpu_detect.hpp"

#if defined(BLAKE3PP_CPU_DETECT_PPC)

#if defined(__linux__)
#include <sys/auxv.h>
#ifndef PPC_FEATURE_HAS_VSX
#define PPC_FEATURE_HAS_VSX 0x00000080
#endif
#endif

namespace blake3pp::detail {

bool platform_cpu_supports(arch a) noexcept {
#if defined(__linux__)
  if (a == arch::vsx) {
    static const bool s =
        (getauxval(AT_HWCAP) & PPC_FEATURE_HAS_VSX) != 0;
    return s;
  }
#endif
  (void)a;
  return false;
}

}  // namespace blake3pp::detail

#endif  // BLAKE3PP_CPU_DETECT_PPC
