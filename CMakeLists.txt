cmake_minimum_required(VERSION 3.28)

project(blake3pp
  VERSION 0.1.0
  DESCRIPTION "A C++20-and-later BLAKE3 implementation built on std::simd / std::execution idioms"
  LANGUAGES CXX)

# The toolchain files under cmake/toolchains/ normally decide the standard.
# When configured bare (no toolchain, no preset), fall back to the C++20
# baseline the library is written against.
if(NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# No C++ modules in this project. Beyond being unnecessary, the scanning
# support CMake enables for C++20+ targets adds -fmodules-ts to every GCC
# compile, and GCC 16 trunk's modules machinery ICEs on stdexec's headers.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(BLAKE3PP_BUILD_TESTS "Build blake3pp tests" ${PROJECT_IS_TOP_LEVEL})
option(BLAKE3PP_BUILD_BENCH "Build blake3pp benchmarks" ${PROJECT_IS_TOP_LEVEL})
# The tools (CLI + benchmarks) use mimalloc by default on every supported
# target: the parallel paths allocate on window/segment granularity, and
# both musl's mallocng and Windows' UCRT heap serialize under threads.
# The LIBRARY stays allocator-neutral, so consumers keep full control;
# this only touches the executables. Off for wasm (unsupported there).
if(EMSCRIPTEN)
  set(_blake3pp_tool_mimalloc_default OFF)
else()
  set(_blake3pp_tool_mimalloc_default ON)
endif()

option(BLAKE3PP_TOOL_MIMALLOC "Use mimalloc as the allocator for CLI tools and benchmarks" ${_blake3pp_tool_mimalloc_default})

if(BLAKE3PP_TOOL_MIMALLOC AND CMAKE_CXX_FLAGS MATCHES "-fsanitize")
  # Sanitizers interpose the allocator themselves; a second interposer
  # (mimalloc) fights their interceptors and breaks the runtime. The
  # sanitizer owns the heap in those builds.
  message(STATUS "blake3pp: sanitizer build - tool mimalloc disabled")
  set(BLAKE3PP_TOOL_MIMALLOC OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)

# --- Hermetic FetchContent (tipi-build/hfc) ----------------------------------
# Shared, content-hash-keyed source cache + isolated per-dependency builds.
# Everything under thirdparty/ is disposable cache (gitignored): one
# population serves every preset build dir and toolchain container, and CI
# caches the whole directory. Pinned by commit, per hfc's own advice.
include(FetchContent)
FetchContent_Populate(hfc
  GIT_REPOSITORY https://github.com/pysco68/hfc.git
  # windows-support branch tip (2026-08-20), rebased to include the
  # SOURCE_SUBDIR precedence fix the interlocked stdexec population relies
  # on. Re-pin to a tipi-build/hfc mainline commit once merged upstream.
  GIT_TAG ce030d44df549cb6d0c8169fd5918c486a3af077
  SOURCE_DIR "${PROJECT_SOURCE_DIR}/thirdparty/cache/hfc/src"
  SUBBUILD_DIR "${PROJECT_SOURCE_DIR}/thirdparty/cache/hfc/subbuild"
  BINARY_DIR "${PROJECT_SOURCE_DIR}/thirdparty/cache/hfc/bin"
)
FetchContent_GetProperties(hfc)
list(APPEND CMAKE_MODULE_PATH "${hfc_SOURCE_DIR}/cmake")
set(HFC_ENABLE_CMAKE_SBOM OFF CACHE BOOL "no SBOM generation for now")

# hfc drives each dependency as an isolated sub-build whose proxy toolchain
# include()s our CMAKE_TOOLCHAIN_FILE but forwards no other cache variables
# unless they are named here. Without this list a compiler or standard chosen
# outside a toolchain file reaches the blake3pp targets and nothing else, and
# mimalloc/xsimd/doctest/CLI11/blake3_upstream quietly configure with CMake's
# defaults instead -- on Windows that meant clang-cl on top of cl.exe-built
# dependencies.
set(HERMETIC_FETCHCONTENT_FORWARDED_CMAKE_VARIABLES
  "CMAKE_PREFIX_PATH"
  "CMAKE_BUILD_TYPE"
  "CMAKE_DEBUG_POSTFIX"
  "CMAKE_EXE_LINKER_FLAGS"
  "CMAKE_C_COMPILER"
  "CMAKE_C_FLAGS"
  "CMAKE_C_COMPILE_FEATURES"
  "CMAKE_C_EXTENSIONS"
  "CMAKE_C_STANDARD"
  "CMAKE_C_STANDARD_REQUIRED"
  "CMAKE_CXX_COMPILER"
  "CMAKE_CXX_FLAGS"
  "CMAKE_CXX_COMPILE_FEATURES"
  "CMAKE_CXX_COMPILER_IMPORT_STD"
  "CMAKE_CXX_EXTENSIONS"
  "CMAKE_CXX_SCAN_FOR_MODULES"
  "CMAKE_CXX_STANDARD"
  "CMAKE_CXX_STANDARD_REQUIRED"
  "BUILD_SHARED_LIBS")

include(HermeticFetchContent)

include(StdFeatures)
include(ArchKernels)

add_library(blake3pp_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  # MSVC-style drivers. /W4 is the sane maximum (/Wall drowns in STL noise),
  # and clang-cl needs it for the same reason cl.exe does: its driver aliases
  # the GNU spelling -Wall to /Wall, i.e. -Weverything, which buries the build
  # in -Wc++98-compat noise about the C++23 the library is written in. /W4
  # maps to -Wall -Wextra under clang-cl, so the two extra GNU-spelled flags
  # below still add up to the set the GNU branch asks for.
  target_compile_options(blake3pp_warnings INTERFACE /W4)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(blake3pp_warnings INTERFACE -Wconversion -Wshadow)
  endif()
else()
  target_compile_options(blake3pp_warnings INTERFACE -Wall -Wextra -Wconversion -Wshadow)
endif()

add_library(blake3pp STATIC
  src/blake3pp.cpp
  src/dispatch/dispatch.cpp
  src/dispatch/cpu_detect_x86.cpp
  src/dispatch/cpu_detect_arm.cpp
  src/dispatch/cpu_detect_riscv.cpp
  src/dispatch/transpose16.cpp
  src/io/file_reader.cpp
  src/io/file_writer.cpp
  src/io_hash.cpp)
add_library(blake3pp::blake3pp ALIAS blake3pp)

target_compile_features(blake3pp PUBLIC cxx_std_20)
target_include_directories(blake3pp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(blake3pp PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}")

# Version stamp, taken from git at BUILD time (a commit between configures
# still lands in the next build). The custom target reruns every build
# (one ~10 ms git call), but the header moves only when the version string
# changes, so only dispatch.cpp ever recompiles for it.
set(_blake3pp_version_stamp "${CMAKE_CURRENT_BINARY_DIR}/blake3pp_version_stamp.hpp")
add_custom_target(blake3pp_version_stamp
  COMMAND "${CMAKE_COMMAND}"
    -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DOUTPUT_FILE=${_blake3pp_version_stamp}
    -DFALLBACK_VERSION=${PROJECT_VERSION}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitVersionStamp.cmake"
  BYPRODUCTS "${_blake3pp_version_stamp}"
  COMMENT "Stamping blake3pp version from git")
add_dependencies(blake3pp blake3pp_version_stamp)

# features is PUBLIC: its defines (BLAKE3PP_HAS_STD_SENDERS etc.) and the
# stdexec include path must reach consumers of <blake3pp/parallel.hpp>.
# blake3pp::features carries this build's simd/execution wiring, including
# include paths that point into the hermetic dependency trees. Those are
# meaningful here and meaningless to someone consuming an INSTALLED blake3pp,
# so it is scoped to the build tree. What an installed consumer needs from it
# is one thing only: the BLAKE3PP_EXECUTION_* macro, so <blake3pp/parallel.hpp>
# selects the same provider branch the library was compiled against. That is
# re-stated on the install interface below.
# Both are wrapped: a STATIC library propagates even PRIVATE dependencies
# into its link interface as $<LINK_ONLY:...>, so an unwrapped warnings
# target would have to join the export set despite carrying nothing but
# compile options for our own sources.
target_link_libraries(blake3pp
  PRIVATE $<BUILD_INTERFACE:blake3pp_warnings>
  PUBLIC $<BUILD_INTERFACE:blake3pp::features>)
string(TOUPPER "${_blake3pp_execution_provider}" _blake3pp_provider_uc)
target_compile_definitions(blake3pp INTERFACE
  $<INSTALL_INTERFACE:BLAKE3PP_EXECUTION_${_blake3pp_provider_uc}=1>)
target_include_directories(blake3pp PUBLIC
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Architecture kernel variants: each ISA family's probes, flag lore and
# blake3pp_add_kernel() registrations live in cmake/KernelVariants.cmake,
# one function per family, mirroring the src/dispatch/cpu_detect_*.cpp
# split. All registered variants land in the one binary and runtime
# dispatch picks among them.
include(KernelVariants)
blake3pp_register_kernels()

blake3pp_target_link_kernels(blake3pp)

# The beman provider needs a program-level parallel_scheduler backend
# (P2079 replaceability: one definition per binary, like a global
# allocator). Executables in this repo link this; downstream applications
# may define query_parallel_scheduler_backend() themselves instead.
if(_blake3pp_execution_provider STREQUAL "beman")
  add_library(blake3pp_beman_backend STATIC src/execution/beman_backend.cpp)
  add_library(blake3pp::beman_backend ALIAS blake3pp_beman_backend)
  target_link_libraries(blake3pp_beman_backend
    PUBLIC blake3pp::features
    PRIVATE blake3pp_stdexec blake3pp_warnings)
endif()

# What an executable needs in order to use mimalloc as its allocator,
# expressed as a target rather than a function: the platform answers differ
# only in which properties they set, and when BLAKE3PP_TOOL_MIMALLOC is off
# this simply stays empty and linking it is a no-op. INTERFACE sources are
# what make the Windows case work: they compile once per CONSUMER, which is
# exactly the one-definition-per-program rule new/delete overrides live under.
add_library(blake3pp_tool_allocator INTERFACE)
add_library(blake3pp::tool_allocator ALIAS blake3pp_tool_allocator)

if(BLAKE3PP_TOOL_MIMALLOC)
  # The hermetic mimalloc build's settings.
  #
  # MI_NO_OPT_ARCH: mimalloc's arch tuning passes -march spellings zig's
  # clang rejects (armv8.1 on aarch64), and it force-enables itself on
  # arm64 unless MI_NO_OPT_ARCH is set (priority over MI_OPT_ARCH).
  # MI_INSTALL_TOPLEVEL: install to <prefix>/lib and <prefix>/include
  # instead of versioned subdirs, keeping the installed layout stable for
  # hfc's export discovery.
  set(_mi_toolchain_extension [=[
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_NO_OPT_ARCH ON CACHE BOOL "" FORCE)
    set(MI_INSTALL_TOPLEVEL ON CACHE BOOL "" FORCE)
  ]=])
  # zig cc enforces reproducible builds (-Werror,-Wdate-time) and mimalloc
  # stamps __DATE__ into its version banner. Pure cl rejects the GNU flag
  # spelling and has no such warning to silence, so it is skipped there.
  if(NOT (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"))
    string(APPEND _mi_toolchain_extension "    set(CMAKE_C_FLAGS_INIT \"\${CMAKE_C_FLAGS_INIT} -Wno-date-time\")\n")
  endif()

  FetchContent_Declare(mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG acf2fdd329f9dc2a7ffe3f12a133fe7175e39378) # v3.4.5
  FetchContent_MakeHermetic(mimalloc
    HERMETIC_BUILD_SYSTEM cmake
    HERMETIC_TOOLCHAIN_EXTENSION "${_mi_toolchain_extension}"
    # mimalloc names its install(EXPORT) set plain "mimalloc", so the generated export file is mimalloc.cmake
    HERMETIC_DISCOVER_TARGETS_FILE_PATTERN [[/mimalloc\.cmake$]])
  HermeticFetchContent_MakeAvailableAtBuildTime(mimalloc)

  if(WIN32)
    # Full malloc override on Windows needs the mimalloc-redirect DLL
    # pair next to every exe (their docs: static malloc override is not
    # reliable there). The reliable static route for C++ programs is the
    # global operator new/delete override, which covers effectively all
    # of the tools' allocation traffic while keeping single-file exes.
    # The overriding TU is a tracked source, not a generated one: the
    # operators it defines end up in every shipped executable, and that is
    # worth being greppable, reviewable and visible in a backtrace. It is
    # added per-executable because these are one-per-program definitions.
    target_sources(blake3pp_tool_allocator INTERFACE "${PROJECT_SOURCE_DIR}/tooling/mimalloc_newdelete.cpp")
    target_link_libraries(blake3pp_tool_allocator INTERFACE mimalloc-static)
  else()
    # POSIX: mimalloc's override machinery (the operator new/delete
    # replacements, and on macOS the malloc-zone interposition) lives in
    # archive members that nothing in the program references. A plain
    # archive link therefore never extracts them (a linker only pulls a
    # member that resolves an undefined symbol) and the override silently
    # does not happen: the binary still initializes mimalloc and looks
    # fine, it just is not the allocator. Force-loading the whole archive
    # is the fix.
    #
    # mimalloc also ships MI_BUILD_OBJECT, a combined mimalloc.o meant for
    # exactly this, and linking that object works equally well. Force-loading
    # is preferred because it needs no second artifact.
    target_link_libraries(blake3pp_tool_allocator INTERFACE
      "$<LINK_LIBRARY:WHOLE_ARCHIVE,mimalloc-static>")
  endif()
endif()

# --- Command-line tooling (CLI tools + benchmarks) ---------------------------
# Everything the executables share, so the two CLI tools and the two
# benchmarks are one kind of program: CLI11 for argument parsing and
# tooling/tool_common.hpp for output and --threads handling.
#
# They also share one extra floor. std::format is a C++20 LIBRARY feature
# that trails the language (libstdc++ ships <format> from GCC 13), and the
# shared header's print shim needs it. Where it is missing, the executables
# are skipped with a notice; the LIBRARY itself keeps its C++20 floor.
include(CheckIncludeFileCXX)
check_include_file_cxx("format" BLAKE3PP_HAS_FORMAT)
if(BLAKE3PP_HAS_FORMAT)
  # CLI11 for argument parsing: robust, pinned.
  FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG cbd58a3696887b34c70949aef21a71735a0c2ad5) # v2.7.2
  FetchContent_MakeHermetic(cli11
    HERMETIC_BUILD_SYSTEM cmake
    HERMETIC_TOOLCHAIN_EXTENSION [=[
      # PRECOMPILED: measured 2.4x faster recompiles of main.cpp (32.7s -> 13.4s).
      set(CLI11_PRECOMPILED ON CACHE BOOL "" FORCE)
      set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
      set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
      set(CLI11_INSTALL ON CACHE BOOL "" FORCE)
    ]=])
  HermeticFetchContent_MakeAvailableAtBuildTime(cli11)

  add_library(blake3pp_tooling INTERFACE)
  add_library(blake3pp::tooling ALIAS blake3pp_tooling)
  target_include_directories(blake3pp_tooling INTERFACE "${PROJECT_SOURCE_DIR}/tooling")
  target_link_libraries(blake3pp_tooling INTERFACE CLI11::CLI11)
else()
  message(STATUS
    "blake3pp: <format> unavailable on this stdlib - CLI tools and "
    "benchmarks skipped")
endif()

if(BLAKE3PP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(BLAKE3PP_BUILD_BENCH AND BLAKE3PP_HAS_FORMAT)
  add_subdirectory(bench)
endif()

option(BLAKE3PP_BUILD_CLI "Build the blake3ppsum utility" ${PROJECT_IS_TOP_LEVEL})
if(BLAKE3PP_BUILD_CLI AND BLAKE3PP_HAS_FORMAT)
  add_subdirectory(cli)
endif()

# --- Install / export --------------------------------------------------------
# Only the library and its public headers ship. The kernels are OBJECT
# libraries already folded into the archive ($<TARGET_OBJECTS:>), the tools
# are development artifacts, and blake3pp::features/warnings describe THIS
# build rather than the installed one.
option(BLAKE3PP_INSTALL "Generate blake3pp install and export rules" ${PROJECT_IS_TOP_LEVEL})
if(BLAKE3PP_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS blake3pp
    EXPORT blake3ppTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/blake3pp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

  set(_blake3pp_cmakedir "${CMAKE_INSTALL_LIBDIR}/cmake/blake3pp")
  install(EXPORT blake3ppTargets
    FILE blake3ppTargets.cmake
    NAMESPACE blake3pp::
    DESTINATION "${_blake3pp_cmakedir}")

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/blake3ppConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/blake3ppConfig.cmake"
    INSTALL_DESTINATION "${_blake3pp_cmakedir}")
  # The ABI is settled by the compiler and standard library, not by our
  # version, so SameMajorVersion is the honest compatibility claim.
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/blake3ppConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/blake3ppConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/blake3ppConfigVersion.cmake"
    DESTINATION "${_blake3pp_cmakedir}")
endif()
