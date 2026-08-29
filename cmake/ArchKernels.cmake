# Compiles src/kernel/kernel.cpp once per architecture variant.
#
# Every variant is an OBJECT library with its own -m flags and its own
# namespace, injected as -DBLAKE3PP_ARCH_NS=<name>. Distinct namespaces mean
# distinct mangled symbols, so LTO can never fold an AVX2-compiled function
# into the scalar fallback path (or vice versa); the ODR is respected by
# construction instead of by luck. IPO is additionally forced off on kernel
# objects so no cross-TU inlining can leak native host instructions out of a
# variant's translation unit.
#
# SIMD types must never appear in a kernel's exported signatures: their ABI
# changes with the -m flags of the TU. The boundary API in
# src/kernel/kernel.hpp is flat pointers only.

include_guard(GLOBAL)

# --- Kernel tuning switches -------------------------------------------------
#
# Each switch below turns a MEASURED optimization on or off in the kernel
# TUs. They exist so a result can be re-measured on hardware the original
# measurement did not cover: every one of them defends a number, and the
# number came from a machine somebody had. They are NOT performance options:
# `auto` is the fastest setting everywhere it has been measured, and turning
# one off makes the library slower (in one case by 40%).
#
# Spelling is deliberate. Each is a tri-state auto/on/off, matching
# BLAKE3PP_EXECUTION_PROVIDER, so that (a) the default is a value this file
# states out loud rather than the absence of a #define, (b) a switch can be
# forced ON as well as off (a negative-only opt-out cannot test a target
# where the default is off), and (c) the resolved value is passed explicitly
# to every kernel TU and printed at configure time, so what got built is in
# the build log instead of inferred from a header's #if.
#
# Adding one: call blake3pp_kernel_switch(), then read the macro in the
# kernel sources as `#if BLAKE3PP_KERNEL_<NAME>`, with an #ifndef fallback
# carrying the same default (the headers are also parsed by tooling that
# does not pass our flags).
function(blake3pp_kernel_switch name default doc)
  set(var "BLAKE3PP_KERNEL_${name}")
  set(${var} "auto" CACHE STRING "${doc} [auto|on|off]")
  set_property(CACHE ${var} PROPERTY STRINGS auto on off)
  if(${var} STREQUAL "auto")
    set(resolved "${default}")
  elseif(${var} STREQUAL "on")
    set(resolved 1)
  elseif(${var} STREQUAL "off")
    set(resolved 0)
  else()
    message(FATAL_ERROR "blake3pp: ${var} must be auto, on or off "
      "(got '${${var}}')")
  endif()
  set_property(GLOBAL APPEND PROPERTY BLAKE3PP_KERNEL_SWITCHES
    "${var}=${resolved}")
  if(NOT ${var} STREQUAL "auto")
    message(STATUS "blake3pp: ${var}=${${var}} (kernel default is "
      "${default}); non-default kernel tuning")
  endif()
endfunction()

# Inlining enforcement for the kernel's call chains. Off costs 6-40%
# depending on compiler and variant: every compiler measured outlines the
# round core or its index_sequence lambda without it. See
# src/kernel/force_inline.hpp.
blake3pp_kernel_switch(INLINE_ENFORCEMENT 1
  "Force-inline the kernel's round core (measured 6-40% faster)")

# aarch64: combine rot12/rot7 with shl+sri rather than the shl+usra the
# generic shift-or selects. Measured +6% on Apple M2 / clang 22; GCC 15
# performs the same selection swap. See src/kernel/transpose.hpp.
blake3pp_kernel_switch(SRI_ROTATE 1
  "aarch64: use shl+sri for rot12/rot7 instead of the generic shift-or")

# aarch64: run each round quartet-staged instead of one g at a time. A small
# measured win on Apple M2 / clang 22; provably inert on GCC 15 (same
# schedule, different register names). Loses on x86, which is why the gate
# is architectural. See src/kernel/kernel.cpp.
blake3pp_kernel_switch(STAGED_ROUNDS 1
  "aarch64: quartet-staged round ordering instead of sequential g")

# aarch64 SVE2 fixed-length variants: fuse every rot<N>(x ^ y) in the round
# into one XAR instruction instead of eor+tbl / eor+shl+sri, the reason an
# SVE2 kernel beats NEON at the same 128-bit width. Measured on Neoverse V2
# (GCP Axion): 1.89 vs 1.61 GiB/s single-thread, off-vs-on the whole
# difference. See xor_rot in src/kernel/transpose.hpp.
blake3pp_kernel_switch(XAR_ROTATE 1
  "aarch64 SVE2: fuse xor+rotate into a single XAR instead of eor + rotate")

