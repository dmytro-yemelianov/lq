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
IMAGE=$BUILD/qsoe.elf
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

# QSOE requires QEMU >= 11.0.1.  Up to 11.0.0, rmw_mip64() OR's mvip into
# mip.SEIP even though OpenSBI sets mvien[9] (delegating the S-external
# signal to the interrupt controller), so a message-signaled interrupt
# never reaches the trap and an MSI-driven device hangs.  Fixed in the
# v11.0.1 tag (qemu 175afdb0d1).  LQ runs on plain `virt` today, but its
# MSI-only PCIe path (devb-nvme) needs this once it comes up.
qver=$("$QEMU" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
if [[ $(printf '%s\n11.0.1\n' "$qver" | sort -V | head -1) != "11.0.1" ]]; then
    echo "error: QEMU $qver is too old — 11.0.1 or newer required." >&2
    echo "       set QEMU=/path/to/newer/qemu-system-riscv64." >&2
    exit 1
fi

# Default device set.  Each toggle below appends to QEMUOPTS.
ATTACH_NVME=1

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
        --)       seen_dashdash=1 ;;
        -gdb)     GDB=1 ;;
        -no-nvme) ATTACH_NVME=0 ;;
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

QEMUOPTS=(-machine virt -nographic -m "$MEM" -smp "$CPUS"
          -bios default -kernel "$IMAGE")

# ---------------------------------------------------------------------
# NVMe — attach a backing-file disk so /sbin/pci-server enumerates the
# QEMU NVMe controller (vid:did 1b36:0010, class 0x010802) at boot.
# Image is just a sparse 64 MiB file at build/nvme.img — actual
# partition / filesystem layout lands in v0.9 once devb-nvme + a real
# QSOE filesystem are up.
#
# Disk attaches via PCIe even when the OS does not yet use it, which
# is the v0.8-rc2 goal: prove that the PCI walker sees the device.
# ---------------------------------------------------------------------
if [[ $ATTACH_NVME -eq 1 ]]; then
    mkdir -p "$BUILD"
    if [[ ! -s $BUILD/nvme.img ]]; then
        echo "emu.sh: creating $BUILD/nvme.img (64 MiB, sparse, blank)..."
        truncate -s 64M "$BUILD/nvme.img"
    fi
    QEMUOPTS+=(-drive   "file=$BUILD/nvme.img,if=none,format=raw,id=nvm0"
               -device  "nvme,drive=nvm0,serial=qsoe-test")
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
