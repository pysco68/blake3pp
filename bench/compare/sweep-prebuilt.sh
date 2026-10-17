#!/bin/sh
# sweep.sh for a machine that builds nothing: same protocol, same digest
# check, against whichever cap binaries were cross-built for this
# architecture. POSIX sh, because small boards ship busybox, not bash.
#
#   ./sweep.sh [--size MiB] [--reps N] [--threads N] [--pin CPU]
set -eu
cd "$(dirname "$0")"

# 512 MiB, or a quarter of physical memory where that is less: the
# default blake3pp_bench uses, so both hash the same size on small boards.
size=$(awk '/^MemTotal:/ { q = int($2 / 4096); print (q < 512 ? q : 512) }' \
  /proc/meminfo 2> /dev/null || true)
size=${size:-512}
reps=5 threads=0 pin=
while [ $# -gt 0 ]; do
  case $1 in
    --size) size=$2; shift 2 ;;
    --reps) reps=$2; shift 2 ;;
    --threads) threads=$2; shift 2 ;;
    --pin) pin=$2; shift 2 ;;
    *) echo "usage: $0 [--size MiB] [--reps N] [--threads N] [--pin CPU]" >&2; exit 2 ;;
  esac
done

set -- --size "$size" --reps "$reps"
if [ "$threads" -gt 0 ]; then set -- "$@" --threads "$threads"; fi
if [ -n "$pin" ]; then set -- "$@" --pin "$pin"; fi

# The crate's only aarch64 backend is NEON and it has none on riscv64,
# so every cap below that compiles to the same portable code and the
# extra binaries are a consistency check rather than data points.
digests=
for bin in ./blake3-rayon-bench-default \
           $(ls ./blake3-rayon-bench-* | grep -v -e '-default$'); do
  cap=${bin#./blake3-rayon-bench-}
  [ -x "$bin" ] || { echo "$cap: binary missing, skipped"; continue; }
  echo "== $cap"
  out=$("$bin" "$@")
  echo "$out"
  digests="$digests $(echo "$out" | sed -n 's/.*digest \([0-9a-f]*\).*/\1/p')"
done

# Every cap hashed the same bytes, so a differing digest means one of
# these backends is not computing BLAKE3 and its throughput row is
# meaningless.
if [ "$(printf '%s\n' $digests | sort -u | wc -l)" -ne 1 ]; then
  echo "DIGESTS DIFFER between caps:$digests" >&2
  exit 1
fi
first=${digests# }
echo "all caps agree on the digest: ${first%% *}"
