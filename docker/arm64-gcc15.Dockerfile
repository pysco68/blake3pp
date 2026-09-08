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
      g++-15-aarch64-linux-gnu qemu-user gcovr; \
    rm -rf /var/lib/apt/lists/*; \
    update-alternatives --install /usr/bin/aarch64-linux-gnu-gcc \
      aarch64-linux-gnu-gcc /usr/bin/aarch64-linux-gnu-gcc-15 100 \
      --slave /usr/bin/aarch64-linux-gnu-g++ aarch64-linux-gnu-g++ \
        /usr/bin/aarch64-linux-gnu-g++-15; \
    qemu-aarch64 --version | head -1

# The gcc/g++ drivers behind a wrapper that keeps reclient's dependency
# scanner from tripping over the aarch64 SME keyword __arm_streaming (the
# scanner's compiler probe is otherwise rejected and no action can be
# distributed); see the script for the details.
COPY --chmod=0755 docker/wrappers/arm64-gcc15/reclient-gcc-driver /usr/local/bin/reclient-gcc-driver
RUN set -eux; \
    for d in aarch64-linux-gnu-gcc-15 aarch64-linux-gnu-g++-15; do \
      mv /usr/bin/$d /usr/bin/$d.real; \
      cp /usr/local/bin/reclient-gcc-driver /usr/bin/$d; \
    done; \
    aarch64-linux-gnu-gcc --version | head -1; \
    printf '__has_attribute(__arm_streaming)\n' > /tmp/probe.c; \
    cp /tmp/probe.c /tmp/goma_compiler_proxy_check_features_test; \
    aarch64-linux-gnu-gcc -x c -E /tmp/goma_compiler_proxy_check_features_test > /dev/null; \
    rm -f /tmp/probe.c /tmp/goma_compiler_proxy_check_features_test
