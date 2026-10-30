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
  # clang-cl's preprocessor is conformant already and reports
  # /Zc:preprocessor as an unused argument, so cl alone is given it.
  target_compile_options(blake3pp_features INTERFACE /Zc:__cplusplus)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(blake3pp_features INTERFACE /Zc:preprocessor)
  endif()
endif()

function(_blake3pp_probe var code)
  if(NOT DEFINED CACHE{${var}})
    set(src "${CMAKE_BINARY_DIR}/CMakeFiles/blake3pp_probes/${var}.cpp")
    file(WRITE "${src}" "${code}")
    # Compile-only: these are header/usability questions, and a linked
    # probe makes zig build its runtime libraries for the probe's
    # optimisation mode first (see blake3pp_probe_flag_candidates).
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    try_compile(${var} SOURCES "${src}")
  endif()
  if(${var})
    target_compile_definitions(blake3pp_features INTERFACE "${var}=1")
    message(STATUS "blake3pp: ${var} = yes")
  else()
    message(STATUS "blake3pp: ${var} = no (polyfill path)")
  endif()
endfunction()

# The simd provider is an explicit choice with an auto default, mirroring
# BLAKE3PP_EXECUTION_PROVIDER below:
#   auto             - std::simd if the standard library ships one, else
#                      std::experimental::simd, else xsimd
#   std              - require C++26 <simd>
#   std-experimental - require the Parallelism TS v2 <experimental/simd>
#   xsimd            - skip the std probes entirely. The riscv64 toolchain
#                      sets this: libstdc++ has no RVV ABI, so its
#                      experimental::simd would pass the probe yet never
#                      vectorize there.
#   xvec             - Intel's xvec, an independent header-only
#                      implementation of the C++26 library (xvec::simd),
#                      fetched from GitHub; the std probes are skipped.
# Note the per-kernel FORCE_XSIMD pin (cmake/ArchKernels.cmake) is a
# separate, narrower mechanism: it overrides the provider for single
# kernel TUs (the SVE/RVV variants) regardless of this choice.
set(BLAKE3PP_SIMD_PROVIDER "auto" CACHE STRING
  "simd provider: auto, std, std-experimental, xsimd, xvec")
set_property(CACHE BLAKE3PP_SIMD_PROVIDER
  PROPERTY STRINGS auto std std-experimental xsimd xvec)
if(NOT BLAKE3PP_SIMD_PROVIDER MATCHES "^(auto|std|std-experimental|xsimd|xvec)$")
  message(FATAL_ERROR "blake3pp: BLAKE3PP_SIMD_PROVIDER must be one of "
    "auto, std, std-experimental, xsimd, xvec (got '${BLAKE3PP_SIMD_PROVIDER}')")
endif()
message(STATUS
  "blake3pp: simd provider choice = ${BLAKE3PP_SIMD_PROVIDER}")

if(BLAKE3PP_SIMD_PROVIDER MATCHES "^(auto|std)$")
  # Both shapes the facade needs: a fixed width to know the API is there,
  # and the NATIVE width it actually spells (src/kernel/simd/std_simd.hpp).
  # An implementation whose arch traits do not cover this target deduces one
  # lane and compiles perfectly, which would be a scalar kernel registered
  # under a vector name -- GCC 16's <simd> off x86 is the live example, and
  # the kernel's own static_assert refuses to build it.
  _blake3pp_probe(BLAKE3PP_HAS_STD_SIMD [[
#include <cstdint>
#include <simd>
int main() {
  std::simd::vec<unsigned, 8> fixed{};
  using native = std::simd::vec<std::uint32_t>;
  static_assert(native::size() > 1,
                "std::simd deduces a single lane for this target");
  return static_cast<int>(fixed.size()) - 8;
}
]])
endif()
if(BLAKE3PP_SIMD_PROVIDER STREQUAL "std" AND NOT BLAKE3PP_HAS_STD_SIMD)
  message(FATAL_ERROR "blake3pp: BLAKE3PP_SIMD_PROVIDER=std, but this "
    "standard library has no usable <simd> (see the probe above)")
