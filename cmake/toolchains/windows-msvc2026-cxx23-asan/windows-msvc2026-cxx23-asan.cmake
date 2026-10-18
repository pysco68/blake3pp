# Hand-written (NOT emitted by tools/gen-toolchains.py -- that generator only
# knows the GNU-driver matrix).
#
# Windows MSVC 2026 (cl.exe), C++23, with AddressSanitizer: the IOCP backend
# is one of the two no Linux sanitizer lane executes, and an async lifetime
# bug is what lives there. cl rather than clang-cl because cl links the ASan
# runtime itself; windows-common.cmake says what goes wrong otherwise.
# Run from a vcvars64 shell.
#
#   cmake -S . -B build/windows-msvc2026-cxx23-asan -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-msvc2026-cxx23-asan/windows-msvc2026-cxx23-asan.cmake

set(TC_C_COMPILER "cl")
set(TC_CXX_COMPILER "cl")
set(TC_CXX_STANDARD "23")
set(TC_DEFAULT_BUILD_TYPE "Release")
set(TC_SANITIZERS address)

include("${CMAKE_CURRENT_LIST_DIR}/../windows-common.cmake")
