# The official BLAKE3 C library (with its hand-written assembly kernels) is
# built alongside the bench as the performance baseline, so every run shows
# our variants against the hand-tuned ceiling on the same machine. Its
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
endif()

# The sized parallel scheduler is one definition per program; the benches
# take it when the build offers one (see the root CMakeLists).
if(TARGET blake3pp_parallel_backend)
  target_link_libraries(blake3pp_bench PRIVATE blake3pp::parallel_backend)
  if(TARGET blake3pp_bench_file)
    target_link_libraries(blake3pp_bench_file PRIVATE blake3pp::parallel_backend)
  endif()
endif()

# Smoke: the benches are shipped tools, so one tiny run each proves they
# start, find their kernels and print; it also runs the shared tooling
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
# wall clock. The kernel flags mirror blake3pp_add_kernel()'s.
function(_blake3pp_dispatch_cost ns)
  set(tgt blake3pp_dispatch_cost_${ns})
  add_executable(${tgt} dispatch_cost.cpp)
  get_property(_dc_switches GLOBAL PROPERTY BLAKE3PP_KERNEL_SWITCHES)
  target_compile_definitions(${tgt} PRIVATE "BLAKE3PP_ARCH_NS=${ns}" ${_dc_switches})
  if(ns STREQUAL "scalar")
    # The same pin blake3pp_add_kernel(scalar FORCE_SCALAR) applies.
    target_compile_definitions(${tgt} PRIVATE BLAKE3PP_FORCE_SCALAR=1)
  elseif(ns MATCHES "^rvv")
    # The RVV kernels are xsimd-provider kernels, as in KernelVariants.
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
  target_compile_options(${tgt} PRIVATE ${ARGN} "$<$<CONFIG:Release,RelWithDebInfo>:-O3>"
    "$<$<CXX_COMPILER_ID:GNU>:-fno-devirtualize;-fno-ipa-cp-clone>")
  target_include_directories(${tgt} PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
  target_link_libraries(${tgt} PRIVATE blake3pp::features)
  set_target_properties(${tgt} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
endfunction()
# Off by default: four extra kernel compilations that only the dispatch
# question needs.
option(BLAKE3PP_BENCH_DISPATCH_COST
  "Build the dispatch-cost benches (one kernel compilation per variant)" OFF)
if(BLAKE3PP_BENCH_DISPATCH_COST
   AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" AND NOT EMSCRIPTEN)
  # The detection ladder's cost: the first call of a fresh process.
  add_executable(blake3pp_detect_cost detect_cost.cpp)
  target_link_libraries(blake3pp_detect_cost PRIVATE blake3pp::blake3pp blake3pp_warnings)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    _blake3pp_dispatch_cost(scalar)
    _blake3pp_dispatch_cost(sse42 -msse4.2)
    _blake3pp_dispatch_cost(avx2 -mavx2)
    _blake3pp_dispatch_cost(avx512 -mavx512f -mavx512cd -mavx512vl -mavx512bw -mavx512dq)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    _blake3pp_dispatch_cost(scalar)
    _blake3pp_dispatch_cost(neon)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(riscv64)$")
    _blake3pp_dispatch_cost(scalar)
    # One RVV width is enough for the call-site question; the two flag
    # spellings are KernelVariants' (its probe result is scoped to its
    # own function, so the probe is repeated here).
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=rv64gcv_zvl256b -mrvv-vector-bits=zvl" BLAKE3PP_DC_RVV_MARCH)
    if(BLAKE3PP_DC_RVV_MARCH)
      _blake3pp_dispatch_cost(rvv256 -march=rv64gcv_zvl256b -mrvv-vector-bits=zvl)
    else()
      # zig's clang wants the cpu+features spelling and a numeric width.
      _blake3pp_dispatch_cost(rvv256 -mcpu=baseline_rv64+v+zvl256b -mrvv-vector-bits=256)
    endif()
  endif()
endif()
