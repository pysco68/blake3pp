# syntax=docker/dockerfile:1
#
# riscv64 cross toolchain image for the linux-riscv64-gcc15-cxx23 preset.
# Mirrors arm64-gcc15.Dockerfile: the toolchain file invokes
# "qemu-riscv64 -L /usr/riscv64-linux-gnu" explicitly as
# CMAKE_CROSSCOMPILING_EMULATOR, so no binfmt registration is needed; the
# cross libc/libstdc++ sysroot arrives via the cross package's dependencies.
# qemu 10.x emulates RVV 1.0 with selectable VLEN (QEMU_CPU=max,vlen=<bits>),
# so the whole rvv dispatch matrix runs from this one image.
#
# What mainline qemu cannot emulate is XTheadVector (draft RVV 0.7.1); the
# series was never merged. The stage below builds T-Head's Xuantie fork for
# that, gated behind a build arg (the default image skips it):
#   docker buildx bake -f docker/docker-bake.hcl riscv64-gcc15 \
#     --set riscv64-gcc15.args.WITH_XUANTIE_QEMU=1 --load
# The fork builds on pinned Ubuntu 22.04, an era-appropriate toolchain,
# because against 26.04 it dies of bit-rot twice over (-Werror on
# discarded-qualifiers under new GCC, struct sched_attr redefinition
# against glibc >= 2.41); glib is forward-compatible, so the finished
# binary runs fine in the 26.04 image with libglib2.0-0 installed.
# Installs as qemu-riscv64-xuantie next to the mainline qemu-riscv64.

FROM ubuntu:22.04@sha256:2edbbc5dc405e9612ba3584ce95480277e3eb374407b5505fe26f17df77c7dbc AS xqemu
ARG WITH_XUANTIE_QEMU=0
RUN set -eux; \
    mkdir -p /opt/xuantie-qemu; \
    if [ "$WITH_XUANTIE_QEMU" = "1" ]; then \
      apt-get update; \
      apt-get install -y --no-install-recommends \
        ca-certificates git build-essential python3 python3-venv ninja-build \
        pkg-config libglib2.0-dev libpixman-1-dev zlib1g-dev flex bison; \
      git clone --depth 1 https://github.com/XUANTIE-RV/qemu.git /tmp/xqemu; \
      cd /tmp/xqemu; \
      ./configure --target-list=riscv64-linux-user --disable-docs \
        --disable-werror --prefix=/opt/xuantie-qemu; \
      make -j"$(nproc)"; \
      make install; \
      cd /; rm -rf /tmp/xqemu; \
    fi

FROM base

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      g++-15-riscv64-linux-gnu qemu-user libglib2.0-0; \
    rm -rf /var/lib/apt/lists/*; \
    update-alternatives --install /usr/bin/riscv64-linux-gnu-gcc \
      riscv64-linux-gnu-gcc /usr/bin/riscv64-linux-gnu-gcc-15 100 \
      --slave /usr/bin/riscv64-linux-gnu-g++ riscv64-linux-gnu-g++ \
        /usr/bin/riscv64-linux-gnu-g++-15; \
    qemu-riscv64 --version | head -1

# Present (and on PATH) only when the arg-gated stage actually built it.
COPY --from=xqemu /opt/xuantie-qemu /opt/xuantie-qemu
RUN set -eux; \
    if [ -x /opt/xuantie-qemu/bin/qemu-riscv64 ]; then \
      ln -s /opt/xuantie-qemu/bin/qemu-riscv64 \
        /usr/local/bin/qemu-riscv64-xuantie; \
      qemu-riscv64-xuantie --version | head -1; \
    fi
