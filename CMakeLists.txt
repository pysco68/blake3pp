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
include(StdFeatures)
include(ArchKernels)

add_library(blake3pp_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
   AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  # cl.exe: /W4 is the sane maximum (/Wall drowns in STL noise); clang-cl
  # takes the GNU-style set below.
  target_compile_options(blake3pp_warnings INTERFACE /W4)
else()
  target_compile_options(blake3pp_warnings INTERFACE
    -Wall -Wextra -Wconversion -Wshadow)
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
  include(FetchContent)
  set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_OBJECT ON CACHE BOOL "" FORCE)
  # mimalloc's arch tuning passes -march spellings zig's clang rejects at
  # build time (armv8.1 on aarch64), and it force-enables itself on arm64
  # unless MI_NO_OPT_ARCH is set (which has priority over MI_OPT_ARCH).
  set(MI_NO_OPT_ARCH ON CACHE BOOL "" FORCE)
  FetchContent_Declare(mimalloc
    URL https://github.com/microsoft/mimalloc/archive/refs/tags/v3.4.5.tar.gz
    URL_HASH SHA256=19a43af0645c57d348e729d5b31e23e912582911bb1047f795790834d3416221
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  FetchContent_MakeAvailable(mimalloc)
  # zig cc enforces reproducible builds (-Werror,-Wdate-time); mimalloc
  # stamps __DATE__ into its version banner. GNU-style flag: pure cl
  # rejects the spelling (and has no such warning to silence).
  if(NOT (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
          AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"))
    foreach(mi_target mimalloc-static mimalloc-obj)
      if(TARGET ${mi_target})
        target_compile_options(${mi_target} PRIVATE -Wno-date-time)
      endif()
    endforeach()
  endif()
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
      target_include_directories(${target} PRIVATE
        "${mimalloc_SOURCE_DIR}/include")
    endfunction()
  else()
    # POSIX: the object-library form statically overrides malloc/free and
    # new/delete wholesale; its definitions enter the link before libc's.
    function(blake3pp_tool_allocator target)
      target_sources(${target} PRIVATE $<TARGET_OBJECTS:mimalloc-obj>)
      target_link_libraries(${target} PRIVATE mimalloc-obj)
    endfunction()
  endif()
else()
  function(blake3pp_tool_allocator target)
  endfunction()
endif()

if(BLAKE3PP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(BLAKE3PP_BUILD_BENCH)
  add_subdirectory(bench)
endif()

option(BLAKE3PP_BUILD_CLI "Build the blake3ppsum utility"
  ${PROJECT_IS_TOP_LEVEL})
if(BLAKE3PP_BUILD_CLI)
  add_subdirectory(cli)
endif()
