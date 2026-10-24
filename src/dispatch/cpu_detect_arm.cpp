// AArch64 capability probe. NEON is architecturally mandatory, so
// presence of the kernel implies availability on every OS.
//
// SVE is probed per OS. Linux reads auxv HWCAP bits for presence and
// prctl for the runtime vector length, both plain syscall surfaces that
// qemu-user emulates faithfully; the constants carry #ifndef fallbacks
// because musl and older glibc headers do not spell them all. Windows
// reports presence through IsProcessorFeaturePresent, whose SDK gained
// ids for SVE, SVE2, SVE2.1 and the SME family in 10.0.26100; they are
// written out here because older kits define none of them. FreeBSD
// carries the same auxv bits as Linux -- they are Arm's, not the
// kernel's -- and reads them with elf_aux_info, which answers a status
// code rather than a value and so cannot report "absent" as zero.
//
// Neither Windows nor FreeBSD exposes a vector length, and dispatch
// selects a kernel by exact length match, so on both the length comes
// from the hardware via rdvl. .arch_extension holds the assembler's SVE
// window open around that one instruction and closes it again, which
// keeps the rest of this translation unit at the baseline: the file
// needs no SVE compile flag, and no SVE instruction is reachable until
// the presence check passes -- which on every OS here means the kernel
// said it saves SVE state, not merely that the CPU has the unit.

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
#elif defined(__FreeBSD__)
#define BLAKE3PP_AARCH64_SVE 1
#define BLAKE3PP_AARCH64_FREEBSD_SVE 1
// <sys/auxv.h> alone: it declares elf_aux_info and pulls in
// <machine/elf.h> for the HWCAP bits, in that order. Naming the machine
// header here instead would break the build -- it needs the BSD type
// names <sys/types.h> brings, which <sys/auxv.h> includes first.
#include <sys/auxv.h>
// Arm defines the HWCAP bit positions, so these are the Linux values;
// spelled out because a base system older than its own SVE support
// defines neither.
#ifndef HWCAP_SVE
#define HWCAP_SVE (1UL << 22)
#endif
#ifndef HWCAP2_SVE2
#define HWCAP2_SVE2 (1UL << 1)
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
#elif defined(BLAKE3PP_AARCH64_SVE)
// The platforms that report presence and not length. AArch64 MSVC has
// no inline assembler, so the length stays unknown under it and no SVE
// kernel is selected. It builds none either: the SVE variants need a
// compiler that accepts an SVE arch flag, which is clang-cl here.
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
#endif

#if defined(BLAKE3PP_AARCH64_WINDOWS_SVE)
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
#elif defined(BLAKE3PP_AARCH64_FREEBSD_SVE)
const sve_state& sve_probe() noexcept {
  static const sve_state s = [] {
    sve_state st{};
    // elf_aux_info answers 0 or an errno, and leaves the buffer alone on
    // a vector the kernel does not carry, so the status is the question
    // and an untouched zero is not an answer.
    unsigned long hwcap = 0;
    if (::elf_aux_info(AT_HWCAP, &hwcap, sizeof hwcap) != 0) {
      return st;
    }
    if ((hwcap & HWCAP_SVE) == 0) {
      // Either the CPU has no SVE or the kernel does not save its state
      // across a context switch (FreeBSD gained that in 15.0). The bit
      // is one answer to both, and the right one: without the kernel's
      // half, the registers are not this thread's to keep.
      return st;
    }
    st.sve = true;
    st.vl_bytes = sve_vl_bytes();
    unsigned long hwcap2 = 0;
    if (::elf_aux_info(AT_HWCAP2, &hwcap2, sizeof hwcap2) == 0) {
      st.sve2 = (hwcap2 & HWCAP2_SVE2) != 0;
    }
    return st;
  }();
  return s;
}
#endif

}  // namespace

// Exact-match on the runtime VL. Vector-length-specific code is only
// guaranteed on hardware whose VL equals the compiled -msve-vector-bits,
// which GCC and Arm both document as exact-match only. A 256-bit kernel on
// a 512-bit machine is therefore not an option at all, degraded or
// otherwise.
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

}  // namespace blake3pp::detail

#endif  // BLAKE3PP_CPU_DETECT_ARM
