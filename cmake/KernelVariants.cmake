# Registers the architecture kernel variants: the build-side mirror of the
# src/dispatch/cpu_detect_*.cpp split: one function per ISA family holding
# that family's compiler probes, flag lore and blake3pp_add_kernel() calls,
# dispatched on the target processor at the bottom. Adding a variant to an
# existing family happens here (plus its arch.def line and its case in the
# family's cpu_detect file); a new family adds a function and a branch.
#
# Every registered variant compiles src/kernel/kernel.cpp (or a SOURCE
# substitute) into its own OBJECT library under its own namespace and -m
# flags (see cmake/ArchKernels.cmake); all of them land in the one binary
# and runtime dispatch picks among them.

include_guard(GLOBAL)

include(CheckCXXSourceCompiles)

function(_blake3pp_register_x86_kernels)
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # Pure cl.exe: /arch is the only ISA dial. /arch:SSE4.2 DOES exist
    # (VS 17.10+), but on its own it changes nothing here: it defines no
    # feature macro, and xsimd detects capability ONLY from the GNU
    # spellings (#ifdef __SSSE3__ ...). Without the -D's below xsimd
    # resolves best_arch = sse2, shuffle_backend::has_byte_shuffle is
    # false, and this "sse42" kernel is a mislabelled SSE2 one with the
    # byte-rotate gate shut -- measured: 0 pshufb, every rotate spelled
    # psrld+pslld+orps. With them the kernel gains the same byte-rotate
    # path every other target has (571 pshufb, shift-or triples halved to
    # just rot12/rot7, 23407 -> 21493 instructions). Safe by
    # construction: dispatch only selects this variant when CPUID reports
    # SSE4.2. xsimd back-fills SSE4_2 -> SSE4_1 -> SSSE3 -> SSE3, so
    # __SSE4_2__ alone would do; all three are spelled out so this does
    # not depend on xsimd keeping that cascade.
    # /w14883 surfaces C4883 (optimizer size-budget bailout, silently
    # emitting /Od-class code) at the default warning level; it produced a
    # 250x cliff on the arm64 twin before it was made visible.
    blake3pp_add_kernel(sse42 ARCH_FLAGS /arch:SSE4.2 /w14883
      -D__SSSE3__ -D__SSE4_1__ -D__SSE4_2__)
    blake3pp_add_kernel(avx2 ARCH_FLAGS /arch:AVX2 /w14883)
    blake3pp_add_kernel(avx512 ARCH_FLAGS /arch:AVX512 /w14883)
  else()
    blake3pp_add_kernel(sse42 ARCH_FLAGS -msse4.2)
    blake3pp_add_kernel(avx2 ARCH_FLAGS -mavx2)
    # The canonical F->CD->DQ->BW progression plus VL: xsimd's arch
    # hierarchy requires CD once DQ is on, and every physical AVX-512 CPU
    # has all five.
    blake3pp_add_kernel(avx512 ARCH_FLAGS -mavx512f -mavx512cd -mavx512vl -mavx512bw -mavx512dq)
  endif()
endfunction()

