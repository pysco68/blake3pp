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

# The build driver. BLAKE3PP_CMAKE_RE=1 configures and builds through
# cmake-re (BLAKE3PP_CMAKE_RE_FLAGS selects where: --host, the default,
# builds with the image's own compilers; --host --distributed sends the
# compile actions to the cluster), the same switch tools/make-release.sh
# has. cmake-re knows no presets, so tools/preset-args.py unrolls the
# preset, and it places build/<preset> as a symlink into its mirror,
# which everything below reads through. Tests, emulator matrices and the
# coverage report are the same under both drivers.
configure_and_build() {  # <preset> <build-dir> [extra cmake args...]
  local p=$1 dir=$2; shift 2
  if [ "${BLAKE3PP_CMAKE_RE:-0}" = 1 ]; then
    local flags=${BLAKE3PP_CMAKE_RE_FLAGS:---host}
    rm -rf "${dir}"
    # The preset's own -B is replaced by the directory asked for (the
    # xthead extra build uses a sibling directory).
    local -a args
    read -r -a args <<< "$(python3 tools/preset-args.py "${p}" | sed -E 's# -B [^ ]+##')"
    # shellcheck disable=SC2086  # one flag per word, on purpose
    cmake-re ${flags} -S . "${args[@]}" -B "${dir}" "$@"
    cmake-re --build "${dir}" ${flags} -j"$(nproc)"
  else
    cmake --preset "${p}" -B "${dir}" "$@"
    cmake --build "${dir}" -j"$(nproc)"
  fi
}

configure_and_build "${preset}" "${build_dir}"

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

# Coverage presets (TC_COVERAGE in the toolchain): every test run below,
# the SDE pass included, contributes to one report written at the end
# to build/<preset>/coverage (summary.txt, coverage.lcov, html/). The
# clang presets use LLVM's source-based coverage, the gcc presets gcov
# through gcovr; both tools ship in the toolchain images.
# The source root the build compiled against: the checkout under plain
# cmake, the tipi mirror's copy under cmake-re.
cov_source_root() {
  local root
  root=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' \
         "${build_dir}/CMakeCache.txt" 2> /dev/null)
  echo "${root:-${PWD}}"
}

coverage=""
case "${preset}" in
  *coverage*)
    coverage_dir="${build_dir}/coverage"
    rm -rf "${coverage_dir}"
    mkdir -p "${coverage_dir}/profraw"
    if [[ "${preset}" == *clang* ]]; then
      coverage=llvm
      export LLVM_PROFILE_FILE="${PWD}/${coverage_dir}/profraw/%p-%m.profraw"
    else
      coverage=gcov
    fi
    ;;
esac

# Tests that run threads of their own, or measure something: giving them
# a core each while the rest of the suite saturates the machine makes
# both slower and the timing-sensitive ones flaky. Everything else is a
# single-core computation and parallelises freely (measured on a full
# qemu lane: 18 s serial against 5 s at -j8, same 80 passes).
CTEST_SERIAL='parallel|multi-core|quadrant|pool|thread|bench_smoke|bench_file_smoke|stdin_parallel|gen_threads|hashes files in sequence'

# A failing pass does not stop the rest: every configuration of the
# emulator matrix, and both halves of each, run to the end, and the
# script fails once at the bottom naming all of them. One broken
# variant otherwise hides whatever the other configurations would have
# said, which on a cross lane is most of the information in the run.
failed_passes=()

