# syntax=docker/dockerfile:1
#
# s390x cross toolchain image for the linux-s390x-gcc15-cxx23 preset,
# the big-endian lane. Mirrors ppc64le-gcc15.Dockerfile; qemu-s390x's
# default/max CPU carries the z14 vector-enhancements facility, and
# QEMU_CPU=z12 (pre-vector) runs the scalar fallback.
FROM base

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-15-s390x-linux-gnu qemu-user gcovr \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/s390x-linux-gnu-gcc-15 /usr/local/bin/s390x-linux-gnu-gcc \
    && ln -s /usr/bin/s390x-linux-gnu-g++-15 /usr/local/bin/s390x-linux-gnu-g++
