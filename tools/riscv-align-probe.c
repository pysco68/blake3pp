// What does this RISC-V core do with a misaligned access?
//
// The BananaPi F3 (SpacemiT X60) answers differently per access class,
// and the class decides which signal a fault raises: a misaligned
// vector load is an illegal instruction there, while a bus error must
// come from somewhere else. Each case runs in its own child, so one
// trap does not hide the rest, and the parent prints what killed it.
//
// Build with the lane's own flags, e.g.
//   riscv64-linux-musl-clang -O1 -static tools/riscv-align-probe.c -o probe
//
// Static musl rather than static glibc: glibc's startup resolves its
// string functions through IFUNC, which dies under qemu-user before
// main ever runs.
//
// Plain rv64gc on purpose: the vector instructions sit inside .option
// arch blocks, so the compiler emits no vector code of its own and the
// probe cannot die of something it was not asked to test.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned char buf[256] __attribute__((aligned(64)));
static unsigned int out[16] __attribute__((aligned(64)));

// Every case reads or writes at an ODD offset into an aligned buffer,
// so only the access itself can be at fault.
static void scalar_load(void) {
  unsigned int v;
  const void* p = buf + 1;
  __asm__ volatile("lw %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
  out[0] = v;
}

static void scalar_store(void) {
  void* p = buf + 1;
  __asm__ volatile("sw %0, 0(%1)" :: "r"(0x12345678u), "r"(p) : "memory");
}

static void doubleword_load(void) {
  unsigned long v;
  const void* p = buf + 4;  // 4-aligned, not 8: what a packed u64 sees
  __asm__ volatile("ld %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
  out[1] = (unsigned int)v;
}

// Atomics are the one class the kernel never emulates: RISC-V requires
// natural alignment for AMO and LR/SC.
static void atomic_add(void) {
  void* p = buf + 2;
  unsigned int old;
  __asm__ volatile("amoadd.w %0, %2, (%1)" : "=&r"(old) : "r"(p), "r"(1u) : "memory");
  out[2] = old;
}

static void vector_element_load(void) {
  const void* p = buf + 1;
  __asm__ volatile(
      ".option push\n\t.option arch, +v\n\t"
      "vsetivli t0, 4, e32, m1, ta, ma\n\t"
      "vle32.v v8, (%0)\n\t"
      "vse32.v v8, (%1)\n\t"
      ".option pop"
      :: "r"(p), "r"(out + 4) : "t0", "memory");
}

// The byte-width form the compiler picks when it knows nothing about
// alignment; it should be legal at any address.
static void vector_byte_load(void) {
  const void* p = buf + 1;
  __asm__ volatile(
      ".option push\n\t.option arch, +v\n\t"
      "vsetvli t0, zero, e8, m1, ta, ma\n\t"
      "vle8.v v8, (%0)\n\t"
      "vse8.v v8, (%1)\n\t"
      ".option pop"
      :: "r"(p), "r"(out + 8) : "t0", "memory");
}

// Whole-register moves, how a spilled vector travels.
static void vector_whole_register(void) {
  const void* p = buf + 1;
  __asm__ volatile(
      ".option push\n\t.option arch, +v\n\t"
      "vl1re32.v v8, (%0)\n\t"
      "vs1r.v v8, (%1)\n\t"
      ".option pop"
      :: "r"(p), "r"(out + 8) : "memory");
}

struct probe {
  const char* name;
  void (*fn)(void);
};

static const struct probe probes[] = {
    {"scalar lw   at +1", scalar_load},
    {"scalar sw   at +1", scalar_store},
    {"scalar ld   at +4", doubleword_load},
    {"amoadd.w    at +2", atomic_add},
    {"vle32.v     at +1", vector_element_load},
    {"vle8.v      at +1", vector_byte_load},
    {"vl1re32.v   at +1", vector_whole_register},
};

int main(void) {
  // A signal kills buffered output with the process, and the last line
  // printed is the evidence.
  setvbuf(stdout, NULL, _IONBF, 0);
  for (unsigned i = 0; i < sizeof buf; ++i) {
    buf[i] = (unsigned char)i;
  }
  for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; ++i) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
      probes[i].fn();
      _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
      const int sig = WTERMSIG(status);
      printf("%-18s killed by signal %d (%s)\n", probes[i].name, sig,
             strsignal(sig));
    } else {
      printf("%-18s ok\n", probes[i].name);
    }
  }
  return 0;
}
