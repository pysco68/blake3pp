#!/bin/sh
# runpipe.sh: run "producer | consumer" for several consumers, counterbalanced,
# and refuse any round that is not a real measurement.
#
#   runpipe.sh -p 'PRODUCER' [-r ROUNDS] [-s SHAPE] [-R 'REFERENCE'] [-t TOOLDIR] \
#              LABEL='CONSUMER' [LABEL='CONSUMER' ...]
#
#   -p  the producer, as the exact command line (it is printed as given)
#   -r  rounds, 6 by default
#   -s  a name for the shape, printed in every row
#   -R  the command whose digest of the stream is the reference; the first
#       consumer when omitted
#   -t  where drain and rux are; the script's own directory, then PATH
#
# POSIX sh for Linux, macOS and FreeBSD. Its Windows twin is runpipe.ps1,
# which does the same checks over a cmd.exe pipe; keep the two parallel.
#
# Every command runs under rux on each side of the pipe:
#
#   rux -o P -- sh -c "exec PRODUCER" | rux -o C -- sh -c "exec CONSUMER" > D
#
# so each side's exit code, CPU and context switches land in its own file
# whatever the shell does with a pipeline's status. A round is refused,
# and the run stops with a non-zero exit, when:
#   1. either side's exit code is not 0, or its report is missing;
#   2. the consumer's digest differs from the reference, which is taken
#      once before the rotation;
#   3. the round is more than 1.5 times faster than the drain ceiling, the
#      best of four drains (two runs each) over the same producer measured
#      before the rotation. A consumer cannot beat a reader that does
#      nothing with the bytes, so such a round is a broken harness. The
#      margin absorbs noise on a short stream or a loaded machine; a
#      broken harness is off by orders of magnitude (2 GiB in 0.013 s). A
#      round faster than the ceiling within the margin is printed with a
#      warning on stderr.
# The arm order reverses every round. Rows are printed in run order, then
# the per-arm medians.

set -eu

rounds=6
shape=-
producer=
reference=
tooldir=
while getopts 'p:r:s:R:t:' opt; do
  case $opt in
    p) producer=$OPTARG ;;
    r) rounds=$OPTARG ;;
    s) shape=$OPTARG ;;
    R) reference=$OPTARG ;;
    t) tooldir=$OPTARG ;;
    *) echo "usage: runpipe.sh -p PRODUCER [-r N] [-s SHAPE] [-R REF] [-t DIR] LABEL=CMD..." >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
[ -n "$producer" ] || { echo "runpipe.sh: -p PRODUCER is required" >&2; exit 2; }
[ $# -ge 1 ] || { echo "runpipe.sh: at least one LABEL=CONSUMER is required" >&2; exit 2; }
case $rounds in '' | *[!0-9]*) echo "runpipe.sh: -r takes a number" >&2; exit 2 ;; esac
for arm in "$@"; do
  case $arm in
    *=*) ;;
    *) echo "runpipe.sh: '$arm' is not LABEL=CONSUMER" >&2; exit 2 ;;
  esac
done

# The instruments: an explicit directory, the script's own, then PATH.
find_tool() {
  for d in "$tooldir" "$(dirname "$0")"; do
    if [ -n "$d" ] && [ -x "$d/$1" ]; then
      echo "$d/$1"
      return
    fi
  done
  command -v "$1" || { echo "runpipe.sh: $1 not found (use -t)" >&2; exit 2; }
}
RUX=$(find_tool rux)
DRAIN=$(find_tool drain)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

fail() {
  echo "runpipe.sh: REFUSED: $*" >&2
  exit 1
}

# One field of a rux report: field FILE NAME
field() {
  sed -n "s/.* $2=\\([^ ]*\\).*/\\1/p" "$1"
}

# One pipeline run: producer into CONSUMER. Sets wall_us and leaves the
# two reports in $work/p and $work/c and the consumer's stdout in $work/d.
run_once() {
  rm -f "$work/p" "$work/c" "$work/d" "$work/e"
  "$RUX" -o "$work/p" -l prod -- sh -c "exec $producer" |
    "$RUX" -o "$work/c" -l cons -- sh -c "exec $1" > "$work/d" 2> "$work/e" || true
  for side in c p; do
    [ -s "$work/$side" ] || fail "$2: no report from the $side side (it did not run)"
    code=$(field "$work/$side" exit)
    [ "$code" = 0 ] || fail "$2: the $side side exited $code: $(cat "$work/e")"
  done
  pw=$(field "$work/p" wall_us)
  cw=$(field "$work/c" wall_us)
  wall_us=$((pw > cw ? pw : cw))
}

