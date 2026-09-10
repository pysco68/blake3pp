# The mips64el cross, statically linked, for the shipped archive.
#
# Every other static release target goes through zig (clang + musl). This
# one cannot: clang's MIPS backend does not lower the kernel's vector
# code to MSA (measured, zig 0.16: zero MSA instructions from the same
# source GCC turns into 3514), so the artifact is GCC plus a static
# glibc. That is as standalone as the musl archives: no interpreter, no
# dynamic section, no GLIBC_ version symbol, so no glibc floor. It gives
# up NSS, which this tool never asks for.
#
# EMULATOR-ONLY, more so than the POWER and IBM z archives beside it:
# those at least have reachable silicon. Nothing here has ever executed
# on an MSA machine.

include("${CMAKE_CURRENT_LIST_DIR}/../linux-mips64el-gcc14-cxx23/linux-mips64el-gcc14-cxx23.cmake")

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(BLAKE3PP_STATIC_RELEASE ON CACHE BOOL "static release archive build")
