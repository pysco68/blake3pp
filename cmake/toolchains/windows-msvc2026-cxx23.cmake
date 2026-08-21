# Hand-written (NOT emitted by tools/gen-toolchains.py -- that generator only
# knows the GNU-driver matrix).
#
# Windows MSVC 2026 (cl.exe), C++23. Run from a vcvars64 shell.
#
#   cmake -S . -B build/windows-msvc2026-cxx23 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-msvc2026-cxx23.cmake

set(TC_C_COMPILER "cl")
set(TC_CXX_COMPILER "cl")
set(TC_CXX_STANDARD "23")
set(TC_DEFAULT_BUILD_TYPE "Release")

include("${CMAKE_CURRENT_LIST_DIR}/windows-common.cmake")
