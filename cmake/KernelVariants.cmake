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

# Probe candidate flag sets against a source snippet; the first set that
# compiles wins (empty result: none did). Grown out of four hand-rolled
# copies of the same loop: every cross family needs it, because the
# same feature spells differently per driver (gcc -march vs zig/clang
# -mcpu; power9 vs pwr9...). Each CANDIDATE is one flag set; multi-flag
# sets are |-joined ("-march=z14|-mzvector") because CMake's argument
# parsing flattens ;-lists. The winner is returned with | intact, and
# callers convert (string REPLACE "|" ";") before ARCH_FLAGS. @VLEN@ in
# a candidate is substituted with PROBE_VLEN for the probe compile only
# (the caller expands the winning pattern per variant);
# PROBE_EXTRA_FLAGS are appended for the probe only.
#
# Compile-only: the question is whether the driver accepts the flags and
# the intrinsics compile, and linking a probe executable costs real time
# on some toolchains. zig builds its libc++/compiler-rt/musl per CPU
# model and optimisation mode at link time, ~40 s and 2000+ cache
# files for every -march candidate a probe would link with, in every
# fresh container; the kernel objects themselves are compile-only and
# the final link carries no per-kernel flags, so nothing else pays it.
function(blake3pp_probe_flag_candidates out_var probe_name)
  cmake_parse_arguments(PARSE_ARGV 2 PF "" "SOURCE;PROBE_VLEN"
    "CANDIDATES;PROBE_EXTRA_FLAGS")
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  set(result "")
  foreach(cand IN LISTS PF_CANDIDATES)
    string(MAKE_C_IDENTIFIER "${probe_name}_${cand}" var)
    set(trial "${cand}")
    if(PF_PROBE_VLEN)
      string(REPLACE "@VLEN@" "${PF_PROBE_VLEN}" trial "${trial}")
      # SVE's cc1 spelling counts 128-bit granules, not bits.
      math(EXPR _vscale "${PF_PROBE_VLEN} / 128")
      string(REPLACE "@VSCALE@" "${_vscale}" trial "${trial}")
    endif()
    string(REPLACE "|" " " trial "${trial}")
    string(REPLACE ";" " " extra "${PF_PROBE_EXTRA_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${trial} ${extra}")
    check_cxx_source_compiles("${PF_SOURCE}" ${var})
    set(CMAKE_REQUIRED_FLAGS "")
    if(${var})
      set(result "${cand}")
      break()
    endif()
  endforeach()
  set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# Smoke-test an EXTERNAL cross compiler (the two-compiler-binary route:
# xthead via riscv gcc, vxe via s390x gcc) by actually compiling the
# same snippet the native path probes. Trailing args are the flags.
function(_blake3pp_external_gcc_smoke out_var compiler smoke_name source)
  set(_src "${CMAKE_BINARY_DIR}/blake3pp_generated/${smoke_name}.cpp")
  file(WRITE "${_src}" "${source}")
  execute_process(
    COMMAND "${compiler}" -std=c++23 ${ARGN} -fsyntax-only "${_src}"
    RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
  if(_rc EQUAL 0)
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} FALSE PARENT_SCOPE)
  endif()
endfunction()

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

