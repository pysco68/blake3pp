# Fully static s390x release toolchain: zig's clang targeting
# s390x-linux-musl, the IBM z (big-endian) member of the static musl family.
# Same shape and workarounds as the riscv64 twin (see that file for the
# lore on module scanning, push-state, and --dependency-file).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR s390x)
set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/../../tools/zig-wrappers/zig-cc-s390x-musl")
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/../../tools/zig-wrappers/zig-cxx-s390x-musl")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_CROSSCOMPILING_EMULATOR "qemu-s390x")
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_C_LINKER_PUSHPOP_STATE_SUPPORTED FALSE)
set(CMAKE_CXX_LINKER_PUSHPOP_STATE_SUPPORTED FALSE)
set(BLAKE3PP_SIMD_PROVIDER "xsimd" CACHE STRING
  "simd provider (set by the s390x musl toolchain)")
