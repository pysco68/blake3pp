# syntax=docker/dockerfile:1
#
# riscv64 cross toolchain image for the linux-riscv64-gcc15-cxx23 preset.
# Mirrors arm64-gcc15.Dockerfile: the toolchain file invokes
# "qemu-riscv64 -L /usr/riscv64-linux-gnu" explicitly as
# CMAKE_CROSSCOMPILING_EMULATOR, so no binfmt registration is needed; the
# cross libc/libstdc++ sysroot arrives via the cross package's dependencies.
# qemu 10.x emulates RVV 1.0 with selectable VLEN (QEMU_CPU=max,vlen=<bits>),
# so the whole rvv dispatch matrix runs from this one image. What it cannot
# emulate is XTheadVector (RVV 0.7.1): mainline qemu never merged it; that
# would arrive as a separate, failable Xuantie-qemu stage.
FROM base

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      g++-15-riscv64-linux-gnu qemu-user; \
    rm -rf /var/lib/apt/lists/*; \
    update-alternatives --install /usr/bin/riscv64-linux-gnu-gcc \
      riscv64-linux-gnu-gcc /usr/bin/riscv64-linux-gnu-gcc-15 100 \
      --slave /usr/bin/riscv64-linux-gnu-g++ riscv64-linux-gnu-g++ \
        /usr/bin/riscv64-linux-gnu-g++-15; \
    qemu-riscv64 --version | head -1

# Optional, FAILABLE stage: T-Head's Xuantie qemu fork, the only emulator
# that executes XTheadVector encodings (mainline never merged the series).
# Source build from the fork's default branch: the project publishes no
# binary releases. Gated behind a build arg so the default image never
# attempts it:
#   docker buildx bake -f docker/docker-bake.hcl riscv64-gcc15 \
#     --set riscv64-gcc15.args.WITH_XUANTIE_QEMU=1 --load
# Installs as qemu-riscv64-xuantie next to the mainline qemu-riscv64.
ARG WITH_XUANTIE_QEMU=0
RUN set -eux; \
    if [ "$WITH_XUANTIE_QEMU" = "1" ]; then \
      apt-get update; \
      apt-get install -y --no-install-recommends \
        git build-essential python3 python3-venv python3-pip ninja-build \
        pkg-config libglib2.0-dev libpixman-1-dev zlib1g-dev flex bison; \
      git clone --depth 1 https://github.com/XUANTIE-RV/qemu.git /tmp/xqemu; \
      cd /tmp/xqemu; \
      ./configure --target-list=riscv64-linux-user --disable-docs \
        --prefix=/opt/xuantie-qemu; \
      make -j"$(nproc)"; \
      make install; \
      ln -s /opt/xuantie-qemu/bin/qemu-riscv64 \
        /usr/local/bin/qemu-riscv64-xuantie; \
      cd /; rm -rf /tmp/xqemu /var/lib/apt/lists/*; \
      qemu-riscv64-xuantie --version | head -1; \
    fi
