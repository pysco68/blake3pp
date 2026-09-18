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
//    would have crashed IS the classifier.
//
// The trap-guarded rungs (the vsetvli unit probe on the SG2042 shape
// and the vlenb classifier on pre-hwprobe kernels) do NOT run during
// default detection: swapping the SIGILL disposition, however briefly,
// is a process-global side effect, and the library never does that as
// a side effect of hashing. Default detection records that one of
// those shapes is present and conservatively claims nothing (scalar).
// blake3pp::run_trap_probes() is the explicit opt-in: it runs the
// guarded rungs once and upgrades the detection state. The shipped
// tools call it at startup; they own their process.
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

#include <atomic>
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

#if defined(BLAKE3PP_TEST_PROBE_SHAPES) && defined(BLAKE3PP_RISCV64_LINUX)
// Test build only (tests/riscv_probe_shapes.cpp compiles this TU with the
// define): the kernel's answers, HWCAP and hwprobe, come from a machine
// description instead of the syscalls, so every rung of the ladder below
// runs against a shape qemu-user cannot present (it reports no vendor id
// and never lacks hwprobe). The guarded probes still execute the real
// instructions; force_trap swaps in an illegal one so the SIGILL guard's
// unwind runs too (qemu-user executes vector instructions whatever the
// CPU model says). The shipped library never sees any of this.
namespace blake3pp::detail::test {
machine g_machine{};
void set_machine(const machine& m) noexcept { g_machine = m; }
}  // namespace blake3pp::detail::test
#endif

