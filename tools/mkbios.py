#!/usr/bin/env python3
"""Build the BIOS boot payload that stage1/stage2 consume.

Layout (all offsets are sectors relative to the payload start, which is what
stage2 adds the payload's own start LBA to):

    LBA 0            header (512 bytes, magic "MCBB")
    LBA 1..          stage2 (padded to whole sectors)
    ...              flattened kernel image (ELF PT_LOAD segments, padded)
    ...              initrd (optional)

The result is padded to a 2048 byte boundary so it can be appended to the ISO
as its own partition.
"""

import argparse
import os
import struct
import sys

SECTOR = 512
PT_LOAD = 1

KERNEL_ADDR = 0x200000
INITRD_ADDR = 0x2000000
MAX_KERNEL_IMAGE = 32 * 1024 * 1024
STAGE2_MAX = 16384


def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def flatten_elf(path):
    """Return (flat_blob, load_addr, entry) for the kernel ELF."""
    with open(path, "rb") as f:
        image = f.read()

    if image[:4] != b"\x7fELF" or image[4] != 2:
        sys.exit("mkbios: %s is not a 64-bit ELF" % path)

    e_entry = struct.unpack_from("<Q", image, 0x18)[0]
    e_phoff = struct.unpack_from("<Q", image, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", image, 0x36)[0]
    e_phnum = struct.unpack_from("<H", image, 0x38)[0]

    segments = []
    lowest = None
    highest = 0
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", image, off)[0]
        p_offset = struct.unpack_from("<Q", image, off + 0x08)[0]
        p_paddr = struct.unpack_from("<Q", image, off + 0x18)[0]
        p_filesz = struct.unpack_from("<Q", image, off + 0x20)[0]
        p_memsz = struct.unpack_from("<Q", image, off + 0x28)[0]
        if p_type != PT_LOAD or p_memsz == 0:
            continue
        start = p_paddr & ~0xFFF
        end = align_up(p_paddr + p_memsz, 0x1000)
        segments.append((p_offset, p_paddr, p_filesz, start))
        lowest = start if lowest is None else min(lowest, start)
        highest = max(highest, end)

    if not segments:
        sys.exit("mkbios: %s has no loadable segments" % path)

    size = highest - lowest
    if size > MAX_KERNEL_IMAGE:
        sys.exit("mkbios: kernel image is %d bytes (max %d)" % (size, MAX_KERNEL_IMAGE))

    flat = bytearray(size)
    for p_offset, p_paddr, p_filesz, start in segments:
        dst = p_paddr - lowest
        flat[dst:dst + p_filesz] = image[p_offset:p_offset + p_filesz]

    return flat, lowest, e_entry


def build_header(stage2_sectors, kernel_lba, kernel_sectors, kernel_addr,
                 kernel_size, kernel_entry, initrd_lba, initrd_sectors,
                 initrd_addr, initrd_size, payload_lba):
    hdr = bytearray(SECTOR)
    struct.pack_into("<I", hdr, 0x00, 0x4242434D)      # "MCBB"
    struct.pack_into("<H", hdr, 0x04, stage2_sectors)
    struct.pack_into("<H", hdr, 0x06, 1)               # header version
    struct.pack_into("<I", hdr, 0x08, kernel_lba)
    struct.pack_into("<I", hdr, 0x0C, kernel_sectors)
    struct.pack_into("<I", hdr, 0x10, kernel_addr)
    struct.pack_into("<I", hdr, 0x14, kernel_size)
    struct.pack_into("<Q", hdr, 0x18, kernel_entry)
    struct.pack_into("<I", hdr, 0x20, initrd_lba)
    struct.pack_into("<I", hdr, 0x24, initrd_sectors)
    struct.pack_into("<I", hdr, 0x28, initrd_addr)
    struct.pack_into("<I", hdr, 0x2C, initrd_size)
    struct.pack_into("<I", hdr, 0x30, payload_lba)
    struct.pack_into("<I", hdr, 0x34, 0)
    return hdr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--initrd", default="")
    ap.add_argument("--stage2", required=True)
    ap.add_argument("--payload-lba", type=int, default=0)
    args = ap.parse_args()

    with open(args.stage2, "rb") as f:
        stage2 = f.read()
    if not stage2:
        sys.exit("mkbios: stage2 is empty")
    if len(stage2) > STAGE2_MAX:
        sys.exit("mkbios: stage2 is %d bytes (max %d)" % (len(stage2), STAGE2_MAX))

    kernel, kernel_addr, kernel_entry = flatten_elf(args.kernel)

    initrd = b""
    if args.initrd and os.path.exists(args.initrd):
        with open(args.initrd, "rb") as f:
            initrd = f.read()

    stage2_sectors = align_up(len(stage2), SECTOR) // SECTOR
    kernel_sectors = align_up(len(kernel), SECTOR) // SECTOR
    initrd_sectors = align_up(len(initrd), SECTOR) // SECTOR

    kernel_lba = 1 + stage2_sectors
    initrd_lba = kernel_lba + kernel_sectors

    header = build_header(
        stage2_sectors, kernel_lba, kernel_sectors, kernel_addr,
        len(kernel), kernel_entry,
        initrd_lba, initrd_sectors,
        INITRD_ADDR if initrd else 0, len(initrd),
        args.payload_lba)

    payload = bytearray()
    payload += header
    payload += stage2
    payload += b"\x00" * (stage2_sectors * SECTOR - len(stage2))
    payload += kernel
    payload += b"\x00" * (kernel_sectors * SECTOR - len(kernel))
    payload += initrd
    payload += b"\x00" * (initrd_sectors * SECTOR - len(initrd))
    payload += b"\x00" * (align_up(len(payload), 2048) - len(payload))

    with open(args.output, "wb") as f:
        f.write(payload)

    print("  BIOS    %s (%d bytes, kernel %d @ 0x%x, entry 0x%x, initrd %d)"
          % (args.output, len(payload), len(kernel), kernel_addr,
             kernel_entry, len(initrd)))


if __name__ == "__main__":
    main()
