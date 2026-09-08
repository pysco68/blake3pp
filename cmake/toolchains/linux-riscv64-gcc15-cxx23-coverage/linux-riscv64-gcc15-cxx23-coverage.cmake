# riscv64 cross toolchain + gcov coverage: the base toolchain with the
# instrumentation common.cmake adds for TC_COVERAGE=gcov (atomic counters:
# the parallel tests race the plain ones, GCC bug 68080). Optimised like
# the plain matrix (RelWithDebInfo): the shipped kernels only exist under
# optimisation, and the emulator matrix of tools/ci-test.sh runs every
# instrumented binary. The .gcda files land in the build tree like any
# other, so the whole matrix accumulates into one report.
include("${CMAKE_CURRENT_LIST_DIR}/../linux-riscv64-gcc15-cxx23/linux-riscv64-gcc15-cxx23.cmake")
string(APPEND CMAKE_C_FLAGS_INIT " --coverage -fprofile-update=atomic")
string(APPEND CMAKE_CXX_FLAGS_INIT " --coverage -fprofile-update=atomic")
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " --coverage")
