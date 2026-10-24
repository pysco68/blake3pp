#!/usr/bin/env bash
# freebsd-guest.sh: the FreeBSD guests the ci lanes run their
# cross-built binaries in. One recipe, two architectures, five
# subcommands, used by .github/workflows/toolchains.yml (which publishes
# the prepared images) and .github/workflows/ci.yml (which boots them),
# and by hand on any developer machine with qemu:
#
#   tools/freebsd-guest.sh prepare   build the guest image, once
#   tools/freebsd-guest.sh start     boot a throwaway overlay of it
#   tools/freebsd-guest.sh wait      block until sshd answers
#   tools/freebsd-guest.sh run ...   run a command in the guest
#   tools/freebsd-guest.sh stop      ACPI powerdown, then kill
#
# Which guest is chosen by FBSD_ARCH, amd64 (the default) or arm64:
#
#   FBSD_ARCH=arm64 tools/freebsd-guest.sh prepare
#
# Everything else is one code path. The two architectures are the same
# operating system configured the same way, and keeping them in one file
# is what stops the second one from quietly rotting; the places they
# genuinely differ are collected in the arch table below and explained
# in "-- Why the two architectures differ" at the end of this header.
#
# There is no copy subcommand: `run` passes stdin through, so
#
#   tar cf - -C build/freebsd-zig-cxx23 cli tests bench \
#     | tools/freebsd-guest.sh run 'tar xf - -C /root'
#
# is the file transfer, and it needs no second ssh identity or scp
# quoting rules.
#
# Every path, port and size below is an environment variable with a
# default that works in a checkout; the lane overrides almost none of
# them.
#
# -- Why the preparation looks the way it does -------------------------
#
# The image is the OFFICIAL plain release VM image, not the
# BASIC-CLOUDINIT one. The cloudinit image was tried first and is a trap
# for this use: it spends roughly eighteen minutes on a pkgbase firstboot
# upgrade before sshd ever starts, it writes a sudoers file naming a
# sudo binary it does not ship, and it locks root, so when the
# cloud-init path does not complete there is no way into the guest at
# all. The plain image boots to a login prompt in about twenty seconds
# and is empty of all of that.
#
# What the plain image does NOT have is any of our configuration, and
# this is where the design had to bend. The intended recipe was to
# inject everything offline over qemu-nbd: mount the root filesystem on
# the host, write the files, disconnect. That is impossible on Linux.
# The root filesystem is UFS2, and writing UFS from Linux needs
# CONFIG_UFS_FS_WRITE, which the kernel itself labels DANGEROUS, which
# defaults to n, and which no distribution enables. Two kernels were
# checked while writing this and neither of them even has CONFIG_UFS_FS:
# there is no ufs module to load, let alone a writable one. So the host
# can read the image's partition table and it can write the EFI System
# Partition, which is FAT, and that is the whole of its reach into a
# FreeBSD disk.
#
# That turns out to be enough, because of one FreeBSD feature: loader.efi
# reads /EFI/freebsd/loader.env off the ESP and imports it as loader
# environment before it touches the root filesystem. So the offline
# qemu-nbd step writes exactly one file there, and the guest boots
# straight to a usable serial console without anything on the UFS side
# having changed. From there the guest configures ITSELF: `prepare`
# boots it once, logs in as root on that serial console (the plain image
# leaves root's password empty and marks ttyu0 secure, which is the
# documented way to use it), pipes a configuration script in, and powers
# it down. A FreeBSD kernel writing its own UFS root is not an
# experimental kernel option; it is just FreeBSD.
#
# The one boot is why `prepare` is slow, and it is also why `prepare`
# runs once per recipe change rather than once per ci run: its output is
# published to GHCR as a toolchain image and every run pulls it.
#
# -- Why the ssh key is not in that image ------------------------------
#
# The published image is a public artifact, so it must not contain a
# private key, and a key pinned at publish time would be a key every run
# shares. The per-run half instead rides in on the ESP: the test job
# generates a throwaway keypair, `start` writes the public half into the
# overlay's ESP over qemu-nbd (FAT, so this one really is a host-side
# file copy), and an rc script baked in at prepare time copies it to
# /root/.ssh/authorized_keys before sshd starts. The published image
# ships the slow, stable half: a configured FreeBSD that is ready to be
# handed a key. It ships no ssh host keys either, for the same reason;
# sshd makes them on first boot, which costs about a second.
#
# -- Why `start` boots an overlay --------------------------------------
#
# The pulled base image is never written to. `start` makes a qcow2 whose
# backing file is the base and boots that, so every run begins from
# byte-identical state and a run that corrupts its filesystem costs the
# next run nothing.
#
# -- Why the two architectures differ ----------------------------------
#
# Four things, and only four. Each of them cost real debugging time, so
# each is stated where it is used as well as here.
#
# 1. ACCELERATION. amd64 runs under KVM on an x86 runner and refuses to
#    run without it. arm64 cannot: GitHub has no arm64 runner that
#    exposes /dev/kvm, and an x64 runner cannot KVM an arm64 guest --
#    KVM virtualises, it does not translate. So the arm64 guest is TCG,
#    and that, not any property of FreeBSD, is why it is the slower of
#    the two lanes: roughly two minutes to boot and a hundred seconds
#    for the suite, against seconds for its amd64 twin. That is cheap
#    enough to run on every push, which is what it does.
#
# 2. FIRMWARE. amd64 needs OVMF for the FIRST boot alone, because the
#    only way to give the stock image a serial console without writing
#    its UFS root is a loader.env on the ESP and only the EFI loader
#    reads that; afterwards the guest's own /boot/loader.conf says
#    comconsole and every later boot is plain BIOS. arm64 has no BIOS at
#    all -- UEFI is the only way an aarch64 machine boots -- so EVERY
#    boot needs the AAVMF pair, a read-only CODE image and a WRITABLE
#    copy of VARS (the firmware writes its variable store, and a
#    read-only one wedges the boot). On Ubuntu they are
#    /usr/share/AAVMF/AAVMF_CODE.fd (a symlink to the no-secboot
#    variant) and /usr/share/AAVMF/AAVMF_VARS.fd, from the package
#    qemu-efi-aarch64; both are already 64 MiB, which is the size
#    -machine virt's pflash wants, so unlike OVMF's 2 MiB legacy pair
#    there is nothing to pad.
#
# 3. WHERE THE ESP IS. Not a fixed partition index. On the amd64 release
#    image it is partition 2; on the arm64 release image it is partition
#    1. Nothing documents either as stable, so both architectures probe
#    for the FAT partition instead of trusting an index -- see esp_find.
#
# 4. THE CONSOLE SETTING. amd64 has a VGA console to move AWAY from, so
#    both loader.env and /boot/loader.conf set console="comconsole".
#    arm64 under -machine virt -display none has no other console to
#    begin with: the EFI firmware, the loader and the kernel all find
#    the pl011 on their own, and naming a console explicitly can only
#    take one away. So on arm64 those files carry the boot-speed
#    settings and say nothing about the console.
set -euo pipefail

