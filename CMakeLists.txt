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

option(BLAKE3PP_BUILD_TESTS "Build blake3pp tests" ${PROJECT_IS_TOP_LEVEL})
option(BLAKE3PP_BUILD_BENCH "Build blake3pp benchmarks" ${PROJECT_IS_TOP_LEVEL})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(StdFeatures)
include(ArchKernels)

add_library(blake3pp_warnings INTERFACE)
target_compile_options(blake3pp_warnings INTERFACE
  -Wall -Wextra -Wconversion -Wshadow)

add_library(blake3pp STATIC
  src/blake3pp.cpp
  src/dispatch/dispatch.cpp)
add_library(blake3pp::blake3pp ALIAS blake3pp)

target_compile_features(blake3pp PUBLIC cxx_std_20)
target_include_directories(blake3pp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(blake3pp PRIVATE src)
target_link_libraries(blake3pp
  PRIVATE blake3pp_warnings blake3pp::features)

# Architecture kernel variants. Each call compiles src/kernel/kernel.cpp into
# its own OBJECT library under its own namespace and -m flags; all registered
# variants land in the one binary and runtime dispatch picks among them.
blake3pp_add_kernel(scalar FORCE_SCALAR)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  blake3pp_add_kernel(sse42 ARCH_FLAGS -msse4.2)
  blake3pp_add_kernel(avx2 ARCH_FLAGS -mavx2)
  # The canonical F->CD->DQ->BW progression plus VL: xsimd's arch hierarchy
  # requires CD once DQ is on, and every physical AVX-512 CPU has all five.
  blake3pp_add_kernel(avx512
    ARCH_FLAGS -mavx512f -mavx512cd -mavx512vl -mavx512bw -mavx512dq)
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
