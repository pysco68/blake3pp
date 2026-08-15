# syntax=docker/dockerfile:1
#
# Ubuntu 26.04 LTS "Resolute Raccoon".
#   GCC   11..16  from the default repos
#   Clang 17..22  from the default repos; 22 and the 23 snapshot via apt.llvm.org
#
# SIZE WARNING: every extra Clang is roughly 1-1.5 GB installed. The default
# selection below lands somewhere around 9-11 GB. Trim CLANG_VERSIONS if that
# hurts more than the matrix helps.
#
ARG UBUNTU_VERSION=26.04
FROM ubuntu:${UBUNTU_VERSION}

# --- compiler matrix ---------------------------------------------------------
# Space-separated, oldest first. Must exist in the repos; the build fails loudly
# with the available list if one does not.
ARG GCC_VERSIONS="12 14 16"
ARG CLANG_VERSIONS="18 20 22"
ARG GCC_DEFAULT=16
ARG CLANG_DEFAULT=22
# apt.llvm.org only carries the dev branch + last two releases, so it is the
# source for 22/23 while older versions come from Ubuntu's own archive.
ARG LLVM_APT_VERSION=22
ARG INSTALL_LLVM_SNAPSHOT=0
ARG LLVM_SNAPSHOT_VERSION=23
# --- build tools -------------------------------------------------------------
ARG CMAKE_VERSION=4.3.2
ARG NINJA_VERSION=1.13.2
# -----------------------------------------------------------------------------

