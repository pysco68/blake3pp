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
target_compile_definitions(blake3pp PRIVATE
  "BLAKE3PP_VERSION=\"${PROJECT_VERSION}\"")
target_include_directories(blake3pp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(blake3pp PRIVATE src)
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
  # NEON is baseline on AArch64; the variant exists so dispatch/reporting can
  # name it, and so the scalar fallback stays honestly scalar.
  blake3pp_add_kernel(neon)
endif()
blake3pp_target_link_kernels(blake3pp)

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
