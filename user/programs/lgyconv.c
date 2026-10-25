/* lgyconv: convert executables into the MCubeAIOS LGY container.
 *
 *   ELF64            -> LGY kind=NATIVE (fully executable)
 *   PE32+ (.exe/.dll)-> LGY kind=PE     (container only; the PE subsystem
 *                                        that maps Win32 images is not part
 *                                        of this build)
 *
 * Any other format is rejected with a clear reason instead of producing a
 * file the kernel cannot run.
 */

#include <mcube/libc.h>
#include <mcube/lgy.h>

#define ELFCLASS64 2
#define EM_X86_64 62
#define EM_AARCH64 183
#define EM_LOONGARCH 258
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define MAX_FILE (128ull << 20)

typedef struct PACKED {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf64_ehdr_t;

typedef struct PACKED {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
} elf64_phdr_t;

static void usage(void) {
    printf("usage: lgyconv <input> [output.lgy]\n");
    printf("  ELF64 x86_64/aarch64/loongarch64 -> native LGY\n");
    printf("  PE32+ (.exe/.dll)                -> LGY container (kind=PE)\n");
}

static int write_all(int fd, const void *buf, u64 len) {
    const u8 *p = (const u8 *)buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) return -1;
        p += n;
        len -= (u64)n;
    }
    return 0;
}

static u8 *load_file(const char *path, u64 *size_out) {
    mcube_stat_t st;
    if (stat(path, &st) != 0 || st.type != VFS_TYPE_FILE) {
        printf("lgyconv: cannot stat %s\n", path);
        return NULL;
    }
    if (st.size < 4 || st.size > MAX_FILE) {
        printf("lgyconv: %s has an unsupported size (%lu bytes)\n",
               path, (unsigned long)st.size);
        return NULL;
    }

    u8 *buf = (u8 *)malloc((size_t)st.size);
    if (!buf) {
        printf("lgyconv: out of memory (%lu bytes)\n", (unsigned long)st.size);
        return NULL;
    }

    int fd = open(path, VFS_O_RDONLY);
    if (fd < 0) {
        printf("lgyconv: cannot open %s\n", path);
        free(buf);
        return NULL;
    }
    u64 got = 0;
    while (got < st.size) {
        ssize_t n = read(fd, buf + got, (size_t)(st.size - got));
        if (n <= 0) break;
        got += (u64)n;
    }
    close(fd);
    if (got != st.size) {
        printf("lgyconv: short read on %s (%lu/%lu)\n", path,
               (unsigned long)got, (unsigned long)st.size);
        free(buf);
        return NULL;
    }
    *size_out = st.size;
    return buf;
}

