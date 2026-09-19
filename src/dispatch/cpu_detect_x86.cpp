// x86 capability probe: one manual cpuid/xgetbv sequence for ALL x86
// toolchains. The tempting alternative, __builtin_cpu_supports, references
// compiler-rt/libgcc's __cpu_model support machinery, a link-time
// dependency that has failed twice here (clang-cl with lld-link on Windows;
// zig/musl static linking): the builtin is only as portable as the runtime
// library du jour. The manual probe is self-contained and does the same
// OSXSAVE/XCR0 dance: "avx2" is only reported when the OS actually saves
// YMM state, not merely when the CPU has the silicon.

#include "dispatch/cpu_detect.hpp"

#if defined(BLAKE3PP_CPU_DETECT_X86)

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace blake3pp::detail {
namespace {

void x86_cpuid(unsigned leaf, unsigned subleaf, unsigned out[4]) noexcept {
#if defined(_MSC_VER)
  // A leaf above the CPU's maximum basic leaf returns the highest leaf's
  // data on Intel and zeros on AMD. Zero it here so feature tests read a
  // definite absence, as the GNU path below does.
  int r[4];
  __cpuid(r, 0);
  if (leaf > static_cast<unsigned>(r[0])) {
    for (int i = 0; i < 4; ++i) {
      out[i] = 0;
    }
    return;
  }
  __cpuidex(r, static_cast<int>(leaf), static_cast<int>(subleaf));
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<unsigned>(r[i]);
  }
#else
  // Returns 0 WITHOUT writing the outputs when the CPU's max basic
  // leaf is below the request; zero them so feature tests read a
  // deterministic "absent" instead of stale registers.
  if (__get_cpuid_count(leaf, subleaf, &out[0], &out[1], &out[2],
                        &out[3]) == 0) {
    out[0] = out[1] = out[2] = out[3] = 0;
  }
#endif
}

unsigned x86_xgetbv0() noexcept {
#if defined(_MSC_VER)
  return static_cast<unsigned>(_xgetbv(0));
#else
  // The intrinsic needs -mxsave on GNU compilers (unavailable in this
  // flag-neutral TU); the two-byte encoding is the portable spelling.
  unsigned eax = 0;
  unsigned edx = 0;
  asm volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0u));
  return eax;
#endif
}

// The three reads every query needs, taken once. cpuid traps under a
// hypervisor, 1.3-2.2 us per query when read on every call and 2-4 us
// on WSL2, and the answer cannot change while the process runs. The Arm
// probe keeps its auxv reads in a static the same way.
struct x86_state {
  unsigned ecx1 = 0;  // cpuid(1).ecx: SSE4.2, OSXSAVE
  unsigned xcr0 = 0;  // XCR0, valid only when OSXSAVE is set
  unsigned ebx7 = 0;  // cpuid(7,0).ebx: AVX2, AVX-512 F/DQ/CD/BW/VL
};

const x86_state& x86_probe() noexcept {
  static const x86_state s = [] {
    x86_state st;
    unsigned r[4];
    x86_cpuid(1, 0, r);
    st.ecx1 = r[2];
    if ((st.ecx1 >> 27) & 1u) {  // OSXSAVE: XGETBV is legal
      st.xcr0 = x86_xgetbv0();
      x86_cpuid(7, 0, r);
      st.ebx7 = r[1];
    }
    return st;
  }();
  return s;
}

}  // namespace

bool platform_cpu_supports(arch a) noexcept {
  const x86_state& st = x86_probe();
  const unsigned ecx1 = st.ecx1;
  if (a == arch::sse42) {
    return (ecx1 >> 20) & 1u;  // SSE4.2; XMM state is OS baseline
  }
  const bool osxsave = (ecx1 >> 27) & 1u;
  if (!osxsave) {
    return false;
  }
  const unsigned xcr0 = st.xcr0;
  const unsigned ebx7 = st.ebx7;
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

// No detection rung on this platform needs a trap-guarded probe; the
// syscall/CPUID rungs tell the whole story (see cpu_detect.hpp).
bool platform_run_trap_probes() noexcept { return false; }

}  // namespace blake3pp::detail

#endif  // BLAKE3PP_CPU_DETECT_X86
