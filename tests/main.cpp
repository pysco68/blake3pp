#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <atomic>

#include <blake3pp/dispatch.hpp>
#include <blake3pp/parallel.hpp>
#if defined(BLAKE3PP_HAS_SIZED_SCHEDULER)
#include <blake3pp/parallel_backend.hpp>
#endif

#include "suite_pool.hpp"

// The test binary owns its process, so it opts into the trap-guarded
// detection rungs before running the suite; on riscv vendor-kernel
// hardware (the Cloud-V Pioneer lane) this is what puts the xthead
// variant on the all-available-arches test loops.
int main(int argc, char** argv) {
  blake3pp::run_trap_probes();
#if defined(BLAKE3PP_HAS_STD_THREAD)
  // Sizing to tests/suite_pool.hpp's width has to happen before anything
  // fetches the scheduler, which is why it is here and not in a fixture.
#if defined(BLAKE3PP_HAS_SIZED_SCHEDULER)
  blake3pp::size_parallel_scheduler(b3test::suite_pool_threads());
#elif defined(BLAKE3PP_EXECUTION_STDEXEC)
  // Without the sized backend there is no public way to bound the process
  // pool, and the suite needs one (tests/suite_pool.hpp). This is the same
  // variable size_parallel_scheduler() writes, and writing it before the
  // first get_parallel_scheduler() call is the same contract.
  blake3pp::detail::process_pool_threads().store(
      b3test::suite_pool_threads(), std::memory_order_relaxed);
#endif
#endif
  return doctest::Context(argc, argv).run();
}
