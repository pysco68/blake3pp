# The official BLAKE3 C library (with its hand-written assembly kernels) is
# built alongside the bench as the performance baseline, so every run shows
# blake3pp's variants against the hand-tuned ceiling on the same machine. Its
# c/CMakeLists.txt is a proper subproject; SOURCE_SUBDIR points there.
option(BLAKE3PP_BENCH_UPSTREAM
  "Include the upstream BLAKE3 C library as a baseline in the benchmark" ON)

add_executable(blake3pp_bench throughput.cpp)
target_link_libraries(blake3pp_bench PRIVATE
  blake3pp::blake3pp
  blake3pp_warnings
  blake3pp::tooling
  blake3pp::tool_allocator
  blake3pp::node_program)
# For the asm-plug-in rows: the bench builds a kernel_ops table around
# upstream's assembly, which needs the internal kernel boundary header.
target_include_directories(blake3pp_bench PRIVATE "${PROJECT_SOURCE_DIR}/src")

# The file bench measures the I/O pipeline; it has no subject without it.
if(BLAKE3PP_WITH_IO)
  add_executable(blake3pp_bench_file file_throughput.cpp)
  # The null source builds the reader engine directly (io/engine.hpp),
  # which is internal to src/.
  target_include_directories(blake3pp_bench_file PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(blake3pp_bench_file PRIVATE
    blake3pp::blake3pp
    blake3pp_warnings
    blake3pp::tooling
    blake3pp::tool_allocator
    blake3pp::node_program)
endif()

if(BLAKE3PP_BENCH_UPSTREAM)
  # The upstream library is C plus hand-written assembly; enabling the
  # languages here (rather than letting its nested project() try) keeps
  # compiler-feature detection in a scope CMake handles reliably. There is
  # no assembly to assemble on wasm.
  enable_language(C)
  if(NOT EMSCRIPTEN)
    enable_language(ASM)
  endif()
  FetchContent_Declare(blake3_upstream
    GIT_REPOSITORY https://github.com/BLAKE3-team/BLAKE3.git
    GIT_TAG 77b257eee7da5cd608eaf6be8343d3a4c9776af2 # 1.8.6
    SOURCE_SUBDIR c)
  if(EMSCRIPTEN)
    # Upstream selects its kernels from CMAKE_SYSTEM_PROCESSOR, which
    # Emscripten reports as "x86", so it picks the x86 intrinsics kernels
    # and then fails on -msse2, which the wasm target rejects. Its own
    # BLAKE3_SIMD_TYPE=none arm builds the portable C alone, which compiles
    # to wasm perfectly well. That keeps the `portable` and `reference`
    # rows on this target: without them a wasm bench run has no
    # same-pipeline control to measure against, only its own noise.
    FetchContent_MakeHermetic(blake3_upstream HERMETIC_BUILD_SYSTEM cmake
      HERMETIC_TOOLCHAIN_EXTENSION [=[
        set(BLAKE3_SIMD_TYPE "none" CACHE STRING "" FORCE)
      ]=])
  else()
    FetchContent_MakeHermetic(blake3_upstream HERMETIC_BUILD_SYSTEM cmake)
  endif()
  HermeticFetchContent_MakeAvailableAtBuildTime(blake3_upstream)
  target_link_libraries(blake3pp_bench PRIVATE BLAKE3::blake3)
  target_compile_definitions(blake3pp_bench PRIVATE BLAKE3PP_BENCH_UPSTREAM=1)
  # Upstream assembles its MASM sources under MSVC (clang-cl included), and
  # those carry no blake3_xof_many_avx512: only the Unix and windows_gnu
  # sources do. The bench's avx512 extended-output row needs that symbol,
  # so it is left out where the reference does not provide it.
  if(MSVC)
    target_compile_definitions(blake3pp_bench PRIVATE BLAKE3PP_BENCH_NO_XOF_ASM=1)
  endif()
endif()

# The sized parallel scheduler is one definition per program; the benches
# take it when the build offers one (see the root CMakeLists).
if(TARGET blake3pp_parallel_backend)
  target_link_libraries(blake3pp_bench PRIVATE blake3pp::parallel_backend)
  if(TARGET blake3pp_bench_file)
    target_link_libraries(blake3pp_bench_file PRIVATE blake3pp::parallel_backend)
  endif()
endif()

# Pipe instruments and the scripts that run them; standalone, so they
# build where the executables above do not. Not under Emscripten: they
# spawn processes and use pipes, which the wasm target does not have
# (wasm-ld reports posix_spawnp undefined in rux).
if(NOT EMSCRIPTEN)
  add_subdirectory(pipe)
endif()

# Smoke: the benches are shipped tools, so one tiny run each proves they
# start, find their kernels and print. It also runs the shared tooling
# (pinning, cooldown, timing) the CLI smoke tests never reach, which the
# coverage lanes would otherwise report as dead.
add_test(NAME bench_smoke
  COMMAND blake3pp_bench --size 4 --reps 1 --cooldown 0 --threads 2)
if(TARGET blake3pp_bench_file)
  add_test(NAME bench_file_smoke
    COMMAND blake3pp_bench_file --make 8 --reps 1 --cooldown 0 --threads 2)
endif()

# The fat binary's cost at the call boundary, isolated (see the file's
# header): the kernel TU compiled into the bench itself, one executable
# per vector variant the compiler can express for this target, then
# called through a table pointer, through a plain call and by name.
# Linux gets hardware counters through perf_event_open; elsewhere the
# wall clock.
#
# The flags and the provider pin are the registered kernel's own, read back
# from the properties blake3pp_add_kernel() records: this compiles the same
# TU as the library and a second spelling of the flags would be a second
# thing to keep in step. A variant this build did not register has none, and
# the bench for it is skipped.
function(_blake3pp_dispatch_cost ns)
  get_property(_dc_kernels GLOBAL PROPERTY BLAKE3PP_KERNELS)
  if(NOT ns IN_LIST _dc_kernels)
    return()
  endif()
  get_property(_dc_flags GLOBAL PROPERTY "BLAKE3PP_KERNEL_${ns}_FLAGS")
  get_property(_dc_provider GLOBAL PROPERTY "BLAKE3PP_KERNEL_${ns}_PROVIDER")
  get_property(_dc_vext_bytes GLOBAL PROPERTY "BLAKE3PP_KERNEL_${ns}_VEXT_BYTES")
  set(tgt blake3pp_dispatch_cost_${ns})
  add_executable(${tgt} dispatch_cost.cpp)
  get_property(_dc_switches GLOBAL PROPERTY BLAKE3PP_KERNEL_SWITCHES)
  target_compile_definitions(${tgt} PRIVATE "BLAKE3PP_ARCH_NS=${ns}" ${_dc_switches})
  if(_dc_provider STREQUAL "scalar")
    target_compile_definitions(${tgt} PRIVATE BLAKE3PP_FORCE_SCALAR=1)
  elseif(_dc_provider STREQUAL "vext")
    target_compile_definitions(${tgt} PRIVATE
      BLAKE3PP_FORCE_VEXT=1 "BLAKE3PP_VEXT_BYTES=${_dc_vext_bytes}")
  elseif(_dc_provider STREQUAL "xsimd")
    target_compile_definitions(${tgt} PRIVATE BLAKE3PP_FORCE_XSIMD=1)
    target_link_libraries(${tgt} PRIVATE xsimd)
  endif()
  # GCC guesses the target of an indirect call whose only plausible
  # target is visible in the TU and emits a compare and a branch to a
  # direct call ahead of it (its devirtualization pass, which also
  # covers plain function pointers): correct, and exactly what the
  # library's separate kernel TUs make impossible, so it is turned off
  # here to measure the real indirect call. IPA-CP cloning is off for
  # the same reason: the direct and in-TU calls pass constant arguments
  # and GCC otherwise specialises a clone of the kernel entry for them,
  # which the indirect call cannot reach, and the two paths stop running
  # the same body. Clang has neither pass to disable.
  target_compile_options(${tgt} PRIVATE ${_dc_flags}
    "$<$<CONFIG:Release,RelWithDebInfo>:-O3>"
    "$<$<CXX_COMPILER_ID:GNU>:-fno-devirtualize;-fno-ipa-cp-clone>")
  target_include_directories(${tgt} PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
  target_link_libraries(${tgt} PRIVATE blake3pp::features blake3pp_warnings)
  set_target_properties(${tgt} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
endfunction()
# Off by default: up to four extra kernel compilations that only the
# dispatch question needs.
option(BLAKE3PP_BENCH_DISPATCH_COST
  "Build the dispatch-cost benches (one kernel compilation per variant)" OFF)
if(BLAKE3PP_BENCH_DISPATCH_COST
   AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" AND NOT EMSCRIPTEN)
  # The detection ladder's cost: the first call of a fresh process.
  add_executable(blake3pp_detect_cost detect_cost.cpp)
  target_link_libraries(blake3pp_detect_cost PRIVATE blake3pp::blake3pp blake3pp_warnings)
  # Which variants the call-site question needs, per architecture; each one
  # is built only if the library registered it, with that kernel's flags.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    foreach(ns IN ITEMS scalar sse42 avx2 avx512)
      _blake3pp_dispatch_cost(${ns})
    endforeach()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    foreach(ns IN ITEMS scalar neon)
      _blake3pp_dispatch_cost(${ns})
    endforeach()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(riscv64)$")
    # One RVV width is enough for the call-site question.
    foreach(ns IN ITEMS scalar rvv256)
      _blake3pp_dispatch_cost(${ns})
    endforeach()
  endif()
endif()

# The reference implementation in its own configuration: the blake3 crate
# over rayon, built from bench/compare/ (see its README). Off by default,
# because it needs a Rust toolchain and nothing in the library depends on
# it. One binary per compile-time cap, since the crate caps its SIMD
# backend through negative features rather than at runtime.
option(BLAKE3PP_BENCH_COMPARE
  "Build the reference implementation's rayon bench (needs cargo)" OFF)
if(BLAKE3PP_BENCH_COMPARE)
  find_program(BLAKE3PP_CARGO cargo)
  if(NOT BLAKE3PP_CARGO)
    message(FATAL_ERROR
      "blake3pp: BLAKE3PP_BENCH_COMPARE=ON needs cargo on PATH "
      "(https://rustup.rs); see bench/compare/README.md")
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(_cmp_caps default cap_avx2 cap_sse41 cap_sse2 portable)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(_cmp_caps default cap_no_neon portable)
  else()
    # Every other architecture the crate has no backend for: its default
    # build is the portable one, and both are built so the digests pair up.
    set(_cmp_caps default portable)
  endif()

  set(_cmp_out "${CMAKE_CURRENT_BINARY_DIR}/compare")
  set(_cmp_src "${CMAKE_CURRENT_SOURCE_DIR}/compare")
  set(_cmp_bins "")
  foreach(cap IN LISTS _cmp_caps)
    set(_cmp_features "")
    if(NOT cap STREQUAL "default")
      set(_cmp_features --features "${cap}")
    endif()
    set(_cmp_bin "${_cmp_out}/blake3-rayon-bench-${cap}")
    # One target directory per cap: the features change the artifact, and
    # cargo would otherwise rebuild the same path back and forth.
    add_custom_command(OUTPUT "${_cmp_bin}"
      COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${_cmp_out}/target-${cap}"
              "${BLAKE3PP_CARGO}" build --release --manifest-path
              "${_cmp_src}/Cargo.toml" ${_cmp_features}
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "${_cmp_out}/target-${cap}/release/blake3-rayon-bench" "${_cmp_bin}"
      DEPENDS "${_cmp_src}/src/main.rs" "${_cmp_src}/Cargo.toml"
              "${_cmp_src}/Cargo.lock"
      COMMENT "Building the reference rayon bench (${cap})"
      VERBATIM)
    list(APPEND _cmp_bins "${_cmp_bin}")
  endforeach()
  add_custom_target(blake3pp_bench_compare ALL DEPENDS ${_cmp_bins})
endif()