echo "producer: $producer"

# The reference digest, once.
[ -n "$reference" ] || reference=${1#*=}
run_once "$reference" reference
ref_digest=$(cut -d' ' -f1 < "$work/d")
[ -n "$ref_digest" ] || fail "the reference '$reference' printed no digest"
echo "reference: $reference -> $ref_digest"

# The drain ceiling: the best of four drains over this producer. Also the
# stream's length, which the throughput figures need.
ceiling_us=
bytes=
for d in '--buf 64' '--buf 64' '--buf 64 --threads 2' '--buf 64 --threads 2' \
         '--buf 1024' '--buf 1024' '--buf 4096 --threads 2' '--buf 4096 --threads 2'; do
  run_once "$DRAIN $d" "drain $d"
  b=$(sed -n 's/^drain: bytes=\([0-9]*\).*/\1/p' "$work/e")
  [ -n "$b" ] || fail "drain $d reported no byte count"
  [ -z "$bytes" ] || [ "$b" = "$bytes" ] || fail "drain $d read $b bytes, an earlier drain read $bytes"
  bytes=$b
  if [ -z "$ceiling_us" ] || [ "$wall_us" -lt "$ceiling_us" ]; then
    ceiling_us=$wall_us
    ceiling_by=$d
  fi
done
[ "$bytes" -gt 0 ] || fail "the producer wrote nothing"
awk -v b="$bytes" -v w="$ceiling_us" -v by="$ceiling_by" 'BEGIN {
  printf "stream: %d bytes; drain ceiling %.1f MiB/s (%.1f ms, drain %s)\n",
         b, b / 1048576 / (w / 1e6), w / 1000, by }'

echo "shape,round,arm,wall_ms,MiBps,cons_user_ms,cons_sys_ms,cons_vcsw,cons_ivcsw,prod_user_ms,prod_sys_ms,prod_vcsw,prod_ivcsw,total_cpu_ms,digest"
: > "$work/rows"
narms=$#
r=1
while [ "$r" -le "$rounds" ]; do
  # Forward on odd rounds, reversed on even ones.
  i=1
  while [ "$i" -le "$narms" ]; do
    if [ $((r % 2)) -eq 1 ]; then k=$i; else k=$((narms - i + 1)); fi
    eval "arm=\${$k}"
    label=${arm%%=*}
    cmd=${arm#*=}
    run_once "$cmd" "round $r, $label"
    digest=$(cut -d' ' -f1 < "$work/d")
    [ "$digest" = "$ref_digest" ] || fail "round $r, $label: digest $digest, reference $ref_digest"
    # More than 1.5 times faster than the best drain: not a measurement.
    [ $((wall_us * 3)) -ge $((ceiling_us * 2)) ] ||
      fail "round $r, $label: $wall_us us is over 1.5x faster than the drain ceiling of $ceiling_us us"
    [ "$wall_us" -ge "$ceiling_us" ] ||
      echo "runpipe.sh: warning: round $r, $label: $wall_us us beats the drain ceiling of $ceiling_us us" >&2
    row=$(awk -v s="$shape" -v r="$r" -v l="$label" -v w="$wall_us" -v b="$bytes" \
      -v cu="$(field "$work/c" user_us)" -v cs="$(field "$work/c" sys_us)" \
      -v cv="$(field "$work/c" vcsw)" -v ci="$(field "$work/c" ivcsw)" \
      -v pu="$(field "$work/p" user_us)" -v ps="$(field "$work/p" sys_us)" \
      -v pv="$(field "$work/p" vcsw)" -v pi="$(field "$work/p" ivcsw)" -v d="$digest" \
      'BEGIN { printf "%s,%d,%s,%.1f,%.1f,%.1f,%.1f,%s,%s,%.1f,%.1f,%s,%s,%.1f,%.16s\n",
               s, r, l, w / 1000, b / 1048576 / (w / 1e6), cu / 1000, cs / 1000, cv, ci,
               pu / 1000, ps / 1000, pv, pi, (cu + cs + pu + ps) / 1000, d }')
    echo "$row"
    echo "$row" >> "$work/rows"
    i=$((i + 1))
  done
  r=$((r + 1))
done

echo "medians: arm,wall_ms,MiBps,total_cpu_ms"
for arm in "$@"; do
  label=${arm%%=*}
  for col in 4 5 14; do
    awk -F, -v l="$label" -v c="$col" '$3 == l { print $c }' "$work/rows" | sort -n |
      awk '{ v[NR] = $1 } END { printf "%.1f\n", NR % 2 ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2 }'
  done | paste -sd, - | sed "s/^/  $label,/"
done
