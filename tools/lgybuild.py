#!/usr/bin/env python3
"""Wrap an ELF64 user program into an LGY v1 executable.

usage: lgybuild.py <input.elf> <output.lgy>

Only PT_LOAD segments are carried over. The result is what the kernel maps
at exec time; ELF is never parsed inside MCubeAIOS.
"""

import struct
import sys
import zlib

LGY_MAGIC = b"LGY\x1a"
LGY_VERSION = 1
LGY_ARCH_X86_64 = 1
LGY_KIND_NATIVE = 1
LGY_SEG_X, LGY_SEG_W, LGY_SEG_R = 1, 2, 4
LGY_MAX_SEGMENTS = 16
LGY_ALIGN = 16

HDR = struct.Struct("<4sHHIIQQQIIII")   # 56 bytes
SEG = struct.Struct("<QQQQII")          # 40 bytes
EHDR = struct.Struct("<16sHHIQQQIHHHHHH")
PHDR = struct.Struct("<IIQQQQQQ")

PT_LOAD = 1
PF_X, PF_W, PF_R = 1, 2, 4
EM_X86_64 = 62


def fail(msg: str) -> int:
    print(f"lgybuild: {msg}", file=sys.stderr)
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: lgybuild.py <input.elf> <output.lgy>", file=sys.stderr)
        return 2

    src, dst = sys.argv[1:3]
    data = open(src, "rb").read()
    if len(data) < EHDR.size:
        return fail(f"{src}: file too small")

    (ident, e_type, e_machine, e_version, e_entry, e_phoff, e_shoff,
     e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum,
     e_shstrndx) = EHDR.unpack_from(data, 0)

    if ident[:4] != b"\x7fELF" or ident[4] != 2:
        return fail(f"{src}: not an ELF64 file")
    if e_machine != EM_X86_64:
        return fail(f"{src}: machine {e_machine} is not x86_64")
    if e_phentsize != PHDR.size or e_phnum == 0:
        return fail(f"{src}: unsupported program header table")
    if e_phoff + e_phnum * e_phentsize > len(data):
        return fail(f"{src}: program headers outside the file")

    segs = []
    for i in range(e_phnum):
        (p_type, p_flags, p_offset, p_vaddr, p_paddr,
         p_filesz, p_memsz, p_align) = PHDR.unpack_from(
            data, e_phoff + i * e_phentsize)
        if p_type != PT_LOAD or p_memsz == 0:
            continue
        if p_filesz > p_memsz:
            return fail(f"{src}: segment {i} has filesz > memsz")
        if p_offset + p_filesz > len(data):
            return fail(f"{src}: segment {i} data outside the file")
        flags = 0
        if p_flags & PF_X:
            flags |= LGY_SEG_X
        if p_flags & PF_W:
            flags |= LGY_SEG_W
        if p_flags & PF_R:
            flags |= LGY_SEG_R
        segs.append((p_vaddr, p_filesz, p_memsz, flags, p_align, p_offset))

    if not segs:
        return fail(f"{src}: no loadable segments")
    if len(segs) > LGY_MAX_SEGMENTS:
        return fail(f"{src}: {len(segs)} segments (max {LGY_MAX_SEGMENTS})")

    base = min(s[0] for s in segs)
    end = max(s[0] + s[2] for s in segs)
    header_size = HDR.size + len(segs) * SEG.size

    payload = bytearray()
    table = []
    for vaddr, filesz, memsz, flags, align, offset in segs:
        while len(payload) % LGY_ALIGN:
            payload.append(0)
        file_off = header_size + len(payload)
        payload += data[offset:offset + filesz]
        table.append((vaddr, filesz, memsz, file_off, flags,
                      max(align, 1)))

    crc = zlib.crc32(payload) & 0xFFFFFFFF

    header = HDR.pack(LGY_MAGIC, LGY_VERSION, LGY_ARCH_X86_64,
                      LGY_KIND_NATIVE, 0, e_entry, base, end - base,
                      len(segs), header_size, crc, 0)
    segtab = b"".join(SEG.pack(*s) for s in table)

    with open(dst, "wb") as f:
        f.write(header)
        f.write(segtab)
        f.write(payload)

    print(f"  LGY     {dst} entry=0x{e_entry:x} segs={len(segs)} "
          f"size={header_size + len(payload)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
