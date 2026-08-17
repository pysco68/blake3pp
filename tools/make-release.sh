#!/bin/sh
# Assembles the binary release archives: fully static Linux tools
# (x86_64 + aarch64, musl + mimalloc via the zig toolchain presets).
# Run from the repo root; needs zig on PATH (or $ZIG) and qemu-aarch64
# for the aarch64 test pass. Library consumers build from source; these
# archives carry only the tools and benchmarks.
set -eu

VERSION=$(sed -n 's/^  VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt)
OUT=release
rm -rf "$OUT" && mkdir -p "$OUT"

for preset in linux-zigmusl-cxx23-static linux-arm64-zigmusl-cxx23-static; do
  case $preset in
    *arm64*) arch=aarch64 ;;
    *)       arch=x86_64 ;;
  esac
  cmake --preset "$preset"
  cmake --build --preset "$preset"
  ctest --preset "$preset" --output-on-failure

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
  elif command -v aarch64-linux-gnu-strip >/dev/null; then
    aarch64-linux-gnu-strip "$stage"/*
  fi
  cp README.md "$stage/"
  tar -C "$OUT" -czf "$OUT/$pkg.tar.gz" "$pkg"
  rm -rf "$stage"
  echo "packaged $OUT/$pkg.tar.gz"
done
ls -la "$OUT"
