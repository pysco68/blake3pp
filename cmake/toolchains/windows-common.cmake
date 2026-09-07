# cmake/toolchains/windows-common.cmake
#
# Shared engine for the MSVC-family toolchain files in this directory, the
# Windows counterpart to common.cmake. A concrete file sets a few TC_*
# variables and then includes this one:
#
#     set(TC_C_COMPILER   "clang-cl")
#     set(TC_CXX_COMPILER "clang-cl")
#     set(TC_CXX_STANDARD "23")
#     include("${CMAKE_CURRENT_LIST_DIR}/../windows-common.cmake")
#
# These files are hand-written, NOT emitted by tools/gen-toolchains.py: that
# generator only knows the GNU-driver matrix (gcc/clang, -stdlib=, -fsanitize=,
# -fuse-ld=), none of which applies to cl.exe or clang-cl.
#
# Why the compiler is pinned HERE and not in the preset's cacheVariables:
# the hermetic FetchContent layer (hfc) configures every dependency as an
# isolated sub-build driven by a generated proxy toolchain. That proxy
# include()s the parent's CMAKE_TOOLCHAIN_FILE verbatim, but it does not
# forward arbitrary cache variables. A compiler selected only through
# cacheVariables therefore reaches the blake3pp targets and nothing else, and
# mimalloc/xsimd/doctest/CLI11/blake3_upstream silently fall back to CMake's
# default compiler -- clang-cl at the top level, cl.exe underneath. Same for
# CMAKE_SYSTEM_NAME/PROCESSOR in the arm64 cross presets, which would
# otherwise leave the dependencies building for x64.
#
# ---------------------------------------------------------------------------
# Recognised inputs (all optional unless noted)
#
#   TC_C_COMPILER          cl | clang-cl                    (required)
#   TC_CXX_COMPILER        cl | clang-cl                    (required)
#   TC_CXX_STANDARD        20 | 23 | 26
#   TC_TARGET_ARCH         arm64 to cross-compile; empty/x64 builds native
#   TC_DEFAULT_BUILD_TYPE  seeded only when the caller passes none
#   TC_EXTRA_CXX_FLAGS     escape hatch, appended last
#   TC_EXTRA_LINK_FLAGS    escape hatch, appended last
#
# Every file here assumes it is read from a Visual Studio developer shell
# (vcvars64.bat, or vcvarsamd64_arm64.bat for the arm64 targets): the compiler
# names below resolve through PATH, and the SDK/CRT locations come from the
# environment that shell sets up.
# ---------------------------------------------------------------------------

if(NOT DEFINED TC_CXX_COMPILER)
  message(FATAL_ERROR "toolchain: TC_CXX_COMPILER must be set before including windows-common.cmake")
endif()

# clang-cl by BARE NAME resolves through PATH, and the hosted runner
# images ship a standalone LLVM in C:\Program Files\LLVM that shadows
# the VS-bundled toolset (`where clang-cl` listed it first). That makes
# the compiler whatever the runner image last dropped there: an image
# update moved the standalone copy and source-identical windows-arm64
# binaries started crashing at startup (2026-08-30). "The VS LLVM
# toolset" is what these toolchains MEAN, so resolve it explicitly from
# the developer-shell environment; bare-name PATH resolution is only
# the fallback, and it warns.
if(TC_CXX_COMPILER STREQUAL "clang-cl" AND DEFINED ENV{VCINSTALLDIR})
  cmake_path(CONVERT "$ENV{VCINSTALLDIR}Tools/Llvm/x64/bin/clang-cl.exe"
             TO_CMAKE_PATH_LIST _tc_vs_clangcl)
  if(EXISTS "${_tc_vs_clangcl}")
    set(TC_C_COMPILER "${_tc_vs_clangcl}")
    set(TC_CXX_COMPILER "${_tc_vs_clangcl}")
    message(STATUS "toolchain: clang-cl pinned to the VS LLVM toolset: ${_tc_vs_clangcl}")
  else()
    message(WARNING "toolchain: developer shell present but no VS LLVM toolset at ${_tc_vs_clangcl}; falling back to clang-cl from PATH; which compiler that is depends on the machine")
  endif()
endif()

set(CMAKE_C_COMPILER   "${TC_C_COMPILER}")
set(CMAKE_CXX_COMPILER "${TC_CXX_COMPILER}")

if(TC_CXX_COMPILER MATCHES "clang")
  set(_tc_is_clang_cl TRUE)
else()
  set(_tc_is_clang_cl FALSE)
endif()

# --------------------------------------------------------------- C++ standard
if(DEFINED TC_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD ${TC_CXX_STANDARD})
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# ------------------------------------------------------------- cross-compiling
# Only declared for a genuine cross target. Setting CMAKE_SYSTEM_NAME for a
# native build would flip CMAKE_CROSSCOMPILING on for no reason and stop ctest
# from running the test binaries.
if(DEFINED TC_TARGET_ARCH AND TC_TARGET_ARCH STREQUAL "arm64")
  set(CMAKE_SYSTEM_NAME Windows)
  set(CMAKE_SYSTEM_PROCESSOR ARM64)
  if(_tc_is_clang_cl)
    # cl.exe picks its target from the developer shell; clang-cl is a single
    # cross-capable binary and needs to be told.
    set(CMAKE_C_COMPILER_TARGET   "arm64-pc-windows-msvc")
    set(CMAKE_CXX_COMPILER_TARGET "arm64-pc-windows-msvc")
  endif()
endif()

# ------------------------------------------------------------------ extras
if(DEFINED TC_EXTRA_CXX_FLAGS)
  list(JOIN TC_EXTRA_CXX_FLAGS " " _tc_extra_cxx)
  set(CMAKE_CXX_FLAGS_INIT "${_tc_extra_cxx}")
endif()
if(DEFINED TC_EXTRA_LINK_FLAGS)
  list(JOIN TC_EXTRA_LINK_FLAGS " " _tc_extra_link)
  set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_tc_extra_link}")
  set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_tc_extra_link}")
  set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_tc_extra_link}")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ------------------------------------------------------- default build type
if(DEFINED TC_DEFAULT_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "${TC_DEFAULT_BUILD_TYPE}" CACHE STRING "Build type" FORCE)
endif()
