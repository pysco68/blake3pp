# syntax=docker/dockerfile:1
#
# GCC toolchain image, parameterized by GCC_VERSION. The "base" build context
# is wired up by docker-bake.hcl to the per-release base instantiation
# (gcc14 -> 24.04, gcc16 -> 26.04).
#
# libgcc-N-dev Depends on libasan / libubsan / libtsan / liblsan, so the image
# gets its matching sanitizer runtimes even with --no-install-recommends.
FROM base

ARG GCC_VERSION
RUN set -eux; \
    apt-get update; \
    if ! apt-get install -y --no-install-recommends \
           "gcc-${GCC_VERSION}" "g++-${GCC_VERSION}" "libstdc++-${GCC_VERSION}-dev"; then \
      echo "=== g++-${GCC_VERSION} is not available. Available g++ packages:"; \
      apt-cache search --names-only '^g\+\+-[0-9]+$' | sort -V; \
      exit 1; \
    fi; \
    rm -rf /var/lib/apt/lists/*; \
    update-alternatives --install /usr/bin/gcc gcc "/usr/bin/gcc-${GCC_VERSION}" 100 \
      --slave /usr/bin/g++  g++  "/usr/bin/g++-${GCC_VERSION}" \
      --slave /usr/bin/gcov gcov "/usr/bin/gcov-${GCC_VERSION}"

# Extras for the newest GCC only (the instrumented/coverage presets live
# there): debuggers, coverage reporters, and Intel SDE for AVX-512 runs.
# qemu-user's TCG never implemented AVX-512, so SDE is the only option.
ARG WITH_EXTRAS=0
ARG SDE_URL="https://downloadmirror.intel.com/924984/sde-external-10.13.1-2026-07-28-lin.tar.xz"
RUN set -eux; \
    if [ "${WITH_EXTRAS}" = "1" ]; then \
      apt-get update; \
      apt-get install -y --no-install-recommends gdb valgrind strace lcov gcovr; \
      rm -rf /var/lib/apt/lists/*; \
      curl -fsSL "${SDE_URL}" -o /tmp/sde.tar.xz; \
      mkdir -p /opt/sde; \
      tar -xJf /tmp/sde.tar.xz -C /opt/sde --strip-components=1; \
      rm -f /tmp/sde.tar.xz; \
      ln -s /opt/sde/sde64 /usr/local/bin/sde64; \
      sde64 -version >/dev/null; \
    fi

ENV CC=/usr/bin/gcc \
    CXX=/usr/bin/g++
