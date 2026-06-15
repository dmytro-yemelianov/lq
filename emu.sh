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

QEMUOPTS=(-machine "$MACHINE" -nographic -m "$MEM" -smp "$CPUS"
          -bios default -kernel "$IMAGE")

# ---------------------------------------------------------------------
# NVMe — mirror NQ.  The backing store is a GPT image (8 x 16 MiB,
# p8 = fs-qrv) shared by both variants and built by the umbrella's
# `make nvme` (host_tools/mkgpt.py); emu.sh never lays it itself.  The
# controller hangs behind a PCIe root port (Type-1 bridge) so the MSI-X
# path is exercised, like the FU740.  `FLAT=1 ./emu.sh` puts it directly
# on bus 0 instead.
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
