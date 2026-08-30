# Fully static riscv64 release toolchain: zig's clang targeting
# riscv64-linux-musl. No glibc version coupling, no dynamic loader; that
# matters double on RISC-V, where real distros ship glibc floors nowhere
# near the cross-gcc sysroot's (Ubuntu resolute is RVA23-baselined; most
# shipping boards are not). zig's own riscv64 baseline is plain rv64gc
# with NO vector extension, so the RVA23 ungated-autovectorization trap
# (see the gcc15 toolchain file) does not exist here by construction; the
# rvv kernel TUs opt in per-TU via the -mcpu spellings the build probes.
#
# Requires the zig binary on PATH or via $ZIG (see tools/zig-wrappers/).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/../../tools/zig-wrappers/zig-cc-riscv64-musl")
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/../../tools/zig-wrappers/zig-cxx-riscv64-musl")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# zig's bundled musl/linux headers predate the riscv_hwprobe syscall, but
# mimalloc's riscv fast path (MI_HAS_ASM_HWPROBEH) references the __NR
# constant. It is a frozen ABI number; supplying it is the whole fix.
set(CMAKE_C_FLAGS_INIT "-D__NR_riscv_hwprobe=258")
set(CMAKE_CXX_FLAGS_INIT "-D__NR_riscv_hwprobe=258")

# Static binaries need no -L sysroot under qemu-user.
set(CMAKE_CROSSCOMPILING_EMULATOR "qemu-riscv64")

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
