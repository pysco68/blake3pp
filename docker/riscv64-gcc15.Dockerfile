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