namespace blake3pp::detail {
namespace {

#if defined(BLAKE3PP_RISCV64_LINUX)
long hwprobe_one(std::int64_t key, std::uint64_t* value) noexcept {
#if defined(BLAKE3PP_TEST_PROBE_SHAPES)
  if (test::g_machine.active) {
    const test::machine& m = test::g_machine;
    if (!m.hwprobe) {
      return -1;
    }
    switch (key) {
      case RISCV_HWPROBE_KEY_MVENDORID: *value = m.mvendorid; return 0;
      case RISCV_HWPROBE_KEY_IMA_EXT_0: *value = m.ima_ext_0; return 0;
      case RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0:
        if (!m.vendor_key) {
          return -1;
        }
        *value = m.vendor_ext_thead_0;
        return 0;
      default: return -1;
    }
  }
#endif
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

bool hwcap_has_v() noexcept {
#if defined(BLAKE3PP_TEST_PROBE_SHAPES)
  if (test::g_machine.active) {
    return test::g_machine.hwcap_v;
  }
#endif
  return (getauxval(AT_HWCAP) & (1UL << ('V' - 'A'))) != 0;
}

#if defined(BLAKE3PP_TEST_PROBE_SHAPES)
bool test_force_trap() noexcept {
  return test::g_machine.active && test::g_machine.force_trap;
}
#else
constexpr bool test_force_trap() noexcept { return false; }
#endif

// The trap-rung classifier state. Only ever touched from
// platform_run_trap_probes() below, whose magic static guarantees
// exactly one thread runs the guarded probes, exactly once; the SIGILL
// handler is scoped to the probed instruction and restored immediately.
// A signal disposition is process-wide, so a SIGILL raised on another
// thread inside that window reaches this handler too. The jump buffer
// is per thread and the flag says whether this thread is probing; a
// foreign fault gets the previous disposition back and returns, so the
// faulting instruction re-executes under it and takes the path it
// would have taken anyway, instead of a siglongjmp into a frame that
// is not on its stack.
thread_local sigjmp_buf g_probe_jmp;
thread_local volatile std::sig_atomic_t g_probing = 0;
struct sigaction g_probe_old {};

void probe_sigill(int) {
  if (g_probing) {
    siglongjmp(g_probe_jmp, 1);
  }
  sigaction(SIGILL, &g_probe_old, nullptr);
}

// Run one probe under a scoped SIGILL guard. Only ever called from
// platform_run_trap_probes() (single thread via its magic static);
// the handler is restored immediately. Signal disposition is
// process-global while the swap lasts, which is exactly why these
// probes run only behind the explicit blake3pp::run_trap_probes()
// opt-in and never as a side effect of default detection.
template <class F>
bool guarded(F&& body) noexcept {
  struct sigaction sa {};
  sa.sa_handler = &probe_sigill;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGILL, &sa, &g_probe_old) != 0) {
    return false;  // cannot make the probe safe -> claim nothing
  }
  bool ok = false;
  g_probing = 1;
  if (sigsetjmp(g_probe_jmp, 1) == 0) {
    body();
    ok = true;
  }
  g_probing = 0;
  sigaction(SIGILL, &g_probe_old, nullptr);
  return ok;
}

unsigned long guarded_vlenb() noexcept {
  unsigned long vlenb = 0;
  if (!guarded([&] {
        if (test_force_trap()) {
          asm volatile("unimp");
        }
        asm volatile(
            ".option push\n\t"
            ".option arch, +v\n\t"
            "csrr %0, vlenb\n\t"
            ".option pop"
            : "=r"(vlenb));
      })) {
    return 0;  // trapped: pre-v0.9 vector (no vlenb CSR), i.e. 0.7.1
  }
  return vlenb;
}

// Which dialect is the vector unit speaking? The vtype immediate layout
// differs: ratified 1.0 keeps vsew in bits 5:3 and vlmul in 2:0, draft
// 0.7.1 keeps vsew in 4:2 and vlmul in 1:0. One vsetvli with zimm =
// 0b0001000 (rs1 = x0: vl becomes VLMAX; rd = a0) therefore selects
// e16/m1 on 1.0 hardware and e32/m1 on 0.7.1 hardware, and VLMAX comes
// out as vlenb/2 or vlenb/4. Hand-encoded for the same reason as below.
// Returns 0 if the instruction trapped.
unsigned long guarded_vsetvli_lanes([[maybe_unused]] unsigned long vlenb) noexcept {
#if defined(BLAKE3PP_TEST_PROBE_SHAPES)
  if (test::g_machine.active && test::g_machine.dialect_071) {
    return vlenb / 4;
  }
#endif
  unsigned long lanes = 0;
  if (!guarded([&] {
        // The hand-encoded word writes a0; a register-bound local inside
        // the lambda (a captured one may not live in a register).
        register unsigned long a0 asm("a0") = 0;
        asm volatile(".word 0x00807557" : "=r"(a0));  // vsetvli a0, x0, 0b0001000
        lanes = a0;
      })) {
    return 0;
  }
  return lanes;
}

// Does the kernel let user mode touch the vector unit at all? vsetvli
// shares its encoding shape between draft 0.7.1 and ratified 1.0 (only
// the vtype immediate layout differs), so this single instruction
// executes on EITHER dialect, and traps iff the kernel left the unit
// disabled. Hand-encoded (.word: vsetvli t0, x0, 0) because this TU is
// compiled flag-neutral by compilers that may know neither dialect.
bool guarded_vector_unit_enabled() noexcept {
  return guarded([] {
    if (test_force_trap()) {
      asm volatile("unimp");
    }
    asm volatile(".word 0x000072D7" ::: "t0");
  });
}

struct vec_state {
  bool rvv = false;     // ratified RVV 1.0
  bool xthead = false;  // draft 0.7.1, T-Head encoding
  bool zvbb = false;
  unsigned long vlenb = 0;
  // Shapes the syscall rungs recognized but could not settle without a
  // trap-guarded probe; resolved only by platform_run_trap_probes().
  bool pending_vendor_unit = false;  // rung 2.5 (SG2042 6.6-pioneer)
  bool pending_legacy = false;       // rung 3 (pre-hwprobe kernels)
};

// Filled once by platform_run_trap_probes(); readers reach it through
// the acquire-loaded pointer, so the upgrade publishes as one unit.
vec_state g_trap_upgraded;
std::atomic<const vec_state*> g_trap_active{nullptr};

// Default detection: the syscall-only rungs. Never installs a signal
// handler; where only a trap-guarded probe could answer, it records the
// pending shape and claims nothing.
const vec_state& base_probe() noexcept {
  static const vec_state s = [] {
    vec_state st{};
    // Rung 1: the authoritative answer, where the kernel is new enough.
    std::uint64_t vendor = 0;
    if (hwprobe_one(RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0, &vendor) == 0 &&
        (vendor & RISCV_HWPROBE_VENDOR_EXT_XTHEADVECTOR) != 0) {
      st.xthead = true;
      return st;
    }
    const bool hwcap_v = hwcap_has_v();
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
        // That is the vendor-kernel 0.7.1 lie, caught in the act. Pure
        // syscall evidence, so it stays in default detection.
        st.xthead = true;
      } else if (!hwcap_v &&
                 hwprobe_one(RISCV_HWPROBE_KEY_MVENDORID, &vendor) == 0 &&
                 vendor == BLAKE3PP_MVENDORID_THEAD) {
        // Rung 2.5 SHAPE, the SG2042 6.6-pioneer: hwprobe exists but
        // predates the vendor key, and the kernel advertises no V
        // ANYWHERE (no hwcap bit, no IMA_V). The vendor patch may
        // still be enabling and context-switching the T-Head vector
        // unit; only the guarded vsetvli can settle that (it traps iff
        // the unit is off), and that probe waits for the opt-in.
        st.pending_vendor_unit = true;
      }
      // Any other contradiction: claim nothing rather than execute a
      // guess.
    } else if (hwcap_v) {
      // Rung 3 SHAPE: pre-hwprobe vendor kernel. vlenb postdates
      // 0.7.1, so the guarded read classifies (a value is legacy RVV
      // 1.0, a trap is T-Head 0.7.1); it waits for the opt-in.
      st.pending_legacy = true;
    }
    return st;
  }();
  return s;
}

