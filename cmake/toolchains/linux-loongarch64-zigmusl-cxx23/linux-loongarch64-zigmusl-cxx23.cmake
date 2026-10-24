# Fully static loongarch64 release toolchain: zig's clang targeting
# loongarch64-linux-musl. zig bundles musl for the port, so this needs no
# cross sysroot and no new toolchain image; the lane is a wrapper pair and
# this file.
#
# Requires the zig binary on PATH or via $ZIG (see docker/wrappers/zig/,
# whose <triple>-clang/-clang++ wrappers this file names).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR loongarch64)

# The wrappers by name: on PATH in the toolchain image (/usr/local/bin),
# in docker/wrappers/zig/ in a checkout. Looked up rather than spelled as a
# path relative to this file because cmake-re copies toolchain files into
# its own environment directory, where a relative path resolves nowhere.
find_program(BLAKE3PP_ZIG_CC loongarch64-linux-musl-clang
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../../docker/wrappers/zig" REQUIRED NO_CACHE)
find_program(BLAKE3PP_ZIG_CXX loongarch64-linux-musl-clang++
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../../docker/wrappers/zig" REQUIRED NO_CACHE)
set(CMAKE_C_COMPILER "${BLAKE3PP_ZIG_CC}")
set(CMAKE_CXX_COMPILER "${BLAKE3PP_ZIG_CXX}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# zig's loongarch64 baseline carries +lsx, unlike its riscv64 baseline
# (plain rv64gc, no vector). Left alone it would put 128-bit LSX in every
# translation unit, including the scalar kernel and the dispatch code that
# decides whether LSX may run at all, and the binary would SIGILL on an
# LA64 part without it (the 2K0300 class). The baseline is therefore
# lowered to no vector here and the kernels opt back in per TU through
# their ARCH_FLAGS, which is the same arrangement every other family has.
# Opting back in needs the cc1 spelling and not -mlsx; see
# _blake3pp_register_loongarch_kernels in cmake/KernelVariants.cmake.
set(CMAKE_C_FLAGS_INIT "-mno-lsx")
set(CMAKE_CXX_FLAGS_INIT "-mno-lsx")

# BLAKE3PP_SIMD_PROVIDER is deliberately left on `auto`, where the
# ppc64le and s390x zigmusl files name xsimd. Both kernels here pin the
# vector-extension provider per TU, so the project-wide choice names
# nothing this build uses -- but no legal value says that: `std` and
# `std-experimental` are a configure error on this toolchain (zig's
# libc++ has neither a usable <simd> nor <experimental/simd>), and
# `xsimd`/`xvec` would name an unused polyfill as loudly as `auto` does.
# The mips64el file can say `std-experimental` only because libstdc++
# has one that compiles. Measured consequence: zero xsimd headers reach
# any translation unit on this target, kernels included.

# Static binaries need no -L sysroot under qemu-user.
# Resolved to an absolute path: under cmake-re the test launcher
# (tipi-test-driver) execve's the command literally, no PATH search.
find_program(BLAKE3PP_QEMU NAMES qemu-loongarch64 REQUIRED NO_CACHE)
set(CMAKE_CROSSCOMPILING_EMULATOR "${BLAKE3PP_QEMU}")

# zig identifies as Clang but ships no clang-scan-deps, 0.17 included,
# so CMake's C++20 module scanning (on by default for C++20+ with Ninja)
# dies with a bare exit 127 at the first .ddi rule. The root CMakeLists'
# OFF only covers the main project; hermetic dependency builds run in
# their own processes and inherit ONLY this toolchain (via hfc's proxy
# toolchain), so the switch must live here too.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# zig's linker driver implements only a subset of GNU ld's options, and
# --push-state/--pop-state are not in it, 0.17 included. CMake probes
# for them by running the linker and looking for an error message that
# NAMES those flags; zig does not answer that way, so the probe
# concludes "supported" and $<LINK_LIBRARY:WHOLE_ARCHIVE,...> (how the
# tools force-load mimalloc's override members) emits a push-state pair
# that zig then rejects with "unsupported linker arg: --push-state".
# Stating the answer here pre-empts the probe (CMake only probes when the
# variable is undefined) and selects the --whole-archive/--no-whole-archive
# spelling, which zig does accept.
set(CMAKE_C_LINKER_PUSHPOP_STATE_SUPPORTED FALSE)
set(CMAKE_CXX_LINKER_PUSHPOP_STATE_SUPPORTED FALSE)