function(_blake3pp_register_aarch64_kernels)
  # NEON is baseline on AArch64, except under pure cl.exe, which is
  # scalar-only BY VERDICT, not by inability. The full story: xsimd 14.3
  # compiles on
  # MSVC-arm64 and validates byte-identical, but cl's optimizer
  # size-budget bailout (C4883, silent, off by default) emitted
  # /Od-class code: <0.005 GiB/s on Cobalt 100, ~100x BELOW scalar,
  # with auto-dispatch preferring it. The fix exists and was verified
  # (/d2OptimizeHugeFunctions: 2.24M -> 29.4k instructions, 14.8MB ->
  # 194KB object), but the economics killed it: 15-20 MINUTES of
  # compile for this one TU (OOM-ing 16GB runners at the default /Ob3,
  # C1002) versus clang-cl producing a faster kernel (1.29 GiB/s) in
  # seconds. Anyone wanting fast hashing on arm64 Windows should use
  # the clang-cl build; the cl build ships correct scalar (0.52).
  # If cl's optimizer economics improve, the recipe that worked is:
  # ARCH_FLAGS /d2OptimizeHugeFunctions /we4883 /Ob2.
  if(NOT (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"))
    blake3pp_add_kernel(neon)
  endif()
  # Fixed-length SVE variants: one kernel per vector length, because VLS
  # code is only valid when the runtime VL EQUALS the compiled one (GCC
  # and Arm document exact-match only), and dispatch checks precisely that.
  # sve256/sve512 are SVE1 so Graviton3/Neoverse-V1-class parts qualify;
  # sve2_128 is the Grace/Graviton4-class variant and carries the XAR
  # fused rotate. GNU-frontend compilers only (MSVC has no
  # -msve-vector-bits); the probe keeps unusual cross setups honest: no
  # probe pass, no kernel registered.
  option(BLAKE3PP_SVE_ALL_VARIANTS
    "Also compile the SVE variants matching no shipping silicon (sve128, sve2_256, sve2_512), for emulator targets"
    OFF)
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    return()
  endif()
  # The exact constructs the kernel relies on: a fixed-length SVE type,
  # bit_cast to/from a GNU vector of the same size (the shuffle backend's
  # currency), and (under SVE2) the XAR intrinsic.
  set(_blake3pp_sve_smoke [=[
    #include <arm_sve.h>
    #include <bit>
    typedef svuint32_t fixed_u32
        __attribute__((arm_sve_vector_bits(__ARM_FEATURE_SVE_BITS)));
    typedef unsigned gnu_vec
        __attribute__((vector_size(sizeof(fixed_u32))));
    int main() {
      fixed_u32 z = svdup_u32(1u);
    #if defined(__ARM_FEATURE_SVE2)
      z = svxar_n_u32(z, z, 7);
    #endif
      gnu_vec g = std::bit_cast<gnu_vec>(z);
      fixed_u32 back = std::bit_cast<fixed_u32>(g);
      return svaddv_u32(svptrue_b32(), back) != 0u ? 0 : 1;
    }
  ]=])
  # Two candidate spellings per generation: GCC/Clang take the -march
  # form; zig cc rejects aarch64 -march outright (its flag model wants
  # -mcpu=<cpu>+<features>), and both clang and zig accept the -mcpu
  # form. First candidate that passes the smoke test wins.
  function(_blake3pp_probe_sve_flags out_var probe_name)
    set(result "")
    foreach(cand IN LISTS ARGN)
      string(MAKE_C_IDENTIFIER "${probe_name}_${cand}" var)
      set(CMAKE_REQUIRED_FLAGS "${cand} -msve-vector-bits=256")
      check_cxx_source_compiles("${_blake3pp_sve_smoke}" ${var})
      if(${var})
        set(result "${cand}")
        break()
      endif()
    endforeach()
    set(${out_var} "${result}" PARENT_SCOPE)
  endfunction()
  _blake3pp_probe_sve_flags(_blake3pp_sve1_flag BLAKE3PP_COMPILER_SVE_VLS
    "-march=armv8.2-a+sve" "-mcpu=generic+sve")
  _blake3pp_probe_sve_flags(_blake3pp_sve2_flag BLAKE3PP_COMPILER_SVE2_VLS
    "-march=armv8.5-a+sve2" "-mcpu=generic+sve2")
  if(_blake3pp_sve1_flag OR _blake3pp_sve2_flag)
    # The SVE kernels pin themselves to xsimd (FORCE_XSIMD) even when the
    # project provider is a std one: libstdc++'s experimental::simd SVE
    # backend emits guarded dynamic initializers containing SVE
    # instructions, which run at LOAD TIME in every including TU: an
    # instant SIGILL on non-SVE hardware, defeating the fat binary.
    _blake3pp_fetch_xsimd()
  endif()
  if(_blake3pp_sve1_flag)
    blake3pp_add_kernel(sve256 FORCE_XSIMD
      ARCH_FLAGS ${_blake3pp_sve1_flag} -msve-vector-bits=256)
    blake3pp_add_kernel(sve512 FORCE_XSIMD
      ARCH_FLAGS ${_blake3pp_sve1_flag} -msve-vector-bits=512)
    if(BLAKE3PP_SVE_ALL_VARIANTS)
      blake3pp_add_kernel(sve128 FORCE_XSIMD
        ARCH_FLAGS ${_blake3pp_sve1_flag} -msve-vector-bits=128)
    endif()
  endif()
  if(_blake3pp_sve2_flag)
    blake3pp_add_kernel(sve2_128 FORCE_XSIMD
      ARCH_FLAGS ${_blake3pp_sve2_flag} -msve-vector-bits=128)
    if(BLAKE3PP_SVE_ALL_VARIANTS)
      blake3pp_add_kernel(sve2_256 FORCE_XSIMD
        ARCH_FLAGS ${_blake3pp_sve2_flag} -msve-vector-bits=256)
      blake3pp_add_kernel(sve2_512 FORCE_XSIMD
        ARCH_FLAGS ${_blake3pp_sve2_flag} -msve-vector-bits=512)
    endif()
  endif()
endfunction()

function(_blake3pp_register_riscv64_kernels)
  # Fixed-VLEN RVV 1.0 variants: -mrvv-vector-bits=zvl pins vscale (min
  # AND max) to the -march zvl bound, so, exactly like SVE, each VLEN is
  # its own kernel and dispatch exact-matches the runtime vlenb. The base
  # build stays rv64gc (pinned in the toolchain file; the distro compiler
  # defaults to the RVA23 baseline, which would plant ungated vector code
  # in every TU); only these TUs speak vector. FORCE_XSIMD because no std
  # simd provider has an RVV ABI at all.
  # The constructs the kernel relies on: a fixed-vlen RVV type, bit_cast
  # to/from a same-size GNU vector (the shuffle backend's currency), and
  # a basic intrinsic round-trip.
  set(_blake3pp_rvv_smoke [=[
    #include <riscv_vector.h>
    #include <bit>
    #ifndef __riscv_v_fixed_vlen
    #error "-mrvv-vector-bits=zvl did not fix the vlen"
    #endif
    typedef vuint32m1_t fixed_u32
        __attribute__((riscv_rvv_vector_bits(__riscv_v_fixed_vlen)));
    typedef unsigned gnu_vec
        __attribute__((vector_size(__riscv_v_fixed_vlen / 8)));
    int main() {
      fixed_u32 z = __riscv_vmv_v_x_u32m1(1u, __riscv_v_fixed_vlen / 32);
      gnu_vec g = std::bit_cast<gnu_vec>(z);
      fixed_u32 back = std::bit_cast<fixed_u32>(g);
      return __riscv_vmv_x_s_u32m1_u32(back) == 1u ? 0 : 1;
    }
  ]=])
  set(_blake3pp_rvv_zvbb_smoke [=[
    #include <riscv_vector.h>
    typedef vuint32m1_t fixed_u32
        __attribute__((riscv_rvv_vector_bits(__riscv_v_fixed_vlen)));
    int main() {
      fixed_u32 z =
          __riscv_vmv_v_x_u32m1(0x80000001u, __riscv_v_fixed_vlen / 32);
      fixed_u32 r = __riscv_vror_vx_u32m1(z, 7, __riscv_v_fixed_vlen / 32);
      return __riscv_vmv_x_s_u32m1_u32(r) != 0u ? 0 : 1;
    }
  ]=])
  # Two candidate flag PATTERNS per extension set, @VLEN@ expanded per
  # variant: GCC and native clang take -march with the zvl-derived fixed
  # vlen; zig cc rejects riscv -march outright (the same objection as its
  # aarch64 one: its flag model wants -mcpu=<cpu>+<features>) and its
  # clang wants the numeric -mrvv-vector-bits. The zig base cpu must be
  # baseline_rv64 (IMAFDC), NOT generic_rv64: -mcpu REPLACES the feature
  # set wholesale, and generic_rv64 is bare RV64I, so musl's atomics fail
  # to assemble the moment anything links. First pattern whose 256-bit
  # expansion passes the smoke test wins.
  function(_blake3pp_probe_rvv_flags out_var probe_name smoke)
    set(result "")
    foreach(cand IN LISTS ARGN)
      string(REPLACE "@VLEN@" "256" trial "${cand}")
      string(MAKE_C_IDENTIFIER "${probe_name}_${cand}" var)
      set(CMAKE_REQUIRED_FLAGS "${trial}")
      check_cxx_source_compiles("${smoke}" ${var})
      if(${var})
        set(result "${cand}")
        break()
      endif()
    endforeach()
    set(${out_var} "${result}" PARENT_SCOPE)
  endfunction()
  _blake3pp_probe_rvv_flags(_blake3pp_rvv_pattern
    BLAKE3PP_COMPILER_RVV_FIXED_VLEN "${_blake3pp_rvv_smoke}"
    "-march=rv64gcv_zvl@VLEN@b -mrvv-vector-bits=zvl"
    "-mcpu=baseline_rv64+v+zvl@VLEN@b -mrvv-vector-bits=@VLEN@")
  if(_blake3pp_rvv_pattern)
    _blake3pp_fetch_xsimd()
    foreach(vlen IN ITEMS 128 256 512)
      string(REPLACE "@VLEN@" "${vlen}" _flags "${_blake3pp_rvv_pattern}")
      separate_arguments(_flags UNIX_COMMAND "${_flags}")
      blake3pp_add_kernel(rvv${vlen} FORCE_XSIMD ARCH_FLAGS ${_flags})
    endforeach()
    # The Zvbb twins: same kernels, single-instruction vror rotates via
    # xor_rot's Zvbb branch (GCC does not fuse the generic pattern on its
    # own). Zvbb-less V hardware exists, hence separate variants.
    _blake3pp_probe_rvv_flags(_blake3pp_rvv_zvbb_pattern
      BLAKE3PP_COMPILER_RVV_ZVBB "${_blake3pp_rvv_zvbb_smoke}"
      "-march=rv64gcv_zvbb_zvl@VLEN@b -mrvv-vector-bits=zvl"
      "-mcpu=baseline_rv64+v+zvbb+zvl@VLEN@b -mrvv-vector-bits=@VLEN@")
    if(_blake3pp_rvv_zvbb_pattern)
      foreach(vlen IN ITEMS 128 256 512)
        string(REPLACE "@VLEN@" "${vlen}" _flags
          "${_blake3pp_rvv_zvbb_pattern}")
        separate_arguments(_flags UNIX_COMMAND "${_flags}")
        blake3pp_add_kernel(rvv${vlen}_zvbb FORCE_XSIMD ARCH_FLAGS ${_flags})
      endforeach()
    endif()
  endif()
  # T-Head XTheadVector (draft RVV 0.7.1): a hand-written standalone TU
  # (see src/kernel/xthead_kernel.cpp for why the facade cannot express
  # it). Opt-in: real 0.7.1 silicon exists (D1, SG2042, TH1520) but
  # mainline qemu cannot execute the th. encodings, so the default build
  # carries only what its test matrix can run.
  option(BLAKE3PP_XTHEAD_KERNEL
    "Compile the hand-written T-Head XTheadVector (RVV 0.7.1) kernel" OFF)
  if(BLAKE3PP_XTHEAD_KERNEL)
    set(CMAKE_REQUIRED_FLAGS "-march=rv64gc_xtheadvector")
    # Compile-only probe: linking a full exe would trip binutils'
    # xtheadvector-vs-v attribute-merge refusal against the distro crt
    # objects, the very conflict the -mno-riscv-attribute pair below
    # solves for the real kernel object.
    set(_blake3pp_saved_tct "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    check_cxx_source_compiles([=[
      #include <riscv_th_vector.h>
      int main() {
        unsigned buf[4] = {1, 2, 3, 4};
        vuint32m1_t a = __riscv_th_vlwu_v_u32m1(buf, 4);
        a = __riscv_vadd_vv_u32m1(a, a, 4);
        __riscv_th_vsw_v_u32m1(buf, a, 4);
        return static_cast<int>(buf[0]) - 2;
      }
    ]=] BLAKE3PP_COMPILER_XTHEADVECTOR)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_blake3pp_saved_tct}")
    unset(CMAKE_REQUIRED_FLAGS)
    if(BLAKE3PP_COMPILER_XTHEADVECTOR)
      # The attribute suppression pair: binutils' ld refuses to merge the
      # xtheadvector ELF arch attribute with the v/zve32x one the distro
      # crt objects carry (the encodings overlap by design, so the
      # attribute machinery models them as mutually exclusive), even
      # though this mix is exactly what a runtime-dispatched fat binary
      # wants. -mno-riscv-attribute silences GCC's directive and
      # -Wa,-mno-arch-attr the assembler's own; the attribute is advisory
      # metadata, and execution gating is runtime dispatch's job.
      blake3pp_add_kernel(xthead SOURCE src/kernel/xthead_kernel.cpp
        ARCH_FLAGS -march=rv64gc_xtheadvector
                   -mno-riscv-attribute -Wa,-mno-arch-attr)
    else()
      # The primary compiler cannot express XTheadVector: LLVM never
      # merged it, so the zig/clang musl chain lands here. Borrow the
      # distro GCC for this ONE TU (present in the zig toolchain image
      # for exactly this purpose) and link its object into the fat
      # binary: two compilers, one binary, because no single compiler
      # speaks every vector dialect. Probed by actually compiling the
      # same smoke snippet the native path uses.
      find_program(BLAKE3PP_XTHEAD_GCC riscv64-linux-gnu-g++)
      if(BLAKE3PP_XTHEAD_GCC)
        set(_xthead_smoke "${CMAKE_BINARY_DIR}/blake3pp_generated/xthead_smoke.cpp")
        file(WRITE "${_xthead_smoke}" [=[
#include <riscv_th_vector.h>
int probe() {
  unsigned buf[4] = {1, 2, 3, 4};
  vuint32m1_t a = __riscv_th_vlwu_v_u32m1(buf, 4);
  a = __riscv_vadd_vv_u32m1(a, a, 4);
  __riscv_th_vsw_v_u32m1(buf, a, 4);
  return static_cast<int>(buf[0]) - 2;
}
]=])
        execute_process(
          COMMAND "${BLAKE3PP_XTHEAD_GCC}" -std=c++23
                  -march=rv64gc_xtheadvector -fsyntax-only "${_xthead_smoke}"
          RESULT_VARIABLE _xthead_gcc_rc
          OUTPUT_QUIET ERROR_QUIET)
        if(_xthead_gcc_rc EQUAL 0)
          message(STATUS "blake3pp: xthead kernel via external ${BLAKE3PP_XTHEAD_GCC}")
          blake3pp_add_kernel(xthead SOURCE src/kernel/xthead_kernel.cpp
            EXTERNAL_COMPILER "${BLAKE3PP_XTHEAD_GCC}"
            ARCH_FLAGS -march=rv64gc_xtheadvector
                       -mno-riscv-attribute -Wa,-mno-arch-attr)
        else()
          message(WARNING "blake3pp: BLAKE3PP_XTHEAD_KERNEL=ON but neither "
            "the primary compiler nor ${BLAKE3PP_XTHEAD_GCC} can build "
            "XTheadVector (needs GCC 14+); kernel skipped")
        endif()
      else()
        message(WARNING "blake3pp: BLAKE3PP_XTHEAD_KERNEL=ON but the "
          "compiler cannot build XTheadVector (needs GCC 14+ with "
          "riscv_th_vector.h) and no riscv64-linux-gnu-g++ exists for "
          "the external-object route; kernel skipped")
      endif()
    endif()
  endif()
