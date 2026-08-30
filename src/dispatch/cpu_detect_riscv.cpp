// riscv64 capability probe, Linux-only (no other riscv64 OS target
// exists here). Vector detection here spans THREE kernel generations,
// because the answer to "does this machine have RVV?" depends on when
// its kernel was built as much as on the silicon:
//
//  * Kernels >= 6.13 report XTheadVector (draft RVV 0.7.1, T-Head
//    encoding) through the hwprobe vendor-extension key. Authoritative
//    when present.
//  * Kernels >= 6.4 have hwprobe: IMA_EXT_0's V bit is the kernel
//    saying "real ratified RVV 1.0, state save/restore enabled"; only
//    then is it safe to execute vector CSR reads unguarded. The
//    vendor-kernel LIE detector lives here too: T-Head SDK kernels set
//    the HWCAP 'V' bit for draft 0.7.1, so HWCAP-V with hwprobe
//    explicitly NOT reporting V is a contradiction; corroborated by
//    mvendorid == 0x5b7 (T-Head), that is a 0.7.1 machine. mvendorid
//    alone would NOT do: T-Head also ships real RVV 1.0 cores (C908 in
//    the Canaan K230, C920v2), so the vendor ID says who built the
//    core, not which vector draft it speaks.
//  * Kernels < 6.4 (no hwprobe, ENOSYS) that still set HWCAP 'V' are
//    vendor kernels of either era: T-Head 0.7.1 SDKs (TH1520, SG2042)
//    or early RVV 1.0 SDKs (K230's 5.10). The discriminator is the
//    vlenb CSR itself: it entered the spec at v0.9, so 0.7.1 hardware
//    TRAPS on it while 1.0 hardware returns the vector length. The
//    read runs under a scoped SIGILL guard: the instruction that
//    would have crashed IS the classifier. (This also fixes what the
//    previous revision would have done on such kernels: read vlenb
//    unguarded straight into the trap.)
//
// Never parse /proc/cpuinfo: old vendor kernels print a bare "v" for
// 0.7.1, and every fact this file needs is available through auxv,
// hwprobe, or the guarded read. The BLAKE3PP_ASSUME_XTHEADVECTOR=1 env
// hook exists for emulator testing (T-Head's qemu fork predates the
// hwprobe key, and qemu-user does not model the vlenb trap) and is
// honored for arch::xthead only.
//
// Zvbb has no single-letter HWCAP bit; the hwprobe IMA_EXT_0 key
// carries it, so on pre-hwprobe kernels it is simply "absent". The
// hwprobe syscall is raw (glibc grew a wrapper only recently and musl
// has none); ENOSYS degrades to "not detectable".

#include "dispatch/cpu_detect.hpp"

#if defined(BLAKE3PP_CPU_DETECT_RISCV)

#include <cstdint>
#include <cstdlib>

#if defined(__linux__)
#define BLAKE3PP_RISCV64_LINUX 1
#include <csetjmp>
#include <csignal>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef BLAKE3PP_NR_riscv_hwprobe
#define BLAKE3PP_NR_riscv_hwprobe 258
#endif
#ifndef RISCV_HWPROBE_KEY_MVENDORID
#define RISCV_HWPROBE_KEY_MVENDORID 0
#endif
#ifndef RISCV_HWPROBE_KEY_IMA_EXT_0
#define RISCV_HWPROBE_KEY_IMA_EXT_0 4
#endif
#ifndef RISCV_HWPROBE_IMA_V
#define RISCV_HWPROBE_IMA_V (1ULL << 2)
#endif
#ifndef RISCV_HWPROBE_EXT_ZVBB
#define RISCV_HWPROBE_EXT_ZVBB (1ULL << 17)
#endif
#ifndef RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0
#define RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0 11
#endif
#ifndef RISCV_HWPROBE_VENDOR_EXT_XTHEADVECTOR
#define RISCV_HWPROBE_VENDOR_EXT_XTHEADVECTOR (1 << 0)
#endif
#define BLAKE3PP_MVENDORID_THEAD 0x5b7ULL
#endif

