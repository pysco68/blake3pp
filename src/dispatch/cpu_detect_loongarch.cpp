// LoongArch capability probe, Linux-only. LSX (128-bit) and LASX
// (256-bit) are both optional in LA64: the 3A5000/3A6000 desktop and
// server parts carry both, the embedded LA264 parts carry LSX alone, and
// the smallest LA64 cores carry neither.
//
// One auxv bit each settles it. The constants carry an #ifndef fallback
// because zig's bundled musl headers declare no HWCAP_LOONGARCH_* at all;
// the values are ABI-stable and come from the kernel's
// arch/loongarch/include/uapi/asm/hwcap.h.
#include "dispatch/cpu_detect.hpp"

#if defined(BLAKE3PP_CPU_DETECT_LOONGARCH)

#if defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_LOONGARCH_LSX
#define HWCAP_LOONGARCH_LSX (1 << 4)
#endif
#ifndef HWCAP_LOONGARCH_LASX
#define HWCAP_LOONGARCH_LASX (1 << 5)
#endif
#endif

namespace blake3pp::detail {

bool platform_cpu_supports(arch a) noexcept {
#if defined(__linux__)
  static const unsigned long hwcap = getauxval(AT_HWCAP);
  if (a == arch::lsx) {
    return (hwcap & HWCAP_LOONGARCH_LSX) != 0;
  }
  if (a == arch::lasx) {
    return (hwcap & HWCAP_LOONGARCH_LASX) != 0;
  }
#endif
  (void)a;
  return false;
}

}  // namespace blake3pp::detail

#endif  // BLAKE3PP_CPU_DETECT_LOONGARCH
