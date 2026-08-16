#!/usr/bin/env bash
# Build an MSan-instrumented libc++ into /opt/libcxx-msan.
#
# MemorySanitizer reports a use of uninitialised memory anywhere in the process,
# including inside code it did not instrument. An ordinary libc++ therefore
# produces a stream of false positives, which is why MSan needs its own build of
# the standard library. Nothing else in this image requires this, so it is opt-in.
#
#   sudo bash tools/build-msan-libcxx.sh          # uses CLANG_DEFAULT
#   sudo LLVM_VERSION=22 bash tools/build-msan-libcxx.sh
#
# Expect 10-30 minutes and roughly 3 GB of scratch space.

set -euo pipefail

LLVM_VERSION="${LLVM_VERSION:-22}"
PREFIX="${PREFIX:-/opt/libcxx-msan}"
SRC="${SRC:-/tmp/llvm-src}"
JOBS="${JOBS:-$(nproc)}"

CLANG="/usr/bin/clang-${LLVM_VERSION}"
CLANGXX="/usr/bin/clang++-${LLVM_VERSION}"

for bin in "$CLANG" "$CLANGXX"; do
  [ -x "$bin" ] || { echo "missing $bin; is clang-${LLVM_VERSION} installed?" >&2; exit 1; }
done

# Match the runtime sources to the compiler so the ABI lines up.
FULL_VERSION="$("$CLANG" --version | sed -n '1s/.*version \([0-9.]*\).*/\1/p')"
echo "building libc++ ${FULL_VERSION} with -fsanitize=memory -> ${PREFIX}"

if [ ! -d "$SRC" ]; then
  git clone --depth 1 --branch "llvmorg-${FULL_VERSION}" \
    https://github.com/llvm/llvm-project.git "$SRC" \
    || git clone --depth 1 --branch "release/${LLVM_VERSION}.x" \
         https://github.com/llvm/llvm-project.git "$SRC"
fi

# PER_TARGET_RUNTIME_DIR would install into lib/<triple>/, which the msan
# toolchain's plain -L${PREFIX}/lib would miss.
#
# Deliberately NOT building libunwind: MSan's report machinery unwinds the
# stack, and doing that through an MSan-INSTRUMENTED unwinder recurses
# (Registers_x86_64's memcpy -> __msan_memcpy -> unwind -> ...) straight
# into a SIGSEGV before any report prints. libc++abi uses the system
# libgcc_s unwinder instead, which is exactly what the sanitizer needs.
cmake -S "${SRC}/runtimes" -B /tmp/libcxx-msan-build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER="$CLANG" \
  -DCMAKE_CXX_COMPILER="$CLANGXX" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
  -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
  -DLLVM_USE_SANITIZER=MemoryWithOrigins \
  -DLIBCXX_ENABLE_SHARED=ON \
  -DLIBCXXABI_ENABLE_SHARED=ON \
  -DLIBCXX_ENABLE_STATIC=OFF \
  -DLIBCXXABI_ENABLE_STATIC=OFF \
  -DLIBCXX_INCLUDE_TESTS=OFF \
  -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF

cmake --build /tmp/libcxx-msan-build -j "$JOBS"
cmake --install /tmp/libcxx-msan-build

echo
echo "done. The 'msan' preset already points at:"
echo "  -nostdinc++ -isystem ${PREFIX}/include/c++/v1"
echo "  -L${PREFIX}/lib -Wl,-rpath,${PREFIX}/lib"
echo
echo "This lives in the image layer, not in a mount, so a container rebuild"
echo "discards it. Move the cmake+install steps into the Dockerfile if you"
echo "want it permanently."
