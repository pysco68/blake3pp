# Probes what the active standard library actually provides, so the same
# source tree uses native C++26 facilities where they exist and polyfills
# (xsimd, stdexec) where they don't.
#
# Feature-test macros are NOT sufficient here: GCC 16 trunk ships a working
# C++26 <simd> (as std::simd::vec) without defining __cpp_lib_simd yet. So we
# probe by compiling real usage under the exact toolchain/standard/stdlib of
# the current configure, which try_compile inherits (including
# CMAKE_CXX_STANDARD and toolchain-file flags).
#
# Results surface as compile definitions on blake3pp::features:
#   BLAKE3PP_HAS_STD_SIMD      <simd> with std::simd::vec usable
#   BLAKE3PP_HAS_STD_SENDERS   <execution> with std::execution::just usable

include_guard(GLOBAL)

add_library(blake3pp_features INTERFACE)
add_library(blake3pp::features ALIAS blake3pp_features)

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  # MSVC reports __cplusplus as 199711L unless told otherwise, which makes
  # stdexec (and any other honest feature-test consumer) refuse to build;
  # the conformant preprocessor is likewise table stakes for its macro
  # machinery. Both are pure conformance switches, safe project-wide.
  target_compile_options(blake3pp_features INTERFACE
    /Zc:__cplusplus /Zc:preprocessor)
endif()

function(_blake3pp_probe var code)
  if(NOT DEFINED CACHE{${var}})
    set(src "${CMAKE_BINARY_DIR}/CMakeFiles/blake3pp_probes/${var}.cpp")
    file(WRITE "${src}" "${code}")
    try_compile(${var} SOURCES "${src}")
  endif()
  if(${var})
    target_compile_definitions(blake3pp_features INTERFACE "${var}=1")
    message(STATUS "blake3pp: ${var} = yes")
  else()
    message(STATUS "blake3pp: ${var} = no (polyfill path)")
  endif()
endfunction()

_blake3pp_probe(BLAKE3PP_HAS_STD_SIMD [[
#include <simd>
int main() {
  std::simd::vec<unsigned, 8> v{};
  return static_cast<int>(v.size()) - 8;
}
]])

_blake3pp_probe(BLAKE3PP_HAS_STD_SENDERS [[
#include <execution>
int main() {
  auto s = std::execution::just(42);
  (void)s;
}
]])

# First transparent fallback: the Parallelism TS v2 <experimental/simd>.
# libstdc++'s implementation (GCC 11+, and Clang against a correctly PINNED
# libstdc++) is complete enough for our facade at C++17 and up. libc++'s is
# not: it hides behind -fexperimental-library and lacks the shift operators,
# so this probe rightly fails there and xsimd takes over.
if(NOT BLAKE3PP_HAS_STD_SIMD)
  _blake3pp_probe(BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD [[
#include <experimental/simd>
#include <cstdint>
int main() {
  namespace stdx = std::experimental;
  using V = stdx::native_simd<std::uint32_t>;
  std::uint32_t a[V::size()];
  V x(7u);
  V y = (x << 3) | (x >> 29);
  y = y + x;
  y = y ^ x;
  y.copy_to(a, stdx::element_aligned);
  V z;
  z.copy_from(a, stdx::element_aligned);
  return static_cast<int>(a[0]) - 56;
}
]])
endif()

# Senders polyfill: no shipping standard library has std::execution yet, so
# NVIDIA's stdexec provides the sender/receiver machinery. Header-only use:
# SOURCE_SUBDIR points at include/, which has no CMakeLists.txt, so
# FetchContent populates without configuring stdexec's own build (which
# would pull rapids-cmake from the network).
if(NOT BLAKE3PP_HAS_STD_SENDERS)
  include(FetchContent)
  FetchContent_Declare(stdexec
    URL https://github.com/NVIDIA/stdexec/archive/refs/tags/nvhpc-26.05.tar.gz
    URL_HASH SHA256=9d2396fecd604698c1eae58f0cb6e4517aa727013846240d1a7b2f35e49884dc
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR include)
  FetchContent_MakeAvailable(stdexec)
  find_package(Threads REQUIRED)
  add_library(blake3pp_stdexec INTERFACE)
  target_include_directories(blake3pp_stdexec SYSTEM INTERFACE
    "${stdexec_SOURCE_DIR}/include")
  target_link_libraries(blake3pp_stdexec INTERFACE Threads::Threads)
  target_link_libraries(blake3pp_features INTERFACE blake3pp_stdexec)
endif()

# Last resort: xsimd (header-only, imported as SYSTEM so its headers stay
# outside our warning net).
if(NOT BLAKE3PP_HAS_STD_SIMD AND NOT BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD)
  include(FetchContent)
  FetchContent_Declare(xsimd
    URL https://github.com/xtensor-stack/xsimd/archive/refs/tags/13.2.0.tar.gz
    URL_HASH SHA256=edd8cd3d548c185adc70321c53c36df41abe64c1fe2c67bc6d93c3ecda82447a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  FetchContent_MakeAvailable(xsimd)
  target_link_libraries(blake3pp_features INTERFACE xsimd)
  target_compile_definitions(blake3pp_features INTERFACE BLAKE3PP_HAS_XSIMD=1)
endif()
