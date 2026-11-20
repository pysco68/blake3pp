#pragma once

// One iteration of a busy wait: tells the core it is spinning where the
// ISA has a way to say so. The stream reader's watch spins on it between
// probes, and so do the pipe bench's tools.
//
// On AArch64 the hint is `isb`, an instruction synchronisation barrier
// used for its latency rather than its ordering: `yield` is the hint the
// architecture names for this, but it is a no-op on most implementations
// and spins at full rate, and `wfe` waits for an event the producer never
// sends and so could sleep until the next timer tick. `isb` drains the
// pipeline, which costs a few tens of cycles and nothing else, and is
// what Rust's std::hint::spin_loop emits on AArch64 for the same reason.
// x86 has `pause`, which is also what Windows' YieldProcessor() is there.
//
// Internal to the library. Needs no library to link.

#if defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || \
    defined(_M_IX86)
#include <immintrin.h>
#endif

namespace blake3pp::detail {

inline void cpu_relax() noexcept {
#if defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
  __isb(_ARM64_BARRIER_SY);
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || \
    defined(_M_IX86)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("isb" ::: "memory");
#else
  asm volatile("" ::: "memory");
#endif
}

}  // namespace blake3pp::detail
