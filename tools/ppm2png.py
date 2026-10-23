#!/usr/bin/env python3
"""Convert a binary P6 PPM (QEMU screendump output) to a PNG.

Kept dependency-free on purpose: the build machines only guarantee Python 3.
"""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()

    # Header: P6, then whitespace separated width, height and maxval. Comments
    # (lines starting with '#') may appear between any of the tokens.
    tokens = []
    i = 0
    while len(tokens) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if i < len(data) and data[i:i + 1] == b"#":
            while i < len(data) and data[i:i + 1] not in (b"\n", b"\r"):
                i += 1
            continue
        start = i
        while i < len(data) and not data[i:i + 1].isspace():
            i += 1
        tokens.append(data[start:i])
    i += 1  # single whitespace byte after maxval

    magic, width, height, maxval = tokens
    if magic != b"P6":
        raise SystemExit("not a P6 PPM")
    width, height, maxval = int(width), int(height), int(maxval)
    if maxval != 255:
        raise SystemExit("unsupported maxval %d" % maxval)
    pixels = data[i:i + width * height * 3]
    if len(pixels) != width * height * 3:
        raise SystemExit("truncated PPM")
    return width, height, pixels


def write_png(path, width, height, rgb):
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)  # filter: none
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: ppm2png.py in.ppm out.png")
    width, height, rgb = read_ppm(sys.argv[1])
    write_png(sys.argv[2], width, height, rgb)
    print("ppm2png: %dx%d -> %s" % (width, height, sys.argv[2]))


if __name__ == "__main__":
    main()
