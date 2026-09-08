# Fully static ppc64le release toolchain: zig's clang targeting
# powerpc64le-linux-musl, the POWER member of the static musl family.
# Same shape and workarounds as the riscv64 twin (see that file for the
# lore on module scanning, push-state, and --dependency-file).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ppc64le)
# The wrappers by name: on PATH in the toolchain image (/usr/local/bin),
# in docker/wrappers/zig/ in a checkout. Looked up rather than spelled as a
# path relative to this file because cmake-re copies toolchain files into
# its own environment directory, where a relative path resolves nowhere.
find_program(BLAKE3PP_ZIG_CC powerpc64le-linux-musl-clang
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../docker/wrappers/zig" REQUIRED NO_CACHE)
find_program(BLAKE3PP_ZIG_CXX powerpc64le-linux-musl-clang++
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../docker/wrappers/zig" REQUIRED NO_CACHE)
set(CMAKE_C_COMPILER "${BLAKE3PP_ZIG_CC}")
set(CMAKE_CXX_COMPILER "${BLAKE3PP_ZIG_CXX}")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_CROSSCOMPILING_EMULATOR "qemu-ppc64le")
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_C_LINKER_PUSHPOP_STATE_SUPPORTED FALSE)
set(CMAKE_CXX_LINKER_PUSHPOP_STATE_SUPPORTED FALSE)
set(BLAKE3PP_SIMD_PROVIDER "xsimd" CACHE STRING
  "simd provider (set by the ppc64le musl toolchain)")
