#!/usr/bin/env python3
"""Generate the sample pictures shipped with MCubeAIOS.

The album scans /pictures at start-up, so the boot images need a few real
bitmaps to exercise all three decoders (PNG, BMP, PPM). Everything is written
with the Python standard library only: PNG via zlib, BMP and PPM by hand.

Usage:
  gen_samples.py OUTDIR
"""

import math
import os
import struct
import sys
import zlib

W, H = 320, 200


def clamp(v):
    return 0 if v < 0 else (255 if v > 255 else int(v))


def lerp(a, b, t):
    return int(a + (b - a) * t)


def sunset(x, y):
    """Vertical sky gradient, sun disc with glow, and two hills."""
    horizon = 124
    if y < horizon:
        t = y / horizon
        r = lerp(24, 245, t)
        g = lerp(38, 156, t)
        b = lerp(86, 96, t)

        dx, dy = x - 238, y - 96
        d2 = dx * dx + dy * dy
        if d2 <= 24 * 24:
            r, g, b = 255, 236, 158
        elif d2 <= 40 * 40:
            k = 1.0 - (math.sqrt(d2) - 24) / 16.0
            r = clamp(r + 70 * k)
            g = clamp(g + 52 * k)
            b = clamp(b + 18 * k)
        return r, g, b

    # Water: mirror the sky, darker and banded.
    if y < 150:
        t = (y - horizon) / 26.0
        r = lerp(150, 40, t)
        g = lerp(104, 34, t)
        b = lerp(92, 52, t)
        if 140 <= x <= 300 and (y + x) % 7 < 2:
            r, g = clamp(r + 40), clamp(g + 26)
        dx = x - 238
        if abs(dx) < 18 and y < 146:
            r, g, b = clamp(r + 60), clamp(g + 40), clamp(b + 12)
        return r, g, b

    # Two overlapping hill silhouettes.
    h1 = 150 + int(16 * math.sin(x / 41.0))
    h2 = 168 + int(12 * math.sin((x + 90) / 33.0))
    if y >= h2:
        return 12, 18, 28
    if y >= h1:
        return 22, 30, 44
    t = (y - 150) / 18.0
    return lerp(52, 24, t), lerp(40, 30, t), lerp(46, 40, t)


def rings(x, y):
    """Concentric colour rings on a grid, plus a soft vignette."""
    dx, dy = x - 160, y - 100
    d = int(math.sqrt(dx * dx + dy * dy))
    palette = [
        (32, 40, 58), (76, 201, 240), (255, 209, 102),
        (239, 71, 111), (6, 214, 160), (131, 56, 236),
    ]
    base = palette[(d // 12) % len(palette)]
    shade = 1.0 - min(0.55, (dx * dx + dy * dy) / 40000.0)
    r = clamp(base[0] * shade)
    g = clamp(base[1] * shade)
    b = clamp(base[2] * shade)
    if 50 <= d <= 75 and (x + y) % 9 == 0:
        r, g, b = clamp(r + 45), clamp(g + 45), clamp(b + 45)
    if x % 40 == 0 or y % 40 == 0:
        r, g, b = clamp(r + 18), clamp(g + 18), clamp(b + 26)
    return r, g, b


def shade(x, y):
    """Diagonal gradient with a bright frame and a fine grid."""
    t = (x + y) / float(W + H)
    r = lerp(18, 214, t)
    g = lerp(92, 54, t)
    b = lerp(120, 196, t)
    if x in (24, 25, W - 25, W - 26) and 20 <= y < H - 20:
        r, g, b = 240, 246, 220
    if y in (20, 21, H - 21, H - 22) and 24 <= x < W - 24:
        r, g, b = 240, 246, 220
    if (x % 16 == 0 or y % 16 == 0) and 30 <= x < W - 30 and 26 <= y < H - 26:
        r, g, b = clamp(r + 22), clamp(g + 22), clamp(b + 26)
    return r, g, b


def write_png(path, w, h, fn):
    raw = bytearray()
    for y in range(h):
        raw.append(0)                       # filter: none
        for x in range(w):
            raw.extend(fn(x, y))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)   # RGB, 8-bit
    body = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
            chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(body)


def write_bmp(path, w, h, fn):
    row = ((w * 3 + 3) // 4) * 4
    data = bytearray()
    for y in range(h - 1, -1, -1):          # bottom-up
        for x in range(w):
            r, g, b = fn(x, y)
            data.extend((b, g, r))
        data.extend(b"\0" * (row - w * 3))

    filesize = 54 + len(data)
    header = b"BM" + struct.pack("<IHHI", filesize, 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data),
                       2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(header + info + data)


def write_ppm(path, w, h, fn):
    data = bytearray()
    for y in range(h):
        for x in range(w):
            data.extend(fn(x, y))
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(data)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gen_samples.py OUTDIR")
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)

    made = [
        ("SUNSET.PNG", write_png, sunset),
        ("RINGS.BMP", write_bmp, rings),
        ("SHADE.PPM", write_ppm, shade),
    ]
    for name, writer, fn in made:
        path = os.path.join(outdir, name)
        writer(path, W, H, fn)
        print("  PICTURE %-10s %dx%d %d bytes" % (name, W, H,
                                                  os.path.getsize(path)))


if __name__ == "__main__":
    main()