// The state everyone reads: the trap-upgraded snapshot once it exists,
// the syscall-only base until then.
const vec_state& vec_probe() noexcept {
  const vec_state* p = g_trap_active.load(std::memory_order_acquire);
  return p != nullptr ? *p : base_probe();
}

bool xthead_supported() noexcept {
  // The env hook is parsed once; the probe state is re-read every time
  // so a later run_trap_probes() upgrade is visible here.
  static const int assume = [] {
    const char* v = std::getenv("BLAKE3PP_ASSUME_XTHEADVECTOR");
    if (v != nullptr && v[0] == '1') {
      return 1;  // an unchecked assertion: the caller vouches for the CPU
    }
    if (v != nullptr && v[0] == '0') {
      return 0;  // explicit opt-out: the ladder's emergency brake
    }
    return -1;
  }();
  if (assume >= 0) {
    return assume == 1;
  }
  return vec_probe().xthead;
}
#endif  // BLAKE3PP_RISCV64_LINUX

}  // namespace

// The explicit opt-in (see blake3pp::run_trap_probes): runs the
// trap-guarded rungs the default detection deferred, once, and
// publishes the upgraded state. Returns whether anything was learned.
bool platform_run_trap_probes() noexcept {
#if defined(BLAKE3PP_RISCV64_LINUX)
  static const bool changed = [] {
    const vec_state& b = base_probe();
    vec_state up = b;
    up.pending_vendor_unit = false;
    up.pending_legacy = false;
    if (b.pending_vendor_unit && guarded_vector_unit_enabled()) {
      // A unit that is ON while the kernel claims no standard V, on a
      // T-Head core, is the vendor th path; a kernel managing REAL 1.0
      // state advertises it through the standard has_vector() plumbing
      // every >=6.4 kernel shares, so it cannot land here.
      up.xthead = true;
    } else if (b.pending_legacy) {
      const unsigned long vlenb = guarded_vlenb();
      if (vlenb == 0) {
        up.xthead = true;  // no vlenb CSR at all: draft 0.7.1
      } else {
        // A readable vlenb does not settle the dialect after all: the
        // C906 under its 5.10 vendor kernel (LicheeRV Nano, 2026-09-08)
        // reads it fine and speaks 0.7.1. The vtype layout does settle it.
        const unsigned long lanes = guarded_vsetvli_lanes(vlenb);
        if (lanes == vlenb / 2) {
          up.rvv = true;
          up.vlenb = vlenb;  // zvbb undetectable here; stays false
        } else if (lanes == vlenb / 4) {
          up.xthead = true;
        }
        // Anything else: claim nothing rather than execute a guess.
      }
    }
    if (up.rvv == b.rvv && up.xthead == b.xthead) {
      return false;  // probes ran (or nothing was pending): no news
    }
    g_trap_upgraded = up;
    g_trap_active.store(&g_trap_upgraded, std::memory_order_release);
    return true;
  }();
  return changed;
#else
  return false;
#endif
}

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
