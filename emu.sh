#! /bin/bash
#
# QSOE — QEMU launcher.
#
# Spawns qemu-system-riscv64 with the right machine config for the
# current QSOE feature set.  Started life as a `make run` one-liner;
# now hosts the disk-image bring-up and device-list assembly that the
# devb-* drivers will exercise as they land in v0.8.
#
# Usage:
#   ./emu.sh                       boot with the default device set
#   ./emu.sh -gdb                  add -s -S so you can attach gdb
#   ./emu.sh -no-nvme              omit the NVMe controller
#   ./emu.sh -- <extra qemu args>  pass-through to qemu after `--`
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
# SPDX-License-Identifier: Apache-2.0

set -e

TOP=$(cd "$(dirname "$0")" && pwd)
BUILD=$TOP/build
# emu.sh always runs the QEMU-virt board image (the SiFive build is for
# real hardware, deployed via boot/).
IMAGE=$BUILD/qsoe-l-qemu.elf
# Note: the userland module package (modpkg.cpio, built by
# `make -C ../quser cpio`) is NOT passed via QEMU `-initrd`.  Taskman
# embeds it directly via .incbin (see taskman/Makefile), so the bytes
# already live inside taskman.elf and travel into RAM as part of the
# kernel image.  A vestigial FDT-driven loader exists at
# taskman/sys/initrd.c -- see comments there before flipping back to
# QEMU-initrd delivery.
TESTDIR=$TOP/test

CPUS=4
MEM=512M
QEMU=${QEMU:-qemu-system-riscv64}

# Interrupt architecture.  Default PLIC: stock seL4 has no AIA support
# and hard-requires the PLIC -- on `virt,aia=aplic-imsic` the PLIC is
# removed and the kernel aborts in its per-hart IRQ init (load access
# fault on the absent PLIC context registers).  `AIA=1 ./emu.sh` selects
# the AIA machine anyway -- the path NQ uses for PCIe MSI/MSI-X -- for
# experiments once seL4 grows IMSIC/APLIC drivers.
if [[ "${AIA:-0}" == "0" ]]; then
    MACHINE="virt"
else
    MACHINE="virt,aia=aplic-imsic"
fi

# The AIA machine needs QEMU >= 11.0.1: up to 11.0.0, rmw_mip64() OR's
# mvip into mip.SEIP even though OpenSBI sets mvien[9] (delegating the
# S-external signal to the IMSIC), so a message-signaled interrupt never
# reaches the trap and an MSI-driven device hangs.  Fixed in the v11.0.1
# tag (qemu 175afdb0d1).  The AIA=0 PLIC machine is unaffected.
if [[ "$MACHINE" == *aia=aplic-imsic* ]]; then
    qver=$("$QEMU" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
    if [[ $(printf '%s\n11.0.1\n' "$qver" | sort -V | head -1) != "11.0.1" ]]; then
        echo "error: QEMU $qver is too old for AIA — 11.0.1 or newer required." >&2
        echo "       run 'AIA=0 ./emu.sh' for a PLIC machine, or set QEMU=<newer>." >&2
        exit 1
    fi
fi

# Default device set.  Each toggle below appends to QEMUOPTS.  QSOE/L
# boots off a virtio-mmio disk: under QEMU it has no NVMe (seL4 has no AIA,
# so PCIe MSI-X can't be delivered).  NVMe stays available behind `-nvme`
# for AIA experiments once seL4 grows IMSIC/APLIC.
ATTACH_VIRTIO=1
ATTACH_NVME=0

# Pass-through to QEMU for anything after `--`.
PASSTHROUGH=()
GDB=0
seen_dashdash=0
for arg in "$@"; do
    if [[ $seen_dashdash -eq 1 ]]; then
        PASSTHROUGH+=("$arg")
        continue
    fi
    case "$arg" in
        --)         seen_dashdash=1 ;;
        -gdb)       GDB=1 ;;
        -no-virtio) ATTACH_VIRTIO=0 ;;
        -nvme)      ATTACH_NVME=1 ;;
        -no-nvme)   ATTACH_NVME=0 ;;
        -h|--help)
            grep '^# ' "$0" | sed 's/^# //'
            exit 0 ;;
        *)
            echo "emu.sh: unknown arg '$arg' (use -- to pass through to qemu)" >&2
            exit 1 ;;
    esac
done

# Refuse to start if the build artefact isn't there — pointing at a
# stale path silently is worse than failing loud.
if [[ ! -f "$IMAGE" ]]; then
    echo "emu.sh: $IMAGE not found — run \`make\` first." >&2
    exit 1
fi

QEMUOPTS=(-machine "$MACHINE" -nographic -m "$MEM" -smp "$CPUS"
          -bios default -kernel "$IMAGE")

# MAINFS names the block device init mounts as the root filesystem; the
# storage choice below sets it, and it is passed to the kernel on the
# command line so init can pick the matching driver.
MAINFS=

# ---------------------------------------------------------------------
# virtio-mmio block disk (QSOE/L default).  Raw whole-disk qrvfs built by
# the umbrella's `make virtio`; force-legacy so the device presents the
# version-1 interface devb-virtio drives.  Served as /dev/vblk0; fs-qrv
# mounts it.
# ---------------------------------------------------------------------
if [[ $ATTACH_VIRTIO -eq 1 ]]; then
    VIRTIO_IMG="$TOP/../build/virtio.img"
    make -C "$TOP/.." virtio          # idempotent; the umbrella owns the image
    QEMUOPTS+=(-global "virtio-mmio.force-legacy=true"
               -drive "file=$VIRTIO_IMG,if=none,format=raw,id=vblk0"
               -device "virtio-blk-device,drive=vblk0")
    MAINFS="/dev/vblk0"
fi

# ---------------------------------------------------------------------
# NVMe — mirror NQ (opt-in via `-nvme`; needs AIA, so only useful once
# seL4 grows IMSIC/APLIC).  GPT image (8 x 16 MiB, p8 = fs-qrv) built by
# the umbrella's `make nvme`.  Controller behind a PCIe root port unless
# `FLAT=1`.
# ---------------------------------------------------------------------
if [[ $ATTACH_NVME -eq 1 ]]; then
    NVME_IMG="$TOP/../build/nvme.img"
    make -C "$TOP/.." nvme            # idempotent; the umbrella owns the image
    QEMUOPTS+=(-drive "file=$NVME_IMG,if=none,format=raw,id=nvm0")
    if [[ "${FLAT:-0}" == "1" ]]; then
        QEMUOPTS+=(-device "nvme,drive=nvm0,serial=qsoe-test")
    else
        QEMUOPTS+=(-device "pcie-root-port,id=rp0,bus=pcie.0,chassis=1"
                   -device "nvme,drive=nvm0,serial=qsoe-test,bus=rp0")
    fi
    MAINFS="/dev/nvme0n1p8"
fi

# Kernel command line -> FDT /chosen/bootargs -> /sys/cmdline: name the
# main fs so init mounts it (and selects the matching block driver).
if [[ -n "$MAINFS" ]]; then
    QEMUOPTS+=(-append "mainfs=$MAINFS")
fi

# ---------------------------------------------------------------------
# Optional toggles.
# ---------------------------------------------------------------------
if [[ $GDB -eq 1 ]]; then
    QEMUOPTS+=(-s -S)
    echo "emu.sh: gdb stub on :1234, CPUs halted on entry."
fi

# ---------------------------------------------------------------------
# Go.
# ---------------------------------------------------------------------
exec "$QEMU" "${QEMUOPTS[@]}" "${PASSTHROUGH[@]}"
