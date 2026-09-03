#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <blake3pp/dispatch.hpp>

// The test binary owns its process, so it opts into the trap-guarded
// detection rungs before running the suite; on riscv vendor-kernel
// hardware (the Cloud-V Pioneer lane) this is what puts the xthead
// variant on the all-available-arches test loops.
int main(int argc, char** argv) {
  blake3pp::run_trap_probes();
  return doctest::Context(argc, argv).run();
}
