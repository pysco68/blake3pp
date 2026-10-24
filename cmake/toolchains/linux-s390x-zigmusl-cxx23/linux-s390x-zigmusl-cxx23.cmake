# Fully static s390x release toolchain: zig's clang targeting
# s390x-linux-musl, the IBM z (big-endian) member of the static musl family.
# Same shape and workarounds as the riscv64 twin (see that file for the
# lore on module scanning and push-state).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR s390x)
# The wrappers by name: on PATH in the toolchain image (/usr/local/bin),
# in docker/wrappers/zig/ in a checkout. Looked up rather than spelled as a
# path relative to this file because cmake-re copies toolchain files into
# its own environment directory, where a relative path resolves nowhere.
find_program(BLAKE3PP_ZIG_CC s390x-linux-musl-clang
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../../docker/wrappers/zig" REQUIRED NO_CACHE)
find_program(BLAKE3PP_ZIG_CXX s390x-linux-musl-clang++
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../../docker/wrappers/zig" REQUIRED NO_CACHE)
set(CMAKE_C_COMPILER "${BLAKE3PP_ZIG_CC}")
set(CMAKE_CXX_COMPILER "${BLAKE3PP_ZIG_CXX}")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
# Baseline z13, pinned for the same reason as the GCC twin: everything
# except the vxe kernel (which KernelVariants.cmake gives its own
# -march=z14) must stay inside what the dispatch verdict gates, so the
# no-vxe config in tools/ci-test.sh stays a legitimate target. Pinned
# rather than left to the driver because zig picks the baseline: 0.16
# targeted z10, 0.17 targets z13.
set(CMAKE_C_FLAGS_INIT "-march=z13")
set(CMAKE_CXX_FLAGS_INIT "-march=z13")
# Resolved to an absolute path: under cmake-re the test launcher
# (tipi-test-driver) execve's the command literally, no PATH search.
find_program(BLAKE3PP_QEMU NAMES qemu-s390x REQUIRED NO_CACHE)
set(CMAKE_CROSSCOMPILING_EMULATOR "${BLAKE3PP_QEMU}")
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_C_LINKER_PUSHPOP_STATE_SUPPORTED FALSE)
set(CMAKE_CXX_LINKER_PUSHPOP_STATE_SUPPORTED FALSE)
set(BLAKE3PP_SIMD_PROVIDER "xsimd" CACHE STRING
  "simd provider (set by the s390x musl toolchain)")
