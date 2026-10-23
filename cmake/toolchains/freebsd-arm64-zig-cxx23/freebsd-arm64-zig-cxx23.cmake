# Cross-compiled arm64 FreeBSD release toolchain: zig's clang targeting
# aarch64-freebsd.14.0, so a Linux container builds the bytes a FreeBSD
# guest runs. The amd64 twin next door
# (cmake/toolchains/freebsd-zig-cxx23/) is the same file with the triple
# changed; every comment it carries about zig's quirks applies here
# unchanged, and is repeated rather than cross-referenced because a
# toolchain file is read by someone debugging ONE build.
#
# What differs from the amd64 twin is the kernel set. Nothing below asks
# for it: cmake/KernelVariants.cmake dispatches on
# CMAKE_SYSTEM_PROCESSOR, so declaring aarch64 is what makes it register
# neon plus the fixed-length SVE variants it can get a flag spelling
# past the compiler -- sve256, sve512 and sve2_128, the three lengths
# shipping silicon actually has. zig cc rejects aarch64 -march outright
# (its flag model wants -mcpu=<cpu>+<features>), which is exactly why
# that probe carries a second candidate spelling; the FreeBSD target
# takes the same -mcpu one the aarch64-linux-musl target takes, because
# the flag is the compiler's business and not the OS's.
#
# Whether those kernels are ever SELECTED is the kernel's business, and
# there the two FreeBSD lanes differ. src/dispatch/cpu_detect_arm.cpp
# reads HWCAP_SVE through elf_aux_info, and that bit is set only by a
# kernel that saves SVE state across a context switch, which FreeBSD
# gained in 15.0. So this binary takes NEON on 14.x and an SVE kernel on
# 15.x, from the same bytes -- which is the whole point of a dispatch
# table, and the reason the ci guest for this lane is a 15.x release
# while the amd64 guest is a 14.x one.
#
# Why no -static here, unlike every other zig preset in this directory:
# zig refuses it outright for this target with
#
#   error: libc of the specified target requires dynamic linking
#
# word for word as it does for x86_64-freebsd. FreeBSD ships no static
# libc that zig will link against, so the artifact is dynamic
# (libc.so.7, libthr.so.3) and the ABI version in the triple becomes the
# floor: it loads on FreeBSD 14.0 and newer.
#
# Requires the zig binary on PATH or via $ZIG (see docker/wrappers/zig/,
# whose <triple>-clang/-clang++ wrappers this file names).

set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# The wrappers by name: on PATH in the toolchain image (/usr/local/bin),
# in docker/wrappers/zig/ in a checkout. Looked up rather than spelled as a
# path relative to this file because cmake-re copies toolchain files into
# its own environment directory, where a relative path resolves nowhere.
find_program(BLAKE3PP_ZIG_CC aarch64-freebsd-clang
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../../docker/wrappers/zig" REQUIRED NO_CACHE)
find_program(BLAKE3PP_ZIG_CXX aarch64-freebsd-clang++
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../../docker/wrappers/zig" REQUIRED NO_CACHE)
set(CMAKE_C_COMPILER "${BLAKE3PP_ZIG_CC}")
set(CMAKE_CXX_COMPILER "${BLAKE3PP_ZIG_CXX}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Deliberately empty, and for a stronger reason than on the amd64 twin.
# There qemu-user has no FreeBSD persona; here it would also be the
# wrong instruction set, so even a hypothetical one would need the
# system emulator underneath it. The guest is driven by the workflow
# (tools/freebsd-guest.sh FBSD_ARCH=arm64 boots it and ssh's the
# binaries in), not by ctest, so an emulator here would only make
# `ctest` fail in a more confusing way than "no tests ran".
set(CMAKE_CROSSCOMPILING_EMULATOR "")

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