namespace blake3pp::detail {
namespace {

#if defined(BLAKE3PP_RISCV64_LINUX)
long hwprobe_one(std::int64_t key, std::uint64_t* value) noexcept {
  struct {
    std::int64_t key;
    std::uint64_t value;
  } pair = {key, 0};
  const long rc =
      syscall(BLAKE3PP_NR_riscv_hwprobe, &pair, 1UL, 0UL, nullptr, 0U);
  // An unknown key comes back as key=-1 with value=0, an old kernel as
  // ENOSYS; both mean "not detectable" and therefore "absent".
  if (rc != 0 || pair.key != key) {
    return -1;
  }
  *value = pair.value;
  return 0;
}

// The rung-3 classifier. Only ever called once, from the magic-static
// initializer below (so exactly one thread runs it, and it runs before
// any hashing happens); the SIGILL handler is scoped to the one csrr
// and restored immediately.
sigjmp_buf g_vlenb_jmp;

void vlenb_sigill(int) { siglongjmp(g_vlenb_jmp, 1); }

unsigned long guarded_vlenb() noexcept {
  struct sigaction sa {};
  struct sigaction old {};
  sa.sa_handler = &vlenb_sigill;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGILL, &sa, &old) != 0) {
    return 0;  // cannot make the read safe -> classify as "no vector"
  }
  unsigned long vlenb = 0;
  if (sigsetjmp(g_vlenb_jmp, 1) == 0) {
    asm volatile(
        ".option push\n\t"
        ".option arch, +v\n\t"
        "csrr %0, vlenb\n\t"
        ".option pop"
        : "=r"(vlenb));
  }  // trapped: vlenb stays 0 -> pre-v0.9 vector, i.e. 0.7.1
  sigaction(SIGILL, &old, nullptr);
  return vlenb;
}

struct vec_state {
  bool rvv = false;     // ratified RVV 1.0
  bool xthead = false;  // draft 0.7.1, T-Head encoding
  bool zvbb = false;
  unsigned long vlenb = 0;
};

const vec_state& vec_probe() noexcept {
  static const vec_state s = [] {
    vec_state st{};
    // Rung 1: the authoritative answer, where the kernel is new enough.
    std::uint64_t vendor = 0;
    if (hwprobe_one(RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0, &vendor) == 0 &&
        (vendor & RISCV_HWPROBE_VENDOR_EXT_XTHEADVECTOR) != 0) {
      st.xthead = true;
      return st;
    }
    const bool hwcap_v = (getauxval(AT_HWCAP) & (1UL << ('V' - 'A'))) != 0;
    std::uint64_t ima = 0;
    if (hwprobe_one(RISCV_HWPROBE_KEY_IMA_EXT_0, &ima) == 0) {
      if ((ima & RISCV_HWPROBE_IMA_V) != 0) {
        // Kernel vouches for ratified V: vector CSRs are safe to read.
        unsigned long vlenb = 0;
        asm(".option push\n\t"
            ".option arch, +v\n\t"
            "csrr %0, vlenb\n\t"
            ".option pop"
            : "=r"(vlenb));
        st.rvv = vlenb != 0;
        st.vlenb = vlenb;
        st.zvbb = st.rvv && (ima & RISCV_HWPROBE_EXT_ZVBB) != 0;
      } else if (hwcap_v &&
                 hwprobe_one(RISCV_HWPROBE_KEY_MVENDORID, &vendor) == 0 &&
                 vendor == BLAKE3PP_MVENDORID_THEAD) {
        // Rung 2: HWCAP says V, hwprobe says no V, the core is T-Head.
        // That is the vendor-kernel 0.7.1 lie, caught in the act.
        st.xthead = true;
      }
      // HWCAP-V + no hwprobe-V + NOT T-Head: some other vendor's
      // fiction; claim nothing rather than execute a guess.
    } else if (hwcap_v) {
      // Rung 3: pre-hwprobe vendor kernel. vlenb postdates 0.7.1, so
      // the guarded read classifies: a value is legacy RVV 1.0 (K230
      // era), a trap is T-Head 0.7.1.
      const unsigned long vlenb = guarded_vlenb();
      if (vlenb != 0) {
        st.rvv = true;
        st.vlenb = vlenb;  // zvbb undetectable here; stays false
      } else {
        st.xthead = true;
      }
    }
    return st;
  }();
  return s;
}

bool xthead_supported() noexcept {
  static const bool s = [] {
    const char* assume = std::getenv("BLAKE3PP_ASSUME_XTHEADVECTOR");
    if (assume != nullptr && assume[0] == '1') {
      return true;
    }
    return vec_probe().xthead;
  }();
  return s;
}
#endif  // BLAKE3PP_RISCV64_LINUX

}  // namespace

// Exact-match on the runtime vlenb, same reasoning as SVE: fixed-vlen
// code pins vscale min AND max, and its whole-register moves are only
// correct at exactly the compiled VLEN.
bool platform_cpu_supports(arch a) noexcept {
#if defined(BLAKE3PP_RISCV64_LINUX)
  if (a == arch::xthead) {
    return xthead_supported();
  }
  const vec_state& s = vec_probe();
  if (!s.rvv) {
    return false;
  }
  switch (a) {
    case arch::rvv128:      return s.vlenb == 16;
    case arch::rvv256:      return s.vlenb == 32;
    case arch::rvv512:      return s.vlenb == 64;
    case arch::rvv128_zvbb: return s.zvbb && s.vlenb == 16;
    case arch::rvv256_zvbb: return s.zvbb && s.vlenb == 32;
    case arch::rvv512_zvbb: return s.zvbb && s.vlenb == 64;
    default:                return false;
  }
#else
  (void)a;
  return false;
#endif
}

}  // namespace blake3pp::detail

#endif  // BLAKE3PP_CPU_DETECT_RISCV
