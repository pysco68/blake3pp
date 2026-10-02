// Compiled into every program this project builds (blake3pp::node_program):
// the tools, the tests and the benchmarks. Empty on every target but one.
//
// LLVM's libunwind on powerpc64 restores the callee-saved vector registers
// v20-v31 when it resumes at a catch only for the bits set in VRSAVE, and
// Linux (ELFv2) leaves VRSAVE at 0. Values the compiler kept in those
// registers then come back as whatever the throwing frames left there,
// after any caught exception. Marking every vector register as live makes
// libunwind restore all of them. Threads created afterwards start with
// the VRSAVE of the thread that created them, so setting it before main()
// covers the pool and the I/O lanes. See docs/static-linux-binaries.md.
// Upstream: https://github.com/llvm/llvm-project/issues/231806.
// Remove it once the toolchain's libunwind restores v20-v31 without
// VRSAVE; the test "the unwinder restores v20-v31 after a caught
// exception" then reports that it should have failed.
#include <version>  // defines _LIBCPP_VERSION under libc++

#if defined(__powerpc64__) && defined(__linux__) && defined(_LIBCPP_VERSION)
namespace {
[[gnu::constructor]] void mark_vector_registers_live() noexcept {
  asm volatile("mtvrsave %0" ::"r"(-1));
}
}  // namespace
#endif
