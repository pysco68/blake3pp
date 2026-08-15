// Runtime architecture routing. Selection stays a plain pointer to a
// constexpr-initialized POD table: no heap, no vtable, no ifunc.
//
// On x86 the check is __builtin_cpu_supports, which goes through libgcc /
// compiler-rt's cpu-model probe; that includes the OSXSAVE/XCR0 check, so
// "avx2" is only reported when the OS actually saves YMM state, not merely
// when the CPU has the silicon. On AArch64, NEON is architecturally
// mandatory, so presence of the kernel implies availability.

#include <blake3pp/dispatch.hpp>

#include <initializer_list>

#include "kernel/kernel.hpp"

namespace blake3pp {

namespace {

bool cpu_supports(arch a) noexcept {
  switch (a) {
    case arch::auto_detect:
    case arch::scalar:
      return true;
#if defined(__x86_64__) || defined(__i386__)
    case arch::sse42:
      return __builtin_cpu_supports("sse4.2");
    case arch::avx2:
      return __builtin_cpu_supports("avx2");
    case arch::avx512:
      return __builtin_cpu_supports("avx512f") &&
             __builtin_cpu_supports("avx512cd") &&
             __builtin_cpu_supports("avx512vl") &&
             __builtin_cpu_supports("avx512bw") &&
             __builtin_cpu_supports("avx512dq");
#endif
#if defined(__aarch64__)
    case arch::neon:
      return true;
#endif
    default:
      return false;
  }
}

bool compiled_in(arch a) noexcept {
  switch (a) {
    case arch::auto_detect:
    case arch::scalar:
      return true;
#if defined(BLAKE3PP_HAS_KERNEL_SSE42)
    case arch::sse42:
      return true;
#endif
#if defined(BLAKE3PP_HAS_KERNEL_AVX2)
    case arch::avx2:
      return true;
#endif
#if defined(BLAKE3PP_HAS_KERNEL_AVX512)
    case arch::avx512:
      return true;
#endif
#if defined(BLAKE3PP_HAS_KERNEL_NEON)
    case arch::neon:
      return true;
#endif
    default:
      return false;
  }
}

}  // namespace

bool is_available(arch a) noexcept {
  return compiled_in(a) && cpu_supports(a);
}

arch best_available() noexcept {
  for (const arch a : {arch::avx512, arch::avx2, arch::sse42, arch::neon}) {
    if (is_available(a)) {
      return a;
    }
  }
  return arch::scalar;
}

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
  if (a == arch::auto_detect || !is_available(a)) {
    a = best_available();
  }
  switch (a) {
#if defined(BLAKE3PP_HAS_KERNEL_SSE42)
    case arch::sse42:
      return &kern::sse42::ops;
#endif
#if defined(BLAKE3PP_HAS_KERNEL_AVX2)
    case arch::avx2:
      return &kern::avx2::ops;
#endif
#if defined(BLAKE3PP_HAS_KERNEL_AVX512)
    case arch::avx512:
      return &kern::avx512::ops;
#endif
#if defined(BLAKE3PP_HAS_KERNEL_NEON)
    case arch::neon:
      return &kern::neon::ops;
#endif
    default:
      return &kern::scalar::ops;
  }
}

}  // namespace detail
}  // namespace blake3pp