run_ctest() {  # <label> [QEMU_CPU value] [extra ctest args...]
  local label=$1 cpu=${2-}
  shift; [ $# -gt 0 ] && shift
  echo "::group::ctest ${preset} [${label}]"
  # --no-tests=ignore: a preset whose suite is one aggregate entry (wasm)
  # matches neither half, and an empty selection is not a failure here.
  local -a env_prefix=(env -u QEMU_CPU)
  [ -n "${cpu}" ] && env_prefix=(env "QEMU_CPU=${cpu}")
  "${env_prefix[@]}" ctest --test-dir "${build_dir}" --output-on-failure \
    --no-tests=ignore -j"$(nproc)" -E "${CTEST_SERIAL}" "$@" \
    || failed_passes+=("${label} (parallel)")
  "${env_prefix[@]}" ctest --test-dir "${build_dir}" --output-on-failure \
    --no-tests=ignore -R "${CTEST_SERIAL}" "$@" \
    || failed_passes+=("${label} (serial)")
  echo "::endgroup::"
}

# The emulator matrix (VLEN, feature-off configurations) runs where it
# earns its minutes: on the musl presets, whose binaries are the ones
# released and run on real boards, and on the coverage presets, where
# each configuration selects a different kernel and so contributes
# different lines. A plain cross lane proves the compiler builds and
# passes, which the default configuration already shows.
full_emulator_matrix() {
  [[ "${preset}" == *zigmusl* || "${preset}" == *coverage* ]]
}

case "${preset}" in
  *arm64*)
    # SVE vector-length matrix (bytes): 64 -> sve512, 32 -> sve256,
    # 16 -> sve2_128, sve=off -> the NEON-only regression, the config
    # that catches load-time SVE leaks (the experimental::simd trap).
    run_ctest default
    if full_emulator_matrix; then
      run_ctest sve-vl256 "max,sve-default-vector-length=32"
      run_ctest sve-vl128 "max,sve-default-vector-length=16"
      run_ctest no-sve "max,sve=off"
    fi
    ;;
  *riscv64*)
    # RVV VLEN matrix (bits; qemu's default max CPU is VLEN=128 with V and
    # Zvbb on), the Zvbb-off and V-off fallbacks. NOT -cpu rv64: it cannot
    # even run a resolute-glibc binary (RVA23 userland).
    run_ctest default
    if full_emulator_matrix; then
      run_ctest rvv-vlen256 "max,vlen=256"
      run_ctest rvv-vlen512 "max,vlen=512"
      run_ctest no-zvbb "max,zvbb=false"
      run_ctest no-v "max,v=false"
    fi
    # The XTheadVector kernel is NOT built a second time here. The musl
    # preset carries it by default through the external-gcc object route
    # (LLVM never merged XTheadVector; see EXTERNAL_COMPILER in
    # cmake/ArchKernels.cmake), so the same source goes through the same
    # GCC in the lane whose binaries ship, and the Cloud-V Pioneer runs
    # that kernel on real 0.7.1 silicon. A second full build here proved
    # only that it compiles, for a variant mainline qemu cannot execute,
    # at the price of building the whole project twice.
    if [[ "${preset}" == *gcc* ]]; then
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
    if full_emulator_matrix; then
      run_ctest power9 power9
      run_ctest power10 power10
    fi
    ;;
  *s390x*)
    # BIG-ENDIAN lane. qemu's max carries z14 vector-enhancements (the
    # vxe kernel runs). Named machine models (z13...) demand KVM-only
    # facilities under qemu-user TCG, so the fallbacks are driven by
    # FEATURE dials instead: vxeh=off keeps base vector but removes the
    # z14 facility (dispatch must refuse vxe), vx=off removes vector
    # entirely; both land on scalar.
    # The toolchain pins -march=z13 for everything but the vxe kernel:
    # this GCC defaults to arch13 (z15), and at that default the upstream
    # reference's keyed init (bench only) carried a vlbrf, a z15 vector
    # instruction, which SIGILLed here (2026-09-08). The config is a
    # legitimate target for the project's own code only; the sysroot's
    # libraries are the distro's.
    run_ctest default
    if full_emulator_matrix; then
      run_ctest no-vxe "max,vxeh=off"
    fi
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
    # implemented it). Only when it is lacking: the hosted runner pool is
    # mixed (Emerald Rapids Xeons and Zen 4 EPYCs have AVX-512, Zen 3
    # EPYCs do not), a native run already covers the kernel where the
    # silicon has it, and the emulated pass costs minutes (about 17 for
    # an instrumented coverage binary). The kernel-oracle and
    # official-vector suites are in the doctest binary; CLI smoke under
    # SDE would add minutes for no dispatch coverage. Skipped for
    # sanitizer presets (SDE's DBT and the sanitizer runtimes fight over
    # the address space).
    if grep -q -w avx512f /proc/cpuinfo 2> /dev/null; then
      echo "avx512 native on this runner ($(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //')): SDE pass skipped"
    elif command -v sde64 > /dev/null 2>&1 && [[ "${preset}" != *san* ]] \
       && [ -x "${build_dir}/tests/blake3pp_tests" ]; then
      echo "::group::SDE avx512 ${preset}"
      sde64 -skx -- "${build_dir}/tests/blake3pp_tests"
      echo "::endgroup::"
    fi
    ;;
esac

