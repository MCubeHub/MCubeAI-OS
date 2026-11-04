#!/usr/bin/env python3
"""Wrap the FAT32 ESP into a bootable disk image (MBR or GPT).

The result boots on UEFI firmware from a hard disk (IDE/SATA/VHD) and, when a
BIOS payload is supplied, on legacy BIOS as well. One EFI system partition at
LBA 2048 carries the loader, kernel, initrd and user programs; an optional
second partition carries the raw BIOS payload that stage1/stage2 find by their
"MCBB" magic.

MBR is the default for maximum compatibility; --gpt produces a modern GPT
layout with a protective MBR and a backup header.

Usage: mkdisk.py OUT.IMG ESP.IMG [--size-mib 128] [--gpt] [--mbr-code FILE]
                              [--bios-payload FILE]
"""

import argparse
import uuid
import zlib

SECTOR = 512
START_LBA = 2048          # 1 MiB alignment
ESP_TYPE_MBR = 0xEF
BIOS_TYPE_MBR = 0x83
MBR_CODE_BYTES = 432      # bytes the MBR boot code area may use (before the
                          # disk signature at 440 and the partition table at 446)
# GPT type GUID C12A7328-F81F-11D2-BA4B-00A0C93EC93B in on-disk byte order.
ESP_TYPE_GPT = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
# GPT type GUID 0FC63DAF-8483-4772-8E79-3D69D8477DE4 (Linux filesystem).
BIOS_TYPE_GPT = bytes.fromhex("af3dc60f838472478e793d69d8477de4")
MIB = 1024 * 1024
ENTRY_COUNT = 128
ENTRY_SIZE = 128
ENTRY_SECTORS = ENTRY_COUNT * ENTRY_SIZE // SECTOR


def gpt_guid_le():
    return uuid.uuid4().bytes_le


def partition_entry(img, slot, ptype, first_lba, sectors):
    e = bytearray(16)
    e[0] = 0x00                 # not active; UEFI ignores the boot flag
    e[1:4] = b"\xfe\xff\xff"    # CHS start (LBA addressing only)
    e[4] = ptype
    e[5:8] = b"\xfe\xff\xff"    # CHS end
    e[8:12] = first_lba.to_bytes(4, "little")
    e[12:16] = sectors.to_bytes(4, "little")
    off = 446 + slot * 16
    img[off:off + 16] = e


def write_mbr(img, esp_sectors, payload_lba=0, payload_sectors=0):
    img[440:444] = b"MCUB"      # disk signature
    img[510:512] = b"\x55\xaa"
    partition_entry(img, 0, ESP_TYPE_MBR, START_LBA, esp_sectors)
    if payload_sectors:
        partition_entry(img, 1, BIOS_TYPE_MBR, payload_lba, payload_sectors)


def write_gpt(img, esp_sectors, total_sectors, payload_lba=0,
              payload_sectors=0):
    first_usable = 2 + ENTRY_SECTORS
    last_usable = total_sectors - ENTRY_SECTORS - 2
    last_lba = START_LBA + esp_sectors - 1
    if last_lba > last_usable:
        raise SystemExit("mkdisk: ESP does not fit in the GPT usable range")

    entries = bytearray(ENTRY_COUNT * ENTRY_SIZE)

    def entry(slot, ptype, first, last, label):
        e = entries[slot * ENTRY_SIZE:]
        e[0:16] = ptype
        e[16:32] = gpt_guid_le()
        e[32:40] = first.to_bytes(8, "little")
        e[40:48] = last.to_bytes(8, "little")
        e[48:56] = (0).to_bytes(8, "little")
        name = label.encode("utf-16-le")
        e[56:56 + len(name)] = name

    entry(0, ESP_TYPE_GPT, START_LBA, last_lba, "MCUBEAIOS")
    if payload_sectors:
        entry(1, BIOS_TYPE_GPT, payload_lba, payload_lba + payload_sectors - 1,
              "MCUBEBIOS")

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

    # Protective MBR: one 0xEE entry covering the whole LBA range. In hybrid
    # mode a second entry points at the BIOS payload so stage1 can find it.
    p = bytearray(16)
    p[0] = 0x00
    p[1:4] = b"\xfe\xff\xff"
    p[4] = 0xEE
    p[5:8] = b"\xfe\xff\xff"
    p[8:12] = (1).to_bytes(4, "little")
    p[12:16] = min(total_sectors - 1, 0xFFFFFFFF).to_bytes(4, "little")
    img[446:462] = p
    if payload_sectors:
        partition_entry(img, 1, BIOS_TYPE_MBR, payload_lba, payload_sectors)
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
    ap.add_argument("--mbr-code", metavar="FILE", default=None,
                    help="raw MBR boot code (build/stage1.bin); its first "
                         "432 bytes are copied to LBA 0")
    ap.add_argument("--bios-payload", metavar="FILE", default=None,
                    help="raw BIOS payload (build/bios-payload.bin) placed in "
                         "its own partition for stage1/stage2")
    args = ap.parse_args()

    with open(args.esp, "rb") as f:
        esp = f.read()
    esp_sectors = (len(esp) + SECTOR - 1) // SECTOR

    payload = b""
    if args.bios_payload:
        with open(args.bios_payload, "rb") as f:
            payload = f.read()

    total_sectors = args.size_mib * MIB // SECTOR
    if START_LBA + esp_sectors > total_sectors:
        raise SystemExit(
            f"mkdisk: {args.esp} ({esp_sectors} sectors) does not fit in "
            f"{args.size_mib} MiB")

    payload_lba = 0
    payload_sectors = 0
    if payload:
        payload_sectors = (len(payload) + SECTOR - 1) // SECTOR
        payload_lba = (START_LBA + esp_sectors + 2047) // 2048 * 2048
        needed = payload_lba + payload_sectors + 2048
        if needed > total_sectors:
            total_sectors = needed

    img = bytearray(total_sectors * SECTOR)
    if args.gpt:
        write_gpt(img, esp_sectors, total_sectors, payload_lba, payload_sectors)
    else:
        write_mbr(img, esp_sectors, payload_lba, payload_sectors)

    # Boot code. Without it the boot code area is zeroes and a BIOS/CSM boot
    # of the stick dies on a blinking cursor with no explanation.
    if args.mbr_code:
        with open(args.mbr_code, "rb") as f:
            code = f.read()
        if len(code) < MBR_CODE_BYTES:
            raise SystemExit(
                f"mkdisk: {args.mbr_code} is {len(code)} bytes, "
                f"need at least {MBR_CODE_BYTES}")
        img[0:MBR_CODE_BYTES] = code[:MBR_CODE_BYTES]

    off = START_LBA * SECTOR
    img[off:off + len(esp)] = esp

    if payload:
        off = payload_lba * SECTOR
        img[off:off + len(payload)] = payload

    with open(args.output, "wb") as f:
        f.write(img)

    extra = ""
    if payload:
        extra = f", BIOS payload {len(payload)} bytes at LBA {payload_lba}"
    print(f"  DISK    {args.output} ({total_sectors * SECTOR // MIB} MiB, "
          f"{'GPT' if args.gpt else 'MBR'}, ESP {len(esp)} bytes "
          f"at LBA {START_LBA}{extra})")


if __name__ == "__main__":
    main()
