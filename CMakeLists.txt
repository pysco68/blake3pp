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
option(BLAKE3PP_TOOL_MIMALLOC
  "Use mimalloc as the allocator for CLI tools and benchmarks"
  ${_blake3pp_tool_mimalloc_default})
if(BLAKE3PP_TOOL_MIMALLOC AND CMAKE_CXX_FLAGS MATCHES "-fsanitize")
  # Sanitizers interpose the allocator themselves; a second interposer
  # (mimalloc) fights their interceptors and breaks the runtime. The
  # sanitizer owns the heap in those builds.
  message(STATUS "blake3pp: sanitizer build - tool mimalloc disabled")
  set(BLAKE3PP_TOOL_MIMALLOC OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

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
include(HermeticFetchContent)

include(StdFeatures)
include(ArchKernels)

add_library(blake3pp_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
   AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  # cl.exe: /W4 is the sane maximum (/Wall drowns in STL noise); clang-cl
  # takes the GNU-style set below.
  target_compile_options(blake3pp_warnings INTERFACE /W4)
else()
  target_compile_options(blake3pp_warnings INTERFACE -Wall -Wextra -Wconversion -Wshadow)
endif()

add_library(blake3pp STATIC
  src/blake3pp.cpp
  src/dispatch/dispatch.cpp
  src/io/file_reader.cpp
  src/io/file_writer.cpp
  src/io_hash.cpp)
add_library(blake3pp::blake3pp ALIAS blake3pp)

target_compile_features(blake3pp PUBLIC cxx_std_20)
target_include_directories(blake3pp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
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
target_link_libraries(blake3pp
  PRIVATE blake3pp_warnings
  PUBLIC blake3pp::features)

# Architecture kernel variants. Each call compiles src/kernel/kernel.cpp into
# its own OBJECT library under its own namespace and -m flags; all registered
# variants land in the one binary and runtime dispatch picks among them.
blake3pp_add_kernel(scalar FORCE_SCALAR)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
     AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # Pure cl.exe: /arch is the only ISA dial. There is no /arch:SSE4.2 on
    # x64 (SSE2 is the baseline), so the "sse42" variant compiles at
    # baseline width: same 4-lane facade, slightly humbler codegen. The
    # dispatch name stays for cross-platform variant parity.
    blake3pp_add_kernel(sse42)
    blake3pp_add_kernel(avx2 ARCH_FLAGS /arch:AVX2)
    blake3pp_add_kernel(avx512 ARCH_FLAGS /arch:AVX512)
  else()
    # GNU-style drivers (GCC, Clang, clang-cl passes -m flags through).
    blake3pp_add_kernel(sse42 ARCH_FLAGS -msse4.2)
    blake3pp_add_kernel(avx2 ARCH_FLAGS -mavx2)
    # The canonical F->CD->DQ->BW progression plus VL: xsimd's arch
    # hierarchy requires CD once DQ is on, and every physical AVX-512 CPU
    # has all five.
    blake3pp_add_kernel(avx512
      ARCH_FLAGS -mavx512f -mavx512cd -mavx512vl -mavx512bw -mavx512dq)
  endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  # NEON is baseline on AArch64; the variant exists so dispatch/reporting
  # can name it, and so the scalar fallback stays honestly scalar.
  # (MSVC-arm64 needs xsimd >= 14.3: upstream revived _M_ARM64 support
  # in 2026; with the 13.2 pin this variant was scalar-only under cl.)
  blake3pp_add_kernel(neon)
elseif(EMSCRIPTEN)
  # WASM SIMD128 is a module-level feature (engines reject SIMD-bearing
  # modules wholesale if unsupported), so this is a compile-time choice;
  # the scalar variant still ships for the honest-fallback story.
  blake3pp_add_kernel(simd128 ARCH_FLAGS -msimd128)
endif()
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

if(BLAKE3PP_TOOL_MIMALLOC)
  # zig cc enforces reproducible builds (-Werror,-Wdate-time); mimalloc
  # stamps __DATE__ into its version banner. GNU-style flag: pure cl
  # rejects the spelling (and has no such warning to silence). Decided
  # HERE, where the compiler is known.
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
     AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(_mi_date_time_flag "")
  else()
    set(_mi_date_time_flag
      [=[set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -Wno-date-time")]=])
  endif()
  # MI_NO_OPT_ARCH: mimalloc's arch tuning passes -march spellings zig's
  # clang rejects (armv8.1 on aarch64), and it force-enables itself on
  # arm64 unless MI_NO_OPT_ARCH is set (priority over MI_OPT_ARCH).
  # @ONLY substitutes the @...@ slot here; the ${...} survive verbatim and
  # expand in the proxy toolchain, not at this configure.
  # MI_INSTALL_TOPLEVEL: install to <prefix>/lib and <prefix>/include
  # instead of versioned subdirs, so the mimalloc.o path below does not
  # encode the mimalloc version.
  string(CONFIGURE [=[
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT ON CACHE BOOL "" FORCE)
    set(MI_NO_OPT_ARCH ON CACHE BOOL "" FORCE)
    set(MI_INSTALL_TOPLEVEL ON CACHE BOOL "" FORCE)
    @_mi_date_time_flag@
  ]=] _mi_toolchain_extension @ONLY)
  FetchContent_Declare(mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG acf2fdd329f9dc2a7ffe3f12a133fe7175e39378) # v3.4.5
  FetchContent_MakeHermetic(mimalloc
    HERMETIC_BUILD_SYSTEM cmake
    HERMETIC_TOOLCHAIN_EXTENSION "${_mi_toolchain_extension}"
    # mimalloc names its install(EXPORT) set plain "mimalloc", so the
    # generated export file is mimalloc.cmake, invisible to hfc's
    # default ([Tt]argets|[Ee]xport(s))\.cmake discovery, which is what
    # used to force the add_subdirectory fallback. With the pattern,
    # mimalloc-static imports hermetically like every other dependency.
    # (Deliberately NOT matching the per-config mimalloc-*.cmake files:
    # the base export file includes them itself.)
    HERMETIC_DISCOVER_TARGETS_FILE_PATTERN [[/mimalloc\.cmake$]])
  HermeticFetchContent_MakeAvailableAtBuildTime(mimalloc)
  # Tripwire: the hermetic import creates only the IMPORTED
  # mimalloc-static. A real mimalloc-obj target can only mean discovery
  # failed and hfc fell back to add_subdirectory into THIS project,
  # which builds mimalloc's shared lib and tests with root flags and
  # (through hfc's extension-drop bug on that path) without the MI_*
  # settings above. Fail loudly instead of shipping that silently.

  if(WIN32)
    # Full malloc override on Windows needs the mimalloc-redirect DLL
    # pair next to every exe (their docs: static malloc override is not
    # reliable there). The reliable static route for C++ programs is the
    # global operator new/delete override, which covers effectively all
    # of the tools' allocation traffic while keeping single-file exes.
    file(CONFIGURE
      OUTPUT "${CMAKE_BINARY_DIR}/blake3pp_generated/mimalloc_newdelete.cpp"
      CONTENT "#include <mimalloc-new-delete.h>\n" @ONLY)
    function(blake3pp_tool_allocator target)
      target_sources(${target} PRIVATE
        "${CMAKE_BINARY_DIR}/blake3pp_generated/mimalloc_newdelete.cpp")
      target_link_libraries(${target} PRIVATE mimalloc-static)
    endfunction()
  else()
    # POSIX: the single combined object file statically overrides
    # malloc/free and new/delete wholesale; its definitions enter the
    # link before libc's (macOS strictly needs the .o: the interpose/zone
    # override objects are referenced by nothing, so an archive would
    # never pull them in). The mimalloc-obj OBJECT library itself cannot
    # cross the hermetic boundary (hfc target caches cannot represent
    # OBJECT libraries; mimalloc's own install(TARGETS mimalloc-obj
    # EXPORT) is commented out upstream for similar reasons), but
    # mimalloc installs the combined mimalloc.o as a plain file precisely
    # for this use case, so we link that.
    #
    # The .o goes in via target_link_options, NOT target_link_libraries:
    # link options are not dependency-tracked, so ninja does not demand a
    # producing rule for a file only the hermetic install step creates.
    # Ordering and relinking ride on mimalloc-static, whose imported
    # location IS a registered byproduct of that same install step.
    set(_mi_objfile
      "${HERMETIC_FETCHCONTENT_INSTALL_DIR}/mimalloc-install/lib/mimalloc.o")
    function(blake3pp_tool_allocator target)
      target_link_options(${target} PRIVATE "${_mi_objfile}")
      target_link_libraries(${target} PRIVATE mimalloc-static)
    endfunction()
  endif()
else()
  function(blake3pp_tool_allocator target)
  endfunction()
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
  target_include_directories(blake3pp_tooling
    INTERFACE "${PROJECT_SOURCE_DIR}/tooling")
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

option(BLAKE3PP_BUILD_CLI "Build the blake3ppsum utility"
  ${PROJECT_IS_TOP_LEVEL})
if(BLAKE3PP_BUILD_CLI AND BLAKE3PP_HAS_FORMAT)
  add_subdirectory(cli)
endif()
