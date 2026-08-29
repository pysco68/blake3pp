// Freestanding correctness harness for the XTheadVector kernel, run under
// T-Head's Xuantie qemu fork on the c906fdv CPU model.
//
// Why freestanding: the fork's th-capable CPU models (c906fdv/c910v/c920)
// predate the RVA23-flavored scalar ISA that Ubuntu resolute's glibc uses
// unconditionally, so ANY glibc-linked binary (static or dynamic) dies
// of SIGILL in libc startup on those models; and the fork's generic
// xtheadvector=on property path is broken for userland outright. No libc,
// no problem: _start, two raw syscalls (write, exit), a local memcpy, and
// the two kernel object files.
//
// Build (see docker/riscv64-gcc15.Dockerfile for the image; all three TUs
// with -fno-exceptions -fno-rtti -fno-stack-protector):
//   riscv64-linux-gnu-g++ -std=c++23 -O2 -march=rv64gc \
//     -DBLAKE3PP_ARCH_NS=scalar -DBLAKE3PP_FORCE_SCALAR=1 \
//     -I src -I include -c src/kernel/kernel.cpp -o scalar.o ...
//   riscv64-linux-gnu-g++ ... -march=rv64gc_xtheadvector \
//     -mno-riscv-attribute -Wa,-mno-arch-attr \
//     -c src/kernel/xthead_kernel.cpp -o xthead.o
//   riscv64-linux-gnu-g++ ... -march=rv64gc -c tests/xthead_verify.cpp
//   riscv64-linux-gnu-g++ -nostdlib -static -o xthead_verify \
//     xthead_verify.o scalar.o xthead.o
// Run:
//   qemu-riscv64-xuantie -cpu c906fdv ./xthead_verify   (exit 0 = match)
//
// The oracle pattern matches the main test suite: the hand-written wide
// kernel must agree byte-for-byte with the scalar kernel on hash_many and
// xof_many over inputs that exercise the wide path, the serial remainder,
// and multi-block chaining.

#include <cstddef>
#include <cstdint>

#include "kernel/kernel.hpp"

namespace kern = blake3pp::kern;

namespace blake3pp::kern {
// Normally defined in dispatch.cpp, which this harness deliberately does
// not link (it would drag in hosted-libc surface). The scalar kernel reads
// it once per batch; any valid value works for width 1.
std::atomic<transpose16_mode> transpose16_active{transpose16_mode::staging};
namespace xthead {
extern const kernel_ops ops;
}
}  // namespace blake3pp::kern

namespace {

// ---- minimal runtime ----------------------------------------------------

void sys_write(const char* s, std::size_t n) noexcept {
  register std::intptr_t a0 asm("a0") = 1;  // stdout
  register std::intptr_t a1 asm("a1") = reinterpret_cast<std::intptr_t>(s);
  register std::intptr_t a2 asm("a2") = static_cast<std::intptr_t>(n);
  register std::intptr_t a7 asm("a7") = 64;  // __NR_write
  asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
}

[[noreturn]] void sys_exit(int code) noexcept {
  register std::intptr_t a0 asm("a0") = code;
  register std::intptr_t a7 asm("a7") = 93;  // __NR_exit
  asm volatile("ecall" : : "r"(a0), "r"(a7));
  __builtin_unreachable();
}

void say(const char* s) noexcept {
  std::size_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  sys_write(s, n);
}

// ---- the checks ---------------------------------------------------------

constexpr std::size_t num_inputs = 9;  // 2 full width-4 batches + 1 tail
constexpr std::size_t blocks = 3;      // multi-block chaining
constexpr std::size_t input_bytes = blocks * kern::block_len;

std::uint8_t data[num_inputs * input_bytes];
std::uint8_t out_scalar[num_inputs * kern::out_len];
std::uint8_t out_xthead[num_inputs * kern::out_len];
std::uint8_t xof_scalar[7 * 64];
std::uint8_t xof_xthead[7 * 64];

bool bytes_equal(const std::uint8_t* a, const std::uint8_t* b,
                 std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

int run() noexcept {
  for (std::size_t i = 0; i < sizeof data; ++i) {
    data[i] = static_cast<std::uint8_t>((i * 7 + 3) % 251);
  }
  const std::uint8_t* inputs[num_inputs];
  for (std::size_t i = 0; i < num_inputs; ++i) {
    inputs[i] = data + i * input_bytes;
  }

  constexpr std::uint64_t counter = 0x1234567890ULL;  // exercises hi word
  kern::scalar::ops.hash_many(inputs, num_inputs, blocks, kern::iv.data(),
                              counter, true, 0, kern::flag_chunk_start,
                              kern::flag_chunk_end, out_scalar);
  kern::xthead::ops.hash_many(inputs, num_inputs, blocks, kern::iv.data(),
                              counter, true, 0, kern::flag_chunk_start,
                              kern::flag_chunk_end, out_xthead);
  if (!bytes_equal(out_scalar, out_xthead, sizeof out_scalar)) {
    say("FAIL: hash_many diverges from scalar\n");
    return 1;
  }

  // XOF: 7 blocks = one wide group of 4 + a serial remainder of 3.
  kern::scalar::ops.xof_many(kern::iv.data(), data, kern::block_len, counter,
                             kern::flag_root, xof_scalar, 7);
  kern::xthead::ops.xof_many(kern::iv.data(), data, kern::block_len, counter,
                             kern::flag_root, xof_xthead, 7);
  if (!bytes_equal(xof_scalar, xof_xthead, sizeof xof_scalar)) {
    say("FAIL: xof_many diverges from scalar\n");
    return 1;
  }

  say("OK: xthead matches scalar on hash_many and xof_many\n");
  return 0;
}

}  // namespace

extern "C" {

// GCC lowers block copies/fills (and pattern-matched loops) to these even
// in freestanding TUs.
std::size_t strlen(const char* s) {
  std::size_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}

void* memcpy(void* dst, const void* src, std::size_t n) {
  auto* d = static_cast<std::uint8_t*>(dst);
  const auto* s = static_cast<const std::uint8_t*>(src);
  for (std::size_t i = 0; i < n; ++i) {
    d[i] = s[i];
  }
  return dst;
}

void* memset(void* dst, int c, std::size_t n) {
  auto* d = static_cast<std::uint8_t*>(dst);
  for (std::size_t i = 0; i < n; ++i) {
    d[i] = static_cast<std::uint8_t>(c);
  }
  return dst;
}

[[noreturn]] void _start() {
  // crt1's job, done by hand: linker relaxation turns global accesses into
  // gp-relative ones, and nothing has set gp yet. norelax so this very
  // sequence is not itself relaxed into a gp-relative load of gp.
  asm volatile(
      ".option push\n\t"
      ".option norelax\n\t"
      "la gp, __global_pointer$\n\t"
      ".option pop" ::: "memory");
  sys_exit(run());
}

}  // extern "C"
