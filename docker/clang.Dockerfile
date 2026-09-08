# syntax=docker/dockerfile:1
#
# Clang toolchain image, parameterized by CLANG_VERSION. The "base" build
# context is wired up by docker-bake.hcl to the per-release base instantiation
# (clang18/20 -> 24.04, clang22 -> 26.04).
#
# Sources: clang-18 comes from the noble archive, clang-22 from the resolute
# archive; only clang-20 needs the apt.llvm.org llvm-toolchain-noble-20 pocket
# (USE_LLVM_APT=1). Each image carries exactly one libc++, which dissolves the
# monolith's "every clang parses the newest clang's libc++" constraint.
#
# GCC_PIN installs the g++ major that the matching -libstdcxx toolchain files
# pin via --gcc-install-dir (/usr/lib/gcc/x86_64-linux-gnu/<pin>): 14 for
# clang18/20, 16 for clang22.
FROM base AS toolchain

ARG CLANG_VERSION
ARG USE_LLVM_APT=0
ARG GCC_PIN=16
RUN set -eux; \
    if [ "${USE_LLVM_APT}" = "1" ]; then \
      apt-get update; \
      apt-get install -y --no-install-recommends gnupg lsb-release software-properties-common; \
      wget -q --tries=5 --waitretry=10 -O /tmp/llvm.sh https://apt.llvm.org/llvm.sh; \
      chmod +x /tmp/llvm.sh; \
      /tmp/llvm.sh "${CLANG_VERSION}"; \
      rm -f /tmp/llvm.sh; \
    fi; \
    apt-get update; \
    if ! apt-get install -y --no-install-recommends \
           "clang-${CLANG_VERSION}" "clang-tools-${CLANG_VERSION}" \
           "libclang-rt-${CLANG_VERSION}-dev" \
           "libc++-${CLANG_VERSION}-dev" "libc++abi-${CLANG_VERSION}-dev" \
           "lld-${CLANG_VERSION}" "llvm-${CLANG_VERSION}" \
           "g++-${GCC_PIN}"; then \
      echo "=== clang-${CLANG_VERSION} is not available. Available clang packages:"; \
      apt-cache search --names-only '^clang-[0-9]+$' | sort -V; \
      exit 1; \
    fi; \
    rm -rf /var/lib/apt/lists/*; \
    update-alternatives --install /usr/bin/clang clang "/usr/bin/clang-${CLANG_VERSION}" 100 \
      --slave /usr/bin/clang++ clang++ "/usr/bin/clang++-${CLANG_VERSION}" \
      --slave /usr/bin/lld     lld     "/usr/bin/lld-${CLANG_VERSION}" \
      --slave /usr/bin/ld.lld  ld.lld  "/usr/bin/ld.lld-${CLANG_VERSION}"; \
    ln -sf "/usr/lib/llvm-${CLANG_VERSION}/bin/llvm-symbolizer" /usr/local/bin/llvm-symbolizer; \
    ln -sf "/usr/lib/llvm-${CLANG_VERSION}/bin/llvm-cov"        /usr/local/bin/llvm-cov; \
    ln -sf "/usr/lib/llvm-${CLANG_VERSION}/bin/llvm-profdata"   /usr/local/bin/llvm-profdata; \
    ln -sf "/usr/lib/llvm-${CLANG_VERSION}/bin/llvm-strip"      /usr/local/bin/llvm-strip

# Extras for the newest clang only (fuzzer/omp presets, debuggers, and Intel
# SDE for AVX-512 runs; qemu-user's TCG never implemented AVX-512).
ARG WITH_EXTRAS=0
ARG SDE_URL="https://downloadmirror.intel.com/924984/sde-external-10.13.1-2026-07-28-lin.tar.xz"
RUN set -eux; \
    if [ "${WITH_EXTRAS}" = "1" ]; then \
      apt-get update; \
      apt-get install -y --no-install-recommends \
        "libfuzzer-${CLANG_VERSION}-dev" "libomp-${CLANG_VERSION}-dev" \
        gdb valgrind strace; \
      rm -rf /var/lib/apt/lists/*; \
      curl -fsSL "${SDE_URL}" -o /tmp/sde.tar.xz; \
      mkdir -p /opt/sde; \
      tar -xJf /tmp/sde.tar.xz -C /opt/sde --strip-components=1; \
      rm -f /tmp/sde.tar.xz; \
      ln -s /opt/sde/sde64 /usr/local/bin/sde64; \
      sde64 -version >/dev/null; \
    fi

ENV CC=/usr/bin/clang \
    CXX=/usr/bin/clang++

# ---- MSan-instrumented libc++ (published stage for clang22 only) ------------
# build-msan-libcxx.sh clones llvm at the exact clang version and builds
# libcxx+libcxxabi with MemoryWithOrigins; ~3 GB of scratch stays in this
# stage, only the 25 MB install is copied into the published image.
FROM toolchain AS msan-build
ARG CLANG_VERSION
COPY tools/build-msan-libcxx.sh /tmp/build-msan-libcxx.sh
RUN LLVM_VERSION="${CLANG_VERSION}" bash /tmp/build-msan-libcxx.sh \
    && rm -rf /tmp/llvm-src /tmp/libcxx-msan-build /tmp/build-msan-libcxx.sh

FROM toolchain AS msan
COPY --from=msan-build /opt/libcxx-msan /opt/libcxx-msan
