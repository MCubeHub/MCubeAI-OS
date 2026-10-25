#pragma once
/* LGY v1: the MCubeAIOS executable container.
 *
 * A .lgy file is a self-describing image: a fixed header followed by a
 * segment table and the segment payload. The kernel maps the segments
 * directly; it never parses ELF at exec time. The arch tag keeps an
 * x86_64 build from being executed on an ARM64 machine, and the payload
 * CRC catches truncated or corrupted files before any page is mapped.
 *
 * Layout (little endian):
 *   lgy_header_t
 *   lgy_segment_t[seg_count]
 *   payload[header_size .. file size)
 */

#include <mcube/types.h>

#define LGY_MAGIC0 'L'
#define LGY_MAGIC1 'G'
#define LGY_MAGIC2 'Y'
#define LGY_MAGIC3 0x1A

#define LGY_VERSION 1u

#define LGY_ARCH_X86_64      1u
#define LGY_ARCH_AARCH64     2u
#define LGY_ARCH_LOONGARCH64 3u

#define LGY_KIND_NATIVE 1u  /* mapped segments, executable */
#define LGY_KIND_PE     2u  /* Windows PE image; needs the PE subsystem */
#define LGY_KIND_FLAT   3u  /* raw blob loaded at a fixed address */
#define LGY_KIND_SCRIPT 4u  /* interpreter path in the payload */

#define LGY_SEG_X 1u
#define LGY_SEG_W 2u
#define LGY_SEG_R 4u

#define LGY_MAX_SEGMENTS 16u
#define LGY_ALIGN 16u

typedef struct PACKED lgy_header {
    u8  magic[4];       /* "LGY\x1a" */
    u16 version;        /* LGY_VERSION */
    u16 arch;           /* LGY_ARCH_* */
    u32 kind;           /* LGY_KIND_* */
    u32 flags;          /* reserved, must be 0 */
    u64 entry;          /* virtual entry point */
    u64 image_base;     /* lowest segment virtual address */
    u64 image_size;     /* highest end - image_base */
    u32 seg_count;      /* 1..LGY_MAX_SEGMENTS */
    u32 header_size;    /* sizeof(header) + seg_count * sizeof(segment) */
    u32 crc32;          /* IEEE CRC-32 of payload, 0 = unchecked */
    u32 reserved;       /* must be 0 */
} lgy_header_t;

typedef struct PACKED lgy_segment {
    u64 vaddr;          /* virtual address */
    u64 filesz;         /* bytes present in the file */
    u64 memsz;          /* bytes present in memory (>= filesz) */
    u64 offset;         /* file offset of the data */
    u32 flags;          /* LGY_SEG_* */
    u32 align;          /* virtual alignment, power of two */
} lgy_segment_t;

static inline int lgy_has_magic(const void *image, u64 size) {
    if (!image || size < 4) return 0;
    const u8 *p = (const u8 *)image;
    return p[0] == LGY_MAGIC0 && p[1] == LGY_MAGIC1 &&
           p[2] == LGY_MAGIC2 && p[3] == LGY_MAGIC3;
}

static inline u32 lgy_crc32(const void *data, u64 size) {
    const u8 *p = (const u8 *)data;
    u32 crc = 0xFFFFFFFFu;
    for (u64 i = 0; i < size; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