static int store_lgy(const char *out, const lgy_header_t *h,
                     const lgy_segment_t *segs, const u8 *payload,
                     u64 payload_size) {
    int fd = open(out, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        printf("lgyconv: cannot create %s\n", out);
        return -1;
    }
    if (write_all(fd, h, sizeof(*h)) != 0 ||
        write_all(fd, segs, (u64)h->seg_count * sizeof(*segs)) != 0 ||
        write_all(fd, payload, payload_size) != 0) {
        printf("lgyconv: write failed for %s\n", out);
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int convert_elf(const char *out, const u8 *data, u64 size) {
    if (size < sizeof(elf64_ehdr_t)) return -1;
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != ELFCLASS64) {
        printf("lgyconv: not an ELF64 file\n");
        return -1;
    }
    if (eh->phentsize != sizeof(elf64_phdr_t) || eh->phnum == 0 ||
        eh->phnum > 64) {
        printf("lgyconv: unsupported ELF program headers\n");
        return -1;
    }
    if (eh->phoff + (u64)eh->phnum * eh->phentsize > size) {
        printf("lgyconv: ELF program headers outside the file\n");
        return -1;
    }

    u16 arch = 0;
    if (eh->machine == EM_X86_64) arch = LGY_ARCH_X86_64;
    else if (eh->machine == EM_AARCH64) arch = LGY_ARCH_AARCH64;
    else if (eh->machine == EM_LOONGARCH) arch = LGY_ARCH_LOONGARCH64;
    else {
        printf("lgyconv: unsupported ELF machine %u\n", (unsigned)eh->machine);
        return -1;
    }

    lgy_segment_t segs[LGY_MAX_SEGMENTS];
    u32 n = 0;
    u64 base = ~0ull, end = 0, payload_size = 0;

    for (u16 i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (n >= LGY_MAX_SEGMENTS) {
            printf("lgyconv: too many loadable segments\n");
            return -1;
        }
        if (ph->filesz > ph->memsz || ph->offset + ph->filesz > size) {
            printf("lgyconv: bad ELF segment %u\n", i);
            return -1;
        }
        u32 flags = 0;
        if (ph->flags & PF_X) flags |= LGY_SEG_X;
        if (ph->flags & PF_W) flags |= LGY_SEG_W;
        if (ph->flags & PF_R) flags |= LGY_SEG_R;

        lgy_segment_t *s = &segs[n++];
        s->vaddr = ph->vaddr;
        s->filesz = ph->filesz;
        s->memsz = ph->memsz;
        s->flags = flags;
        s->align = ph->align ? ph->align : 1;
        s->offset = 0;

        if (ph->vaddr < base) base = ph->vaddr;
        if (ph->vaddr + ph->memsz > end) end = ph->vaddr + ph->memsz;
        payload_size = (payload_size + LGY_ALIGN - 1) & ~(u64)(LGY_ALIGN - 1);
        payload_size += ph->filesz;
    }

    if (n == 0 || base == ~0ull || end <= base) {
        printf("lgyconv: no loadable ELF segments\n");
        return -1;
    }

    u32 header_size = (u32)(sizeof(lgy_header_t) + n * sizeof(lgy_segment_t));
    u8 *payload = (u8 *)malloc((size_t)payload_size);
    if (!payload) {
        printf("lgyconv: out of memory for payload\n");
        return -1;
    }

    u64 cursor = 0;
    u32 si = 0;
    for (u16 i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        cursor = (cursor + LGY_ALIGN - 1) & ~(u64)(LGY_ALIGN - 1);
        segs[si].offset = header_size + cursor;
        if (ph->filesz) memcpy(payload + cursor, data + ph->offset, ph->filesz);
        cursor += ph->filesz;
        si++;
    }

    lgy_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic[0] = LGY_MAGIC0;
    h.magic[1] = LGY_MAGIC1;
    h.magic[2] = LGY_MAGIC2;
    h.magic[3] = LGY_MAGIC3;
    h.version = LGY_VERSION;
    h.arch = arch;
    h.kind = LGY_KIND_NATIVE;
    h.entry = eh->entry;
    h.image_base = base;
    h.image_size = end - base;
    h.seg_count = n;
    h.header_size = header_size;
    h.crc32 = lgy_crc32(payload, payload_size);

    int rc = store_lgy(out, &h, segs, payload, payload_size);
    free(payload);
    if (rc == 0) {
        printf("lgyconv: native LGY written: %s\n", out);
        printf("  arch=%u entry=0x%lx segs=%u image=0x%lx+0x%lx\n",
               (unsigned)arch, (unsigned long)h.entry, n,
               (unsigned long)base, (unsigned long)h.image_size);
    }
    return rc;
}

static int convert_pe(const char *out, const u8 *data, u64 size) {
    if (size < 64 || data[0] != 'M' || data[1] != 'Z') return -1;

    u32 pe_off = (u32)data[0x3C] | ((u32)data[0x3D] << 8) |
                 ((u32)data[0x3E] << 16) | ((u32)data[0x3F] << 24);
    if (pe_off + 24 > size ||
        data[pe_off] != 'P' || data[pe_off + 1] != 'E' ||
        data[pe_off + 2] != 0 || data[pe_off + 3] != 0) {
        printf("lgyconv: invalid PE header\n");
        return -1;
    }

    const u8 *coff = data + pe_off + 4;
    u16 machine = (u16)(coff[0] | (coff[1] << 8));
    u16 nsections = (u16)(coff[2] | (coff[3] << 8));
    u16 opt_size = (u16)(coff[16] | (coff[17] << 8));
    u32 opt_off = pe_off + 24;
    if (opt_off + opt_size > size || opt_size < 64) {
        printf("lgyconv: truncated PE optional header\n");
        return -1;
    }

    const u8 *opt = data + opt_off;
    u16 magic = (u16)(opt[0] | (opt[1] << 8));
    if (magic != 0x20B) {
        printf("lgyconv: only PE32+ images are supported (magic 0x%x)\n",
               (unsigned)magic);
        return -1;
    }
    if (machine != 0x8664 && machine != 0xAA64) {
        printf("lgyconv: unsupported PE machine 0x%x\n", (unsigned)machine);
        return -1;
    }

#define RD32(p, o) ((u32)(p)[o] | ((u32)(p)[(o) + 1] << 8) | \
                    ((u32)(p)[(o) + 2] << 16) | ((u32)(p)[(o) + 3] << 24))
#define RD64(p, o) ((u64)RD32(p, o) | ((u64)RD32(p, (o) + 4) << 32))

    u32 entry_rva = RD32(opt, 16);
    u64 image_base = RD64(opt, 24);
    u32 image_size = RD32(opt, 56);
    u32 headers_size = RD32(opt, 60);

    printf("lgyconv: PE32+ %s, %u sections, base=0x%lx entry=0x%lx\n",
           machine == 0x8664 ? "x86_64" : "arm64", (unsigned)nsections,
           (unsigned long)image_base,
           (unsigned long)(image_base + entry_rva));

    /* A PE image is stored whole: the PE subsystem (not the native loader)
     * owns section mapping, imports and relocations. */
    u32 n = 1;
    u64 header_size = sizeof(lgy_header_t) + (u64)n * sizeof(lgy_segment_t);
    lgy_segment_t seg;
    seg.vaddr = image_base;
    seg.filesz = size;
    seg.memsz = image_size > size ? image_size : size;
    seg.offset = header_size;
    seg.flags = LGY_SEG_R | LGY_SEG_X;
    seg.align = 0x1000;

    lgy_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic[0] = LGY_MAGIC0;
    h.magic[1] = LGY_MAGIC1;
    h.magic[2] = LGY_MAGIC2;
    h.magic[3] = LGY_MAGIC3;
    h.version = LGY_VERSION;
    h.arch = machine == 0x8664 ? LGY_ARCH_X86_64 : LGY_ARCH_AARCH64;
    h.kind = LGY_KIND_PE;
    h.entry = image_base + entry_rva;
    h.image_base = image_base;
    h.image_size = seg.memsz;
    h.seg_count = n;
    h.header_size = (u32)header_size;
    h.crc32 = lgy_crc32(data, size);

    int rc = store_lgy(out, &h, &seg, data, size);
    if (rc == 0) {
        printf("lgyconv: PE container written: %s\n", out);
        printf("  headers=%u bytes; kind=PE\n", headers_size);
        printf("  NOTE: this build has no PE32+ runtime, so the image is\n");
        printf("        stored and identifiable but cannot be executed yet.\n");
    }
    return rc;
}

