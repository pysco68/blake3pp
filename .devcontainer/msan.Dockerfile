# Derived image: the blake3pp devcontainer plus an MSan-instrumented libc++
# baked into /opt/libcxx-msan, so it survives container rebuilds (the
# in-container build documented in tools/build-msan-libcxx.sh dies with the
# container layer).
#
# Build from the repo root, pointing BASE at the current devcontainer image
# (docker images | grep vsc-blake3pp):
#
#   docker build -f .devcontainer/msan.Dockerfile \
#     --build-arg BASE=vsc-blake3pp-<hash>-uid:latest \
#     -t blake3pp-devcontainer:msan .
#
# To use it as the devcontainer, replace the "build" block in
# devcontainer.json with: "image": "blake3pp-devcontainer:msan"
ARG BASE
FROM ${BASE}

USER root
COPY tools/build-msan-libcxx.sh /tmp/build-msan-libcxx.sh
# Clone, build and clean in one layer so the ~3 GB of LLVM sources and
# build tree do not become image weight.
RUN bash /tmp/build-msan-libcxx.sh \
    && rm -rf /tmp/llvm-src /tmp/libcxx-msan-build /tmp/build-msan-libcxx.sh
# Match the base image's user; devcontainer.json's remoteUser handles the
# rest at runtime.
USER root