ARG USERNAME=vscode
ARG USER_UID=1000
ARG USER_GID=1000

ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# ---------------------------------------------------------------- base packages
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl wget gnupg lsb-release software-properties-common \
        apt-transport-https \
        git git-lfs openssh-client \
        less nano vim-tiny procps psmisc file unzip zip xz-utils zstd \
        build-essential pkg-config make mold \
        python3 python3-venv python3-pip pipx \
        locales sudo bash-completion man-db \
        libssl-dev zlib1g-dev \
    && sed -i '/en_US.UTF-8/s/^# //g' /etc/locale.gen && locale-gen \
    && rm -rf /var/lib/apt/lists/*

ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8

# ---------------------------------------------- debugging / analysis / dynamic
# valgrind covers memcheck, helgrind, DRD, massif and callgrind; the compiler
# sanitizers cover the rest. gcovr/lcov for coverage, iwyu + cppcheck for static
# passes, heaptrack for allocation profiles, hyperfine for A/B timing.
RUN apt-get update && apt-get install -y --no-install-recommends \
        gdb valgrind strace ltrace \
        cppcheck iwyu \
        lcov gcovr \
        heaptrack hyperfine \
        binutils elfutils \
        libbenchmark-dev libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

# ------------------------------------------------------------------------- GCC
# libgcc-N-dev Depends on libasan / libubsan / libtsan / liblsan, so each GCC
# drags in its own matching sanitizer runtimes even with --no-install-recommends.
RUN set -eux; \
    apt-get update; \
    for v in ${GCC_VERSIONS}; do \
      if ! apt-get install -y --no-install-recommends \
             "gcc-${v}" "g++-${v}" "libstdc++-${v}-dev"; then \
        echo "=== g++-${v} is not available. Available g++ packages:"; \
        apt-cache search --names-only '^g\+\+-[0-9]+$' | sort -V; \
        echo "=== consider: add-apt-repository ppa:ubuntu-toolchain-r/test"; \
        exit 1; \
      fi; \
    done; \
    rm -rf /var/lib/apt/lists/*; \
    update-alternatives --install /usr/bin/gcc gcc "/usr/bin/gcc-${GCC_DEFAULT}" 100 \
      --slave /usr/bin/g++  g++  "/usr/bin/g++-${GCC_DEFAULT}" \
      --slave /usr/bin/gcov gcov "/usr/bin/gcov-${GCC_DEFAULT}"

# ----------------------------------------------------------------------- Clang
RUN set -eux; \
    wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh; \
    chmod +x /tmp/llvm.sh; \
    /tmp/llvm.sh "${LLVM_APT_VERSION}"; \
    if [ "${INSTALL_LLVM_SNAPSHOT}" = "1" ]; then /tmp/llvm.sh "${LLVM_SNAPSHOT_VERSION}"; fi; \
    rm -f /tmp/llvm.sh; \
    apt-get update; \
    for v in ${CLANG_VERSIONS}; do \
      if ! apt-get install -y --no-install-recommends \
             "clang-${v}" "clang-tools-${v}" \
             "libclang-rt-${v}-dev" \
             "libc++-${v}-dev" "libc++abi-${v}-dev" \
             "lld-${v}" "llvm-${v}"; then \
        echo "=== clang-${v} is not available. Available clang packages:"; \
        apt-cache search --names-only '^clang-[0-9]+$' | sort -V; \
        exit 1; \
      fi; \
    done; \
    apt-get install -y --no-install-recommends \
      "clangd-${CLANG_DEFAULT}" "clang-tidy-${CLANG_DEFAULT}" \
      "clang-format-${CLANG_DEFAULT}" "lldb-${CLANG_DEFAULT}" \
      "libfuzzer-${CLANG_DEFAULT}-dev" "libomp-${CLANG_DEFAULT}-dev"; \
    rm -rf /var/lib/apt/lists/*; \
    update-alternatives --install /usr/bin/clang clang "/usr/bin/clang-${CLANG_DEFAULT}" 100 \
      --slave /usr/bin/clang++      clang++      "/usr/bin/clang++-${CLANG_DEFAULT}" \
      --slave /usr/bin/clangd       clangd       "/usr/bin/clangd-${CLANG_DEFAULT}" \
      --slave /usr/bin/clang-tidy   clang-tidy   "/usr/bin/clang-tidy-${CLANG_DEFAULT}" \
      --slave /usr/bin/clang-format clang-format "/usr/bin/clang-format-${CLANG_DEFAULT}" \
      --slave /usr/bin/lld          lld          "/usr/bin/lld-${CLANG_DEFAULT}" \
      --slave /usr/bin/ld.lld       ld.lld       "/usr/bin/ld.lld-${CLANG_DEFAULT}" \
      --slave /usr/bin/lldb         lldb         "/usr/bin/lldb-${CLANG_DEFAULT}"; \
    ln -sf "/usr/lib/llvm-${CLANG_DEFAULT}/bin/llvm-symbolizer" /usr/local/bin/llvm-symbolizer; \
    ln -sf "/usr/lib/llvm-${CLANG_DEFAULT}/bin/llvm-cov"        /usr/local/bin/llvm-cov; \
    ln -sf "/usr/lib/llvm-${CLANG_DEFAULT}/bin/llvm-profdata"   /usr/local/bin/llvm-profdata

# ----------------------------------------------------------------------- CMake
RUN set -eux; \
    case "$(dpkg --print-architecture)" in \
      amd64) CM_ARCH=x86_64 ;; \
      arm64) CM_ARCH=aarch64 ;; \
      *) echo "unsupported arch" >&2; exit 1 ;; \
    esac; \
    wget -qO /tmp/cmake.sh \
      "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${CM_ARCH}.sh"; \
    sh /tmp/cmake.sh --skip-license --prefix=/usr/local; \
    rm -f /tmp/cmake.sh; \
    cmake --version

# ----------------------------------------------------------------------- Ninja
RUN set -eux; \
    case "$(dpkg --print-architecture)" in \
      amd64) NJ_ZIP=ninja-linux.zip ;; \
      arm64) NJ_ZIP=ninja-linux-aarch64.zip ;; \
      *) echo "unsupported arch" >&2; exit 1 ;; \
    esac; \
    wget -qO /tmp/ninja.zip \
      "https://github.com/ninja-build/ninja/releases/download/v${NINJA_VERSION}/${NJ_ZIP}"; \
    unzip -q -o /tmp/ninja.zip -d /usr/local/bin; \
    chmod +x /usr/local/bin/ninja; \
    rm -f /tmp/ninja.zip; \
    ninja --version

# ------------------------------------------------------------------- dev user
RUN set -eux; \
    if existing="$(getent passwd ${USER_UID} | cut -d: -f1)"; [ -n "${existing:-}" ]; then \
      userdel -r "$existing" 2>/dev/null || userdel "$existing"; \
    fi; \
    if ! getent group ${USER_GID} >/dev/null; then groupadd --gid ${USER_GID} ${USERNAME}; fi; \
    useradd --uid ${USER_UID} --gid ${USER_GID} --create-home --shell /bin/bash ${USERNAME}; \
    echo "${USERNAME} ALL=(root) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME}; \
    chmod 0440 /etc/sudoers.d/${USERNAME}; \
    install -d -o ${USERNAME} -g ${USERNAME} \
      /home/${USERNAME}/.local/bin \
      /home/${USERNAME}/.claude

# Recorded so tools/feature-matrix.py and the presets can see what this image has.
ENV DEVCONTAINER_GCC_VERSIONS="${GCC_VERSIONS}" \
    DEVCONTAINER_CLANG_VERSIONS="${CLANG_VERSIONS}" \
    PATH=/home/${USERNAME}/.local/bin:${PATH} \
    CC=/usr/bin/gcc \
    CXX=/usr/bin/g++ \
    CMAKE_GENERATOR=Ninja

USER root
