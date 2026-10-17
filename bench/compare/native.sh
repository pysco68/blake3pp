#!/usr/bin/env bash
# Build the sweep natively for macOS on arm64. It is the one platform
# without a cross script: cross-compiling to macOS needs Apple's SDK,
# which a Linux host does not have, so these binaries are built on the Mac
# itself.
#
#   ./native.sh
#
# Output: dist/macos-arm64/, laid out as the cross scripts lay theirs out,
# which is what a bundle packer copies from.
set -euo pipefail
cd "$(dirname "$0")"

[ -n "${CARGO_HOME:-}" ] || command -v cargo > /dev/null || . "${HOME}/.cargo/env"

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) OUT=dist/macos-arm64; CAPS=(default cap_no_neon portable) ;;
  *) echo "$0: builds the macOS binaries on a Mac; Linux and Windows" \
          "come from cross-linux.sh and cross-windows.sh" >&2; exit 2 ;;
esac

mkdir -p "${OUT}"
# `default` is a feature like any other, an empty one, so it is passed
# through rather than special-cased into an empty array the way the cross
# scripts do: this script runs on the Mac, whose bash is 3.2, and there
# an empty array expands as unset and `set -u` kills the build.
for cap in "${CAPS[@]}"; do
  echo "building ${cap}"
  cargo build --release --target-dir "target/macos-arm64-${cap}" --features "${cap}" \
    > "target-macos-arm64-${cap}.build.log" 2>&1 \
    || { echo "${cap}: build failed, see target-macos-arm64-${cap}.build.log" >&2; exit 1; }
  cp "target/macos-arm64-${cap}/release/blake3-rayon-bench" \
     "${OUT}/blake3-rayon-bench-${cap}"
done

cp sweep-prebuilt.sh "${OUT}/sweep.sh"
chmod +x "${OUT}/sweep.sh" "${OUT}"/blake3-rayon-bench-*
echo
echo "${OUT}:"
ls -l "${OUT}"