endif()

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
if((BLAKE3PP_SIMD_PROVIDER STREQUAL "auto" AND NOT BLAKE3PP_HAS_STD_SIMD)
   OR BLAKE3PP_SIMD_PROVIDER STREQUAL "std-experimental")
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
if(BLAKE3PP_SIMD_PROVIDER STREQUAL "std-experimental"
   AND NOT BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD)
  message(FATAL_ERROR "blake3pp: BLAKE3PP_SIMD_PROVIDER=std-experimental, "
    "but this standard library has no usable <experimental/simd> (see the "
    "probe above)")
endif()

# ------------------------------------------------------ sender/receiver
# Provider selection: `auto` resolves by probe, the named values are taken
# as given:
#   auto    - std::execution when the standard library ships a usable one,
#             NVIDIA stdexec otherwise (beman is never auto-selected while
#             its upstream labels itself pre-production)
#   std     - require the standard library's std::execution
#   beman   - beman.execution: the conformance-first polyfill (C++23+),
#             including its own parallel_scheduler backend
#   stdexec - NVIDIA stdexec: the performance workhorse (C++20+)
# Exactly one of BLAKE3PP_EXECUTION_{STD,BEMAN,STDEXEC} lands on
# blake3pp::features; <blake3pp/parallel.hpp> switches on it.
# The opt-in sized parallel scheduler (<blake3pp/parallel_backend.hpp>).
# P2079's scheduler is process-wide and takes no size; a program chooses
# otherwise by replacing the backend, one definition per program. ON
# builds that replacement into its own target, and under beman also builds
# beman's own default backend out. OFF by default: a program that brings
# its own scheduler, or wants every core, links neither.
option(BLAKE3PP_SIZED_PARALLEL_SCHEDULER
  "build blake3pp::parallel_backend, the sizable process-wide scheduler" OFF)

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

# Does this toolchain have OS threads at all?  A freestanding target
# typically does not: the Zephyr SDK's libstdc++, for instance, is built
# without gthreads, so <thread>, <mutex> and <condition_variable> are
# empty headers there.  <blake3pp/parallel.hpp> uses the answer to decide
# whether it can offer a process-wide pool behind get_parallel_scheduler();
# the scheduler-taking hash overloads, which are the primary API, work
# either way.
_blake3pp_probe(BLAKE3PP_HAS_STD_THREAD [[
#include <thread>
#include <mutex>
#include <condition_variable>
int main() {
  std::mutex m;
  std::condition_variable cv;
  (void)m; (void)cv;
  return static_cast<int>(std::thread::hardware_concurrency());
}
]])

