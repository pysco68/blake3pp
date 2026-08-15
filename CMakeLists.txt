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
option(BLAKE3PP_BUILD_BENCH "Build blake3pp benchmarks" OFF)

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
# its own OBJECT library under its own namespace and -m flags; M2 adds
# sse42/avx2/avx512/neon entries here.
blake3pp_add_kernel(scalar)
blake3pp_target_link_kernels(blake3pp)

if(BLAKE3PP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
