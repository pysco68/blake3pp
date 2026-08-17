# syntax=docker/dockerfile:1
#
# Shared base recipe for every blake3pp toolchain image. Instantiated once per
# Ubuntu release by docker-bake.hcl (BASE_IMAGE arg): each compiler sits on
# the distro contemporary to it, so its binaries carry the oldest practical
# glibc floor for that vintage (22.04 -> 2.35, 24.04 -> 2.39, 26.04 -> 2.42).
# Binary compatibility is one-directional by design: old-floor artifacts run
# everywhere newer; the reverse is unsupported.
#
# Keep this list identical across releases and compiler-agnostic; anything
# compiler-specific belongs in the family Dockerfiles. Notably absent:
#   - build-essential: a stray host gcc would shadow the image's compiler.
#   - mold: no toolchain file uses it (GCC stays on bfd, clang brings lld-N).
#   - gtest/benchmark dev packages: dependencies come via FetchContent, which
#     is also why images need git + network at configure time.
ARG BASE_IMAGE=ubuntu:26.04
FROM ${BASE_IMAGE}

ARG CMAKE_VERSION=4.3.2
ARG NINJA_VERSION=1.13.2
ARG USERNAME=vscode
ARG USER_UID=1000
ARG USER_GID=1000

ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl wget git \
        less procps file unzip xz-utils zstd \
        make pkg-config binutils libc6-dev \
        python3 locales sudo \
    && sed -i '/en_US.UTF-8/s/^# //g' /etc/locale.gen && locale-gen \
    && rm -rf /var/lib/apt/lists/*

ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8

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

# uid/gid 1000 to match the devcontainer and the workspace bind mount; 24.04+
# ships a default "ubuntu" user at uid 1000 that has to be evicted first.
# No USER switch: tools/tc runs containers with -u 1000:1000.
RUN set -eux; \
    if existing="$(getent passwd ${USER_UID} | cut -d: -f1)"; [ -n "${existing:-}" ]; then \
      userdel -r "$existing" 2>/dev/null || userdel "$existing"; \
    fi; \
    if ! getent group ${USER_GID} >/dev/null; then groupadd --gid ${USER_GID} ${USERNAME}; fi; \
    useradd --uid ${USER_UID} --gid ${USER_GID} --create-home --shell /bin/bash ${USERNAME}; \
    echo "${USERNAME} ALL=(root) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME}; \
    chmod 0440 /etc/sudoers.d/${USERNAME}; \
    chown -R ${USERNAME}:${USERNAME} /home/${USERNAME}

ENV CMAKE_GENERATOR=Ninja
