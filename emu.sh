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
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
# SPDX-License-Identifier: Apache-2.0

set -e

TOP=$(cd "$(dirname "$0")" && pwd)
BUILD=$TOP/build
IMAGE=$BUILD/qsoe.elf
TESTDIR=$TOP/test

CPUS=4
MEM=512M
QEMU=qemu-system-riscv64

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
# Image is just a sparse 64 MiB file at test/nvme.img — actual
# partition / filesystem layout lands in v0.9 once devb-nvme + a real
# QSOE filesystem are up.
#
# Disk attaches via PCIe even when the OS does not yet use it, which
# is the v0.8-rc2 goal: prove that the PCI walker sees the device.
# ---------------------------------------------------------------------
if [[ $ATTACH_NVME -eq 1 ]]; then
    mkdir -p "$TESTDIR"
    if [[ ! -s $TESTDIR/nvme.img ]]; then
        echo "emu.sh: creating $TESTDIR/nvme.img (64 MiB, sparse, blank)..."
        truncate -s 64M "$TESTDIR/nvme.img"
    fi
    QEMUOPTS+=(-drive   "file=$TESTDIR/nvme.img,if=none,format=raw,id=nvm0"
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