# ------------------------------------------------------------- arch --
# amd64 or arm64. Everything that differs between the two is resolved in
# the table below and nowhere else.
FBSD_ARCH="${FBSD_ARCH:-amd64}"
case "${FBSD_ARCH}" in
  amd64|arm64) ;;
  *) printf '[freebsd-guest] error: FBSD_ARCH must be amd64 or arm64, not %s\n' "${FBSD_ARCH}" >&2; exit 1 ;;
esac

if [ "${FBSD_ARCH}" = amd64 ]; then
  # 14.5-RELEASE is deliberately close to the 14.0 ABI floor the
  # cross-built artifact claims (see cmake/toolchains/freebsd-zig-cxx23/),
  # so the lane tests the claim rather than testing a decade of forward
  # compatibility.
  FBSD_VERSION="${FBSD_VERSION:-14.5-RELEASE}"
  # The release tree's directory for this architecture and the infix in
  # the image's own file name. They are not the same word, and neither
  # of them is "amd64" on the other architecture.
  FBSD_REL_DIR=amd64
  FBSD_REL_INFIX=amd64
  FBSD_QEMU=qemu-system-x86_64
  FBSD_NAME=freebsd-guest
  FBSD_SSH_PORT_DEFAULT=2222
  # KVM, and only KVM: this lane exists to measure a real I/O backend on
  # real timings, and the runner has the silicon for it.
  FBSD_KVM_REQUIRED=1
  # UEFI for the preparation boot alone; see the header, point 2.
  FBSD_UEFI_ALWAYS=0
  FBSD_BOOT_TIMEOUT_DEFAULT=300
else
  # 15.x, not 14.x, and this is the one deliberate divergence from the
  # amd64 lane's "stay next to the ABI floor" rule. The SVE kernels this
  # architecture carries are selected from HWCAP_SVE, and that bit is
  # set only by a kernel that saves SVE state across a context switch,
  # which FreeBSD gained in 15.0 (see src/dispatch/cpu_detect_arm.cpp).
  # A 14.x guest would run the binaries correctly and take NEON, testing
  # the ABI floor but covering none of the vector kernels; a 15.x guest
  # covers the kernels, and the floor is still asserted by the binary
  # loading at all, since nothing about it changed.
  FBSD_VERSION="${FBSD_VERSION:-15.1-RELEASE}"
  # Note both: the directory is "aarch64" and the file name says
  # "arm64-aarch64". Guessing either from the other does not work.
  FBSD_REL_DIR=aarch64
  FBSD_REL_INFIX=arm64-aarch64
  FBSD_QEMU=qemu-system-aarch64
  FBSD_NAME=freebsd-arm64-guest
  FBSD_SSH_PORT_DEFAULT=2322
  FBSD_KVM_REQUIRED=0
  FBSD_UEFI_ALWAYS=1
  # TCG, so everything takes longer: ~2 min to the login prompt on a
  # four-core runner. Generous enough that a loaded runner is still
  # reported as slow rather than as broken.
  FBSD_BOOT_TIMEOUT_DEFAULT=900
fi

FBSD_MIRROR="${FBSD_MIRROR:-https://download.freebsd.org/releases/VM-IMAGES}"

# Where everything lives. Under build/ so a checkout stays clean and
# .gitignore already covers it. Per architecture, so the two guests can
# exist side by side in one checkout.
FBSD_DIR="${FBSD_DIR:-build/${FBSD_NAME}}"
FBSD_IMAGE="${FBSD_IMAGE:-${FBSD_DIR}/${FBSD_NAME}.qcow2}"
FBSD_OVERLAY="${FBSD_OVERLAY:-${FBSD_DIR}/overlay.qcow2}"
FBSD_SERIAL="${FBSD_SERIAL:-${FBSD_DIR}/serial.log}"
FBSD_MONITOR="${FBSD_MONITOR:-${FBSD_DIR}/monitor.sock}"
FBSD_PIDFILE="${FBSD_PIDFILE:-${FBSD_DIR}/qemu.pid}"
FBSD_SSH_KEY="${FBSD_SSH_KEY:-${FBSD_DIR}/id_ed25519}"

