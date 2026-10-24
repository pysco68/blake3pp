# Cross-compiled amd64 FreeBSD release toolchain: zig's clang targeting
# x86_64-freebsd.14.0, so a Linux container builds the bytes a FreeBSD
# guest runs. Runtime SIMD dispatch keeps working across the OS change
# because blake3pp's dispatch is a plain cpuid + function-pointer table
# by design; the POSIX AIO on a kqueue engine and O_DIRECT are selected
# by CMAKE_SYSTEM_NAME below, exactly as on a native FreeBSD build.
#
# Why no -static here, unlike every other zig preset in this directory:
# zig refuses it outright for this target with
#
#   error: libc of the specified target requires dynamic linking
#
# FreeBSD ships no static libc that zig will link against, so the
# artifact is dynamic (libc.so.7, libthr.so.3) and the ABI version in
# the triple becomes the floor: it loads on FreeBSD 14.0 and newer. That
# floor is the reason the ci lane runs the binaries on 14.5-RELEASE,
# a release away from the claim rather than ten.
#
# Requires the zig binary on PATH or via $ZIG (see docker/wrappers/zig/,
# whose <triple>-clang/-clang++ wrappers this file names).

set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# The wrappers by name: on PATH in the toolchain image (/usr/local/bin),
# in docker/wrappers/zig/ in a checkout. Looked up rather than spelled as a
# path relative to this file because cmake-re copies toolchain files into
# its own environment directory, where a relative path resolves nowhere.
find_program(BLAKE3PP_ZIG_CC x86_64-freebsd-clang
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../../docker/wrappers/zig" REQUIRED NO_CACHE)
find_program(BLAKE3PP_ZIG_CXX x86_64-freebsd-clang++
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../../docker/wrappers/zig" REQUIRED NO_CACHE)
set(CMAKE_C_COMPILER "${BLAKE3PP_ZIG_CC}")
set(CMAKE_CXX_COMPILER "${BLAKE3PP_ZIG_CXX}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Deliberately empty: nothing on this host can execute a FreeBSD binary,
# and there is no qemu-user persona for one. The guest is driven by the
# workflow (tools/freebsd-guest.sh boots it and ssh's the binaries in),
# not by ctest, so an emulator here would only make `ctest` fail in a
# more confusing way than "no tests ran".
set(CMAKE_CROSSCOMPILING_EMULATOR "")

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