function(blake3pp_add_kernel ns)
  cmake_parse_arguments(PARSE_ARGV 1 AK "FORCE_SCALAR;FORCE_XSIMD" "SOURCE"
    "ARCH_FLAGS")

  # Almost every variant is an instantiation of the one kernel TU; SOURCE
  # substitutes a standalone hand-written TU for the ISAs the facade
  # cannot express (xthead: sizeless 0.7.1 vector types cannot back u32v).
  # The substitute must export the same kern::<ns>::ops table.
  set(_ak_src "${PROJECT_SOURCE_DIR}/src/kernel/kernel.cpp")
  if(AK_SOURCE)
    set(_ak_src "${PROJECT_SOURCE_DIR}/${AK_SOURCE}")
  endif()

  set(tgt "blake3pp_kernel_${ns}")
  add_library(${tgt} OBJECT "${_ak_src}")
  target_compile_definitions(${tgt} PRIVATE "BLAKE3PP_ARCH_NS=${ns}")
  # The resolved tuning switches, passed explicitly rather than left to the
  # headers' fallback defaults.
  get_property(_ak_switches GLOBAL PROPERTY BLAKE3PP_KERNEL_SWITCHES)
  target_compile_definitions(${tgt} PRIVATE ${_ak_switches})
  if(AK_FORCE_SCALAR)
    # The scalar fallback must be genuinely scalar: without this, the simd
    # facade would still pick the baseline vector width (SSE2 on x86-64).
    target_compile_definitions(${tgt} PRIVATE "BLAKE3PP_FORCE_SCALAR=1")
  endif()
  if(AK_FORCE_XSIMD)
    # Pin this TU to the xsimd provider regardless of the project-wide
    # selection (see BLAKE3PP_FORCE_XSIMD in src/kernel/simd_facade.hpp;
    # the SVE variants need it to stay free of load-time SVE code). The
    # caller must have made the xsimd target available first
    # (_blake3pp_fetch_xsimd in cmake/StdFeatures.cmake).
    target_compile_definitions(${tgt} PRIVATE "BLAKE3PP_FORCE_XSIMD=1")
    target_link_libraries(${tgt} PRIVATE xsimd)
  endif()
  if(AK_ARCH_FLAGS)
    target_compile_options(${tgt} PRIVATE ${AK_ARCH_FLAGS})
  endif()
  # The kernels are the hot loop and measurably faster at -O3 (GCC's
  # std::simd path is ~2x slower at -O2). Applied only to optimized configs
  # so Debug/sanitizer builds keep their debuggability; appended after the
  # config flags, so it wins over RelWithDebInfo's -O2. MSVC-frontend
  # spelling: cl has no /O3 (Release's /O2 is already its ceiling);
  # clang-cl takes the flag through its /clang: passthrough.
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      target_compile_options(${tgt} PRIVATE
        "$<$<CONFIG:Release,RelWithDebInfo>:/clang:-O3>")
    else()
      # cl's default inlining budget (/Ob2) gives up on the facade's
      # template chains (measured: the sse42 kernel came out as a call
      # soup with 12 vector adds total). /Ob3 raises the budget.
      target_compile_options(${tgt} PRIVATE
        "$<$<CONFIG:Release,RelWithDebInfo>:/Ob3>")
    endif()
  else()
    target_compile_options(${tgt} PRIVATE
      "$<$<CONFIG:Release,RelWithDebInfo>:-O3>")
  endif()
  # GCC's post-RA scheduler measurably hurts this register-saturated kernel
  # (+6% from disabling it, 3/3 paired runs on znver3): with ~32 live vector
  # values on 16 registers, its static ILP-driven reordering only disturbs
  # the dataflow order the out-of-order core exploits natively. Clang is the
  # opposite: disabling its (pressure-aware) MachineScheduler costs ~10%,
  # so it stays on. Measured, not assumed.
  target_compile_options(${tgt} PRIVATE
    "$<$<CXX_COMPILER_ID:GNU>:-fno-schedule-insns2>")
  # Experimentation hook: extra flags for kernel TUs only (scheduler knobs,
  # tuning trials). Semicolon-separated list; empty by default.
  if(BLAKE3PP_KERNEL_EXTRA_FLAGS)
    target_compile_options(${tgt} PRIVATE ${BLAKE3PP_KERNEL_EXTRA_FLAGS})
  endif()
  target_include_directories(${tgt} PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/include")
  target_compile_features(${tgt} PRIVATE cxx_std_20)
  target_link_libraries(${tgt} PRIVATE blake3pp::features)
  set_target_properties(${tgt} PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION OFF
    POSITION_INDEPENDENT_CODE ON)

  set_property(GLOBAL APPEND PROPERTY BLAKE3PP_KERNELS "${ns}")
endfunction()

# Attaches every registered kernel's objects to <target> and generates the
# kernel registry, an X-macro list of exactly the variants in THIS binary.
# dispatch.cpp expands it into extern declarations and its dispatch/query
# tables, so the blake3pp_add_kernel() calls are the single source of
# truth: registering a variant here is all it takes for it to appear in
# dispatch, compiled_arches() and available_arches().
function(blake3pp_target_link_kernels target)
  get_property(kernels GLOBAL PROPERTY BLAKE3PP_KERNELS)
  set(registry "// GENERATED by cmake/ArchKernels.cmake -- do not edit.\n")
  foreach(ns IN LISTS kernels)
    target_sources(${target} PRIVATE "$<TARGET_OBJECTS:blake3pp_kernel_${ns}>")
    string(APPEND registry "BLAKE3PP_KERNEL(${ns})\n")
  endforeach()
  # file(CONFIGURE) only rewrites on content change, so dispatch.cpp is not
  # recompiled by every reconfigure.
  file(CONFIGURE
    OUTPUT "${CMAKE_BINARY_DIR}/blake3pp_generated/blake3pp_kernel_registry.inc"
    CONTENT "${registry}" @ONLY)
  target_include_directories(${target} PRIVATE
    "${CMAKE_BINARY_DIR}/blake3pp_generated")
endfunction()
