// The riscv detection ladder against machine shapes qemu-user cannot
// present. This executable compiles src/dispatch/cpu_detect_riscv.cpp
// itself with BLAKE3PP_TEST_PROBE_SHAPES, which replaces the kernel's
// answers (HWCAP, hwprobe) with a description set here; the ladder's
// logic and the guarded probes run for real. One shape per process, since
// detection is computed once per process: the shape is argv[1], and
// tests/CMakeLists.txt registers one test per shape.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dispatch/cpu_detect.hpp"

namespace {

using blake3pp::arch;
using blake3pp::detail::platform_cpu_supports;
using blake3pp::detail::platform_run_trap_probes;
using blake3pp::detail::test::machine;

constexpr std::uint64_t thead = 0x5b7;
constexpr std::uint64_t ima_v = 1ULL << 2;
constexpr std::uint64_t ext_zvbb = 1ULL << 17;

struct shape {
  const char* name;
  machine m;            // what the kernel answers (defaults: no V anywhere,
                        // hwprobe present without the vendor key, vendor 0)
  bool expect_learned;  // run_trap_probes() had something to add
  bool expect_xthead;   // T-Head 0.7.1 vector claimed afterwards
  bool expect_rvv;      // some RVV 1.0 variant claimed (vlenb is qemu's)
};

// Every shape starts from the same machine: hwprobe present, no vendor
// key, no V in HWCAP or IMA_EXT_0, vendor id 0, probes execute for real.
// A shape overrides only what defines it. active=true switches the seam on.
constexpr machine base{.active = true};

const shape shapes[] = {
    // Rung 1: a >= 6.15 kernel whose hwprobe carries the T-Head vendor
    // key with the XTheadVector bit; no probe needed.
    {.name = "vendor",
     .m = {.active = true, .vendor_key = true, .vendor_ext_thead_0 = 1, .mvendorid = thead},
     .expect_learned = false, .expect_xthead = true, .expect_rvv = false},
    // Rung 2: HWCAP says V, hwprobe's IMA_EXT_0 says no V, the core is
    // T-Head: the vendor-kernel 0.7.1 lie, pure syscall evidence.
    {.name = "thead-lie",
     .m = {.active = true, .hwcap_v = true, .mvendorid = thead},
     .expect_learned = false, .expect_xthead = true, .expect_rvv = false},
    // Rung 2.5 (the SG2042 shape): hwprobe without the vendor key, no V
    // anywhere, T-Head core. Only the guarded vsetvli can settle it; it
    // executes under qemu, so the unit counts as on.
    {.name = "sg2042",
     .m = {.active = true, .mvendorid = thead},
     .expect_learned = true, .expect_xthead = true, .expect_rvv = false},
    // The same shape with the probe trapping: the unit is off, nothing claimed.
    {.name = "sg2042-off",
     .m = {.active = true, .mvendorid = thead, .force_trap = true},
     .expect_learned = false, .expect_xthead = false, .expect_rvv = false},
    // Rung 3: a pre-6.4 kernel (no hwprobe at all) advertising HWCAP V.
    // The guarded vlenb read plus the vsetvli dialect probe classify;
    // qemu answers as 1.0 hardware, so RVV.
    {.name = "legacy",
     .m = {.active = true, .hwcap_v = true, .hwprobe = false},
     .expect_learned = true, .expect_xthead = false, .expect_rvv = true},
    // The same with the read trapping: no vlenb CSR, so T-Head 0.7.1.
    {.name = "legacy-trap",
     .m = {.active = true, .hwcap_v = true, .hwprobe = false, .force_trap = true},
     .expect_learned = true, .expect_xthead = true, .expect_rvv = false},
    // The C906 shape (LicheeRV Nano): vlenb readable, but the vtype
    // layout says 0.7.1, so T-Head.
    {.name = "legacy-071",
     .m = {.active = true, .hwcap_v = true, .hwprobe = false, .dialect_071 = true},
     .expect_learned = true, .expect_xthead = true, .expect_rvv = false},
    // Rung 1's standard path: the kernel vouches for V and Zvbb.
    {.name = "modern",
     .m = {.active = true, .hwcap_v = true, .ima_ext_0 = ima_v | ext_zvbb},
     .expect_learned = false, .expect_xthead = false, .expect_rvv = true},
    // No V anywhere on an ordinary core: scalar, nothing pending.
    {.name = "nothing",
     .m = base,
     .expect_learned = false, .expect_xthead = false, .expect_rvv = false},
    // BLAKE3PP_ASSUME_XTHEADVECTOR=1 on the "nothing" machine asserts
    // T-Head without any probe (set by main() before detection runs)...
    {.name = "assume-1",
     .m = base,
     .expect_learned = false, .expect_xthead = true, .expect_rvv = false},
    // ...and =0 vetoes it on the sg2042 shape whose probe would say yes
    // (the probe still runs and reports having learned something).
    {.name = "assume-0",
     .m = {.active = true, .mvendorid = thead},
     .expect_learned = true, .expect_xthead = false, .expect_rvv = false},
};

bool any_rvv() {
  for (const arch a : {arch::rvv128, arch::rvv256, arch::rvv512, arch::rvv128_zvbb,
                       arch::rvv256_zvbb, arch::rvv512_zvbb}) {
    if (platform_cpu_supports(a)) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fputs("usage: riscv_probe_shapes <shape>\n", stderr);
    return 2;
  }
  for (const shape& s : shapes) {
    if (std::strcmp(s.name, argv[1]) != 0) {
      continue;
    }
    if (std::strcmp(s.name, "assume-1") == 0) {
      setenv("BLAKE3PP_ASSUME_XTHEADVECTOR", "1", 1);
    } else if (std::strcmp(s.name, "assume-0") == 0) {
      setenv("BLAKE3PP_ASSUME_XTHEADVECTOR", "0", 1);
    }
    blake3pp::detail::test::set_machine(s.m);
    const bool before_rvv = any_rvv();
    const bool before_xthead = platform_cpu_supports(arch::xthead);
    const bool learned = platform_run_trap_probes();
    const bool rvv = any_rvv();
    const bool xthead = platform_cpu_supports(arch::xthead);
    std::printf("%s: before rvv=%d xthead=%d; probes learned=%d; after rvv=%d xthead=%d\n",
                s.name, before_rvv, before_xthead, learned, rvv, xthead);
    if (learned != s.expect_learned || xthead != s.expect_xthead || rvv != s.expect_rvv) {
      std::printf("expected learned=%d xthead=%d rvv=%d\n", s.expect_learned,
                  s.expect_xthead, s.expect_rvv);
      return 1;
    }
    return 0;
  }
  std::fprintf(stderr, "unknown shape '%s'\n", argv[1]);
  return 2;
}
