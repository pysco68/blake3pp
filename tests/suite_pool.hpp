#pragma once

// What the suite's parallel cases may take of the machine.
//
// Every doctest case is its own ctest test and its own process, so a
// `ctest -j32` of a suite whose cases each fan out over every core runs
// 32 x 32 workers: on a 32-core box the cases that assert correctness
// spend minutes being descheduled, and one of them looked like a hang.
// Four threads exercise every parallel path the suite has -- none of them
// asserts a thread count -- and `PROCESSORS` in tests/CMakeLists.txt tells
// ctest the same number, so the two parallelisms multiply to the machine
// rather than to thirty-two times it.

#include <algorithm>
#include <thread>

namespace b3test {

inline constexpr unsigned suite_threads = 4;

// Whether tests/main.cpp can bound the process scheduler: through the
// sized backend, or through stdexec's pool size. beman's default backend
// takes every core and has no such knob.
inline constexpr bool suite_pool_bounded =
#if defined(BLAKE3PP_HAS_SIZED_SCHEDULER) || defined(BLAKE3PP_EXECUTION_STDEXEC)
    true;
#else
    false;
#endif

// The width of the process scheduler the suite runs on, and so the width
// a pool-busy share is taken over.
[[nodiscard]] inline unsigned suite_pool_threads() noexcept {
  const unsigned cores = std::thread::hardware_concurrency();
  return suite_pool_bounded ? std::min(suite_threads, std::max(2u, cores))
                            : std::max(1u, cores);
}

}  // namespace b3test
