#!/usr/bin/env python3
# mkgpt.py: write a minimal valid GPT to an image file.
#
# Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
# SPDX-License-Identifier: Apache-2.0
#
# Borrowed from the QRV-OS host tool of the same name (Apache-2.0) and
# kept bit-compatible with QSOE's libgpt, which reads the PRIMARY GPT at
# LBA 1 only and verifies the protective MBR, the header signature, and
# BOTH CRC32s.
#
# Usage: mkgpt.py [--fsqrv N] <image> <part1_mib> [part2_mib ...]
#
# Writes:
#   LBA 0:       protective MBR (one 0xEE partition spanning the disk)
#   LBA 1:       primary GPT header
#   LBAs 2..33:  primary partition entry array (128 x 128 bytes)
# No backup header/entries are written -- libgpt uses the primary only.
#
# Each partition gets the Linux-filesystem-data type GUID and the name
# "qsoe-test-pN".  --fsqrv N (1-based index) instead marks partition N
# with the fs-qrv (qrvfs) partition-type GUID and names it "fsqrv" --
# the on-disk type the fs-qrv server keys on, identical to the GUID used
# on real hardware (NVMe pN = fs-qrv).
#
# The image file is NOT resized; the caller ensures it is big enough.

import os
import struct
import sys
import uuid
import zlib

LBA_SIZE      = 512
ENTRIES_LBA   = 2
N_ENTRIES     = 128
ENTRY_SIZE    = 128
ENTRIES_BYTES = N_ENTRIES * ENTRY_SIZE           # 16 KiB
FIRST_USABLE  = 34                               # LBA after header+entries

# Linux filesystem data.
TYPE_GUID       = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")
# fs-qrv (qrvfs) partition type -- carried over from QRV so the ported
# fs-qrv server recognizes the partition by the same on-disk GUID.
FSQRV_TYPE_GUID = uuid.UUID("51525611-322e-4017-bae8-e4d9c9d4e979")


def utf16le(s: str, n_chars: int) -> bytes:
    out = s.encode("utf-16-le")[: 2 * n_chars]
    return out + b"\x00" * (2 * n_chars - len(out))


def crc32(buf: bytes) -> int:
    return zlib.crc32(buf) & 0xFFFFFFFF


def write_pmbr(img_size_lbas: int) -> bytes:
    p = bytearray(LBA_SIZE)
    # One protective entry: type 0xEE, first LBA 1, size = disk - 1
    # (clamped to 32 bits).
    size = min(img_size_lbas - 1, 0xFFFFFFFF)
    p[446 + 4] = 0xEE
    struct.pack_into("<I", p, 446 + 8, 1)
    struct.pack_into("<I", p, 446 + 12, size)
    p[510] = 0x55
    p[511] = 0xAA
    return bytes(p)


def build_entries(parts):
    buf = bytearray(ENTRIES_BYTES)
    for i, (start, end, name, type_guid) in enumerate(parts):
        off = i * ENTRY_SIZE
        struct.pack_into("<16s", buf, off, type_guid.bytes_le)
        struct.pack_into("<16s", buf, off + 16, uuid.uuid4().bytes_le)
        struct.pack_into("<Q", buf, off + 32, start)
        struct.pack_into("<Q", buf, off + 40, end)
        struct.pack_into("<Q", buf, off + 48, 0)            # attributes
        buf[off + 56 : off + 56 + 72] = utf16le(name, 36)
    return bytes(buf)


def build_header(disk_lbas, disk_guid, entries_crc):
    hdr = bytearray(92)
    struct.pack_into("<8s", hdr, 0, b"EFI PART")
    struct.pack_into("<I", hdr, 8, 0x00010000)              # revision 1.0
    struct.pack_into("<I", hdr, 12, 92)                     # header_size
    # header_crc32 @16 left zero until the whole struct is built.
    struct.pack_into("<Q", hdr, 24, 1)                      # current_lba
    struct.pack_into("<Q", hdr, 32, disk_lbas - 1)          # backup_lba
    struct.pack_into("<Q", hdr, 40, FIRST_USABLE)           # first_usable_lba
    struct.pack_into("<Q", hdr, 48, disk_lbas - FIRST_USABLE)  # last_usable
    struct.pack_into("<16s", hdr, 56, disk_guid.bytes_le)
    struct.pack_into("<Q", hdr, 72, ENTRIES_LBA)
    struct.pack_into("<I", hdr, 80, N_ENTRIES)
    struct.pack_into("<I", hdr, 84, ENTRY_SIZE)
    struct.pack_into("<I", hdr, 88, entries_crc)
    struct.pack_into("<I", hdr, 16, crc32(bytes(hdr)))      # header CRC32
    return bytes(hdr) + bytes(LBA_SIZE - len(hdr))


def main():
    args = sys.argv[1:]
    fsqrv_idx = None
    i = 0
    while i < len(args):
        if args[i] == "--fsqrv" and i + 1 < len(args):
            fsqrv_idx = int(args[i + 1])
            del args[i : i + 2]
            continue
        i += 1

    if len(args) < 2:
        print("usage: mkgpt.py [--fsqrv N] <image> <part1_mib> [part2_mib ...]",
              file=sys.stderr)
        sys.exit(1)
    img_path = args[0]
    part_mibs = [int(x) for x in args[1:]]

    disk_lbas = os.path.getsize(img_path) // LBA_SIZE
    if disk_lbas < FIRST_USABLE + 1:
        print("image too small", file=sys.stderr)
        sys.exit(1)

    # Pack partitions back-to-back from FIRST_USABLE.
    parts = []
    cursor = FIRST_USABLE
    for i, mib in enumerate(part_mibs):
        nlba = (mib * 1024 * 1024) // LBA_SIZE
        end = cursor + nlba - 1
        if end > disk_lbas - FIRST_USABLE:
            print(f"partition {i + 1} exceeds disk capacity", file=sys.stderr)
            sys.exit(1)
        if fsqrv_idx is not None and (i + 1) == fsqrv_idx:
            parts.append((cursor, end, "fsqrv", FSQRV_TYPE_GUID))
        else:
            parts.append((cursor, end, f"qsoe-test-p{i + 1}", TYPE_GUID))
        cursor = end + 1

    entries = build_entries(parts)
    disk_guid = uuid.uuid4()
    header_lba = build_header(disk_lbas, disk_guid, crc32(entries))
    pmbr = write_pmbr(disk_lbas)

    with open(img_path, "r+b") as f:
        f.seek(0)
        f.write(pmbr)
        f.seek(LBA_SIZE)
        f.write(header_lba)
        f.seek(ENTRIES_LBA * LBA_SIZE)
        f.write(entries)

    print(f"mkgpt: {img_path} -- {len(parts)} partition(s), disk GUID {disk_guid}")
    for i, (start, end, name, type_guid) in enumerate(parts, 1):
        nlba = end - start + 1
        kind = "fsqrv" if type_guid == FSQRV_TYPE_GUID else "linux-data"
        print(f"  p{i}: LBA {start}..{end} "
              f"({nlba * LBA_SIZE // (1024 * 1024)} MiB) "
              f"name=\"{name}\" [{kind}]")


if __name__ == "__main__":
    main()
