// AArch64 capability probe. NEON is architecturally mandatory, so
// presence of the kernel implies availability on every OS.
//
// SVE is probed per OS. Linux reads auxv HWCAP bits for presence and
// prctl for the runtime vector length, both plain syscall surfaces that
// qemu-user emulates faithfully; the constants carry #ifndef fallbacks
// because musl and older glibc headers do not spell them all. Windows
// reports presence through IsProcessorFeaturePresent, whose SDK gained
// ids for SVE, SVE2, SVE2.1 and the SME family in 10.0.26100; they are
// written out here because older kits define none of them.
//
// Windows exposes no vector length, and dispatch selects a kernel by
// exact length match, so the length comes from the hardware via rdvl.
// .arch_extension holds the assembler's SVE window open around that one
// instruction and closes it again, which keeps the rest of this
// translation unit at the baseline: the file needs no SVE compile flag,
// and no SVE instruction is reachable until the presence check passes.

#include "dispatch/cpu_detect.hpp"

#if defined(BLAKE3PP_CPU_DETECT_ARM)

#if defined(_WIN32)
#define BLAKE3PP_AARCH64_SVE 1
#define BLAKE3PP_AARCH64_WINDOWS_SVE 1
#include <windows.h>
// Windows SDK 10.0.26100 and later define these; older kits do not.
#ifndef PF_ARM_SVE_INSTRUCTIONS_AVAILABLE
#define PF_ARM_SVE_INSTRUCTIONS_AVAILABLE 46
#endif
#ifndef PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE
#define PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE 47
#endif
#elif defined(__linux__)
#define BLAKE3PP_AARCH64_SVE 1
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

#if defined(BLAKE3PP_AARCH64_SVE)
struct sve_state {
  bool sve = false;
  bool sve2 = false;
  unsigned vl_bytes = 0;
};
#endif

#if defined(BLAKE3PP_AARCH64_LINUX_SVE)
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
        st.vl_bytes = static_cast<unsigned>(vl) & PR_SVE_VL_LEN_MASK;
        st.sve2 = (getauxval(AT_HWCAP2) & HWCAP2_SVE2) != 0;
      }
    }
    return st;
  }();
  return s;
}
#elif defined(BLAKE3PP_AARCH64_WINDOWS_SVE)
// AArch64 MSVC has no inline assembler, so the length stays unknown
// under it and no SVE kernel is selected. It builds none either: the
// SVE variants need a compiler that accepts an SVE arch flag, which is
// clang-cl here.
#if defined(__GNUC__) || defined(__clang__)
unsigned sve_vl_bytes() noexcept {
  unsigned long long vl = 0;
  __asm__ volatile(".arch_extension sve\n\trdvl %0, #1\n\t.arch_extension nosve"
                   : "=r"(vl));
  return static_cast<unsigned>(vl);
}
#else
unsigned sve_vl_bytes() noexcept { return 0; }
#endif

const sve_state& sve_probe() noexcept {
  static const sve_state s = [] {
    sve_state st{};
    if (IsProcessorFeaturePresent(PF_ARM_SVE_INSTRUCTIONS_AVAILABLE) != 0) {
      st.sve = true;
      st.sve2 =
          IsProcessorFeaturePresent(PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE) != 0;
      st.vl_bytes = sve_vl_bytes();
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
#if defined(BLAKE3PP_AARCH64_SVE)
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

// No detection rung on this platform needs a trap-guarded probe; the
// syscall/CPUID rungs tell the whole story (see cpu_detect.hpp).
bool platform_run_trap_probes() noexcept { return false; }

}  // namespace blake3pp::detail

#endif  // BLAKE3PP_CPU_DETECT_ARM
