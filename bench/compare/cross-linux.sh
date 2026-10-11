#!/usr/bin/env bash
# Cross-build the sweep for Linux on arm64 or riscv64 from an x86-64 box,
# statically.
#
#   ./cross-linux.sh arm64|riscv64
#
# The companion to cross-windows-arm64.sh, and the reason it is a
# separate script is the mechanism: MSVC targets need cargo-xwin for the
# CRT and SDK import libraries, whereas a musl target needs a C
# cross-compiler for the crate's SIMD backends and a static linker, both
# of which zig already is. Static because the machines these run on are
# whatever the cloud or the board offers, and a musl binary does not
# care which libc is installed.
#
# Prerequisites: cargo-zigbuild and zig (pip install ziglang), plus the
# target's musl std (rustup target add).
#
# Output: dist/linux-<arch>/, one binary per compile-time cap plus the
# runner that reproduces sweep.sh from prebuilt binaries.
set -euo pipefail
cd "$(dirname "$0")"

[ -n "${CARGO_HOME:-}" ] || command -v cargo > /dev/null || . "${HOME}/.cargo/env"

# The crate has no riscv64 backend at all, so there its default build is
# the portable one; both are built so the digest check has a pair.
arch=${1:-}
case "${arch}" in
  x86_64)  TARGET=x86_64-unknown-linux-musl;    CAPS=(default cap_avx2 cap_sse41 cap_sse2 portable) ;;
  arm64)   TARGET=aarch64-unknown-linux-musl;   CAPS=(default cap_no_neon portable) ;;
  riscv64) TARGET=riscv64gc-unknown-linux-musl; CAPS=(default portable) ;;
  *) echo "usage: $0 x86_64|arm64|riscv64" >&2; exit 2 ;;
esac
OUT=dist/linux-${arch}

mkdir -p "${OUT}"
for cap in "${CAPS[@]}"; do
  feature_args=()
  [ "${cap}" = default ] || feature_args=(--features "${cap}")
  echo "building ${cap}"
  cargo zigbuild --release --target "${TARGET}" \
    --target-dir "target/linux-${arch}-${cap}" "${feature_args[@]}" \
    > "target-linux-${arch}-${cap}.build.log" 2>&1 \
    || { echo "${cap}: build failed, see target-linux-${arch}-${cap}.build.log" >&2; exit 1; }
  cp "target/linux-${arch}-${cap}/${TARGET}/release/blake3-rayon-bench" \
     "${OUT}/blake3-rayon-bench-${cap}"
done

cp sweep-prebuilt.sh "${OUT}/sweep.sh"
chmod +x "${OUT}/sweep.sh" "${OUT}"/blake3-rayon-bench-*
echo
echo "${OUT}:"
ls -l "${OUT}"
