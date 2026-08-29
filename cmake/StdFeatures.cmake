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

# ------------------------------------------------------ sender/receiver
# The provider is an explicit choice, not pure detection:
#   auto    - std::execution when the standard library ships a usable one,
#             NVIDIA stdexec otherwise (beman is never auto-selected while
#             its upstream labels itself pre-production)
#   std     - require the standard library's std::execution
#   beman   - beman.execution: the conformance-first polyfill (C++23+);
#             the executables additionally link blake3pp::beman_backend,
#             which backs get_parallel_scheduler() with a real pool
#   stdexec - NVIDIA stdexec: the performance workhorse (C++20+)
# Exactly one of BLAKE3PP_EXECUTION_{STD,BEMAN,STDEXEC} lands on
# blake3pp::features; <blake3pp/parallel.hpp> switches on it.
set(BLAKE3PP_EXECUTION_PROVIDER "auto" CACHE STRING
  "sender/receiver provider: auto, std, beman, stdexec")
set_property(CACHE BLAKE3PP_EXECUTION_PROVIDER
  PROPERTY STRINGS auto std beman stdexec)

if(BLAKE3PP_EXECUTION_PROVIDER STREQUAL "auto")
  if(BLAKE3PP_HAS_STD_SENDERS)
    set(_blake3pp_execution_provider std)
  else()
    set(_blake3pp_execution_provider stdexec)
  endif()
elseif(BLAKE3PP_EXECUTION_PROVIDER MATCHES "^(std|beman|stdexec)$")
  set(_blake3pp_execution_provider "${BLAKE3PP_EXECUTION_PROVIDER}")
else()
  message(FATAL_ERROR "blake3pp: BLAKE3PP_EXECUTION_PROVIDER must be one of "
    "auto, std, beman, stdexec (got '${BLAKE3PP_EXECUTION_PROVIDER}')")
endif()
message(STATUS "blake3pp: execution provider = ${_blake3pp_execution_provider} "
  "(BLAKE3PP_EXECUTION_PROVIDER=${BLAKE3PP_EXECUTION_PROVIDER})")