# Guest shape and the loopback port its sshd is forwarded to. The port
# is a variable because a developer box may already have a guest on the
# obvious one, and it differs per architecture so that both guests can
# run at once.
FBSD_SSH_PORT="${FBSD_SSH_PORT:-${FBSD_SSH_PORT_DEFAULT}}"
FBSD_SMP="${FBSD_SMP:-4}"
FBSD_MEM="${FBSD_MEM:-4096}"
FBSD_DISK="${FBSD_DISK:-12G}"

# Timeouts, both generous: a cold runner boots slower than a desktop, a
# TCG guest slower than either, and a guest that is merely slow must not
# be reported as a guest that is broken.
FBSD_BOOT_TIMEOUT="${FBSD_BOOT_TIMEOUT:-${FBSD_BOOT_TIMEOUT_DEFAULT}}"
FBSD_STOP_TIMEOUT="${FBSD_STOP_TIMEOUT:-60}"

# The nbd device the offline steps borrow, and how to become root for
# them. modprobe, qemu-nbd and mount are privileged; everything else in
# this script is not. FBSD_SUDO= (empty) is how a container that is
# already root, or a developer who would rather not, turns it off.
FBSD_NBD="${FBSD_NBD:-/dev/nbd0}"
if [ "$(id -u)" = 0 ]; then FBSD_SUDO="${FBSD_SUDO-}"; else FBSD_SUDO="${FBSD_SUDO-sudo}"; fi

# Firmware. On amd64 this is OVMF and `prepare` alone reads it (header,
# point 2). On arm64 it is the AAVMF pair and EVERY boot reads it, so
# `start` needs it too. AAVMF_CODE.fd is a symlink to the no-secboot
# build on Ubuntu, which is the one we want: a secure-boot firmware
# would refuse FreeBSD's unsigned loader.
if [ "${FBSD_ARCH}" = amd64 ]; then
  FBSD_FW_CODE="${FBSD_FW_CODE:-${FBSD_OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}}"
  FBSD_FW_VARS="${FBSD_FW_VARS:-${FBSD_OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}}"
  FBSD_FW_PKG="ovmf"
else
  FBSD_FW_CODE="${FBSD_FW_CODE:-/usr/share/AAVMF/AAVMF_CODE.fd}"
  FBSD_FW_VARS="${FBSD_FW_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}"
  FBSD_FW_PKG="qemu-efi-aarch64"
fi

# The arm64 guest's SVE vector length, in 128-bit granules: 1 means a
# 128-bit vector, which makes dispatch select sve2_128, the
# configuration essentially all real Armv9 silicon has (Neoverse V2,
# Grace, Graviton4). The probe behind that selection was validated by
# hand at 128, 256 and 512 bits and with SVE off entirely, selecting
# sve2_128, sve256, sve512 and neon respectively and rejecting every
# length that did not match; the lane PINS one length rather than
# sweeping them, because a lane should test the common case and a sweep
# of four TCG boots would cost four times as much to learn what the
# dispatch unit tests already assert.
#
# This is also the lane's guard against the setting being silently
# ignored: qemu refuses to start on an unknown -cpu property (a
# misspelling is an error, not a shrug), and the test job additionally
# asserts that the kernel dispatch actually engaged is an SVE2 one. One
# lock for "the knob still exists", one for "the knob still does
# something".
FBSD_SVE_VQ="${FBSD_SVE_VQ:-1}"

say() { printf '[freebsd-guest:%s] %s\n' "${FBSD_ARCH}" "$*" >&2; }
die() { printf '[freebsd-guest:%s] error: %s\n' "${FBSD_ARCH}" "$*" >&2; exit 1; }

ssh_opts() {
  printf '%s\n' \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o GlobalKnownHostsFile=/dev/null \
    -o LogLevel=ERROR \
    -o ConnectTimeout=5 \
    -o BatchMode=yes \
    -o ServerAliveInterval=15 \
    -i "${FBSD_SSH_KEY}" \
    -p "${FBSD_SSH_PORT}"
}

# ---------------------------------------------------------- qemu args --
# The machine, the accelerator and the processor model, which is where
# the two architectures stop looking alike. Printed one per line and
# read into an array by the callers, so a value may contain commas
# without any word-splitting question.
qemu_machine_args() {
  if [ "${FBSD_ARCH}" = amd64 ]; then
    # qemu's default i440fx machine, KVM, and the host's own CPU model:
    # this lane is a measurement, so the guest should see the silicon.
    printf '%s\n' -enable-kvm -cpu host
  else
    # -machine virt is the only sensible aarch64 board (no legacy
    # anything, virtio for everything). thread=multi lets TCG use more
    # than one host core for the guest's vCPUs, which is most of the
    # difference between "slow" and "unusable".
    #
    # -cpu max is what carries SVE at all; sve-max-vq pins the vector
    # length, see FBSD_SVE_VQ above. A wrong property name here is a
    # hard qemu error rather than a default, which is deliberate.
    printf '%s\n' -machine virt -accel "tcg,thread=multi" -cpu "max,sve-max-vq=${FBSD_SVE_VQ}"
  fi
}

