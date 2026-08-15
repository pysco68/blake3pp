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

function(blake3pp_add_kernel ns)
  cmake_parse_arguments(PARSE_ARGV 1 AK "FORCE_SCALAR" "" "ARCH_FLAGS")

  set(tgt "blake3pp_kernel_${ns}")
  add_library(${tgt} OBJECT "${PROJECT_SOURCE_DIR}/src/kernel/kernel.cpp")
  target_compile_definitions(${tgt} PRIVATE "BLAKE3PP_ARCH_NS=${ns}")
  if(AK_FORCE_SCALAR)
    # The scalar fallback must be genuinely scalar: without this, the simd
    # facade would still pick the baseline vector width (SSE2 on x86-64).
    target_compile_definitions(${tgt} PRIVATE "BLAKE3PP_FORCE_SCALAR=1")
  endif()
  if(AK_ARCH_FLAGS)
    target_compile_options(${tgt} PRIVATE ${AK_ARCH_FLAGS})
  endif()
  # The kernels are the hot loop and measurably faster at -O3 (GCC's
  # std::simd path is ~2x slower at -O2). Applied only to optimized configs
  # so Debug/sanitizer builds keep their debuggability; appended after the
  # config flags, so it wins over RelWithDebInfo's -O2.
  target_compile_options(${tgt} PRIVATE
    "$<$<CONFIG:Release,RelWithDebInfo>:-O3>")
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

# Attaches every registered kernel's objects to <target> and defines
# BLAKE3PP_HAS_KERNEL_<NS> so kernel.hpp / dispatch.cpp know which variant
# namespaces exist in this build.
function(blake3pp_target_link_kernels target)
  get_property(kernels GLOBAL PROPERTY BLAKE3PP_KERNELS)
  foreach(ns IN LISTS kernels)
    target_sources(${target} PRIVATE "$<TARGET_OBJECTS:blake3pp_kernel_${ns}>")
    string(TOUPPER "${ns}" ns_upper)
    target_compile_definitions(${target} PRIVATE "BLAKE3PP_HAS_KERNEL_${ns_upper}=1")
  endforeach()
endfunction()
