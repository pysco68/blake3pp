# Homebrew LLVM 22 on macOS, C++26, under ThreadSanitizer: the Darwin twin
# of linux-clang22-cxx26-tsan, for the pool and I/O races that only Apple
# silicon's memory ordering shows.
#
#   cmake -S . -B build/macos-clang22-cxx26-tsan -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/macos-clang22-cxx26-tsan/macos-clang22-cxx26-tsan.cmake
#
# Hand-written like its base, which it includes for the Homebrew keg lookup
# and the libc++ link plumbing; the base sets no sanitizers, so this one
# reaches common.cmake.
set(TC_SANITIZERS thread)
include("${CMAKE_CURRENT_LIST_DIR}/../macos-clang22-cxx26/macos-clang22-cxx26.cmake")
