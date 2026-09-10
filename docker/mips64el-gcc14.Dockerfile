# syntax=docker/dockerfile:1
#
# mips64el cross toolchain image for the linux-mips64el-gcc14-cxx23 preset.
# Mirrors ppc64le-gcc15.Dockerfile with two deliberate differences.
#
# Debian rather than Ubuntu (see base-debian13 in docker-bake.hcl): no
# Ubuntu release packages a mips64el cross compiler.
#
# GCC 14 rather than 15: Debian packages no newer mips64el cross either.
# It costs the lane nothing, because the msa kernel pins the vector-
# extension provider rather than a simd library (no std provider deduces a
# width on this target and xsimd has no MSA backend).
#
# The toolchain file invokes "qemu-mips64el -L /usr/mips64el-linux-gnuabi64"
# as CMAKE_CROSSCOMPILING_EMULATOR. QEMU_CPU selects the model: unset gives
# an r2-era core with no MSA, which must dispatch to scalar, and
# Loongson-3A4000 is the one qemu model that implements MSA correctly AND
# sets HWCAP_MIPS_MSA. Not every model that decodes MSA computes it: qemu's
# XBurstR2 runs the encodings, reports no MSA in HWCAP, and returns wrong
# answers, which is exactly the case the capability check exists to refuse.
FROM base

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-14-mips64el-linux-gnuabi64 qemu-user gcovr \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/mips64el-linux-gnuabi64-gcc-14 /usr/local/bin/mips64el-linux-gnuabi64-gcc \
    && ln -s /usr/bin/mips64el-linux-gnuabi64-g++-14 /usr/local/bin/mips64el-linux-gnuabi64-g++
