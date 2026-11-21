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
# performs the same selection swap. See src/kernel/rotate.hpp.
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
# difference. See xor_rot in src/kernel/rotate.hpp.
blake3pp_kernel_switch(XAR_ROTATE 1
  "aarch64 SVE2: fuse xor+rotate into a single XAR instead of eor + rotate")

# riscv Zvbb variants: single-instruction vror instead of the 3-op
# shift-or (base RVV 1.0 has no vector rotate; GCC 15 does not fuse the
# pattern on its own: measured, 0 vror emitted). The XAR playbook one ISA
# over; unmeasured on real Zvbb silicon so far, which is what this switch
# is for. See xor_rot in src/kernel/rotate.hpp.
blake3pp_kernel_switch(VROR_ROTATE 1
  "riscv Zvbb: single-instruction vror rotate instead of the 3-op shift-or")

# The shuffle-tree bypass: transposes as constexpr-index shuffle networks
# and the byte-granular rotates as byte permutes, instead of the scalar
# staging gather and shift-or the std providers are left with (no permute
# API in <simd>). Off restores the pre-bypass spelling for the A/B. See
# src/kernel/shuffle.hpp.
blake3pp_kernel_switch(SHUFFLE_TREE 1
  "transposes and byte rotates through the constexpr shuffle networks instead of the scalar staging gather")

# The eight-lane transpose escape: GCC with a raw vector-extension register
# and AVX-512 available takes the staging gather there, because it lowers
# the network's cross-lane stage to an index-register permute per stage.
# Off restores the network for the A/B. See src/kernel/transpose.hpp.
blake3pp_kernel_switch(VEXT_W8_GATHER 1
  "GCC with a vector-extension register at eight lanes and AVX-512 available transposes through the staging gather, not the shuffle network")

# At full AVX-512 width the state occupies half the register file and the
# message cannot fit beside it: letting its address escape once keeps it in
# memory operands instead. Off holds it in registers again, for the A/B.
# See all_rounds in src/kernel/kernel.cpp.
blake3pp_kernel_switch(PIN_MESSAGE 1
  "AVX-512 width reads the message through memory operands instead of holding it in registers")

# x86 clang: spell rot16 as the generic shift-or, which LLVM canonicalises to
# one pshufb, where every other compiler gets that instruction from the
# explicit byte shuffle (which LLVM lowers to pshuflw+pshufhw instead). Off
# gives clang the byte shuffle like everyone else. See prefer_byte_rot in
# src/kernel/rotate.hpp.
blake3pp_kernel_switch(ROT16_PER_COMPILER 1
  "x86 clang: rot16 as the shift-or LLVM folds to one pshufb, not the byte shuffle it lowers to two")

# The widest simd_degree any kernel in THIS build may have. It sizes the
# caller-side staging buffers for hash_many batches, and with them the
# subtree sweep's (src/core/subtree.hpp): the part form's frame is 6512
# bytes at 8 where 16 needs 12784 (docs/freestanding.md), which is the
# difference between fitting an RTOS thread stack and not. The default is
# the ceiling of every variant the project can compile; a narrower target
# lowers it, and each kernel TU static_asserts its own degree against it,
# so a value too low fails the build instead of overflowing a staging
# buffer. Not derived from the registered kernels yet: an externally
# compiled kernel's command line is fixed when it registers, before the
# set is known.
set(BLAKE3PP_MAX_SIMD_DEGREE 16 CACHE STRING
  "Widest simd_degree any compiled kernel may have (sizes the subtree staging buffers)")
if(NOT BLAKE3PP_MAX_SIMD_DEGREE MATCHES "^(1|2|4|8|16)$")
  message(FATAL_ERROR
    "BLAKE3PP_MAX_SIMD_DEGREE must be 1, 2, 4, 8 or 16, got '${BLAKE3PP_MAX_SIMD_DEGREE}'")
endif()
# On features, which the kernel objects link and the library propagates, so
# every TU that includes src/kernel/kernel.hpp agrees on the value.
target_compile_definitions(blake3pp_features
  INTERFACE "BLAKE3PP_MAX_SIMD_DEGREE=${BLAKE3PP_MAX_SIMD_DEGREE}")

# The deepest queue the I/O engines and the pipeline accept
# (include/blake3pp/detail/io_geometry.hpp). A device is not what bounds it
# -- NVMe takes tens of thousands of outstanding commands -- the arena is:
# every window in flight costs window_bytes, so 256 at the default 8 MiB
# window is 2 GiB, and the measured sweet spot on every device so far is
# four to eight. The ceiling is a guard against a typo, and a caller who
# knows the device and the budget moves it.
set(BLAKE3PP_MAX_QUEUE_DEPTH "" CACHE STRING
  "Deepest I/O queue a caller may ask for [default 256]")