# The pflash pair, for the boots that need firmware. <vars file>, which
# must already be a WRITABLE copy: the firmware stores its boot
# variables there and a read-only one wedges the boot on arm64.
qemu_firmware_args() {  # <vars copy>
  printf '%s\n' \
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=${FBSD_FW_CODE}" \
    -drive "if=pflash,format=raw,unit=1,file=$1"
}

# ---------------------------------------------------------------- nbd --
# Connect the image at FBSD_NBD and mount its ESP. Both halves are
# undone by nbd_detach, which every caller installs as a trap: a
# qemu-nbd left connected outlives the script and makes the next run's
# connect fail with a message about the device being busy, which reads
# like a corrupt image rather than like leftover state.
ESP_MNT=""
ESP_PART=""

# Which partition is the EFI System Partition. PROBED, never assumed:
# it is partition 2 on the amd64 release image and partition 1 on the
# arm64 one, the FreeBSD release engineering scripts document neither as
# an interface, and a hardcoded index silently mounts the wrong thing or
# fails with a message about a missing device. blkid is the cheap
# answer; where it is missing, a read-only mount attempt is the same
# question asked the slow way.
esp_find() {
  local p
  for p in "${FBSD_NBD}"p*; do
    [ -b "${p}" ] || continue
    if command -v blkid > /dev/null 2>&1; then
      case "$(${FBSD_SUDO} blkid -p -s TYPE -o value "${p}" 2> /dev/null)" in
        vfat) printf '%s\n' "${p}"; return 0 ;;
        ?*)   continue ;;   # a type we know and it is not FAT
      esac
    fi
    local probe
    probe=$(mktemp -d)
    if ${FBSD_SUDO} mount -t vfat -o ro "${p}" "${probe}" 2> /dev/null; then
      ${FBSD_SUDO} umount "${probe}" > /dev/null 2>&1 || true
      rmdir "${probe}" > /dev/null 2>&1 || true
      printf '%s\n' "${p}"
      return 0
    fi
    rmdir "${probe}" > /dev/null 2>&1 || true
  done
  return 1
}

nbd_attach() {  # <qcow2>
  local img=$1
  ${FBSD_SUDO} modprobe nbd max_part=16
  ${FBSD_SUDO} qemu-nbd -d "${FBSD_NBD}" > /dev/null 2>&1 || true
  ${FBSD_SUDO} qemu-nbd -c "${FBSD_NBD}" -f qcow2 "${img}"
  # The kernel publishes the partition nodes asynchronously after the
  # connect returns, so the retry loop is the wait: it ends when the
  # probe finds a FAT partition, which also covers the case of the ESP
  # appearing after its neighbours.
  local i
  for i in $(seq 1 50); do
    ESP_PART=$(esp_find) && break
    ESP_PART=""
    sleep 0.2
  done
  [ -n "${ESP_PART}" ] || die "no FAT (EFI System) partition among ${FBSD_NBD}p*; is ${img} a FreeBSD ${FBSD_ARCH} VM image?"
  say "ESP is ${ESP_PART}"
  ESP_MNT=$(mktemp -d)
  ${FBSD_SUDO} mount -t vfat "${ESP_PART}" "${ESP_MNT}"
}

nbd_detach() {
  if [ -n "${ESP_MNT}" ]; then
    ${FBSD_SUDO} umount "${ESP_MNT}" > /dev/null 2>&1 || true
    rmdir "${ESP_MNT}" > /dev/null 2>&1 || true
    ESP_MNT=""
  fi
  ESP_PART=""
  ${FBSD_SUDO} qemu-nbd -d "${FBSD_NBD}" > /dev/null 2>&1 || true
}

# Write the public half of the run's keypair where the guest's rc script
# will look for it. Called by `start`, against the overlay.
esp_put_key() {  # <qcow2> <pubkey file>
  nbd_attach "$1"
  ${FBSD_SUDO} mkdir -p "${ESP_MNT}/blake3pp"
  ${FBSD_SUDO} cp "$2" "${ESP_MNT}/blake3pp/authorized_keys"
  ${FBSD_SUDO} sync
  nbd_detach
}

# The loader settings the two boots share. On amd64 the console has to
# be moved to the serial line explicitly, because the image's default is
# the VGA one the lane does not have. On arm64 there is nothing to move
# it away from -- -machine virt -display none has no other console, and
# firmware, loader and kernel all find the pl011 themselves -- so naming
# one could only take the working console away.
loader_settings() {
  if [ "${FBSD_ARCH}" = amd64 ]; then
    printf 'console="comconsole"\n'
  fi
  printf 'autoboot_delay="0"\nbeastie_disable="YES"\n'
}

