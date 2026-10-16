#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <algorithm>
#include <thread>

#include <blake3pp/dispatch.hpp>
#include <blake3pp/parallel.hpp>
#if defined(BLAKE3PP_HAS_SIZED_SCHEDULER)
#include <blake3pp/parallel_backend.hpp>
#endif

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
//
// Sizing has to happen before anything fetches the scheduler, which is why
// it is here and not in a fixture.
constexpr unsigned suite_threads = 4;

// The test binary owns its process, so it opts into the trap-guarded
// detection rungs before running the suite; on riscv vendor-kernel
// hardware (the Cloud-V Pioneer lane) this is what puts the xthead
// variant on the all-available-arches test loops.
int main(int argc, char** argv) {
  blake3pp::run_trap_probes();
#if defined(BLAKE3PP_HAS_STD_THREAD)
  const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
  const unsigned want = std::min(suite_threads, cores);
#if defined(BLAKE3PP_HAS_SIZED_SCHEDULER)
  blake3pp::size_parallel_scheduler(want);
#elif defined(BLAKE3PP_EXECUTION_STDEXEC)
  // Without the sized backend there is no public way to bound the process
  // pool, and the suite needs one for the reason above. This is the same
  // variable size_parallel_scheduler() writes, and writing it before the
  // first get_parallel_scheduler() call is the same contract.
  blake3pp::detail::process_pool_threads().store(want,
                                                 std::memory_order_relaxed);
#endif
#endif
  return doctest::Context(argc, argv).run();
}
