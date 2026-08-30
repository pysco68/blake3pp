// riscv64 capability probe, Linux-only (no other riscv64 OS target
// exists here). Three layers of history in one file:
//
//  * RVV 1.0: the auxv HWCAP bit for the single-letter 'V' extension
//    (bits 0-25 map A-Z), then the vlenb CSR for the vector length.
//    Reading a vector CSR needs V enabled at the ASSEMBLER level; the
//    .option push/arch/pop dance scopes that to one instruction so this
//    flag-neutral TU still builds as plain rv64gc, and the read only
//    ever EXECUTES behind the hwcap check.
//  * Zvbb: no single-letter HWCAP bit exists; the hwprobe IMA_EXT_0 key
//    carries it. Raw syscall: glibc grew a wrapper only recently and
//    musl has none; ENOSYS (pre-6.4 kernels) degrades to "absent".
//  * XTheadVector (draft RVV 0.7.1, T-Head encoding): Linux 6.13+
//    reports it through the hwprobe vendor-extension key. Never parse
//    /proc/cpuinfo: old vendor kernels print a bare "v" for 0.7.1. The
//    BLAKE3PP_ASSUME_XTHEADVECTOR=1 env hook exists for emulator testing
//    (T-Head's qemu fork predates the hwprobe key) and is honored for
//    this one arch only.

#include "dispatch/cpu_detect.hpp"

#if defined(BLAKE3PP_CPU_DETECT_RISCV)

#include <cstdint>
#include <cstdlib>

#if defined(__linux__)
#define BLAKE3PP_RISCV64_LINUX 1
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef BLAKE3PP_NR_riscv_hwprobe
#define BLAKE3PP_NR_riscv_hwprobe 258
#endif
#ifndef RISCV_HWPROBE_KEY_IMA_EXT_0
#define RISCV_HWPROBE_KEY_IMA_EXT_0 4
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

struct rvv_state {
  bool v = false;
  bool zvbb = false;
  unsigned long vlenb = 0;
};

const rvv_state& rvv_probe() noexcept {
  static const rvv_state s = [] {
    rvv_state st{};
    if ((getauxval(AT_HWCAP) & (1UL << ('V' - 'A'))) != 0) {
      unsigned long vlenb = 0;
      asm(".option push\n\t"
          ".option arch, +v\n\t"
          "csrr %0, vlenb\n\t"
          ".option pop"
          : "=r"(vlenb));
      st.v = vlenb != 0;
      st.vlenb = vlenb;
      if (st.v) {
        std::uint64_t ext = 0;
        st.zvbb = hwprobe_one(RISCV_HWPROBE_KEY_IMA_EXT_0, &ext) == 0 &&
                  (ext & RISCV_HWPROBE_EXT_ZVBB) != 0;
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
    std::uint64_t ext = 0;
    return hwprobe_one(RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0, &ext) == 0 &&
           (ext & RISCV_HWPROBE_VENDOR_EXT_XTHEADVECTOR) != 0;
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
  const rvv_state& s = rvv_probe();
  if (!s.v) {
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
