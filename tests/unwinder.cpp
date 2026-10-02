// What the C++ runtime's unwinder restores after a caught exception, on
// the one target where that has gone wrong: powerpc64 Linux. LLVM's
// libunwind there restores v20-v31 only for the bits set in VRSAVE.
// tooling/vrsave_workaround.cpp sets every bit before main(), so the
// programs this project builds are not affected; the first case clears
// VRSAVE to look at the toolchain alone, the second checks the
// workaround reaches a thread created later. Elsewhere this file is empty.
#include <doctest/doctest.h>

#if defined(__powerpc64__) && defined(__linux__)
#include <altivec.h>

#include <stdexcept>
#include <thread>

namespace {

#if defined(_LIBCPP_VERSION)
// LLVM's libc++ comes with LLVM's libunwind on this target.
inline constexpr bool vrsave_unwind_bug = true;
#else
inline constexpr bool vrsave_unwind_bug = false;
#endif

[[gnu::noinline]] void clobber_v31_and_throw() {
  // Declaring v31 clobbered makes the compiler save it here and describe
  // where in the CFI, which is what the unwinder restores it from.
  asm volatile("vspltisw 31,-1" ::: "v31");
  throw std::runtime_error("unwind");
}

// Whether a value held in v31 across a call that throws survives the
// catch.
[[gnu::noinline]] bool v31_survives_a_catch() {
  register __vector unsigned int x asm("v31") = {1, 2, 3, 4};
  asm volatile("" : "+v"(x));
  try {
    clobber_v31_and_throw();
  } catch (const std::exception&) {
  }
  asm volatile("" : "+v"(x));
  return x[0] == 1 && x[1] == 2 && x[2] == 3 && x[3] == 4;
}

unsigned long read_vrsave() noexcept {
  unsigned long v = 0;
  asm volatile("mfvrsave %0" : "=r"(v));
  return v;
}

void write_vrsave(unsigned long v) noexcept {
  asm volatile("mtvrsave %0" ::"r"(v));
}

}  // namespace

// Expected to fail where the toolchain has the bug. Once it reports that
// it should have failed, the toolchain restores v20-v31 by itself and
// tooling/vrsave_workaround.cpp can go.
TEST_CASE("the unwinder restores v20-v31 after a caught exception" *
          doctest::should_fail(vrsave_unwind_bug)) {
  const unsigned long saved = read_vrsave();
  write_vrsave(0);  // as Linux leaves it
  const bool survived = v31_survives_a_catch();
  write_vrsave(saved);
  CHECK(survived);
}

TEST_CASE("v20-v31 survive a caught exception on a thread created later") {
  if constexpr (vrsave_unwind_bug) {
    REQUIRE(read_vrsave() != 0);  // tooling/vrsave_workaround.cpp ran
  }
  bool survived = false;
  std::thread([&] { survived = v31_survives_a_catch(); }).join();
  CHECK(survived);
}

#endif
