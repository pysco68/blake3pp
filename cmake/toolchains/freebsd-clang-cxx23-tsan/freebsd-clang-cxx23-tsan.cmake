# freebsd-clang-cxx23 with ThreadSanitizer: the lane that watches the one
# member another thread may call (wake(), an EVFILT_USER trigger on the
# context's kqueue) against the poll() it releases.
#
#   cmake -S . -B build/freebsd-clang-cxx23-tsan -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/freebsd-clang-cxx23-tsan/freebsd-clang-cxx23-tsan.cmake

set(TC_C_COMPILER "cc")
set(TC_CXX_COMPILER "c++")
set(TC_CXX_STANDARD "23")
set(TC_SANITIZERS thread)
set(TC_DEFAULT_BUILD_TYPE "RelWithDebInfo")

include("${CMAKE_CURRENT_LIST_DIR}/../common.cmake")
