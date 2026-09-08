# Fully static aarch64 release toolchain: zig's clang targeting
# aarch64-linux-musl. No glibc version coupling and no dynamic loader, so
# the binary runs on any Linux kernel of the last decade. Runtime SIMD
# dispatch keeps working because blake3pp's dispatch is a plain cpuid +
# function-pointer table by design; it never uses GNU ifunc, which does
# not survive static linking.
#
# Requires the zig binary on PATH or via $ZIG (see docker/wrappers/zig/,
# whose <triple>-clang/-clang++ wrappers this file names).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# The wrappers by name: on PATH in the toolchain image (/usr/local/bin),
# in docker/wrappers/zig/ in a checkout. Looked up rather than spelled as a
# path relative to this file because cmake-re copies toolchain files into
# its own environment directory, where a relative path resolves nowhere.
find_program(BLAKE3PP_ZIG_CC aarch64-linux-musl-clang
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../docker/wrappers/zig" REQUIRED NO_CACHE)
find_program(BLAKE3PP_ZIG_CXX aarch64-linux-musl-clang++
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../docker/wrappers/zig" REQUIRED NO_CACHE)
set(CMAKE_C_COMPILER "${BLAKE3PP_ZIG_CC}")
set(CMAKE_CXX_COMPILER "${BLAKE3PP_ZIG_CXX}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# Static binaries need no -L sysroot under qemu-user.
# Resolved to an absolute path: under cmake-re the test launcher
# (tipi-test-driver) execve's the command literally, no PATH search.
find_program(BLAKE3PP_QEMU NAMES qemu-aarch64 REQUIRED NO_CACHE)
set(CMAKE_CROSSCOMPILING_EMULATOR "${BLAKE3PP_QEMU}")

# zig 0.16 segfaults on lld's --dependency-file flag, which CMake >= 3.27
# passes for link-dependency tracking; disable that feature here.
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)

# zig identifies as Clang but ships no clang-scan-deps, so CMake's C++20
# module scanning (on by default for C++20+ with Ninja) dies with a bare
# exit 127 at the first .ddi rule. The root CMakeLists' OFF only covers
# the main project; hermetic dependency builds run in their own processes
# and inherit ONLY this toolchain (via hfc's proxy toolchain), so the
# switch must live here too.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# zig's linker driver implements only a subset of GNU ld's options, and
# --push-state/--pop-state are not in it. CMake probes for them by running
# the linker and looking for an error message that NAMES those flags; zig
# does not answer that way, so the probe concludes "supported" and
# $<LINK_LIBRARY:WHOLE_ARCHIVE,...> (how the tools force-load mimalloc's
# override members) emits a push-state pair that zig then rejects with
# "unsupported linker arg: --push-state". Stating the answer here pre-empts
# the probe (CMake only probes when the variable is undefined) and selects
# the --whole-archive/--no-whole-archive spelling, which zig does accept.
set(CMAKE_C_LINKER_PUSHPOP_STATE_SUPPORTED FALSE)
set(CMAKE_CXX_LINKER_PUSHPOP_STATE_SUPPORTED FALSE)
