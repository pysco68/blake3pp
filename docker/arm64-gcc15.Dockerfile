# syntax=docker/dockerfile:1
#
# aarch64 cross toolchain image for the linux-arm64-gcc15-cxx23 preset.
# The toolchain file invokes "qemu-aarch64 -L /usr/aarch64-linux-gnu"
# explicitly as CMAKE_CROSSCOMPILING_EMULATOR, so no binfmt registration is
# needed; the cross libc/libstdc++ sysroot arrives via the cross package's
# dependencies.
FROM base

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      g++-15-aarch64-linux-gnu qemu-user; \
    rm -rf /var/lib/apt/lists/*; \
    update-alternatives --install /usr/bin/aarch64-linux-gnu-gcc \
      aarch64-linux-gnu-gcc /usr/bin/aarch64-linux-gnu-gcc-15 100 \
      --slave /usr/bin/aarch64-linux-gnu-g++ aarch64-linux-gnu-g++ \
        /usr/bin/aarch64-linux-gnu-g++-15; \
    qemu-aarch64 --version | head -1