static int default_output(const char *in, char *out, u64 cap) {
    u64 len = strlen(in);
    if (len + 5 > cap) return -1;
    memcpy(out, in, len + 1);

    i64 dot = -1;
    for (u64 i = 0; i < len; i++) {
        if (out[i] == '/') dot = -1;
        else if (out[i] == '.') dot = (i64)i;
    }
    u64 base = dot > 0 ? (u64)dot : len;
    if (base + 5 > cap) return -1;
    memcpy(out + base, ".lgy", 5);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    const char *in = argv[1];
    char auto_out[256];
    const char *out = argv[2];
    if (!out) {
        if (default_output(in, auto_out, sizeof(auto_out)) != 0) {
            printf("lgyconv: input path is too long\n");
            return 2;
        }
        out = auto_out;
    }

    u64 size = 0;
    u8 *data = load_file(in, &size);
    if (!data) return 1;

    int rc;
    if (lgy_has_magic(data, size)) {
        printf("lgyconv: %s is already an LGY file\n", in);
        rc = 0;
    } else if (data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' &&
               data[3] == 'F') {
        rc = convert_elf(out, data, size);
    } else if (data[0] == 'M' && data[1] == 'Z') {
        rc = convert_pe(out, data, size);
    } else {
        printf("lgyconv: unsupported input format (not ELF64 or PE32+)\n");
        rc = 1;
    }

    free(data);
    return rc == 0 ? 0 : 1;
}
