#!/usr/bin/env bash
# Sweep the blake3 crate across the instruction sets it can reach.
#
# The crate dispatches at runtime but is capped at COMPILE time through
# negative features, so one binary per cap is the only way to make it
# use a narrower backend than the CPU offers. Each cap gets its own
# target directory, so switching between them does not rebuild the
# others.
#
#   ./sweep.sh [--size MiB] [--reps N] [--threads N] [--pin CPU]
#
# What the crate can reach at all: sse2, sse41, avx2, avx512 and neon.
# It has no SVE, no RVV, no VSX and no IBM z backend, so on those
# architectures its best is portable Rust, which is the comparison
# rather than a caveat.
set -euo pipefail
cd "$(dirname "$0")"

# rustup installs into $HOME/.cargo, which a non-login shell has not
# sourced.
[ -n "${CARGO_HOME:-}" ] || [ -x "$(command -v cargo 2> /dev/null)" ] \
  || . "${HOME}/.cargo/env"

size=512 reps=5 threads=0 pin=
while [ $# -gt 0 ]; do
  case $1 in
    --size) size=$2; shift 2 ;;
    --reps) reps=$2; shift 2 ;;
    --threads) threads=$2; shift 2 ;;
    --pin) pin=$2; shift 2 ;;
    *) echo "usage: $0 [--size MiB] [--reps N] [--threads N] [--pin CPU]" >&2; exit 2 ;;
  esac
done

case "$(uname -m)" in
  x86_64|amd64) caps=(default cap_avx2 cap_sse41 cap_sse2 portable) ;;
  aarch64|arm64) caps=(default cap_no_neon portable) ;;
  *)             caps=(default portable) ;;
esac

args=(--size "${size}" --reps "${reps}")
[ "${threads}" -gt 0 ] && args+=(--threads "${threads}")
[ -n "${pin}" ] && args+=(--pin "${pin}")

digests=()
for cap in "${caps[@]}"; do
  feature_args=()
  [ "${cap}" = default ] || feature_args=(--features "${cap}")
  cargo build --release --target-dir "target/${cap}" "${feature_args[@]}" \
    > "target-${cap}.build.log" 2>&1 \
    || { echo "${cap}: build failed, see target-${cap}.build.log"; continue; }
  echo "== ${cap}"
  out=$("target/${cap}/release/blake3-rayon-bench" "${args[@]}")
  echo "${out}"
  digests+=("$(echo "${out}" | sed -n 's/.*digest \([0-9a-f]*\).*/\1/p')")
done

# Every cap hashed the same bytes, so a differing digest means one of
# these backends is not computing BLAKE3 and its throughput row is
# meaningless.
if [ "$(printf '%s\n' "${digests[@]}" | sort -u | wc -l)" -ne 1 ]; then
  echo "DIGESTS DIFFER between caps: ${digests[*]}" >&2
  exit 1
fi
echo "all caps agree on the digest: ${digests[0]}"
