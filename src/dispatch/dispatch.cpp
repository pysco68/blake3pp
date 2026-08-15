// Runtime architecture routing. M1 wires the seam with the scalar table
// only; M2 adds cpuid/xgetbv (x86) and getauxval (ARM-Linux) detection and
// the SIMD variant tables. Selection stays a plain pointer to a
// constexpr-initialized POD table: no heap, no vtable, no ifunc.

#include <blake3pp/dispatch.hpp>

#include "kernel/kernel.hpp"

namespace blake3pp {

bool is_available(arch a) noexcept {
  switch (a) {
    case arch::auto_detect:
    case arch::scalar:
      return true;
    default:
      return false;
  }
}

arch best_available() noexcept { return arch::scalar; }

const char* to_string(arch a) noexcept {
  switch (a) {
    case arch::auto_detect:
      return "auto";
    case arch::scalar:
      return "scalar";
    case arch::sse42:
      return "sse42";
    case arch::avx2:
      return "avx2";
    case arch::avx512:
      return "avx512";
    case arch::neon:
      return "neon";
  }
  return "unknown";
}

namespace detail {

const kern::kernel_ops* resolve(arch a) noexcept {
  switch (a) {
    case arch::auto_detect:
    case arch::scalar:
    default:
      return &kern::scalar::ops;
  }
}

}  // namespace detail
}  // namespace blake3pp
