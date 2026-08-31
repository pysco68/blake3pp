// s390x capability probe, Linux-only. The vxe kernel targets z14's
// vector-enhancements facility 1; the kernel reports the base vector
// facility (z13) and the extension through two HWCAP bits, and both are
// required: the kernel must also be context-switching the vector
// registers, which is exactly what the HWCAP bit asserts. Constants
// carry #ifndef fallbacks (musl / older glibc headers).
#include "dispatch/cpu_detect.hpp"

#if defined(BLAKE3PP_CPU_DETECT_S390)

#if defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_S390_VXRS
#define HWCAP_S390_VXRS 2048
#endif
#ifndef HWCAP_S390_VXRS_EXT
#define HWCAP_S390_VXRS_EXT 8192
#endif
#endif

namespace blake3pp::detail {

bool platform_cpu_supports(arch a) noexcept {
#if defined(__linux__)
  if (a == arch::vxe) {
    static const bool s =
        (getauxval(AT_HWCAP) & (HWCAP_S390_VXRS | HWCAP_S390_VXRS_EXT)) ==
        (HWCAP_S390_VXRS | HWCAP_S390_VXRS_EXT);
    return s;
  }
#endif
  (void)a;
  return false;
}

}  // namespace blake3pp::detail

#endif  // BLAKE3PP_CPU_DETECT_S390
