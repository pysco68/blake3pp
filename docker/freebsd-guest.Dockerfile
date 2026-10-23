# syntax=docker/dockerfile:1
#
# The prepared FreeBSD amd64 guest image, packaged as a container image
# so GHCR can store and version it next to the toolchain images.
#
# This image is TRANSPORT, not a runtime: nothing ever runs in it. The
# ci lane pulls it, `docker create` + `docker cp` the qcow2 out, and
# boots it with qemu on the runner itself, because the guest needs
# /dev/kvm and the host needs nbd and mount, none of which a job
# container has. FROM scratch because a single file is the whole
# payload and a base layer would only make the pull bigger.
#
# It is packaged rather than prepared here on purpose. Preparation
# (tools/freebsd-guest.sh prepare) boots the guest once under KVM to let
# FreeBSD configure its own UFS root, which a docker build cannot do
# without --security=insecure and a privileged builder. So the recipe
# runs as an ordinary workflow step, on the runner, and this Dockerfile
# only wraps the result. .github/workflows/toolchains.yml does both, in
# that order, in the freebsd-guest leg of its image matrix.
#
# Content addressing comes for free and needs no new scheme: the image
# carries the same tree-<hash> tag as its siblings, and
# tools/toolchain-image-tag.sh hashes tools/freebsd-guest.sh along with
# the docker/ subtree, so changing the preparation recipe mints a new
# tag exactly as changing a Dockerfile does. The prepared qcow2 itself
# is untracked, so it cannot perturb that hash, which is the behaviour
# we want: the recipe is the input, its output is not.

ARG CHECK_IMAGE=busybox:1.37.0-uclibc@sha256:e246aa22ad2cbdfbd19e2a6ca2b275e26245a21920e2b2d0666324cee3f15549

# A stage whose only job is to say what to do when the artifact is
# missing. A bare COPY of an absent file fails with a line number and
# nothing else, and the first person to run `docker buildx bake
# freebsd-guest` by hand will hit exactly that.
FROM ${CHECK_IMAGE} AS check
COPY docker/.artifacts /artifacts
RUN test -s /artifacts/freebsd-guest.qcow2 || { \
      echo ""; \
      echo "docker/.artifacts/freebsd-guest.qcow2 is missing or empty."; \
      echo ""; \
      echo "This Dockerfile packages a prepared guest image; it does not"; \
      echo "build one. Produce it first (needs /dev/kvm, qemu, ovmf and"; \
      echo "root for nbd):"; \
      echo ""; \
      echo "  FBSD_IMAGE=\$PWD/docker/.artifacts/freebsd-guest.qcow2 \\"; \
      echo "    tools/freebsd-guest.sh prepare"; \
      echo ""; \
      exit 1; \
    }

FROM scratch
COPY --from=check /artifacts/freebsd-guest.qcow2 /freebsd-guest.qcow2