endfunction()

function(_blake3pp_register_ppc64_kernels)
  # POWER VSX through the xsimd facade (128-bit, width 4). The distro
  # ppc64le baseline is POWER8, but xsimd's VSX backend and this kernel
  # are probed at power9 (the first probe that passed; relaxing to
  # power8 is a candidate-flag exercise for whoever has such silicon).
  # Correctness was validated byte-identical against the x86 golden
  # under qemu-ppc64le before this family existed. GNU-frontend
  # spelling only, like the other cross ISAs.
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    return()
  endif()
  # Mirrors xsimd's own gate (XSIMD_WITH_VSX = __VEC__ && __VSX__)
  # without needing its headers on the probe include path. Candidate
  # spellings, the riscv precedent: GCC says power9, clang/zig say pwr9.
  set(_vsx_flags "")
  foreach(_cand "-mcpu=power9" "-mcpu=pwr9")
    string(MAKE_C_IDENTIFIER "BLAKE3PP_COMPILER_VSX_${_cand}" _var)
    set(CMAKE_REQUIRED_FLAGS "${_cand}")
    check_cxx_source_compiles([=[
      #if !(defined(__VEC__) && defined(__VSX__))
      #error no vsx
      #endif
      int main() { return 0; }
    ]=] ${_var})
    set(CMAKE_REQUIRED_FLAGS "")
    if(${_var})
      set(_vsx_flags "${_cand}")
      break()
    endif()
  endforeach()
  if(_vsx_flags)
    blake3pp_add_kernel(vsx FORCE_XSIMD ARCH_FLAGS ${_vsx_flags})
  endif()