if(BLAKE3PP_MAX_QUEUE_DEPTH)
  if(NOT BLAKE3PP_MAX_QUEUE_DEPTH MATCHES "^[0-9]+$"
     OR BLAKE3PP_MAX_QUEUE_DEPTH LESS 2)
    message(FATAL_ERROR
      "BLAKE3PP_MAX_QUEUE_DEPTH must be an integer of at least 2, got "
      "'${BLAKE3PP_MAX_QUEUE_DEPTH}'")
  endif()
  target_compile_definitions(blake3pp_features
    INTERFACE "BLAKE3PP_MAX_QUEUE_DEPTH=${BLAKE3PP_MAX_QUEUE_DEPTH}")
endif()

# How many chunk batches a subtree reduction hashes before it compresses
# their parents (src/core/subtree.hpp). A kernel body at full width is
# large enough that the chunk loop and the one-block parent call do not
# stay hot together, so the burst is worth more than the locality it gives
# up: on Zen 5 AVX-512, 6.7 GiB/s at one batch per burst, 7.6 at eight,
# 7.8 at thirty-two, against 8.5 for the chunk batches alone. Eight is the
# default; a power of two, since the groups merge through a binary
# counter.
set(BLAKE3PP_SUBTREE_GROUP "" CACHE STRING
  "Chunk batches per parent burst in the subtree reduction [<power of two>]")
if(BLAKE3PP_SUBTREE_GROUP)
  if(NOT BLAKE3PP_SUBTREE_GROUP MATCHES "^[0-9]+$")
    message(FATAL_ERROR
      "BLAKE3PP_SUBTREE_GROUP must be a power of two, got '${BLAKE3PP_SUBTREE_GROUP}'")
  endif()
  target_compile_definitions(blake3pp_features
    INTERFACE "BLAKE3PP_SUBTREE_GROUP=${BLAKE3PP_SUBTREE_GROUP}")
  message(STATUS "blake3pp: BLAKE3PP_SUBTREE_GROUP=${BLAKE3PP_SUBTREE_GROUP} "
    "(default is 8); non-default kernel tuning")
endif()

# Opt-in: reduce a subtree one batch at a time, merging through a
# binary-counter stack, instead of the grouped sweep the library takes by
# default (both in src/core/subtree.hpp). The value is the stack bound in levels, so it is also the
# working set: 12 levels is 384 bytes plus one group's staging, and ON means
# 54, the largest subtree BLAKE3 defines. For microcontrollers: the fold
# trades lane occupancy in the parent compressions for a working set that
# does not grow with the subtree's depth, which costs 11% on sse42 and avx2
# and nothing measurable on a scalar kernel, where there are no lanes to
# leave idle.
set(BLAKE3PP_SUBTREE_FOLD OFF CACHE STRING
  "Fold subtrees through a bounded CV stack instead of per-level buffers [OFF|ON|<levels>]")
if(BLAKE3PP_SUBTREE_FOLD)
  set(_ak_fold_levels "${BLAKE3PP_SUBTREE_FOLD}")
  if(BLAKE3PP_SUBTREE_FOLD STREQUAL "ON")
    set(_ak_fold_levels 54)
  elseif(NOT BLAKE3PP_SUBTREE_FOLD MATCHES "^[0-9]+$")
    message(FATAL_ERROR
      "BLAKE3PP_SUBTREE_FOLD must be OFF, ON or a number of levels, got '${BLAKE3PP_SUBTREE_FOLD}'")
  endif()
  target_compile_definitions(blake3pp_features
    INTERFACE "BLAKE3PP_SUBTREE_FOLD=${_ak_fold_levels}")
endif()

# A kernel that pins none of the three per-TU providers compiles
# src/kernel/simd_facade.hpp's fallback arms, which is the only way the
# project-wide simd provider acquires a consumer: the facade is included
# from kernel translation units and nowhere else. Record that here so
# cmake/StdFeatures.cmake can resolve the provider of last resort after
# registration instead of before it.
function(_blake3pp_note_simd_provider_use provider)
  if(provider STREQUAL "facade")
    set_property(GLOBAL PROPERTY BLAKE3PP_KERNELS_NEED_SIMD_PROVIDER 1)
  endif()
endfunction()