# The coverage report: library, headers and tools only (dependencies,
# tests and benches excluded). The tool version follows the toolchain's
# compiler (clang++-22 -> llvm-cov-22, g++-16 -> gcov-16), read from
# CMake's compiler record (a toolchain-set compiler is not in the cache).
report_coverage() {
  local cxx objects=() f
  cxx=$(sed -n 's/^set(CMAKE_CXX_COMPILER "\(.*\)")$/\1/p' \
        "${build_dir}"/CMakeFiles/*/CMakeCXXCompiler.cmake | head -1)
  [ -n "${cxx}" ] || { echo "coverage: no compiler record under ${build_dir}" >&2; return 1; }
  for f in tests/blake3pp_tests cli/blake3ppsum cli/blake3ppgen \
           bench/blake3pp_bench bench/blake3pp_bench_file; do
    [ -x "${build_dir}/${f}" ] && objects+=("${build_dir}/${f}")
  done
  echo "::group::coverage report ${preset}"
  case "${coverage}" in
    llvm)
      local profdata="llvm-profdata-${cxx##*-}" cov="llvm-cov-${cxx##*-}"
      command -v "${profdata}" > /dev/null || profdata=llvm-profdata
      command -v "${cov}" > /dev/null || cov=llvm-cov
      "${profdata}" merge -sparse "${coverage_dir}"/profraw/*.profraw \
        -o "${coverage_dir}/merged.profdata"
      local -a args=(-instr-profile "${coverage_dir}/merged.profdata"
                     -ignore-filename-regex='(/_deps/|/thirdparty/|/tests/|/bench/|^/usr/)'
                     "${objects[0]}")
      for f in "${objects[@]:1}"; do args+=(-object "${f}"); done
      "${cov}" report "${args[@]}" | tee "${coverage_dir}/summary.txt"
      "${cov}" export -format=lcov "${args[@]}" > "${coverage_dir}/coverage.lcov"
      "${cov}" show -format=html -output-dir "${coverage_dir}/html" "${args[@]}"
      ;;
    gcov)
      # g++-16 -> gcov-16; a cross g++ has only the versioned gcov, and
      # may itself be a wrapper elsewhere on PATH, so resolve by name. The
      # search path (last argument) matters: without it gcovr walks the
      # whole checkout and folds every other build tree's .gcda in.
      local gcov_name gcov
      gcov_name=$(basename "${cxx}"); gcov_name=${gcov_name/g++/gcov}
      gcov=$(command -v "${gcov_name}" \
             || command -v "${gcov_name}-$("${cxx}" -dumpversion | cut -d. -f1)")
      mkdir -p "${coverage_dir}/html"
      # The report runs FROM the source root the build compiled against,
      # not from the checkout: gcovr resolves a relative --filter against
      # the current directory rather than against --root, and a cmake-re
      # build compiles a copy of the tree inside its mirror, so filters
      # written against the checkout match nothing and the report comes
      # out empty. CMakeCache.txt names the root either driver used, and
      # everything the report reads or writes is absolute from here.
      local src_root abs_build abs_cov
      src_root=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' \
                 "${build_dir}/CMakeCache.txt")
      [ -n "${src_root}" ] || src_root=${PWD}
      abs_build=$(cd "${build_dir}" && pwd -P)
      abs_cov=$(cd "${coverage_dir}" && pwd -P)
      # gcovr writes its intermediate .gcov files into the current
      # directory and cleans them up only on success; sweep them on
      # failure so a crash does not litter the tree.
      ( cd "${src_root}" &&
        gcovr --root . --object-directory "${abs_build}" -j"$(nproc)" \
          --gcov-executable "${gcov}" \
          --gcov-exclude-directories '.*/_deps/.*' \
          --gcov-exclude-directories '.*/CMakeFiles/[0-9.]+/.*' \
          --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
          --filter 'src/' --filter 'include/' --filter 'cli/' --filter 'tooling/' \
          --exclude-throw-branches \
          --txt "${abs_cov}/summary.txt" \
          --lcov "${abs_cov}/coverage.lcov" \
          --html-details "${abs_cov}/html/index.html" \
          "${abs_build}" ) || { rm -f "${src_root}"/*'##'*.gcov; return 1; }
      cat "${coverage_dir}/summary.txt"
      ;;
  esac
  # One lcov dialect for every lane, so release.yml can merge them:
  # repo-relative paths, no test names and no per-file version stamps
  # (gcovr writes both, llvm-cov neither, and lcov keeps records apart on
  # either difference), and no generated files from the build tree.
  # (gcovr also writes a non-numeric block id, BRDA:<line>,None,..., for
  # branches gcov reports without one; lcov rejects the file over it.)
  # Both roots: the checkout, and the mirror a cmake-re build compiled
  # from, which is what llvm-cov reads out of the binaries.
  sed -i -e "s#^SF:${PWD}/#SF:#" -e "s#^SF:$(cov_source_root)/#SF:#" \
    -e '/^TN:/d' -e '/^VER:/d' \
    -e 's/^BRDA:\([0-9]*\),None,/BRDA:\1,0,/' "${coverage_dir}/coverage.lcov"
  awk '/^SF:build\//{skip=1} !skip{print} /^end_of_record/{skip=0}' \
    "${coverage_dir}/coverage.lcov" > "${coverage_dir}/coverage.lcov.tmp"
  mv "${coverage_dir}/coverage.lcov.tmp" "${coverage_dir}/coverage.lcov"
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    { echo "### coverage: ${preset}"; echo '```'
      cat "${coverage_dir}/summary.txt"; echo '```'; } >> "${GITHUB_STEP_SUMMARY}"
  fi
  echo "::endgroup::"
}
if [ -n "${coverage}" ]; then
  report_coverage
fi

# The coverage report is written first, so a lane that fails still
# uploads what its passing tests covered.
if [ "${#failed_passes[@]}" -gt 0 ]; then
  echo "::error::${preset}: ${#failed_passes[@]} failing test pass(es): ${failed_passes[*]}"
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    { echo "### failing test passes: ${preset}"
      printf -- '- %s\n' "${failed_passes[@]}"; } >> "${GITHUB_STEP_SUMMARY}"
  fi
  exit 1
fi
