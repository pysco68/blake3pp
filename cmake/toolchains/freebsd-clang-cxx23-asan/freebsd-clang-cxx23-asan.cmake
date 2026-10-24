# freebsd-clang-cxx23 with AddressSanitizer and UndefinedBehaviorSanitizer.
# The base system's clang_rt ships both runtimes for amd64.
#
#   cmake -S . -B build/freebsd-clang-cxx23-asan -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/freebsd-clang-cxx23-asan/freebsd-clang-cxx23-asan.cmake

set(TC_C_COMPILER "cc")
set(TC_CXX_COMPILER "c++")
set(TC_CXX_STANDARD "23")
set(TC_SANITIZERS address undefined)
set(TC_DEFAULT_BUILD_TYPE "RelWithDebInfo")

include("${CMAKE_CURRENT_LIST_DIR}/../common.cmake")