# stdexec is needed by the stdexec provider alone: beman ships its own
# parallel_scheduler backend as of 2026-09-12, so choosing beman no longer
# pulls stdexec into the build.
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
      GIT_TAG 792e1342f8c983e205d899f840038d6d57fc9bd1 # main, 2026-10-14
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
  # parallel_scheduler (merged 2026-07-12) and its default backend
  # (2026-09-12), which is what makes this provider stand alone.
  FetchContent_Declare(beman_execution
    GIT_REPOSITORY https://github.com/bemanproject/execution.git
    GIT_TAG 8e358c01f4f4435e06b0691de433f4b2a07367be) # main, 2026-09-22
  set(_blake3pp_beman_extension [=[
      set(BEMAN_USE_MODULES OFF CACHE BOOL "" FORCE)
      # beman gates its (large, occasionally non-compiling) test suite on
      # its own option, not BUILD_TESTING. Hermetic makes it top-level,
      # which would default the tests on.
      # No semicolons anywhere in this block, comments included: hfc
      # carries the extension through a CMake list and drops everything
      # after the first one.
      set(BEMAN_EXECUTION_BUILD_TESTS OFF CACHE BOOL "" FORCE)
      set(BEMAN_EXECUTION_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
      set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    ]=])
  if(BLAKE3PP_SIZED_PARALLEL_SCHEDULER)
    # src/execution/parallel_backend.cpp defines the replacement, so
    # beman's own default backend is built out: the program holds one
    # definition of query_parallel_scheduler_backend().
    # At the pinned commit the option reaches consumers as
    # BEMAN_EXECUTION_WITH_DEFAULT_PARALLEL_SCHEDULER_BACKEND=0, and
    # beman's headers default the macro to 1 when it is absent. Building
    # the backend out therefore depends on that INTERFACE definition
    # arriving on every blake3pp compile line.
    string(APPEND _blake3pp_beman_extension [=[
      set(BEMAN_EXECUTION_WITH_DEFAULT_PARALLEL_SCHEDULER_BACKEND OFF CACHE BOOL "" FORCE)
    ]=])
  endif()
  FetchContent_MakeHermetic(beman_execution
    HERMETIC_BUILD_SYSTEM cmake
    HERMETIC_TOOLCHAIN_EXTENSION "${_blake3pp_beman_extension}")
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

# xvec by explicit choice: header-only and without a build system of its
# own, so it is populated with plain FetchContent (not hermetic: nothing to
# build) and exposed as an INTERFACE target over its include directory.
if(BLAKE3PP_SIMD_PROVIDER STREQUAL "xvec")
  FetchContent_Declare(xvec
    GIT_REPOSITORY https://github.com/intel/xvec.git
    GIT_TAG d876204b506ecb001d47e88ca0cc97ad61c34c26) # main, 2026-10-06
  FetchContent_MakeAvailable(xvec)
  add_library(xvec INTERFACE)
  add_library(xvec::xvec ALIAS xvec)
  target_include_directories(xvec SYSTEM INTERFACE "${xvec_SOURCE_DIR}/include")
  target_link_libraries(blake3pp_features INTERFACE xvec)
  target_compile_definitions(blake3pp_features INTERFACE BLAKE3PP_HAS_XVEC=1)
  message(STATUS "blake3pp: simd provider = xvec (${xvec_SOURCE_DIR})")
endif()

# Last resort: the project-wide simd provider falls back to xsimd.
#
# Resolved after blake3pp_register_kernels(), not here. src/kernel/
# simd_facade.hpp is included from kernel translation units and nowhere
# else, so the project-wide provider has a consumer only when some kernel
# pins none of its own; which kernels exist is not known until they have
# registered, which is long after this file runs. A build whose every
# kernel is pinned (loongarch64: scalar plus two vector-extension kernels)
# would otherwise clone and build xsimd, and put its include path and
# BLAKE3PP_HAS_XSIMD on every compile line, for a header nothing includes.
#
# An explicitly named provider is resolved whether or not a kernel falls
# back to it: BLAKE3PP_SIMD_PROVIDER=xsimd is also how the cross-compiling
# toolchains make simd_provider() name the provider their pinned vector
# kernel uses.
macro(blake3pp_resolve_simd_provider)
  if(NOT BLAKE3PP_HAS_STD_SIMD AND NOT BLAKE3PP_HAS_STD_EXPERIMENTAL_SIMD
     AND NOT BLAKE3PP_SIMD_PROVIDER STREQUAL "xvec")
    get_property(_blake3pp_simd_needed GLOBAL PROPERTY
      BLAKE3PP_KERNELS_NEED_SIMD_PROVIDER)
    if(_blake3pp_simd_needed OR BLAKE3PP_SIMD_PROVIDER STREQUAL "xsimd")
      _blake3pp_fetch_xsimd()
      target_link_libraries(blake3pp_features INTERFACE xsimd)
      target_compile_definitions(blake3pp_features INTERFACE BLAKE3PP_HAS_XSIMD=1)
    else()
      message(STATUS "blake3pp: simd provider = none; every kernel pins its own")
    endif()
  endif()
endmacro()
