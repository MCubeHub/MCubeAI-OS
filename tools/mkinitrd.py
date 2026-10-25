#!/usr/bin/env python3
"""Pack the MCubeAIOS runtime into a compact MCIR initrd image.

The UEFI loader reads the resulting file with firmware services only, so the
same image boots from a USB stick, a CD/ISO or a hard disk even when the
kernel has no driver for the boot medium.

Format (little endian):
  header:  "MCIR" u32 version=1 u32 count u32 reserved
  entry:   u16 name_len u8 type(1=dir,2=file) u8 flags
           u32 data_len u32 reserved
           name (no NUL, '/' separated, no leading slash)
           data padded to 4 bytes

Directories are emitted implicitly by the kernel parser; this packer writes
files only.

Usage:
  mkinitrd.py OUT.img --kernel KERNEL.ELF --model TINY.GGU [--prog X.LGY ...]
"""

import argparse
import struct
import sys

MAGIC = b"MCIR"
VERSION = 1
TYPE_FILE = 2


def add(entries, arcname, path):
    with open(path, "rb") as f:
        data = f.read()
    entries.append((arcname.strip("/").lower(), data))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--model")
    ap.add_argument("--prog", action="append", default=[])
    args = ap.parse_args()

    entries = []
    add(entries, "kernel.elf", args.kernel)
    if args.model:
        add(entries, "models/tiny.ggu", args.model)
    for p in args.prog:
        name = p.replace("\\", "/").rsplit("/", 1)[-1]
        add(entries, "bin/" + name, p)

    entries.sort(key=lambda e: e[0])

    out = bytearray()
    out += MAGIC
    out += struct.pack("<III", VERSION, len(entries), 0)
    for name, data in entries:
        raw = name.encode("ascii")
        if len(raw) > 65535:
            sys.exit(f"mkinitrd: name too long: {name}")
        out += struct.pack("<HBBII", len(raw), TYPE_FILE, 0, len(data), 0)
        out += raw
        out += b"\0" * (-len(raw) % 4)
        out += data
        out += b"\0" * (-len(data) % 4)

    with open(args.output, "wb") as f:
        f.write(out)

    total = sum(len(d) for _, d in entries)
    print(f"  INITRD  {args.output} ({len(entries)} files, {total} bytes payload)")
    for name, data in entries:
        print(f"          {name:<24} {len(data):>8}")


if __name__ == "__main__":
    main()