# ------------------------------------------------------------ prepare --
cmd_prepare() {
  command -v "${FBSD_QEMU}" > /dev/null || die "${FBSD_QEMU} not found"
  command -v qemu-img       > /dev/null || die "qemu-img not found"
  command -v qemu-nbd       > /dev/null || die "qemu-nbd not found"
  [ -r "${FBSD_FW_CODE}" ] || die "no EFI firmware at ${FBSD_FW_CODE} (apt-get install ${FBSD_FW_PKG}); the configuration boot needs the EFI loader to read the ESP's loader.env"
  [ -r "${FBSD_FW_VARS}" ] || die "no EFI variable store at ${FBSD_FW_VARS} (apt-get install ${FBSD_FW_PKG})"
  if [ "${FBSD_KVM_REQUIRED}" = 1 ]; then
    [ -e /dev/kvm ] || die "no /dev/kvm; the amd64 guest boots under KVM and will not do it under TCG"
  fi

  mkdir -p "${FBSD_DIR}"
  local base="${FBSD_DIR}/download-${FBSD_VERSION}-${FBSD_REL_INFIX}-ufs.qcow2"
  local url="${FBSD_MIRROR}/${FBSD_VERSION}/${FBSD_REL_DIR}/Latest/FreeBSD-${FBSD_VERSION}-${FBSD_REL_INFIX}-ufs.qcow2.xz"

  if [ ! -f "${base}" ]; then
    say "downloading ${url}"
    curl -fL --retry 3 --retry-delay 5 -o "${base}.xz" "${url}"
    say "decompressing"
    xz -dT0 "${base}.xz"
  else
    say "reusing ${base}"
  fi

  # A working copy, so a second prepare starts from the pristine
  # download rather than from a half-configured image.
  local work="${FBSD_DIR}/work.qcow2"
  rm -f "${work}"
  cp "${base}" "${work}"
  # The stock image's root filesystem is 5 GB and nearly full. The
  # binaries, the generated bench file and a core dump all have to fit.
  qemu-img resize "${work}" "${FBSD_DISK}" > /dev/null
  say "resized to ${FBSD_DISK}"

  # The one offline injection the host is capable of: the ESP, so the
  # first boot comes up the way the console driver below expects. See
  # the header, and loader_settings for what goes in on each
  # architecture.
  trap nbd_detach EXIT
  nbd_attach "${work}"
  ${FBSD_SUDO} mkdir -p "${ESP_MNT}/EFI/freebsd"
  loader_settings | ${FBSD_SUDO} tee "${ESP_MNT}/EFI/freebsd/loader.env" > /dev/null
  say "seeded ESP /EFI/freebsd/loader.env"
  nbd_detach
  trap - EXIT

  # Boot it once, UEFI so loader.env is read, and talk to the console
  # over a unix socket rather than a log file, because this boot is a
  # conversation and not a transcript.
  local vars="${FBSD_DIR}/efi-vars.fd"
  local consock="${FBSD_DIR}/prepare-console.sock"
  local prepare_log="${FBSD_DIR}/prepare-console.log"
  cp -f "${FBSD_FW_VARS}" "${vars}"
  chmod u+w "${vars}"
  rm -f "${consock}" "${prepare_log}"

  local -a machine fw
  mapfile -t machine < <(qemu_machine_args)
  mapfile -t fw      < <(qemu_firmware_args "${vars}")

  say "booting once to configure the guest from the inside"
  "${FBSD_QEMU}" "${machine[@]}" -smp "${FBSD_SMP}" -m "${FBSD_MEM}" \
    "${fw[@]}" \
    -drive file="${work}",if=virtio,format=qcow2,cache=unsafe \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0,romfile= \
    -display none -serial "unix:${consock},server=on,wait=off" \
    -pidfile "${FBSD_DIR}/prepare.pid" -daemonize

  # The configuration itself, run as root inside the guest. Kept as one
  # script rather than as a sequence of typed commands so that it is
  # reviewable as a unit and so that a single sentinel reports whether
  # all of it succeeded.
  # The heredoc is QUOTED, so nothing in it is expanded by this shell:
  # the guest script is full of $ and backslashes that belong to the
  # guest's shell, and an expanding heredoc turns one missed escape into
  # a configuration that silently did something else. The two values
  # that really do come from out here are spliced in afterwards, by
  # name.
  local guest_script
  guest_script=$(cat <<'GUEST'
set -eu

# What a first boot would otherwise have to be told. The release image
# configures hostname, growfs and ifconfig_DEFAULT="DHCP inet6
# accept_rtadv" and nothing else: sshd in particular is enabled only in
# the cloud variants. sysrc edits /etc/rc.conf in place and is the
# supported way to do it. ifconfig_vtnet0 is set explicitly on top of
# the image's ifconfig_DEFAULT so the lane does not depend on a default
# that a future release could restate differently.
sysrc sshd_enable=YES
sysrc hostname=blake3pp-freebsd-@ARCH@
sysrc ifconfig_vtnet0=DHCP
sysrc blake3ppkey_enable=YES
# No crash dumps: the guest's swap partition is 1 GB and a dump there
# only delays the next boot while savecore copies it somewhere.
sysrc dumpdev=NO

# Key-only root login. The image ships root with an empty password and
# that stays true on the console, which is fine for a loopback-only CI
# guest, but it must never be reachable that way over the network.
sed -i '' -E 's/^#?[[:space:]]*PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config
grep -q '^PermitRootLogin prohibit-password' /etc/ssh/sshd_config \
  || echo 'PermitRootLogin prohibit-password' >> /etc/ssh/sshd_config

# The same loader settings, now on the UFS side, so every later boot
# gets them without the ESP. The plain release image has no
# /boot/loader.conf of its own at all (vmimage.subr writes one only for
# the ZFS variants), so create it before editing it rather than letting
# sed fail on a missing file.
touch /boot/loader.conf
sed -i '' -E '/^(console|autoboot_delay|beastie_disable)=/d' /boot/loader.conf
cat >> /boot/loader.conf <<'LOADEREOF'
@LOADER_SETTINGS@
LOADEREOF

# The per-run key relay. The published image carries no key; this reads
# whatever public key the host wrote onto the ESP and installs it before
# sshd starts. BEFORE: LOGIN rather than BEFORE: sshd because sshd
# itself requires LOGIN, so this is the ordering that actually holds.
mkdir -p /usr/local/etc/rc.d
cat > /usr/local/etc/rc.d/blake3ppkey <<'RCEOF'
#!/bin/sh
#
# PROVIDE: blake3ppkey
# REQUIRE: mountcritremote
# BEFORE: LOGIN
# KEYWORD: nojail
#
# Install the run's authorized_keys from the EFI System Partition, where
# tools/freebsd-guest.sh put it before boot. The published guest image
# deliberately contains no key of its own.

. /etc/rc.subr

name="blake3ppkey"
rcvar="blake3ppkey_enable"
start_cmd="blake3ppkey_start"
stop_cmd=":"

blake3ppkey_start()
{
	# The release image's own /etc/fstab already mounts the ESP at
	# /boot/efi, so in the normal case there is nothing to mount. The
	# explicit mount is the fallback for an image whose fstab lost that
	# entry, and it is deliberately read-only. The device is SEARCHED
	# FOR, not named: the ESP is partition 2 on the amd64 release image
	# and partition 1 on the arm64 one, and this script is the same on
	# both.
	_src=/boot/efi
	_mnt=""
	if [ ! -f "${_src}/blake3pp/authorized_keys" ]; then
		_mnt=/var/run/blake3ppkey
		mkdir -p "${_mnt}"
		_src=""
		for _dev in /dev/gpt/efiesp /dev/vtbd0p1 /dev/vtbd0p2 /dev/vtbd0p3; do
			[ -e "${_dev}" ] || continue
			if mount -t msdosfs -o ro "${_dev}" "${_mnt}" 2>/dev/null; then
				_src="${_mnt}"
				break
			fi
		done
		if [ -z "${_src}" ]; then
			echo "blake3ppkey: no ESP found; leaving /root/.ssh alone"
			return 0
		fi
	fi
	if [ -f "${_src}/blake3pp/authorized_keys" ]; then
		mkdir -p /root/.ssh
		cp "${_src}/blake3pp/authorized_keys" /root/.ssh/authorized_keys
		chmod 0700 /root/.ssh
		chmod 0600 /root/.ssh/authorized_keys
		chown -R root:wheel /root/.ssh
		echo "blake3ppkey: installed /root/.ssh/authorized_keys from ${_src}"
	else
		echo "blake3ppkey: no authorized_keys on the ESP"
	fi
	[ -n "${_mnt}" ] && umount "${_mnt}" 2>/dev/null
	return 0
}

load_rc_config $name
: ${blake3ppkey_enable:="NO"}
run_rc_command "$1"
RCEOF
chmod 0755 /usr/local/etc/rc.d/blake3ppkey

mkdir -p /root/.ssh
chmod 0700 /root/.ssh
chown root:wheel /root/.ssh

# No ssh host keys are generated here on purpose: they would be private
# keys inside a public image. sshd makes them on first boot.
rm -f /etc/ssh/ssh_host_*

# The plain image DOES ship a /firstboot sentinel, but only so that
# growfs_enable can grow the root filesystem into whatever the disk was
# resized to; the host resized it before this boot, so that already
# happened above and rc's own firstboot_sentinel removes the file at the
# end of this boot. Remove it here too and assert it is gone: a
# surviving trigger is the entire reason the cloudinit image was
# unusable, and the published image must boot straight to multi-user.
rm -f /firstboot
test ! -e /firstboot

# Leave the filesystem small and the next boot quiet.
rm -rf /var/db/freebsd-update/* /var/cache/pkg/* /tmp/* 2>/dev/null || true
GUEST
)
  guest_script=${guest_script//@ARCH@/${FBSD_ARCH}}
  guest_script=${guest_script//@LOADER_SETTINGS@/$(loader_settings)}

  say "driving the guest console"
  if ! FBSD_GUEST_SCRIPT="${guest_script}" python3 - "${consock}" "${prepare_log}" "${FBSD_BOOT_TIMEOUT}" <<'PYEOF'
import os, re, socket, sys, time, base64

sock_path, log_path, timeout = sys.argv[1], sys.argv[2], float(sys.argv[3])
deadline = time.time() + timeout

# qemu creates the listening socket as it starts; give it a moment.
sock = None
while time.time() < deadline:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(sock_path)
        break
    except OSError:
        sock = None
        time.sleep(0.25)
if sock is None:
    sys.exit("could not connect to the guest console socket")
sock.settimeout(1.0)

log = open(log_path, "ab", buffering=0)
buf = bytearray()

def pump():
    try:
        chunk = sock.recv(65536)
    except socket.timeout:
        return b""
    if not chunk:
        raise SystemExit("guest console closed unexpectedly")
    log.write(chunk)
    buf.extend(chunk)
    return chunk

def expect(pattern, what, limit=None):
    """Read until pattern matches, then DISCARD everything up to the end
    of the match, so the next expect cannot re-match stale output."""
    rx = re.compile(pattern)
    end = time.time() + (limit if limit else timeout)
    while time.time() < end:
        m = rx.search(bytes(buf))
        if m:
            del buf[:m.end()]
            return
        pump()
    sys.stderr.write("---- guest console so far ----\n")
    sys.stderr.write(bytes(buf[-4000:]).decode("utf-8", "replace"))
    sys.stderr.write("\n------------------------------\n")
    raise SystemExit("timed out waiting for %s" % what)

def send(line):
    sock.sendall(line.encode() + b"\r")

PROMPT = rb"root@[^\r\n]*# "

expect(rb"login: ", "the login prompt")
send("root")
expect(PROMPT, "a root shell")

# The script goes in base64, in short lines. A serial console is a tty
# in canonical mode and a line longer than its MAX_CANON (1024 on
# FreeBSD) is silently truncated, which would corrupt the script in a
# way that is very hard to see. Base64 also removes every quoting
# question between here and the guest's shell.
blob = base64.b64encode(os.environ["FBSD_GUEST_SCRIPT"].encode()).decode()
send(": > /tmp/cfg.b64")
expect(PROMPT, "the prompt after truncating /tmp/cfg.b64")
for i in range(0, len(blob), 160):
    send("echo %s >> /tmp/cfg.b64" % blob[i:i+160])
    expect(PROMPT, "the prompt after a script chunk")

send("b64decode -r /tmp/cfg.b64 > /tmp/cfg.sh && sh /tmp/cfg.sh; echo CFG_EXIT=$?")
expect(rb"CFG_EXIT=(\d+)", "the configuration script to finish")
# The match was consumed; recover the status from the log's tail.
tail = open(log_path, "rb").read()[-4000:]
status = re.findall(rb"CFG_EXIT=(\d+)", tail)[-1].decode()
if status != "0":
    sys.stderr.write(tail.decode("utf-8", "replace") + "\n")
    raise SystemExit("guest configuration script exited %s" % status)

expect(PROMPT, "the prompt after the configuration script")
send("poweroff")
# Powerdown closes the socket; that is the success signal.
end = time.time() + 120
while time.time() < end:
    try:
        if not pump():
            continue
    except SystemExit:
        sys.exit(0)
sys.exit("guest did not power down")
PYEOF
  then
    say "the configuration boot failed; full console transcript follows"
    cat "${prepare_log}" >&2 || true
    ${FBSD_SUDO} kill "$(cat "${FBSD_DIR}/prepare.pid" 2>/dev/null)" 2> /dev/null || true
    die "prepare failed"
  fi

  # qemu exits on powerdown, but not instantly.
  local i
  for i in $(seq 1 60); do
    kill -0 "$(cat "${FBSD_DIR}/prepare.pid" 2>/dev/null)" 2> /dev/null || break
    sleep 1
  done
  kill -9 "$(cat "${FBSD_DIR}/prepare.pid" 2>/dev/null)" 2> /dev/null || true

  # Compress the result INTO the qcow2 rather than beside it: the image
  # is published and pulled as a container layer, so an externally
  # compressed file would have to be decompressed by every consumer,
  # while qcow2's own compression is transparent to qemu and still
  # shrinks the layer. zstd where this qemu can (far faster to read than
  # zlib at a similar ratio), zlib where it cannot.
  say "compressing"
  rm -f "${FBSD_IMAGE}"
  if ! qemu-img convert -c -O qcow2 -o compression_type=zstd "${work}" "${FBSD_IMAGE}" 2> /dev/null; then
    say "this qemu-img has no zstd qcow2 compression; falling back to zlib"
    qemu-img convert -c -O qcow2 "${work}" "${FBSD_IMAGE}"
  fi
  rm -f "${work}" "${vars}" "${consock}"

  say "prepared image: ${FBSD_IMAGE}"
  ls -lh "${FBSD_IMAGE}" >&2
  qemu-img info "${FBSD_IMAGE}" >&2
  printf '%s\n' "${FBSD_IMAGE}"
}

# -------------------------------------------------------------- start --
cmd_start() {
  [ -f "${FBSD_IMAGE}" ] || die "no prepared image at ${FBSD_IMAGE}; run 'prepare' or pull the published one"
  if [ "${FBSD_KVM_REQUIRED}" = 1 ]; then
    [ -e /dev/kvm ] || die "no /dev/kvm; the amd64 lane is not worth running under TCG"
  fi
  if [ "${FBSD_UEFI_ALWAYS}" = 1 ]; then
    [ -r "${FBSD_FW_CODE}" ] || die "no EFI firmware at ${FBSD_FW_CODE} (apt-get install ${FBSD_FW_PKG}); arm64 is UEFI only, there is no BIOS fallback"
    [ -r "${FBSD_FW_VARS}" ] || die "no EFI variable store at ${FBSD_FW_VARS} (apt-get install ${FBSD_FW_PKG})"
  fi
  mkdir -p "${FBSD_DIR}"

  # A throwaway keypair per boot. Generated here rather than by the
  # caller so that `start` is self-contained on a developer machine; the
  # ci job can drop its own key in beforehand and this leaves it alone.
  if [ ! -f "${FBSD_SSH_KEY}" ]; then
    ssh-keygen -t ed25519 -N '' -C blake3pp-ci -f "${FBSD_SSH_KEY}" > /dev/null
    say "generated a throwaway keypair at ${FBSD_SSH_KEY}"
  fi

  # The base is never written to; see the header.
  rm -f "${FBSD_OVERLAY}"
  qemu-img create -f qcow2 -b "$(cd "$(dirname "${FBSD_IMAGE}")" && pwd)/$(basename "${FBSD_IMAGE}")" \
    -F qcow2 "${FBSD_OVERLAY}" > /dev/null
  say "overlay ${FBSD_OVERLAY} over ${FBSD_IMAGE}"

  trap nbd_detach EXIT
  esp_put_key "${FBSD_OVERLAY}" "${FBSD_SSH_KEY}.pub"
  trap - EXIT
  say "wrote this run's public key onto the overlay's ESP"

  # Firmware, or not. On amd64 the prepared image's own
  # /boot/loader.conf puts the console on the serial line, so OVMF has
  # nothing left to contribute and the lane boots plain BIOS without the
  # firmware files at all. On arm64 there IS no BIOS: UEFI is how the
  # machine boots, so the pair goes in on every boot, with a fresh
  # writable copy of the variable store per boot (the firmware falls
  # back to the removable-media path \EFI\BOOT\BOOTAA64.EFI, which the
  # release image provides, so a blank store costs nothing but is still
  # a store the firmware can write).
  local -a machine fw
  mapfile -t machine < <(qemu_machine_args)
  fw=()
  if [ "${FBSD_UEFI_ALWAYS}" = 1 ]; then
    local vars="${FBSD_DIR}/efi-vars.fd"
    cp -f "${FBSD_FW_VARS}" "${vars}"
    chmod u+w "${vars}"
    mapfile -t fw < <(qemu_firmware_args "${vars}")
  fi

  rm -f "${FBSD_SERIAL}" "${FBSD_MONITOR}" "${FBSD_PIDFILE}"
  "${FBSD_QEMU}" "${machine[@]}" -smp "${FBSD_SMP}" -m "${FBSD_MEM}" \
    ${fw[@]+"${fw[@]}"} \
    -drive file="${FBSD_OVERLAY}",if=virtio,format=qcow2 \
    -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:${FBSD_SSH_PORT}-:22" \
    -device virtio-net-pci,netdev=n0,romfile= \
    -display none \
    -serial "file:$(cd "$(dirname "${FBSD_SERIAL}")" && pwd)/$(basename "${FBSD_SERIAL}")" \
    -monitor "unix:${FBSD_MONITOR},server=on,wait=off" \
    -pidfile "${FBSD_PIDFILE}" -daemonize

  say "started: ssh on 127.0.0.1:${FBSD_SSH_PORT}, console at ${FBSD_SERIAL}, monitor at ${FBSD_MONITOR}"
}

# --------------------------------------------------------------- wait --
cmd_wait() {
  local deadline=$(( $(date +%s) + FBSD_BOOT_TIMEOUT ))
  local -a opts; mapfile -t opts < <(ssh_opts)
  say "waiting up to ${FBSD_BOOT_TIMEOUT}s for sshd on 127.0.0.1:${FBSD_SSH_PORT}"
  while [ "$(date +%s)" -lt "${deadline}" ]; do
    if ssh "${opts[@]}" root@127.0.0.1 true 2> /dev/null; then
      say "guest is up after $(( FBSD_BOOT_TIMEOUT - (deadline - $(date +%s)) ))s"
      return 0
    fi
    # A qemu that has already exited will never answer; fail now rather
    # than at the deadline.
    if [ -f "${FBSD_PIDFILE}" ] && ! kill -0 "$(cat "${FBSD_PIDFILE}")" 2> /dev/null; then
      say "qemu exited while we were waiting"
      break
    fi
    sleep 2
  done
  # A guest that does not come up is undebuggable from the outside, so
  # the console is the failure message, not a footnote to it.
  say "guest never answered on 127.0.0.1:${FBSD_SSH_PORT}; serial console follows"
  echo "================ ${FBSD_SERIAL} ================" >&2
  cat "${FBSD_SERIAL}" >&2 2> /dev/null || echo "(no serial log at ${FBSD_SERIAL})" >&2
  echo "==========================================================" >&2
  exit 1
}

# ---------------------------------------------------------------- run --
cmd_run() {
  [ $# -ge 1 ] || die "run needs a command"
  local -a opts; mapfile -t opts < <(ssh_opts)
  ssh "${opts[@]}" root@127.0.0.1 "$@"
}

# --------------------------------------------------------------- stop --
cmd_stop() {
  local pid=""
  [ -f "${FBSD_PIDFILE}" ] && pid=$(cat "${FBSD_PIDFILE}" 2> /dev/null) || true
  if [ -S "${FBSD_MONITOR}" ]; then
    say "sending ACPI powerdown"
    python3 - "${FBSD_MONITOR}" <<'PYEOF' || true
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sys.argv[1])
time.sleep(0.2)
s.sendall(b"system_powerdown\n")
time.sleep(0.5)
s.close()
PYEOF
  fi
  if [ -n "${pid}" ]; then
    local i
    for i in $(seq 1 "${FBSD_STOP_TIMEOUT}"); do
      kill -0 "${pid}" 2> /dev/null || { say "guest powered down cleanly"; rm -f "${FBSD_PIDFILE}"; return 0; }
      sleep 1
    done
    say "guest ignored the powerdown after ${FBSD_STOP_TIMEOUT}s; killing it"
    kill -9 "${pid}" 2> /dev/null || true
  fi
  rm -f "${FBSD_PIDFILE}"
}

usage() { sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'; }

[ $# -ge 1 ] || { usage; exit 1; }
sub=$1; shift
case "${sub}" in
  prepare) cmd_prepare "$@" ;;
  start)   cmd_start   "$@" ;;
  wait)    cmd_wait    "$@" ;;
  run)     cmd_run     "$@" ;;
  stop)    cmd_stop    "$@" ;;
  -h|--help|help) usage ;;
  *) die "unknown subcommand '${sub}' (prepare, start, wait, run, stop)" ;;
esac