# stdexec is needed by the stdexec provider AND by the beman bridge backend
# (which drives beman's parallel_scheduler with stdexec's pool until beman
# ships a default backend).
#
# It is also the one INTERLOCKED (populate-only) hfc content, by policy: its
# upstream CMake pulls rapids-cmake from the network at configure time, so
# it must never be configured. SOURCE_SUBDIR points at include/, which has
# no CMakeLists, so hfc populates the shared source cache (cross-process
# locked via goldilock) and stops; the INTERFACE target below is the sole
# consumer surface. Every other dependency is a classic hermetic content.
macro(_blake3pp_fetch_stdexec)
  if(NOT TARGET blake3pp_stdexec)
    include(FetchContent)
    FetchContent_Declare(stdexec
      GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
      GIT_TAG 6d7ad689f4d4831c5136e4abe1c601f9a3b64e43 # nvhpc-26.05
      SOURCE_SUBDIR include
      BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/stdexec-build")
    hfc_FetchContent_MakeAvailable_interlocked(stdexec)
    find_package(Threads REQUIRED)
    add_library(blake3pp_stdexec INTERFACE)
    target_include_directories(blake3pp_stdexec SYSTEM INTERFACE
      "${stdexec_SOURCE_DIR}/include")
    target_link_libraries(blake3pp_stdexec INTERFACE Threads::Threads)
  endif()
endmacro()

if(_blake3pp_execution_provider STREQUAL "std")
  if(NOT BLAKE3PP_HAS_STD_SENDERS)
    message(FATAL_ERROR "blake3pp: BLAKE3PP_EXECUTION_PROVIDER=std, but this "
      "standard library has no usable std::execution (see the "
      "BLAKE3PP_HAS_STD_SENDERS probe above)")
  endif()
  target_compile_definitions(blake3pp_features INTERFACE
    BLAKE3PP_EXECUTION_STD=1)

elseif(_blake3pp_execution_provider STREQUAL "beman")
  if(DEFINED CMAKE_CXX_STANDARD AND CMAKE_CXX_STANDARD LESS 23)
    message(FATAL_ERROR "blake3pp: BLAKE3PP_EXECUTION_PROVIDER=beman, but "
      "beman.execution requires C++23 or newer; this project's cxx20 "
      "presets cannot use it")
  endif()
  # Pinned commit: beman has no tagged releases yet. Includes P2079R10
  # parallel_scheduler (merged 2026-07-12). Classic hermetic content:
  # header-only, so the "build" is just the header/config install.
  FetchContent_Declare(beman_execution
    GIT_REPOSITORY https://github.com/bemanproject/execution.git
    GIT_TAG cc721c44496bc4b5ae63ad4ea47fe965818e52ae)
  FetchContent_MakeHermetic(beman_execution
    HERMETIC_BUILD_SYSTEM cmake
    HERMETIC_TOOLCHAIN_EXTENSION [=[
      set(BEMAN_USE_MODULES OFF CACHE BOOL "" FORCE)
      # beman gates its (large, occasionally non-compiling) test suite on
      # its own option, not BUILD_TESTING; hermetic makes it top-level,
      # which would default the tests on.
      set(BEMAN_EXECUTION_BUILD_TESTS OFF CACHE BOOL "" FORCE)
      set(BEMAN_EXECUTION_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
      set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    ]=])
  HermeticFetchContent_MakeAvailableAtBuildTime(beman_execution)
  target_link_libraries(blake3pp_features INTERFACE beman::execution)
  # TODO(upstream hfc): beman's install export carries its include dirs only
  # via HEADERS file sets on CMake >= 3.23 (INTERFACE_HEADER_SETS /
  # BASE_DIRS), which hfc's target discovery doesn't fold into
  # INTERFACE_INCLUDE_DIRECTORIES yet, so the reconstructed beman::execution
  # arrives include-less. Bridge it explicitly until hfc learns file sets.
  target_include_directories(blake3pp_features SYSTEM INTERFACE
    "${HERMETIC_FETCHCONTENT_INSTALL_DIR}/beman_execution-install/include")
  target_compile_definitions(blake3pp_features INTERFACE
    BLAKE3PP_EXECUTION_BEMAN=1)
  _blake3pp_fetch_stdexec()  # engine room for blake3pp::beman_backend

else()  # stdexec
  _blake3pp_fetch_stdexec()
  target_link_libraries(blake3pp_features INTERFACE blake3pp_stdexec)
  target_compile_definitions(blake3pp_features INTERFACE
    BLAKE3PP_EXECUTION_STDEXEC=1)
endif()

# xsimd (header-only, imported as SYSTEM so its headers stay outside our
# warning net). Fetched on demand: as the project-wide provider of last
# resort below, and by kernels that pin themselves to xsimd via
# blake3pp_add_kernel(... FORCE_XSIMD) even when a std provider exists.
macro(_blake3pp_fetch_xsimd)
  if(NOT TARGET xsimd)
    FetchContent_Declare(xsimd
      GIT_REPOSITORY https://github.com/xtensor-stack/xsimd.git
      GIT_TAG e88a72831858123924f7118f345dfe5d70d95991) # 14.3.0
    FetchContent_MakeHermetic(xsimd HERMETIC_BUILD_SYSTEM cmake)
    HermeticFetchContent_MakeAvailableAtBuildTime(xsimd)
  endif()
endmacro()

# Last resort: the project-wide simd provider falls back to xsimd.
if(NOT BLAKE3PP_HAS_STD_SIMD AND NOT BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD)
  _blake3pp_fetch_xsimd()
  target_link_libraries(blake3pp_features INTERFACE xsimd)
  target_compile_definitions(blake3pp_features INTERFACE BLAKE3PP_HAS_XSIMD=1)
endif()
