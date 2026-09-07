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

# The zig wrappers on PATH, so a cmake-re toolchain can name them bare
# (cmake-re copies toolchain files into its own environment directory,
# where a path relative to the file resolves nowhere). zig's prewarmed
# cache gets the base image's shared-group treatment, since any of the
# three uids may drive a build; the checks compile through a wrapper as
# each user, which writes the cache.
COPY tools/zig-wrappers/ /usr/local/bin/
RUN set -eux; \
    chgrp -R tipi /opt/zig-cache; \
    chmod -R g+rwX /opt/zig-cache; \
    find /opt/zig-cache -type d -exec chmod g+s {} +; \
    printf 'int main() { return 0; }\n' > /tmp/probe.c; \
    for user in vscode tipi tipi-rbe; do \
      su "${user}" -c "zig-cc-x86_64-musl -O2 -static /tmp/probe.c -o /tmp/probe-${user} && /tmp/probe-${user}"; \
    done; \
    su tipi-rbe -c "zig-cc-aarch64-musl -O2 -static /tmp/probe.c -o /tmp/probe-a64 && qemu-aarch64 /tmp/probe-a64"; \
    rm -f /tmp/probe*
