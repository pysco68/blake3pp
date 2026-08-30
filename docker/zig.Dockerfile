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

RUN apt-get update && apt-get install -y --no-install-recommends \
        qemu-user binutils-aarch64-linux-gnu binutils-riscv64-linux-gnu \
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
