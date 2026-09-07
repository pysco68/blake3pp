# Hand-written (NOT emitted by tools/gen-toolchains.py -- that generator only
# knows the GNU-driver matrix).
#
# Windows arm64 cross with MSVC (cl.exe), C++23. Run from a vcvarsamd64_arm64
# shell. Scalar-only: xsimd has no MSVC-arm64 support at the pinned version.
#
#   cmake -S . -B build/windows-arm64-msvc2026-cxx23 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-arm64-msvc2026-cxx23/windows-arm64-msvc2026-cxx23.cmake

set(TC_C_COMPILER "cl")
set(TC_CXX_COMPILER "cl")
set(TC_CXX_STANDARD "23")
set(TC_TARGET_ARCH "arm64")
set(TC_DEFAULT_BUILD_TYPE "Release")

include("${CMAKE_CURRENT_LIST_DIR}/../windows-common.cmake")
