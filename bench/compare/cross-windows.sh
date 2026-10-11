#!/usr/bin/env bash
# Cross-build the sweep for Windows on x64 or Arm from a Linux box.
#
#   ./cross-windows.sh x64|arm64
#
# There is no Rust on the Windows machines this targets, and no reason
# to put one there: cargo-xwin fetches the MSVC CRT and Windows SDK
# import libraries and drives clang-cl and lld-link, so the result is an
# ordinary MSVC-ABI binary built without Visual Studio. The blake3 crate
# compiles its SIMD backends from C, which goes through the same
# clang-cl.
#
# Prerequisites: cargo-xwin (cargo install cargo-xwin), the target's
# std (rustup target add), and LLVM's clang-cl, lld-link and llvm-lib on
# PATH.
#
# Output: dist/windows-<arch>/, holding one binary per compile-time cap
# plus the runner that reproduces sweep.sh on the target.
set -euo pipefail
cd "$(dirname "$0")"

[ -n "${CARGO_HOME:-}" ] || command -v cargo > /dev/null || . "${HOME}/.cargo/env"

arch=${1:-}
case "${arch}" in
  x64)   TARGET=x86_64-pc-windows-msvc;  CAPS=(default cap_avx2 cap_sse41 cap_sse2 portable) ;;
  arm64) TARGET=aarch64-pc-windows-msvc; CAPS=(default cap_no_neon portable) ;;
  *) echo "usage: $0 x64|arm64" >&2; exit 2 ;;
esac
OUT=dist/windows-${arch}

for tool in clang-cl lld-link llvm-lib; do
  command -v "$tool" > /dev/null \
    || { echo "$0: $tool is not on PATH (try /usr/lib/llvm-*/bin)" >&2; exit 1; }
done

# A static CRT, so the binaries run on machines without the VC++
# redistributable and without DLLs placed beside them.
export RUSTFLAGS="${RUSTFLAGS:-} -C target-feature=+crt-static"

mkdir -p "${OUT}"
for cap in "${CAPS[@]}"; do
  feature_args=()
  [ "${cap}" = default ] || feature_args=(--features "${cap}")
  echo "building ${cap}"
  cargo xwin build --release --target "${TARGET}" \
    --target-dir "target/win-${arch}-${cap}" "${feature_args[@]}" \
    > "target-win-${arch}-${cap}.build.log" 2>&1 \
    || { echo "${cap}: build failed, see target-win-${arch}-${cap}.build.log" >&2; exit 1; }
  cp "target/win-${arch}-${cap}/${TARGET}/release/blake3-rayon-bench.exe" \
     "${OUT}/blake3-rayon-bench-${cap}.exe"
done

cp sweep.ps1 "${OUT}/sweep.ps1"
echo
echo "${OUT}:"
ls -l "${OUT}"
