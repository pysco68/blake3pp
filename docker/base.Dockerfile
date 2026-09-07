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

# A docker CLIENT (the static tarball; Ubuntu's only docker package is the
# whole runtime), for cmake-re: given the host's socket it resolves an
# environment's image locally, pulls it, or builds its "digital twin" from
# it, and writes the container.lock. A GitHub job container gets the
# socket by default and its own image is already on the runner's daemon,
# so cmake-re --remote works from inside it with nothing installed on the
# runner. openssh-server is what cmake-re's local containerized mode runs
# the build through: it starts the environment container with
# `ssh-keygen -A; /usr/sbin/sshd -D` and talks to it over a published
# port 22 (tipi's images are set up the same way; ssh-rsa stays accepted
# for its generated keys). Nothing listens at image-build time.
ARG DOCKER_CLI_VERSION=28.3.3
RUN set -eux; \
    case "$(dpkg --print-architecture)" in \
      amd64) DK_ARCH=x86_64 ;; \
      arm64) DK_ARCH=aarch64 ;; \
      *) echo "unsupported arch" >&2; exit 1 ;; \
    esac; \
    curl -fsSL --retry 5 --retry-all-errors \
      "https://download.docker.com/linux/static/stable/${DK_ARCH}/docker-${DOCKER_CLI_VERSION}.tgz" \
      -o /tmp/docker.tgz; \
    tar -xzf /tmp/docker.tgz -C /tmp docker/docker; \
    install -m 0755 /tmp/docker/docker /usr/local/bin/docker; \
    rm -rf /tmp/docker /tmp/docker.tgz; \
    docker --version; \
    apt-get update && apt-get install -y --no-install-recommends openssh-server \
    && rm -rf /var/lib/apt/lists/*; \
    mkdir -p /run/sshd /etc/ssh/sshd_config.d; \
    printf 'PubkeyAcceptedKeyTypes +ssh-rsa\n' > /etc/ssh/sshd_config.d/add-ssh-rsa.conf; \
    ssh-keygen -A; /usr/sbin/sshd -t; rm -f /etc/ssh/ssh_host_*

# tipi cmake-re (portable package): the remote-execution front end that
# replaced ccache in the plan. The zip is flat, so its binaries go straight
# to /usr/local/bin; LICENSE/NOTICE/version.txt keep company under
# share/doc. The tipi-*-driver files are one binary dispatching on
# argv[0]; the zip lacks the tipi-test-driver name that cmake-re registers
# as CMAKE_TEST_LAUNCHER (ctest reports every test "Not Run" without it),
# so that one is a link. x86_64 package only; an arm64 base gets none.
#
# TIPI_DISTRO_MODE=none, set before any of the binaries runs for the first
# time: without it that first run provisions tipi's whole distro into
# /usr/local/share/.tipi (its own cmake, ninja, make, reclient, ~900 MB,
# fetched from third-party buckets) next to the ones this image already
# carries. In "none" mode it uses what is on PATH.
ARG CMAKE_RE_VERSION=0.0.87
ENV TIPI_DISTRO_MODE=none
RUN set -eux; \
    if [ "$(dpkg --print-architecture)" != amd64 ]; then echo "cmake-re: x86_64 only, skipping"; exit 0; fi; \
    curl -fsSL --retry 5 --retry-all-errors \
      "https://github.com/tipi-build/cli/releases/download/v${CMAKE_RE_VERSION}/cmake-re-portable-v${CMAKE_RE_VERSION}-linux-x86_64.zip" \
      -o /tmp/cmake-re.zip; \
    mkdir -p /tmp/cmake-re /usr/local/share/doc/cmake-re; \
    unzip -q /tmp/cmake-re.zip -d /tmp/cmake-re; \
    mv /tmp/cmake-re/LICENSE /tmp/cmake-re/NOTICE /tmp/cmake-re/version.txt \
       /usr/local/share/doc/cmake-re/; \
    install -m 0755 /tmp/cmake-re/* /usr/local/bin/; \
    rm -rf /tmp/cmake-re /tmp/cmake-re.zip; \
    ln -s tipi /usr/local/bin/tipi-test-driver; \
    cmake-re --version

# tipi keeps its state in /usr/local/share/.tipi (hard-wired; it refuses to
# run without it) and, depending on how a container is entered, any of
# three uids is the one using it: vscode (1000, tools/tc and the
# devcontainer), tipi (1001) or tipi-rbe (108, the RBE worker uid). Each
# container is ephemeral, so the requirement is only that every one of
# them can write there, not that they share files: one group holds all
# three, the tree is group-writable, and setgid keeps whatever gets
# created inside it in that group. Family images that add other shared
# caches (zig's) apply the same treatment. The checks run as each user.
RUN set -eux; \
    groupadd --gid 1001 tipi; \
    useradd --system --uid 1001 --gid tipi --create-home --shell /bin/bash tipi; \
    groupadd --gid 108 tipi-rbe; \
    useradd --system --uid 108 --gid tipi-rbe -G tipi --create-home --shell /bin/bash tipi-rbe; \
    usermod -aG tipi ${USERNAME}; \
    mkdir -p /home/tipi/.ssh; chown tipi:tipi /home/tipi/.ssh; chmod 0700 /home/tipi/.ssh; \
    for uid in ${USER_UID} 1001 108; do \
      mkdir -p /run/user/${uid}; chown ${uid}:${uid} /run/user/${uid}; chmod 0700 /run/user/${uid}; \
    done; \
    mkdir -p /usr/local/share/.tipi; \
    chgrp tipi /usr/local/share/.tipi; \
    chmod 2775 /usr/local/share/.tipi; \
    for user in ${USERNAME} tipi tipi-rbe; do \
      su "${user}" -c "cmake-re --version >/dev/null && \
        touch /usr/local/share/.tipi/probe-${user} && rm /usr/local/share/.tipi/probe-${user}"; \
    done; \
    test "$(du -s /usr/local/share/.tipi | cut -f1)" -le 8
