#!/bin/sh
# Assembles the binary release archives: fully static Linux tools
# (x86_64 + aarch64, musl + mimalloc via the zig toolchain presets).
# Run from the repo root; needs zig on PATH (or $ZIG) and qemu-aarch64
# for the aarch64 test pass. Library consumers build from source; these
# archives carry only the tools and benchmarks.
#
# BLAKE3PP_CMAKE_RE=1 drives the same three steps through cmake-re instead
# of cmake (BLAKE3PP_CMAKE_RE_FLAGS, e.g. "--host --distributed", selects
# where it builds; default --host). cmake-re knows no presets, so
# tools/preset-args.py unrolls the preset into plain arguments, and it
# places a symlink at build/<preset> pointing into its mirror, which the
# packaging below reads through like any build tree.
set -eu

VERSION=$(sed -n 's/^  VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt)
OUT=release
rm -rf "$OUT" && mkdir -p "$OUT"

configure_build_test() {  # <preset>
  if [ "${BLAKE3PP_CMAKE_RE:-0}" = 1 ]; then
    flags=${BLAKE3PP_CMAKE_RE_FLAGS:---host}
    rm -rf "build/$1"
    # shellcheck disable=SC2046  # word-split on purpose: one argument per word
    cmake-re $flags -S . $(python3 tools/preset-args.py "$1")
    cmake-re --build "build/$1" $flags -j"$(nproc)"
    ctest --test-dir "build/$1" --output-on-failure
  else
    cmake --preset "$1"
    cmake --build --preset "$1"
    ctest --preset "$1" --output-on-failure
  fi
}

for preset in linux-zigmusl-cxx23-static linux-arm64-zigmusl-cxx23-static \
              linux-riscv64-zigmusl-cxx23-static; do
  case $preset in
    *arm64*)   arch=aarch64 ;;
    *riscv64*) arch=riscv64 ;;
    *)         arch=x86_64 ;;
  esac
  configure_build_test "$preset"

  pkg="blake3pp-$VERSION-linux-$arch-static"
  stage="$OUT/$pkg"
  mkdir -p "$stage"
  for bin in cli/blake3ppsum cli/blake3ppgen bench/blake3pp_bench \
             bench/blake3pp_bench_file; do
    cp "build/$preset/$bin" "$stage/"
  done
  # llvm-strip handles both architectures; fall back per-arch otherwise
  # (the zig toolchain image carries binutils + binutils-aarch64-linux-gnu,
  # not LLVM).
  STRIP=$(command -v llvm-strip || ls /usr/bin/llvm-strip-* 2>/dev/null | sort -V | tail -1 || true)
  if [ -n "$STRIP" ]; then
    "$STRIP" "$stage"/*
  elif [ "$arch" = x86_64 ]; then
    strip "$stage"/*
  elif command -v "$arch-linux-gnu-strip" >/dev/null; then
    "$arch-linux-gnu-strip" "$stage"/*
  else
    echo "make-release: no strip for $arch; archiving unstripped" >&2
  fi
  cp README.md "$stage/"
  tar -C "$OUT" -czf "$OUT/$pkg.tar.gz" "$pkg"
  rm -rf "$stage"
  echo "packaged $OUT/$pkg.tar.gz"
done
ls -la "$OUT"
