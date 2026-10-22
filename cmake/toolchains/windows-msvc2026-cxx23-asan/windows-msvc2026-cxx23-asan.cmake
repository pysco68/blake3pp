# Hand-written (NOT emitted by tools/gen-toolchains.py -- that generator only
# knows the GNU-driver matrix).
#
# Windows MSVC 2026 (cl.exe), C++23, with AddressSanitizer: the IOCP backend
# is one of the two no Linux sanitizer lane executes, and an async lifetime
# bug is what lives there. cl rather than clang-cl because cl links the ASan
# runtime itself; windows-common.cmake says what goes wrong otherwise.
# Run from a vcvars64 shell.
#
# KNOWN SLOW, and deliberately not in CI yet: cl plus ASan on the KERNEL
# translation units does not finish in a usable time. Measured on a Ryzen AI
# MAX+ 395, three kernel objects compiling in parallel: past 500 s of CPU
# each at the kernels' /Ob3, and past 335 s each with the inlining budget
# dropped to /Ob1, neither producing an object. What the lane exists for is
# the I/O backend and the pipeline, not a leaf kernel whose hazards five
# Linux lanes already cover, so the fix is to stop instrumenting the
# kernels -- which needs the sanitizer flags to move out of the toolchain's
# global CMAKE_CXX_FLAGS into a target-scoped interface library, a change
# that touches every sanitizer lane. This preset is here because that
# refactor should start from a lane that exists.
#
#   cmake -S . -B build/windows-msvc2026-cxx23-asan -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-msvc2026-cxx23-asan/windows-msvc2026-cxx23-asan.cmake

set(TC_C_COMPILER "cl")
set(TC_CXX_COMPILER "cl")
set(TC_CXX_STANDARD "23")
set(TC_DEFAULT_BUILD_TYPE "Release")
set(TC_SANITIZERS address)

include("${CMAKE_CURRENT_LIST_DIR}/../windows-common.cmake")
