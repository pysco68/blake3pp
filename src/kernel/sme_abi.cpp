// The SME ABI support routine clang's locally streaming prologue calls when
// SVE is not a target feature: __arm_get_current_vg, the current vector
// granule for unwinding. compiler-rt provides it; zig's runtime does not,
// so a weak definition rides along with the streaming kernels on ELF. The
// routine may touch x0 only. In streaming mode the granule comes from
// rdsvl; otherwise from cntd where the ID register (read at EL0 through
// the kernel's emulation) reports SVE, and it is 0 on an SME-only part,
// where cntd would trap.
#if defined(__aarch64__) && defined(__clang__) && defined(__ELF__)
__asm__(
    "    .text\n"
    "    .weak __arm_get_current_vg\n"
    "    .type __arm_get_current_vg, %function\n"
    "__arm_get_current_vg:\n"
    "    .arch_extension sme\n"
    "    mrs x0, SVCR\n"
    "    tbz x0, #0, 1f\n"
    "    rdsvl x0, #1\n"
    "    lsr x0, x0, #3\n"
    "    ret\n"
    "1:  mrs x0, ID_AA64PFR0_EL1\n"
    "    ubfx x0, x0, #32, #4\n"
    "    cbz x0, 2f\n"
    "    .arch_extension sve\n"
    "    cntd x0\n"
    "    ret\n"
    "2:  mov x0, #0\n"
    "    ret\n"
    "    .size __arm_get_current_vg, .-__arm_get_current_vg\n");
#endif