function(blake3pp_add_kernel ns)
  cmake_parse_arguments(PARSE_ARGV 1 AK "FORCE_SCALAR;FORCE_XSIMD;FORCE_VEXT"
    "SOURCE;EXTERNAL_COMPILER;VEXT_BYTES" "ARCH_FLAGS")

  # Almost every variant is an instantiation of the one kernel TU; SOURCE
  # substitutes a standalone hand-written TU for the ISAs the facade
  # cannot express (xthead: sizeless 0.7.1 vector types cannot back u32v).
  # The substitute must export the same kern::<ns>::ops table.
  set(_ak_src "${PROJECT_SOURCE_DIR}/src/kernel/kernel.cpp")
  if(AK_SOURCE)
    set(_ak_src "${PROJECT_SOURCE_DIR}/${AK_SOURCE}")
  endif()

  # Registered whichever way the object is built.
  set_property(GLOBAL APPEND PROPERTY BLAKE3PP_KERNELS "${ns}")
  # What this variant was compiled with, for anything that has to reproduce
  # the kernel's own build without re-spelling it: the dispatch-cost benches
  # compile the same TU per variant and read these back.
  set_property(GLOBAL PROPERTY "BLAKE3PP_KERNEL_${ns}_FLAGS" "${AK_ARCH_FLAGS}")
  set(_ak_provider "facade")
  if(AK_FORCE_SCALAR)
    set(_ak_provider "scalar")
  elseif(AK_FORCE_VEXT)
    set(_ak_provider "vext")
  elseif(AK_FORCE_XSIMD)
    set(_ak_provider "xsimd")
  endif()
  set_property(GLOBAL PROPERTY "BLAKE3PP_KERNEL_${ns}_PROVIDER" "${_ak_provider}")
  set_property(GLOBAL PROPERTY "BLAKE3PP_KERNEL_${ns}_VEXT_BYTES" "${AK_VEXT_BYTES}")
  _blake3pp_note_simd_provider_use("${_ak_provider}")

  # EXTERNAL_COMPILER: the object is produced by a SECOND compiler and
  # linked into the same fat binary, for dialects the primary compiler
  # cannot express at all (LLVM never merged XTheadVector, so the zig/
  # musl chain borrows the distro GCC for that one TU). The recipe
  # mirrors the freestanding verifier's: no exceptions, no rtti, no
  # stack protector; the object's only external references are
  # memcpy-class symbols every libc provides, and the Itanium C++ ABI
  # makes the kern::<ns>::ops table link cleanly across compilers.
  if(AK_EXTERNAL_COMPILER)
    set(_ak_obj "${CMAKE_BINARY_DIR}/blake3pp_generated/kernel_${ns}${CMAKE_CXX_OUTPUT_EXTENSION}")
    get_property(_ak_switches GLOBAL PROPERTY BLAKE3PP_KERNEL_SWITCHES)
    set(_ak_defs "")
    foreach(d IN LISTS _ak_switches)
      list(APPEND _ak_defs "-D${d}")
    endforeach()
    # This command line predates the features target's reach.
    list(APPEND _ak_defs "-DBLAKE3PP_MAX_SIMD_DEGREE=${BLAKE3PP_MAX_SIMD_DEGREE}")
    # Facade kernels built externally need the xsimd headers and the
    # provider pin on the external command line too (the vxe kernel is
    # the customer: clang scalarizes xsimd's VXE ops, GCC emits real
    # vector code including verllf hardware rotates).
    if(AK_FORCE_XSIMD)
      # Registration runs before the project-wide provider is resolved,
      # so the kernel fetches xsimd itself, as the in-tree ones do. The
      # hermetic fetch may leave xsimd_SOURCE_DIR unset; the populated
      # source tree in the shared cache is content-addressed and equally
      # authoritative.
      _blake3pp_fetch_xsimd()
      set(_ak_xsimd_inc "")
      if(xsimd_SOURCE_DIR)
        set(_ak_xsimd_inc "${xsimd_SOURCE_DIR}/include")
      else()
        file(GLOB _ak_xsimd_candidates
             "${PROJECT_SOURCE_DIR}/thirdparty/cache/xsimd-*-src/include")
        list(GET _ak_xsimd_candidates 0 _ak_xsimd_inc)
      endif()
      if(NOT _ak_xsimd_inc OR NOT EXISTS "${_ak_xsimd_inc}")
        message(FATAL_ERROR "external kernel ${ns}: FORCE_XSIMD but no xsimd headers found (populate xsimd first)")
      endif()
      # xsimd is third-party: its headers take -isystem so the project's
      # warning set, which the external compile carries, does not apply to
      # them.
      list(APPEND _ak_defs "-DBLAKE3PP_HAS_XSIMD=1" "-DBLAKE3PP_FORCE_XSIMD=1"
           "-isystem" "${_ak_xsimd_inc}")
    endif()
    # The warning set the primary compiler gets, spelled for GCC, which is
    # the compiler that builds this object. The clang-only checks of
    # blake3pp_warnings are left out: GCC does not know them.
    set(_ak_warn -Wall -Wextra -Wconversion -Wshadow -Wsign-conversion)
    if(BLAKE3PP_WARNINGS_AS_ERRORS)
      list(APPEND _ak_warn -Werror)
    endif()
    add_custom_command(OUTPUT "${_ak_obj}"
      COMMAND "${AK_EXTERNAL_COMPILER}" -std=c++23 -O3 -fPIC
              -fno-exceptions -fno-rtti -fno-stack-protector ${_ak_warn}
              ${AK_ARCH_FLAGS} ${_ak_defs} "-DBLAKE3PP_ARCH_NS=${ns}"
              "-I${PROJECT_SOURCE_DIR}/src" "-I${PROJECT_SOURCE_DIR}/include"
              -MD -MF "${_ak_obj}.d"
              -c "${_ak_src}" -o "${_ak_obj}"
      # The compiler's own list of what the object read, so an edit to any
      # header under src/kernel (or dispatch.hpp, arch.def, xsimd) rebuilds
      # it; DEPENDS alone would name only what is spelled here.
      DEPFILE "${_ak_obj}.d"
      DEPENDS "${_ak_src}" "${PROJECT_SOURCE_DIR}/src/kernel/kernel.hpp"
      COMMENT "kernel ${ns} via external compiler: ${AK_EXTERNAL_COMPILER}"
      VERBATIM)
    add_custom_target(blake3pp_kernel_${ns}_external DEPENDS "${_ak_obj}")
    set_property(GLOBAL PROPERTY "BLAKE3PP_KERNEL_${ns}_OBJECT" "${_ak_obj}")
    return()
  endif()

  set(tgt "blake3pp_kernel_${ns}")
  add_library(${tgt} OBJECT "${_ak_src}")
  target_compile_definitions(${tgt} PRIVATE "BLAKE3PP_ARCH_NS=${ns}")
  target_link_libraries(${tgt} PRIVATE blake3pp_warnings)
  # cl reports the code after an `if constexpr` branch that returns as
  # unreachable (C4702) in every instantiation that takes the branch.
  target_compile_options(${tgt} PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/wd4702>")
  # The resolved tuning switches, passed explicitly rather than left to the
  # headers' fallback defaults.
  get_property(_ak_switches GLOBAL PROPERTY BLAKE3PP_KERNEL_SWITCHES)
  target_compile_definitions(${tgt} PRIVATE ${_ak_switches})
  if(AK_FORCE_SCALAR)
    # The scalar fallback must be genuinely scalar: without this, the simd
    # facade would still pick the baseline vector width (SSE2 on x86-64).
    target_compile_definitions(${tgt} PRIVATE "BLAKE3PP_FORCE_SCALAR=1")
  endif()
  if(AK_FORCE_VEXT)
    # Pin this TU to the vector-extension provider at a width the caller
    # names (see BLAKE3PP_FORCE_VEXT in src/kernel/simd_facade.hpp). For
    # ISAs no provider covers: the std providers deduce width 1 on a
    # target their ISA list does not know, and xsimd has no backend.
    if(NOT AK_VEXT_BYTES)
      message(FATAL_ERROR "kernel ${ns}: FORCE_VEXT needs VEXT_BYTES")
    endif()
    target_compile_definitions(${tgt} PRIVATE
      "BLAKE3PP_FORCE_VEXT=1" "BLAKE3PP_VEXT_BYTES=${AK_VEXT_BYTES}")
  endif()
  if(AK_FORCE_XSIMD)
    # Pin this TU to the xsimd provider regardless of the project-wide
    # selection (see BLAKE3PP_FORCE_XSIMD in src/kernel/simd_facade.hpp;
    # the SVE variants need it to stay free of load-time SVE code). The
    # project-wide choice may be one that needs no xsimd at all -- std, or
    # xvec, which skips the fetch -- so a pinned variant asks for it here.
    _blake3pp_fetch_xsimd()
    target_compile_definitions(${tgt} PRIVATE "BLAKE3PP_FORCE_XSIMD=1")
    target_link_libraries(${tgt} PRIVATE xsimd)
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
  # Per-kernel ARCH_FLAGS come AFTER the shared optimization flags so a
  # kernel can override them; last flag wins on every driver here, which
  # makes ARCH_FLAGS a true escape hatch (e.g. a per-kernel /Ob2 beating
  # the /Ob3 above).
  if(AK_ARCH_FLAGS)
    target_compile_options(${tgt} PRIVATE ${AK_ARCH_FLAGS})
  endif()
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
    get_property(_ext_obj GLOBAL PROPERTY "BLAKE3PP_KERNEL_${ns}_OBJECT")
    if(_ext_obj)
      # Externally-compiled kernel: the raw object is the source (CMake
      # treats *.o sources as EXTERNAL_OBJECT), plus a dependency on the
      # custom target that produces it.
      target_sources(${target} PRIVATE "${_ext_obj}")
      add_dependencies(${target} "blake3pp_kernel_${ns}_external")
    else()
      target_sources(${target} PRIVATE "$<TARGET_OBJECTS:blake3pp_kernel_${ns}>")
    endif()
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
