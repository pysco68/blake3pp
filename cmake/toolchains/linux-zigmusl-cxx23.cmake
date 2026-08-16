# Fully static x86_64 release toolchain: zig's clang targeting
# x86_64-linux-musl. No glibc version coupling and no dynamic loader, so
# the binary runs on any Linux kernel of the last decade. Runtime SIMD
# dispatch keeps working because blake3pp's dispatch is a plain cpuid +
# function-pointer table by design; it never uses GNU ifunc, which does
# not survive static linking.
#
# Requires the zig binary on PATH or via $ZIG (see tools/zig-wrappers/).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/../../tools/zig-wrappers/zig-cc-x86_64-musl")
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/../../tools/zig-wrappers/zig-cxx-x86_64-musl")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# Host-run: the produced binaries execute natively on this machine.
set(CMAKE_CROSSCOMPILING_EMULATOR "")

# zig 0.16 segfaults on lld's --dependency-file flag, which CMake >= 3.27
# passes for link-dependency tracking; disable that feature here.
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)
