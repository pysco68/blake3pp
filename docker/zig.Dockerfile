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
    ln -s zig /opt/zig/clang; \
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
        binutils-powerpc64le-linux-gnu \
        g++-riscv64-linux-gnu g++-s390x-linux-gnu \
    && rm -rf /var/lib/apt/lists/*

# Prewarm zig's global cache for every musl target the presets build.
# REQUIRED, not an optimization: tc containers are --rm-ephemeral and so is
# a remote execution sandbox, and without the cache zig's first LINK for a
# target builds musl, compiler-rt and libc++ (measured: 23-26 s per
# target, ~70 MB of cache each). zig keys those builds on the target CPU
# model and the optimisation mode: -O1/-O2/-O3 share one, no -O / -O0 is
# another (CMake's compiler-id and ABI links carry no -O; 16 s more), -Os
# a third (unused here), and any -march/-mcpu on a link line a whole new
# set (~40 s; the build keeps such flags to compile-only steps, see
# cmake/KernelVariants.cmake). Both modes are warmed per target. Any uid
# may use the cache (the three local ones, and whatever uid a remote
# worker runs the wrapper as): the files are born world-writable (umask)
# and the finds touch only the ones zig gave an explicit mode, so the
# check layer below never re-records this one. ZIG_TARGETS is a build
# arg so a per-target image is a bake entry away.
ENV ZIG_GLOBAL_CACHE_DIR=/opt/zig-cache
ARG ZIG_TARGETS="x86_64 aarch64 riscv64 powerpc64le s390x"
COPY --chmod=0755 docker/wrappers/zig/musl-run /usr/local/bin/musl-run
RUN set -eux; umask 000; \
    mkdir -p /opt/zig-cache; chmod 0777 /opt/zig-cache; \
    printf '#include <cstdio>\nint main() { std::puts("ok"); }\n' > /tmp/hello.cpp; \
    for arch in ${ZIG_TARGETS}; do \
      zig c++ -target ${arch}-linux-musl -O2 -static /tmp/hello.cpp -o /tmp/hello-${arch}; \
      zig c++ -target ${arch}-linux-musl -static /tmp/hello.cpp -o /tmp/hello-${arch}-dbg; \
      musl-run ${arch} /tmp/hello-${arch}; \
      musl-run ${arch} /tmp/hello-${arch}-dbg; \
    done; \
    rm -f /tmp/hello*; \
    find /opt/zig-cache ! -perm -o=rw -exec chmod a+rwX {} +; \
    du -sh /opt/zig-cache

# The zig wrappers on PATH, so a cmake-re toolchain can name them bare
# (cmake-re copies toolchain files into its own environment directory,
# where a path relative to the file resolves nowhere). The /opt/zig/clang
# link above is what the wrappers report as the -cc1 program to
# dependency scanners (see docker/wrappers/zig). The checks compile
# through a wrapper as each user, which writes the prewarmed cache; one
# source per user, since a container is single-user and zig's manifest
# for a source is not meant to be handed from one uid to the next.
COPY --chmod=0755 docker/wrappers/zig/ /usr/local/bin/
RUN set -eux; \
    arch=${ZIG_TARGETS%% *}; \
    for user in vscode tipi tipi-rbe; do \
      printf '/* %s */ int main() { return 0; }\n' "${user}" > /tmp/probe-${user}.c; \
      su "${user}" -c "umask 000; ${arch}-linux-musl-clang -O2 -static /tmp/probe-${user}.c -o /tmp/probe-${user} && musl-run ${arch} /tmp/probe-${user}"; \
    done; \
    rm -f /tmp/probe*; \
    find /opt/zig-cache ! -perm -o=rw -exec chmod a+rwX {} +; \
    test "$(find /opt/zig-cache ! -perm -o=rw | wc -l)" = 0
