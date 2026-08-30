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
    # that.
    xthead_dir="build/${preset}-xthead"
    cmake --preset "${preset}" -B "${xthead_dir}" -DBLAKE3PP_XTHEAD_KERNEL=ON
    cmake --build "${xthead_dir}" -j"$(nproc)"
    echo "::group::ctest ${preset} [xthead-compiled-in]"
    ctest --test-dir "${xthead_dir}" --output-on-failure
    echo "::endgroup::"
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
