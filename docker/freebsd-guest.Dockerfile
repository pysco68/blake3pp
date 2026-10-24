# syntax=docker/dockerfile:1
#
# A prepared FreeBSD guest image, packaged as a container image so GHCR
# can store and version it next to the toolchain images. ONE recipe, two
# images: the amd64 guest the x86 cross lane runs in and the arm64 guest
# the aarch64 cross lane runs in, selected by GUEST_ARTIFACT (see the
# two freebsd*-guest targets in docker/docker-bake.hcl). Nothing in here
# is architecture-specific -- the architecture is entirely a property of
# the qcow2 that tools/freebsd-guest.sh produced -- so the two differ by
# one build arg and nothing else.
#
# This image is TRANSPORT, not a runtime: nothing ever runs in it. The
# ci lane pulls it, `docker create` + `docker cp` the qcow2 out, and
# boots it with qemu on the runner itself, because the guest needs
# /dev/kvm (amd64) or simply a host qemu (arm64, which is TCG) and the
# host needs nbd and mount, none of which a job container has. FROM
# scratch because a single file is the whole payload and a base layer
# would only make the pull bigger.
#
# The payload is always /freebsd-guest.qcow2 whatever the architecture,
# so the consuming job's `docker cp` line is the same in both lanes and
# the image's TAG is the only thing that says which guest this is. That
# is deliberate: the tag is already the thing ci selects on.
#
# It is packaged rather than prepared here on purpose. Preparation
# (tools/freebsd-guest.sh prepare) boots the guest once to let FreeBSD
# configure its own UFS root, which a docker build cannot do without
# --security=insecure and a privileged builder (and, for amd64, access
# to /dev/kvm). So the recipe runs as an ordinary workflow step, on the
# runner, and this Dockerfile only wraps the result.
# .github/workflows/toolchains.yml does both, in that order, in the
# freebsd-guest and freebsd-arm64-guest legs of its image matrix.
#
# Content addressing comes for free and needs no new scheme: the images
# carry the same tree-<hash> tag as their siblings, and
# tools/toolchain-image-tag.sh hashes tools/freebsd-guest.sh along with
# the docker/ subtree, so changing the preparation recipe mints a new
# tag exactly as changing a Dockerfile does. The prepared qcow2 itself
# is untracked, so it cannot perturb that hash, which is the behaviour
# we want: the recipe is the input, its output is not.

ARG CHECK_IMAGE=busybox:1.37.0-uclibc@sha256:e246aa22ad2cbdfbd19e2a6ca2b275e26245a21920e2b2d0666324cee3f15549

# Which prepared image this build packages, as a file name under
# docker/.artifacts/. The default is the amd64 guest, because that is
# what `docker buildx bake freebsd-guest` means and because the amd64
# lane existed first; the arm64 target overrides it.
ARG GUEST_ARTIFACT=freebsd-guest.qcow2

# A stage whose only job is to say what to do when the artifact is
# missing. A bare COPY of an absent file fails with a line number and
# nothing else, and the first person to run `docker buildx bake
# freebsd-guest` by hand will hit exactly that.
FROM ${CHECK_IMAGE} AS check
ARG GUEST_ARTIFACT
COPY docker/.artifacts /artifacts
RUN set -eu; \
    arch=amd64; prep=""; \
    case "${GUEST_ARTIFACT}" in *arm64*) arch=arm64; prep="FBSD_ARCH=arm64 " ;; esac; \
    test -s "/artifacts/${GUEST_ARTIFACT}" || { \
      echo ""; \
      echo "docker/.artifacts/${GUEST_ARTIFACT} is missing or empty."; \
      echo ""; \
      echo "This Dockerfile packages a prepared guest image; it does not"; \
      echo "build one. Produce it first (needs qemu, nbd and root, plus"; \
      echo "/dev/kvm on amd64 and the qemu-efi-aarch64 firmware on arm64):"; \
      echo ""; \
      echo "  ${prep}FBSD_IMAGE=\$PWD/docker/.artifacts/${GUEST_ARTIFACT} \\"; \
      echo "    tools/freebsd-guest.sh prepare"; \
      echo ""; \
      echo "(that is the ${arch} guest)"; \
      echo ""; \
      exit 1; \
    }

FROM scratch
ARG GUEST_ARTIFACT
COPY --from=check /artifacts/${GUEST_ARTIFACT} /freebsd-guest.qcow2
