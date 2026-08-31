# syntax=docker/dockerfile:1
#
# ppc64le cross toolchain image for the linux-ppc64le-gcc15-cxx23 preset.
# Mirrors riscv64-gcc15.Dockerfile (minus the vendor-qemu stage): the
# toolchain file invokes "qemu-ppc64le -L /usr/powerpc64le-linux-gnu"
# explicitly as CMAKE_CROSSCOMPILING_EMULATOR; QEMU_CPU=power8/9/10
# selects the CPU generation for the vsx dispatch matrix.
FROM base

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-15-powerpc64le-linux-gnu qemu-user \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/powerpc64le-linux-gnu-gcc-15 /usr/local/bin/powerpc64le-linux-gnu-gcc \
    && ln -s /usr/bin/powerpc64le-linux-gnu-g++-15 /usr/local/bin/powerpc64le-linux-gnu-g++
