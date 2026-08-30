// AArch64 capability probe. NEON is architecturally mandatory, so
// presence of the kernel implies availability on every OS. SVE probing is
// Linux-only for now: auxv HWCAP bits for presence, prctl for the runtime
// vector length. Both are plain syscall surfaces, usable from this
// flag-neutral TU (no SVE codegen needed, unlike an rdvl/svcntb read) and
// emulated faithfully by qemu-user. Constants carry #ifndef fallbacks
// because musl and older glibc auxv/prctl headers do not spell them all.
// Windows-on-ARM never registers SVE kernels (no -msve-vector-bits on the
// MSVC frontend), so answering false there is consistent, not a gap.

#include "dispatch/cpu_detect.hpp"

#if defined(BLAKE3PP_CPU_DETECT_ARM)

#if defined(__linux__)
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

namespace blake3pp::detail {
namespace {

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
#endif

}  // namespace

// Exact-match on the runtime VL: vector-length-specific code is only
// guaranteed on hardware whose VL equals the compiled -msve-vector-bits
// (GCC and Arm both document exact-match only), so a 256-bit kernel on a
// 512-bit machine is not a degraded option: it is not an option at all.
bool platform_cpu_supports(arch a) noexcept {
  if (a == arch::neon) {
    return true;
  }
#if defined(BLAKE3PP_AARCH64_LINUX_SVE)
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
#else
  return false;
#endif
}

}  // namespace blake3pp::detail

#endif  // BLAKE3PP_CPU_DETECT_ARM