endfunction()

function(_blake3pp_register_s390x_kernels)
  # IBM z vector-enhancements-1 (z14) through the xsimd facade, the
  # first BIG-ENDIAN SIMD target. BLAKE3 is defined little-endian; the
  # kernel's load32/store32 byteswap on BE and the whole path was
  # validated byte-identical against the x86 golden under qemu-s390x
  # before this family existed. -mzvector is what unlocks xsimd's
  # VXE backend.
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    return()
  endif()
  # Mirrors xsimd's own gate (XSIMD_WITH_VXE = __VEC__ >= 10304 &&
  # __ARCH__ >= 12) without needing its headers on the probe path.
  # Candidate spellings: GCC takes -march=z14, clang/zig -mcpu=z14.
  set(_vxe_flags "")
  foreach(_cand "-march=z14;-mzvector" "-mcpu=z14;-mzvector")
    string(MAKE_C_IDENTIFIER "BLAKE3PP_COMPILER_VXE_${_cand}" _var)
    string(REPLACE ";" " " _cand_str "${_cand}")
    set(CMAKE_REQUIRED_FLAGS "${_cand_str}")
    check_cxx_source_compiles([=[
      #if !(defined(__VEC__) && __VEC__ >= 10304 && defined(__ARCH__) && __ARCH__ >= 12)
      #error no vxe
      #endif
      int main() { return 0; }
    ]=] ${_var})
    set(CMAKE_REQUIRED_FLAGS "")
    if(${_var})
      set(_vxe_flags "${_cand}")
      break()
    endif()
  endforeach()
  if(_vxe_flags)
    blake3pp_add_kernel(vxe FORCE_XSIMD ARCH_FLAGS ${_vxe_flags})
  endif()
endfunction()

function(_blake3pp_register_wasm_kernels)
  # WASM SIMD128 is a module-level feature (engines reject SIMD-bearing
  # modules wholesale if unsupported), so this really is a compile-time
  # choice
  blake3pp_add_kernel(simd128 ARCH_FLAGS -msimd128)
endfunction()

# Registers scalar plus every variant the target processor and compiler
# can express. Call once, then blake3pp_target_link_kernels().
function(blake3pp_register_kernels)
  blake3pp_add_kernel(scalar FORCE_SCALAR)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    _blake3pp_register_x86_kernels()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    _blake3pp_register_aarch64_kernels()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(riscv64)$")
    _blake3pp_register_riscv64_kernels()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ppc64le|powerpc64le|ppc64)$")
    _blake3pp_register_ppc64_kernels()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(s390x)$")
    _blake3pp_register_s390x_kernels()
  elseif(EMSCRIPTEN)
    _blake3pp_register_wasm_kernels()
  endif()
endfunction()
