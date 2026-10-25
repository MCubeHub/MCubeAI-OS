#!/usr/bin/env python3
"""Wrap the FAT32 ESP into a bootable disk image (MBR or GPT).

The result boots on UEFI firmware from a hard disk (IDE/SATA/VHD) and can be
written to a USB stick with dd or Rufus (DD mode). One EFI system partition at
LBA 2048 carries the loader, kernel, initrd and user programs.

MBR is the default for maximum compatibility; --gpt produces a modern GPT
layout with a protective MBR and a backup header.

Usage: mkdisk.py OUT.IMG ESP.IMG [--size-mib 128] [--gpt]
"""

import argparse
import uuid
import zlib

SECTOR = 512
START_LBA = 2048          # 1 MiB alignment
ESP_TYPE_MBR = 0xEF
# GPT type GUID C12A7328-F81F-11D2-BA4B-00A0C93EC93B in on-disk byte order.
ESP_TYPE_GPT = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
MIB = 1024 * 1024
ENTRY_COUNT = 128
ENTRY_SIZE = 128
ENTRY_SECTORS = ENTRY_COUNT * ENTRY_SIZE // SECTOR


def gpt_guid_le():
    return uuid.uuid4().bytes_le


def write_mbr(img, esp_sectors):
    img[440:444] = b"MCUB"      # disk signature
    img[510:512] = b"\x55\xaa"

    e = bytearray(16)
    e[0] = 0x00                 # not active; UEFI ignores the boot flag
    e[1:4] = b"\xfe\xff\xff"    # CHS start (LBA addressing only)
    e[4] = ESP_TYPE_MBR
    e[5:8] = b"\xfe\xff\xff"    # CHS end
    e[8:12] = START_LBA.to_bytes(4, "little")
    e[12:16] = esp_sectors.to_bytes(4, "little")
    img[446:462] = e


def write_gpt(img, esp_sectors, total_sectors):
    first_usable = 2 + ENTRY_SECTORS
    last_usable = total_sectors - ENTRY_SECTORS - 2
    last_lba = START_LBA + esp_sectors - 1
    if last_lba > last_usable:
        raise SystemExit("mkdisk: ESP does not fit in the GPT usable range")

    entries = bytearray(ENTRY_COUNT * ENTRY_SIZE)
    e = entries
    e[0:16] = ESP_TYPE_GPT
    e[16:32] = gpt_guid_le()
    e[32:40] = START_LBA.to_bytes(8, "little")
    e[40:48] = last_lba.to_bytes(8, "little")
    e[48:56] = (0).to_bytes(8, "little")
    name = "MCUBEAIOS".encode("utf-16-le")
    e[56:56 + len(name)] = name

    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF
    disk_guid = gpt_guid_le()

    def header(current, backup, entry_lba):
        h = bytearray(SECTOR)
        h[0:8] = b"EFI PART"
        h[8:12] = (0x00010000).to_bytes(4, "little")
        h[12:16] = (92).to_bytes(4, "little")
        h[24:32] = current.to_bytes(8, "little")
        h[32:40] = backup.to_bytes(8, "little")
        h[40:48] = first_usable.to_bytes(8, "little")
        h[48:56] = last_usable.to_bytes(8, "little")
        h[56:72] = disk_guid
        h[72:80] = entry_lba.to_bytes(8, "little")
        h[80:84] = ENTRY_COUNT.to_bytes(4, "little")
        h[84:88] = ENTRY_SIZE.to_bytes(4, "little")
        h[88:92] = entries_crc.to_bytes(4, "little")
        crc = zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF
        h[16:20] = crc.to_bytes(4, "little")
        return h

    def put(lba, data):
        off = lba * SECTOR
        img[off:off + len(data)] = data

    # Protective MBR: one 0xEE entry covering the whole LBA range.
    p = bytearray(16)
    p[0] = 0x00
    p[1:4] = b"\xfe\xff\xff"
    p[4] = 0xEE
    p[5:8] = b"\xfe\xff\xff"
    p[8:12] = (1).to_bytes(4, "little")
    p[12:16] = min(total_sectors - 1, 0xFFFFFFFF).to_bytes(4, "little")
    img[446:462] = p
    img[510:512] = b"\x55\xaa"

    put(1, header(1, total_sectors - 1, 2))
    put(2, entries)
    put(total_sectors - ENTRY_SECTORS - 1, entries)
    put(total_sectors - 1, header(total_sectors - 1, 1,
                                  total_sectors - ENTRY_SECTORS - 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("esp")
    ap.add_argument("--size-mib", type=int, default=128)
    ap.add_argument("--gpt", action="store_true")
    args = ap.parse_args()

    with open(args.esp, "rb") as f:
        esp = f.read()
    esp_sectors = (len(esp) + SECTOR - 1) // SECTOR
    total_sectors = args.size_mib * MIB // SECTOR
    if START_LBA + esp_sectors > total_sectors:
        raise SystemExit(
            f"mkdisk: {args.esp} ({esp_sectors} sectors) does not fit in "
            f"{args.size_mib} MiB")

    img = bytearray(total_sectors * SECTOR)
    if args.gpt:
        write_gpt(img, esp_sectors, total_sectors)
    else:
        write_mbr(img, esp_sectors)

    off = START_LBA * SECTOR
    img[off:off + len(esp)] = esp

    with open(args.output, "wb") as f:
        f.write(img)

    print(f"  DISK    {args.output} ({args.size_mib} MiB, "
          f"{'GPT' if args.gpt else 'MBR'}, ESP {len(esp)} bytes "
          f"at LBA {START_LBA})")


if __name__ == "__main__":
    main()