# <pattern> <vlen> <out>: the probed candidate with its vector length
# filled in, split into arguments.
function(_blake3pp_sve_flags pattern vlen out)
  math(EXPR _vscale "${vlen} / 128")
  string(REPLACE "@VLEN@" "${vlen}" _f "${pattern}")
  string(REPLACE "@VSCALE@" "${_vscale}" _f "${_f}")
  separate_arguments(_f UNIX_COMMAND "${_f}")
  # -Xclang applies to the ONE argument after it, and CMake removes
  # duplicate compile options: a plain list holding it twice reaches the
  # compiler with a single -Xclang, so the second flag lands on the
  # driver instead and is ignored. Such a build still succeeds and still
  # registers a kernel, which then holds NEON code. SHELL: keeps each
  # pair together and exempt from de-duplication.
  set(_out "")
  list(LENGTH _f _n)
  set(_i 0)
  while(_i LESS _n)
    list(GET _f ${_i} _item)
    math(EXPR _next "${_i} + 1")
    if(_item STREQUAL "-Xclang" AND _next LESS _n)
      list(GET _f ${_next} _arg)
      list(APPEND _out "SHELL:-Xclang ${_arg}")
      math(EXPR _i "${_i} + 2")
    else()
      list(APPEND _out "${_item}")
      math(EXPR _i "${_i} + 1")
    endif()
  endwhile()
  set(${out} "${_out}" PARENT_SCOPE)
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
  # fused rotate. The probe keeps unusual cross setups honest: no probe
  # pass, no kernel registered.
  option(BLAKE3PP_SVE_ALL_VARIANTS
    "Also compile the SVE variants matching no shipping silicon (sve128, sve2_256, sve2_512), for emulator targets"
    OFF)
  # cl has no SVE in any spelling, but clang-cl does, so the gate is on
  # the compiler rather than on the frontend it imitates: gating on the
  # MSVC frontend variant leaves arm64 Windows with neon and scalar.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
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
  # form.
  # The vector length is part of the candidate because its spelling
  # varies with the driver, not with the compiler: clang-cl rejects
  # -msve-vector-bits= as a GNU-driver flag, and routing it through
  # -Xclang does not help either, since cc1 spells the same thing as a
  # vscale range counted in 128-bit granules. Without the third
  # candidate an arm64 Windows build registers no SVE kernel at all.
  blake3pp_probe_flag_candidates(_blake3pp_sve1_flag BLAKE3PP_COMPILER_SVE_VLS
    SOURCE "${_blake3pp_sve_smoke}" PROBE_VLEN 256
    CANDIDATES "-march=armv8.2-a+sve -msve-vector-bits=@VLEN@"
               "-mcpu=generic+sve -msve-vector-bits=@VLEN@"
               "-march=armv8.2-a+sve -Xclang -mvscale-min=@VSCALE@ -Xclang -mvscale-max=@VSCALE@")
  blake3pp_probe_flag_candidates(_blake3pp_sve2_flag BLAKE3PP_COMPILER_SVE2_VLS
    SOURCE "${_blake3pp_sve_smoke}" PROBE_VLEN 256
    CANDIDATES "-march=armv8.5-a+sve2 -msve-vector-bits=@VLEN@"
               "-mcpu=generic+sve2 -msve-vector-bits=@VLEN@"
               "-march=armv8.5-a+sve2 -Xclang -mvscale-min=@VSCALE@ -Xclang -mvscale-max=@VSCALE@")

  if(_blake3pp_sve1_flag OR _blake3pp_sve2_flag)
    # The SVE kernels pin themselves to xsimd (FORCE_XSIMD) even when the
    # project provider is a std one: libstdc++'s experimental::simd SVE
    # backend emits guarded dynamic initializers containing SVE
    # instructions, which run at LOAD TIME in every including TU: an
    # instant SIGILL on non-SVE hardware, defeating the fat binary.
    _blake3pp_fetch_xsimd()
  endif()
  if(_blake3pp_sve1_flag)
    _blake3pp_sve_flags("${_blake3pp_sve1_flag}" 256 _flags)
    blake3pp_add_kernel(sve256 FORCE_XSIMD ARCH_FLAGS ${_flags})
    _blake3pp_sve_flags("${_blake3pp_sve1_flag}" 512 _flags)
    blake3pp_add_kernel(sve512 FORCE_XSIMD ARCH_FLAGS ${_flags})
    if(BLAKE3PP_SVE_ALL_VARIANTS)
      _blake3pp_sve_flags("${_blake3pp_sve1_flag}" 128 _flags)
      blake3pp_add_kernel(sve128 FORCE_XSIMD ARCH_FLAGS ${_flags})
    endif()
  endif()
  if(_blake3pp_sve2_flag)
    _blake3pp_sve_flags("${_blake3pp_sve2_flag}" 128 _flags)
    blake3pp_add_kernel(sve2_128 FORCE_XSIMD ARCH_FLAGS ${_flags})
    if(BLAKE3PP_SVE_ALL_VARIANTS)
      _blake3pp_sve_flags("${_blake3pp_sve2_flag}" 256 _flags)
      blake3pp_add_kernel(sve2_256 FORCE_XSIMD ARCH_FLAGS ${_flags})
      _blake3pp_sve_flags("${_blake3pp_sve2_flag}" 512 _flags)
      blake3pp_add_kernel(sve2_512 FORCE_XSIMD ARCH_FLAGS ${_flags})
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
  # to assemble the moment anything links.
  blake3pp_probe_flag_candidates(_blake3pp_rvv_pattern
    BLAKE3PP_COMPILER_RVV_FIXED_VLEN
    SOURCE "${_blake3pp_rvv_smoke}" PROBE_VLEN 256
    CANDIDATES "-march=rv64gcv_zvl@VLEN@b -mrvv-vector-bits=zvl"
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
    blake3pp_probe_flag_candidates(_blake3pp_rvv_zvbb_pattern
      BLAKE3PP_COMPILER_RVV_ZVBB
      SOURCE "${_blake3pp_rvv_zvbb_smoke}" PROBE_VLEN 256
      CANDIDATES "-march=rv64gcv_zvbb_zvl@VLEN@b -mrvv-vector-bits=zvl"
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
        _blake3pp_external_gcc_smoke(_xthead_gcc_ok
          "${BLAKE3PP_XTHEAD_GCC}" xthead_smoke [=[
#include <riscv_th_vector.h>
int probe() {
  unsigned buf[4] = {1, 2, 3, 4};
  vuint32m1_t a = __riscv_th_vlwu_v_u32m1(buf, 4);
  a = __riscv_vadd_vv_u32m1(a, a, 4);
  __riscv_th_vsw_v_u32m1(buf, a, 4);
  return static_cast<int>(buf[0]) - 2;
}
]=] -march=rv64gc_xtheadvector)
        if(_xthead_gcc_ok)
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
  # spellings: GCC says power9, clang/zig say pwr9.
  set(_blake3pp_vsx_smoke [=[
    #if !(defined(__VEC__) && defined(__VSX__))
    #error no vsx
    #endif
    int main() { return 0; }
  ]=])
  blake3pp_probe_flag_candidates(_vsx_flags BLAKE3PP_COMPILER_VSX
    SOURCE "${_blake3pp_vsx_smoke}"
    CANDIDATES "-mcpu=power9" "-mcpu=pwr9")
  if(_vsx_flags)
    blake3pp_add_kernel(vsx FORCE_XSIMD ARCH_FLAGS ${_vsx_flags})
  endif()
endfunction()

function(_blake3pp_register_mips_kernels)
  # MIPS MSA, 128-bit lanes, through the vector-extension provider rather
  # than a simd library: xsimd has no MSA backend, and libstdc++ deduces a
  # NATIVE WIDTH OF 1 here because its ABI list has never heard of the
  # ISA, so a kernel written against either would compile to scalar code
  # on a machine with a vector unit. Measured, GCC 14 cross to mips64el at
  # -march=mips64r5 -mmsa: native_simd<uint32_t> 0 MSA instructions,
  # explicit 16-byte vector 6. The compiler reaches the ISA; only the
  # library's width guess does not.
  #
  # EMULATOR-ONLY. Correctness is validated byte-identical against the
  # x86 golden vectors under qemu-mips64el; no MSA silicon has ever run
  # this kernel, and every throughput number for it would be a qemu
  # number. It ships with that stated, on the same terms as the POWER and
  # IBM z variants, minus their hardware.
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    return()
  endif()
  # -mmsa alone is not enough: MSA is an r5 extension and the distro
  # baseline is r2, so the arch has to be raised with it. GCC predefines
  # __mips_msa and __mips_msa_width when both land.
  set(_blake3pp_msa_smoke [=[
    #if !(defined(__mips_msa) && __mips_msa_width == 128)
    #error no msa
    #endif
    int main() { return 0; }
  ]=])
  blake3pp_probe_flag_candidates(_msa_flags BLAKE3PP_COMPILER_MSA
    SOURCE "${_blake3pp_msa_smoke}"
    CANDIDATES "-march=mips64r5|-mmsa" "-march=mips32r5|-mmsa" "-mmsa")
  if(_msa_flags)
    string(REPLACE "|" ";" _msa_flags "${_msa_flags}")
    blake3pp_add_kernel(msa FORCE_VEXT VEXT_BYTES 16 ARCH_FLAGS ${_msa_flags})
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
  set(_blake3pp_vxe_smoke [=[
    #if !(defined(__VEC__) && __VEC__ >= 10304 && defined(__ARCH__) && __ARCH__ >= 12)
    #error no vxe
    #endif
    int main() { return 0; }
  ]=])
  blake3pp_probe_flag_candidates(_vxe_flags BLAKE3PP_COMPILER_VXE
    SOURCE "${_blake3pp_vxe_smoke}"
    CANDIDATES "-march=z14|-mzvector" "-mcpu=z14|-mzvector")
  string(REPLACE "|" ";" _vxe_flags "${_vxe_flags}")
  if(_vxe_flags)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      blake3pp_add_kernel(vxe FORCE_XSIMD ARCH_FLAGS ${_vxe_flags})
    else()
      # clang/LLVM SystemZ SCALARIZES xsimd's VXE ops. Measured on the
      # zig musl build: 10 vector instructions in the whole binary vs
      # GCC's thousands (vaf/vx/verllf), and TCG showing gcc's kernel
      # 3.6x over scalar where clang's ties it. A compile probe cannot
      # see that (the mislabeled-kernel lesson, again), so under a
      # non-GNU primary the kernel comes from the distro s390x GCC as
      # an EXTERNAL object (the xthead pattern; the gcc lives in the
      # zig image for exactly this TU), or not at all: correct-but-
      # scalar wearing a vector name is not shipped.
      find_program(BLAKE3PP_VXE_GCC s390x-linux-gnu-g++)
      set(_vxe_gcc_ok FALSE)
      if(BLAKE3PP_VXE_GCC)
        _blake3pp_external_gcc_smoke(_vxe_gcc_ok "${BLAKE3PP_VXE_GCC}"
          vxe_smoke "${_blake3pp_vxe_smoke}" -march=z14 -mzvector)
      endif()
      if(_vxe_gcc_ok)
        message(STATUS "blake3pp: vxe kernel via external ${BLAKE3PP_VXE_GCC}")
        blake3pp_add_kernel(vxe FORCE_XSIMD
          EXTERNAL_COMPILER "${BLAKE3PP_VXE_GCC}"
          ARCH_FLAGS -march=z14 -mzvector)
      else()
        message(STATUS "blake3pp: vxe kernel skipped; non-GNU compiler "
          "scalarizes it and no s390x-linux-gnu-g++ found for the "
          "external-object route")
      endif()
    endif()
  endif()
endfunction()

function(_blake3pp_register_wasm_kernels)
  # WASM SIMD128 is a module-level feature (engines reject SIMD-bearing
  # modules wholesale if unsupported), so this really is a compile-time
  # choice: the option is what produces the scalar-only module an
  # embedder serves to an engine without SIMD (README, "The wasm build").
  option(BLAKE3PP_WASM_SIMD128
    "Compile the simd128 kernel (OFF builds a module that loads on engines without wasm SIMD)"
    ON)
  if(BLAKE3PP_WASM_SIMD128)
    blake3pp_add_kernel(simd128 ARCH_FLAGS -msimd128)
  endif()
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
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(mips64el|mipsel|mips64|mips)$")
    _blake3pp_register_mips_kernels()
  elseif(EMSCRIPTEN)
    _blake3pp_register_wasm_kernels()
  endif()
endfunction()
