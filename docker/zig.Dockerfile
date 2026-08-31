# syntax=docker/dockerfile:1
#
# zig cc/c++ musl toolchain image for the linux-{,arm64-}zigmusl-cxx23-static
# presets and tools/make-release.sh. zig lands on PATH (the monolith had it
# only at /opt/zig, reachable via $ZIG alone); qemu-user runs the aarch64 and
# riscv64 test passes; the cross binutils provide per-arch strip for
# the release packaging.
FROM base

ARG ZIG_VERSION=0.16.0
RUN set -eux; \
    curl -fsSL "https://ziglang.org/download/${ZIG_VERSION}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz" \
      -o /tmp/zig.tar.xz; \
    mkdir -p /opt/zig; \
    tar -xJf /tmp/zig.tar.xz -C /opt/zig --strip-components=1; \
    rm -f /tmp/zig.tar.xz; \
    ln -s /opt/zig/zig /usr/local/bin/zig; \
    zig version

# The cross g++ packages are here for exactly ONE translation unit
# each: riscv64 for the XTheadVector (RVV 0.7.1) kernel, s390x for the
# z14 vxe kernel (clang/LLVM scalarizes xsimd's VXE ops; measured: 10
# vector instructions in the whole binary vs GCC's thousands including
# verllf hardware rotates). LLVM never merged XTheadVector, so
# zig/clang cannot compile it: the riscv64 musl static binary links a
# GCC-built object for that kernel (see cmake/ArchKernels.cmake,
# EXTERNAL_COMPILER) while zig builds everything else. Two compilers,
# one fat binary, because no single compiler speaks every vector dialect.
RUN apt-get update && apt-get install -y --no-install-recommends \
        qemu-user binutils-aarch64-linux-gnu binutils-riscv64-linux-gnu \
        g++-riscv64-linux-gnu g++-s390x-linux-gnu \
    && rm -rf /var/lib/apt/lists/*

# Prewarm zig's global cache for both musl targets. REQUIRED, not an
# optimization: tc containers are --rm-ephemeral, and without this layer every
# run rebuilds musl/compiler-rt/libc++ from scratch for minutes.
ENV ZIG_GLOBAL_CACHE_DIR=/opt/zig-cache
RUN set -eux; \
    mkdir -p /opt/zig-cache; \
    printf '#include <cstdio>\nint main() { std::puts("ok"); }\n' > /tmp/hello.cpp; \
    zig c++ -target x86_64-linux-musl -O2 -static /tmp/hello.cpp -o /tmp/hello-x86_64; \
    zig c++ -target aarch64-linux-musl -O2 -static /tmp/hello.cpp -o /tmp/hello-aarch64; \
    /tmp/hello-x86_64; \
    qemu-aarch64 /tmp/hello-aarch64; \
    rm -f /tmp/hello*; \
    chown -R 1000:1000 /opt/zig-cache
