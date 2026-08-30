#!/usr/bin/env bash
# Prints the content hash that keys the toolchain images: the git tree
# hash of docker/ combined with the blob hashes of the two files outside
# it that shape image content (the msan-libc++ build script baked into
# clang22, and the image workflow itself). Identical content always
# prints the same hash (across rebases, reverts and merges), which is
# what lets CI skip image builds whenever the tag already exists in GHCR.
# Used by .github/workflows/{toolchains,ci,release}.yml; run it locally
# to know which tree-<hash> tag matches your checkout.
# Everything hashes from HEAD (not the working tree), so a dirty checkout
# reports the tag of its last commit, which is what CI will build.
set -euo pipefail
cd "$(dirname "$0")/.."
git rev-parse HEAD:docker HEAD:tools/build-msan-libcxx.sh \
    HEAD:.github/workflows/toolchains.yml \
  | sha256sum | cut -c1-16
