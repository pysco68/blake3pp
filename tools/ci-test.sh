#!/usr/bin/env bash
# ci-test.sh <preset>: configure, build, and run the preset's FULL test
# matrix: plain ctest plus every emulator configuration the preset's
# architecture supports. This is the single entry point .github/workflows/
# ci.yml calls, and it runs identically inside a toolchain image locally:
#
#   tools/tc <preset> -- tools/ci-test.sh <preset>
#
# The per-architecture matrices reproduce the bring-up verification runs.
# A fat binary only proves itself when the emulator can also REMOVE
# features: half these configs exist to run the dispatch fallbacks, not
# the fast paths.
set -euo pipefail

preset=${1:?usage: ci-test.sh <preset>}
build_dir="build/${preset}"

cmake --preset "${preset}"
cmake --build "${build_dir}" -j"$(nproc)"

# The kernel audit (tools/objscan.py, rules in tools/kernel-audit.json):
# every variant's object carries its instruction class and nothing above
# it, the hot functions are inlined and unrolled, and the linked tool
# carries nothing above the baseline outside the kernels. A wasm module
# has no objdump.
audit_kernels() {  # <build-dir>
  case "${preset}" in
    wasm32-*) ;;
    *) python3 tools/objscan.py audit "$1" --binary "$1/cli/blake3ppsum" ;;
  esac
}
audit_kernels "${build_dir}"

run_ctest() {  # <label> [QEMU_CPU value]
  local label=$1 cpu=${2-}
  echo "::group::ctest ${preset} [${label}]"
  if [ -n "${cpu}" ]; then
    QEMU_CPU="${cpu}" ctest --test-dir "${build_dir}" --output-on-failure
  else
    env -u QEMU_CPU ctest --test-dir "${build_dir}" --output-on-failure
  fi
  echo "::endgroup::"
}

case "${preset}" in
  *arm64*)
    # SVE vector-length matrix (bytes): 64 -> sve512, 32 -> sve256,
    # 16 -> sve2_128, sve=off -> the NEON-only regression, the config
    # that catches load-time SVE leaks (the experimental::simd trap).
    run_ctest default
    run_ctest sve-vl256 "max,sve-default-vector-length=32"
    run_ctest sve-vl128 "max,sve-default-vector-length=16"
    run_ctest no-sve "max,sve=off"
    ;;
  *riscv64*)
    # RVV VLEN matrix (bits; qemu's default max CPU is VLEN=128 with V and
    # Zvbb on), the Zvbb-off and V-off fallbacks. NOT -cpu rv64: it cannot
    # even run a resolute-glibc binary (RVA23 userland).
    run_ctest default
    run_ctest rvv-vlen256 "max,vlen=256"
    run_ctest rvv-vlen512 "max,vlen=512"
    run_ctest no-zvbb "max,zvbb=false"
    run_ctest no-v "max,v=false"
    # XTheadVector compile coverage: the kernel builds and links into the
    # fat binary; execution needs the Xuantie qemu fork (not in the CI
    # image; see docker/riscv64-gcc15.Dockerfile), so under mainline
    # qemu the variant reports unavailable and the suite proves exactly
    # that. The gcc preset gets an extra opt-in build here; the MUSL
    # preset carries xthead by DEFAULT via the external-gcc object route
    # (LLVM never merged XTheadVector; see EXTERNAL_COMPILER in
    # cmake/ArchKernels.cmake), covered by the plain matrix above.
    if [[ "${preset}" == *gcc* ]]; then
      xthead_dir="build/${preset}-xthead"
      cmake --preset "${preset}" -B "${xthead_dir}" -DBLAKE3PP_XTHEAD_KERNEL=ON
      cmake --build "${xthead_dir}" -j"$(nproc)"
      audit_kernels "${xthead_dir}"
      echo "::group::ctest ${preset} [xthead-compiled-in]"
      ctest --test-dir "${xthead_dir}" --output-on-failure
      echo "::endgroup::"
      # Freestanding xthead verifier (tests/xthead_verify.cpp): no libc,
      # raw syscalls, runs on ANY riscv64 Linux. Built here so the
      # Cloud-V Pioneer job can exercise the 0.7.1 kernel on real
      # silicon without any toolchain on the board.
      vd="build/xthead-verify"
      vf="-std=c++23 -O2 -fno-exceptions -fno-rtti -fno-stack-protector -Isrc -Iinclude"
      mkdir -p "${vd}"
      riscv64-linux-gnu-g++ ${vf} -march=rv64gc -DBLAKE3PP_ARCH_NS=scalar \
        -DBLAKE3PP_FORCE_SCALAR=1 -c src/kernel/kernel.cpp -o "${vd}/scalar.o"
      riscv64-linux-gnu-g++ ${vf} -march=rv64gc_xtheadvector \
        -mno-riscv-attribute -Wa,-mno-arch-attr -DBLAKE3PP_ARCH_NS=xthead \
        -c src/kernel/xthead_kernel.cpp -o "${vd}/xthead.o"
      riscv64-linux-gnu-g++ ${vf} -march=rv64gc \
        -c tests/xthead_verify.cpp -o "${vd}/main.o"
      riscv64-linux-gnu-g++ -nostdlib -static -o "${vd}/xthead_verify" \
        "${vd}/main.o" "${vd}/scalar.o" "${vd}/xthead.o"
    fi
    ;;
  *ppc64le*)
    # VSX is POWER7+ and Ubuntu's ppc64le USERLAND is built power9+
    # (a power8 CPU model SIGILLs the distro libstdc++ before main),
    # so the meaningful qemu spread is the supported
    # generations. The scalar fallback path is exercised by the generic
    # all-arch tests; no ppc64le model is vector-less.
    run_ctest default
    run_ctest power9 power9
    run_ctest power10 power10
    ;;
  *s390x*)
    # BIG-ENDIAN lane. qemu's max carries z14 vector-enhancements (the
    # vxe kernel runs). Named machine models (z13...) demand KVM-only
    # facilities under qemu-user TCG, so the fallbacks are driven by
    # FEATURE dials instead: vxeh=off keeps base vector but removes the
    # z14 facility (dispatch must refuse vxe), vx=off removes vector
    # entirely; both land on scalar.
    run_ctest default
    run_ctest no-vxe "max,vxeh=off"
    # (max,vx=off is NOT a runnable config: Ubuntu's s390x userland
    # baseline is z13-with-vector, and removing vx kills ld.so before
    # main, the same class as ppc64le's power9+ userland baseline.)
    ;;
  wasm32-*)
    run_ctest default
    ;;
  *)
    # x86 (native and zig/musl static): the runner's own CPU decides the
    # available variants...
    run_ctest default
    # ...and Intel SDE supplies what it lacks: AVX-512 (qemu's TCG never
    # implemented it). The kernel-oracle and official-vector suites are in
    # the doctest binary; CLI smoke under SDE would add minutes for no
    # dispatch coverage. Skipped for sanitizer presets (SDE's DBT and the
    # sanitizer runtimes fight over the address space).
    if command -v sde64 > /dev/null 2>&1 && [[ "${preset}" != *san* ]] \
       && [ -x "${build_dir}/tests/blake3pp_tests" ]; then
      echo "::group::SDE avx512 ${preset}"
      sde64 -skx -- "${build_dir}/tests/blake3pp_tests"
      echo "::endgroup::"
    fi
    ;;
esac
