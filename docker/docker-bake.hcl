# Toolchain image matrix for blake3pp.
#
#   docker buildx bake -f docker/docker-bake.hcl            # everything
#   docker buildx bake -f docker/docker-bake.hcl clang22    # one image
#
# One image per compiler; each sits on the Ubuntu release contemporary to it
# so its binaries carry the oldest practical glibc floor for that vintage
# (see docker/README.md). The same file drives local builds and CI.

variable "REGISTRY" { default = "ghcr.io/pysco68/blake3pp" }
variable "TAG"      { default = "latest" }
# CI sets EXTRA_TAG=sha-<short> for an immutable tag alongside TAG.
variable "EXTRA_TAG" { default = "" }

function "tc_tags" {
  params = [name]
  result = EXTRA_TAG == "" ? [
    "${REGISTRY}/toolchain-${name}:${TAG}",
  ] : [
    "${REGISTRY}/toolchain-${name}:${TAG}",
    "${REGISTRY}/toolchain-${name}:${EXTRA_TAG}",
  ]
}

# Digest-pinned glibc anchors. Bump deliberately, here only.
variable "UBUNTU" {
  default = {
    "2404" = "ubuntu:24.04@sha256:561618e2c15bf2397621dd04f96926663a3b5616c189cf7e38db7e82f5c538ea"
    "2604" = "ubuntu:26.04@sha256:678c6550cc43645e08669028bc177f50be4e7c5b8cca677067b1914d4afc7a03"
  }
}

variable "CMAKE_VERSION" { default = "4.3.2" }
variable "NINJA_VERSION" { default = "1.13.2" }
variable "CMAKE_RE_VERSION" { default = "0.0.87" }
variable "ZIG_VERSION"   { default = "0.16.0" }

group "default" {
  targets = ["base", "gcc", "clang", "arm64-gcc15", "riscv64-gcc15", "ppc64le-gcc15", "s390x-gcc15", "zig",
             "emscripten"]
}

# One recipe instantiated per release. Pushed too (cheap) so CI matrix jobs
# can pin the exact digest their cohort was built from.
target "base" {
  name       = "base-${item.rel}"
  matrix     = { item = [{ rel = "2404" }, { rel = "2604" }] }
  context    = "."
  dockerfile = "docker/base.Dockerfile"
  args = {
    BASE_IMAGE       = UBUNTU[item.rel]
    CMAKE_VERSION    = CMAKE_VERSION
    NINJA_VERSION    = NINJA_VERSION
    CMAKE_RE_VERSION = CMAKE_RE_VERSION
  }
  tags = tc_tags("base-${item.rel}")
}

target "gcc" {
  name = "gcc${item.v}"
  matrix = { item = [
    { v = "14", rel = "2404" },
    { v = "16", rel = "2604" },
  ]}
  context    = "."
  dockerfile = "docker/gcc.Dockerfile"
  contexts   = { base = "target:base-${item.rel}" }
  args = {
    GCC_VERSION = item.v
    WITH_EXTRAS = item.v == "16" ? "1" : "0"
  }
  tags = tc_tags("gcc${item.v}")
}

# clang-18 from the noble archive, clang-22 from the resolute archive; only
# clang-20 needs the apt.llvm.org noble-20 pocket. GCC_PIN mirrors the
# --gcc-install-dir pins in the committed -libstdcxx toolchain files.
target "clang" {
  name = "clang${item.v}"
  matrix = { item = [
    { v = "18", rel = "2404", apt = "0", pin = "14" },
    { v = "20", rel = "2404", apt = "1", pin = "14" },
    { v = "22", rel = "2604", apt = "0", pin = "16" },
  ]}
  context    = "."
  dockerfile = "docker/clang.Dockerfile"
  contexts   = { base = "target:base-${item.rel}" }
  target     = item.v == "22" ? "msan" : "toolchain"
  args = {
    CLANG_VERSION = item.v
    USE_LLVM_APT  = item.apt
    GCC_PIN       = item.pin
    WITH_EXTRAS   = item.v == "22" ? "1" : "0"
  }
  tags = tc_tags("clang${item.v}")
}

target "arm64-gcc15" {
  context    = "."
  dockerfile = "docker/arm64-gcc15.Dockerfile"
  contexts   = { base = "target:base-2604" }
  tags       = tc_tags("arm64-gcc15")
}

target "riscv64-gcc15" {
  context    = "."
  dockerfile = "docker/riscv64-gcc15.Dockerfile"
  contexts   = { base = "target:base-2604" }
  tags       = tc_tags("riscv64-gcc15")
}

target "zig" {
  context    = "."
  dockerfile = "docker/zig.Dockerfile"
  contexts   = { base = "target:base-2604" }
  args       = { ZIG_VERSION = ZIG_VERSION }
  tags       = tc_tags("zig")
}

target "emscripten" {
  context    = "."
  dockerfile = "docker/emscripten.Dockerfile"
  contexts   = { base = "target:base-2604" }
  tags       = tc_tags("emscripten")
}

target "ppc64le-gcc15" {
  context    = "."
  dockerfile = "docker/ppc64le-gcc15.Dockerfile"
  contexts   = { base = "target:base-2604" }
  tags       = tc_tags("ppc64le-gcc15")
}

target "s390x-gcc15" {
  context    = "."
  dockerfile = "docker/s390x-gcc15.Dockerfile"
  contexts   = { base = "target:base-2604" }
  tags       = tc_tags("s390x-gcc15")
}
