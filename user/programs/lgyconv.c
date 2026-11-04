/* lgyconv: convert executables into the MCubeAIOS LGY container.
 *
 *   ELF64             -> LGY kind=NATIVE (fully executable)
 *   PE32+ (.exe/.dll) -> LGY kind=PE     (container only; the PE subsystem
 *                                        that maps Win32 images is not part
 *                                        of this build)
 *
 * Any other format is rejected with a clear reason instead of producing a
 * file the kernel cannot run.
 *
 * Two entry points share one conversion core (lgy_convert):
 *   - CLI: "lgyconv <input> [output.lgy]" prints to the console.
 *   - GUI: "LGY Builder", started by the desktop, which passes the command
 *          pipe on fd 3 and the event pipe on fd 4 (see <mcube/gui.h>).
 * gui_init() is also the detector: it only succeeds when the INIT message
 * reaches the desktop, so a shell invocation without those pipes falls back
 * to the CLI and keeps its messages and exit codes.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>
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

/* ------------------------------------------------------------------ */
/* Formats                                                            */
/* ------------------------------------------------------------------ */

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

/* What the converter learned about an input file; the GUI shows it, the CLI
 * ignores it. */
typedef struct lgy_info {
    char format[24];    /* "ELF64 x86_64", "PE32+", "LGY", "unknown" */
    u64  entry;
    u32  segments;
    u64  size;          /* input file size */
    u64  out_size;      /* bytes written */
    int  already;       /* input was already an LGY file */
} lgy_info_t;

/* Console reporter: every conversion message is printed for the CLI and, when
 * the GUI asked for one, copied into the reason buffer used by the status bar.
 * g_quiet is set in GUI mode so the serial log stays clean. */
static char *g_why;
static int   g_why_len;
static int   g_quiet;

static void say(const char *fmt, ...) {
    char buf[192];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    if (g_why && g_why_len > 0)
        snprintf(g_why, (size_t)g_why_len, "%s", buf);
    if (!g_quiet) printf("%s\n", buf);
}

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
        say("lgyconv: cannot stat %s", path);
        return NULL;
    }
    if (st.size < 4 || st.size > MAX_FILE) {
        say("lgyconv: %s has an unsupported size (%lu bytes)",
            path, (unsigned long)st.size);
        return NULL;
    }

    u8 *buf = (u8 *)malloc((size_t)st.size);
    if (!buf) {
        say("lgyconv: out of memory (%lu bytes)", (unsigned long)st.size);
        return NULL;
    }

    int fd = open(path, VFS_O_RDONLY);
    if (fd < 0) {
        say("lgyconv: cannot open %s", path);
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
        say("lgyconv: short read on %s (%lu/%lu)", path,
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
        say("lgyconv: cannot create %s", out);
        return -1;
    }
    if (write_all(fd, h, sizeof(*h)) != 0 ||
        write_all(fd, segs, (u64)h->seg_count * sizeof(*segs)) != 0 ||
        write_all(fd, payload, payload_size) != 0) {
        say("lgyconv: write failed for %s", out);
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void set_format(lgy_info_t *info, const char *s) {
    snprintf(info->format, sizeof(info->format), "%s", s);
}

static int convert_elf(const char *out, const u8 *data, u64 size,
                       lgy_info_t *info) {
    if (size < sizeof(elf64_ehdr_t)) return -1;
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != ELFCLASS64) {
        say("lgyconv: not an ELF64 file");
        return -1;
    }
    if (eh->phentsize != sizeof(elf64_phdr_t) || eh->phnum == 0 ||
        eh->phnum > 64) {
        say("lgyconv: unsupported ELF program headers");
        return -1;
    }
    if (eh->phoff + (u64)eh->phnum * eh->phentsize > size) {
        say("lgyconv: ELF program headers outside the file");
        return -1;
    }

    u16 arch = 0;
    if (eh->machine == EM_X86_64) arch = LGY_ARCH_X86_64;
    else if (eh->machine == EM_AARCH64) arch = LGY_ARCH_AARCH64;
    else if (eh->machine == EM_LOONGARCH) arch = LGY_ARCH_LOONGARCH64;
    else {
        say("lgyconv: unsupported ELF machine %u", (unsigned)eh->machine);
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
            say("lgyconv: too many loadable segments");
            return -1;
        }
        if (ph->filesz > ph->memsz || ph->offset + ph->filesz > size) {
            say("lgyconv: bad ELF segment %u", i);
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
        say("lgyconv: no loadable ELF segments");
        return -1;
    }

    u32 header_size = (u32)(sizeof(lgy_header_t) + n * sizeof(lgy_segment_t));
    u8 *payload = (u8 *)malloc((size_t)payload_size);
    if (!payload) {
        say("lgyconv: out of memory for payload");
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

    set_format(info, arch == LGY_ARCH_X86_64 ? "ELF64 x86_64" :
                     arch == LGY_ARCH_AARCH64 ? "ELF64 aarch64" :
                                               "ELF64 loongarch64");
    info->entry = h.entry;
    info->segments = n;
    info->out_size = (u64)header_size + payload_size;

    if (rc == 0 && !g_quiet) {
        printf("lgyconv: native LGY written: %s\n", out);
        printf("  arch=%u entry=0x%lx segs=%u image=0x%lx+0x%lx\n",
               (unsigned)arch, (unsigned long)h.entry, n,
               (unsigned long)base, (unsigned long)h.image_size);
    }
    return rc;
}

static int convert_pe(const char *out, const u8 *data, u64 size,
                      lgy_info_t *info) {
    if (size < 64 || data[0] != 'M' || data[1] != 'Z') return -1;

    u32 pe_off = (u32)data[0x3C] | ((u32)data[0x3D] << 8) |
                 ((u32)data[0x3E] << 16) | ((u32)data[0x3F] << 24);
    if (pe_off + 24 > size ||
        data[pe_off] != 'P' || data[pe_off + 1] != 'E' ||
        data[pe_off + 2] != 0 || data[pe_off + 3] != 0) {
        say("lgyconv: invalid PE header");
        return -1;
    }

    const u8 *coff = data + pe_off + 4;
    u16 machine = (u16)(coff[0] | (coff[1] << 8));
    u16 nsections = (u16)(coff[2] | (coff[3] << 8));
    u16 opt_size = (u16)(coff[16] | (coff[17] << 8));
    u32 opt_off = pe_off + 24;
    if (opt_off + opt_size > size || opt_size < 64) {
        say("lgyconv: truncated PE optional header");
        return -1;
    }

    const u8 *opt = data + opt_off;
    u16 magic = (u16)(opt[0] | (opt[1] << 8));
    if (magic != 0x20B) {
        say("lgyconv: only PE32+ images are supported (magic 0x%x)",
            (unsigned)magic);
        return -1;
    }
    if (machine != 0x8664 && machine != 0xAA64) {
        say("lgyconv: unsupported PE machine 0x%x", (unsigned)machine);
        return -1;
    }

#define RD32(p, o) ((u32)(p)[o] | ((u32)(p)[(o) + 1] << 8) | \
                    ((u32)(p)[(o) + 2] << 16) | ((u32)(p)[(o) + 3] << 24))
#define RD64(p, o) ((u64)RD32(p, o) | ((u64)RD32(p, (o) + 4) << 32))

    u32 entry_rva = RD32(opt, 16);
    u64 image_base = RD64(opt, 24);
    u32 image_size = RD32(opt, 56);
    u32 headers_size = RD32(opt, 60);

    if (!g_quiet) {
        printf("lgyconv: PE32+ %s, %u sections, base=0x%lx entry=0x%lx\n",
               machine == 0x8664 ? "x86_64" : "arm64", (unsigned)nsections,
               (unsigned long)image_base,
               (unsigned long)(image_base + entry_rva));
    }

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

    set_format(info, "PE32+");
    info->entry = h.entry;
    info->segments = n;
    info->out_size = header_size + size;

    if (rc == 0 && !g_quiet) {
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

/* The single conversion core used by the CLI and the GUI. Returns 0 on
 * success, -1 on failure; a human readable reason is stored in why. */
static int lgy_convert(const char *in, const char *out, lgy_info_t *info,
                       char *why, int why_len) {
    lgy_info_t local;
    if (!info) info = &local;
    memset(info, 0, sizeof(*info));
    set_format(info, "unknown");

    if (why && why_len > 0) why[0] = 0;
    g_why = why;
    g_why_len = why_len;

    u64 size = 0;
    u8 *data = load_file(in, &size);
    if (!data) {
        g_why = NULL;
        g_why_len = 0;
        return -1;
    }
    info->size = size;

    int rc;
    if (lgy_has_magic(data, size)) {
        say("lgyconv: %s is already an LGY file", in);
        set_format(info, "LGY");
        info->already = 1;
        info->out_size = size;
        rc = 0;
    } else if (data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' &&
               data[3] == 'F') {
        rc = convert_elf(out, data, size, info);
    } else if (data[0] == 'M' && data[1] == 'Z') {
        rc = convert_pe(out, data, size, info);
    } else {
        say("lgyconv: unsupported input format (not ELF64 or PE32+)");
        rc = -1;
    }

    free(data);
    g_why = NULL;
    g_why_len = 0;
    return rc;
}

static int cli_main(int argc, char **argv) {
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

    lgy_info_t info;
    char why[192];
    int rc = lgy_convert(in, out, &info, why, (int)sizeof(why));
    return rc == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* LGY Builder window                                                 */
/* ------------------------------------------------------------------ */

#define MAX_ENTRIES 128
#define TITLE_H     24
#define STATUS_H    22
#define LIST_W      300
#define LIST_TOP    44
#define ROW_H       18

#define C_BG     0x0E141C
#define C_PANEL  0x161F2B
#define C_ACCENT 0x3B82F6
#define C_DIM    0x8FA0B6
#define C_TEXT   0xE6ECF5
#define C_ERR    0xFF5A5A
#define C_FIELD  0x0B1119
#define C_WHITE  0xFFFFFF

typedef struct {
    char name[64];
    char path[128];
    u64  size;
} cand_t;

static cand_t g_list[MAX_ENTRIES];
static int g_count;
static int g_sel;
static int g_top;
static char g_target[160];
static char g_status[200];
static int g_status_ok = 1;
static u64 g_last_scan_ms;
static lgy_info_t g_sel_info;
static int g_need_redraw = 1;

static int g_btn_conv_x, g_btn_conv_y, g_btn_conv_w, g_btn_conv_h;
static int g_btn_files_x, g_btn_files_y, g_btn_files_w, g_btn_files_h;

static u64 now_ms(void) {
    mcube_time_t ts;
    if (mc_time(&ts) != 0) return 0;
    return ts.uptime_ms;
}

/* Case-insensitive suffix test: FAT names may arrive upper-cased. */
static int ends_with(const char *name, const char *ext) {
    size_t n = strlen(name), m = strlen(ext);
    if (n < m) return 0;
    for (size_t i = 0; i < m; i++) {
        char a = name[n - m + i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != ext[i]) return 0;
    }
    return 1;
}

static int is_candidate(const char *name) {
    return ends_with(name, ".elf") || ends_with(name, ".exe") ||
           ends_with(name, ".dll") || ends_with(name, ".lgy");
}

static void add_entry(const char *full, const char *name, u64 size) {
    if (g_count >= MAX_ENTRIES) return;
    cand_t *c = &g_list[g_count++];
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->path, sizeof(c->path), "%s", full);
    c->size = size;
}

static void scan_dir(const char *dir) {
    int fd = open(dir, VFS_O_RDONLY);
    if (fd < 0) return;

    mcube_dirent_t ent;
    while (g_count < MAX_ENTRIES && getdents64(fd, &ent, sizeof(ent)) > 0) {
        u32 n = ent.namelen;
        if (n >= sizeof(ent.name)) n = sizeof(ent.name) - 1;
        char name[80];
        memcpy(name, ent.name, n);
        name[n] = 0;
        if (!is_candidate(name)) continue;

        char full[160];
        if (strcmp(dir, "/") == 0) snprintf(full, sizeof(full), "/%s", name);
        else snprintf(full, sizeof(full), "%s/%s", dir, name);

        mcube_stat_t st;
        u64 size = 0;
        u32 type = ent.type;
        if (stat(full, &st) == 0) {
            type = st.type;
            size = st.size;
        }
        if (type != VFS_TYPE_FILE) continue;
        add_entry(full, name, size);
    }
    close(fd);
}

/* Bounded header probe so the details pane can show a format before the file
 * is converted. Only the first 512 bytes are read. */
static void probe_file(const char *path, lgy_info_t *info) {
    memset(info, 0, sizeof(*info));
    set_format(info, "unknown");

    mcube_stat_t st;
    if (stat(path, &st) != 0) return;
    info->size = st.size;

    u8 buf[512];
    int fd = open(path, VFS_O_RDONLY);
    if (fd < 0) return;
    u64 got = 0;
    while (got < sizeof(buf)) {
        ssize_t n = read(fd, buf + got, sizeof(buf) - (size_t)got);
        if (n <= 0) break;
        got += (u64)n;
    }
    close(fd);

    if (got < 4) return;
    if (lgy_has_magic(buf, got)) {
        set_format(info, "LGY");
        return;
    }

    if (buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') {
        if (got < sizeof(elf64_ehdr_t) || buf[4] != ELFCLASS64) return;
        const elf64_ehdr_t *eh = (const elf64_ehdr_t *)buf;
        const char *m = "unknown";
        if (eh->machine == EM_X86_64) m = "x86_64";
        else if (eh->machine == EM_AARCH64) m = "aarch64";
        else if (eh->machine == EM_LOONGARCH) m = "loongarch64";
        snprintf(info->format, sizeof(info->format), "ELF64 %s", m);
        info->entry = eh->entry;
        info->segments = eh->phnum;
        if (eh->phentsize == sizeof(elf64_phdr_t) && eh->phnum &&
            eh->phoff + (u64)eh->phnum * sizeof(elf64_phdr_t) <= got) {
            u32 loads = 0;
            for (u16 i = 0; i < eh->phnum; i++) {
                const elf64_phdr_t *ph = (const elf64_phdr_t *)
                    (buf + eh->phoff + (u64)i * sizeof(elf64_phdr_t));
                if (ph->type == PT_LOAD && ph->memsz) loads++;
            }
            info->segments = loads;
        }
        return;
    }

    if (buf[0] == 'M' && buf[1] == 'Z' && got >= 0x40) {
        u32 pe_off = (u32)buf[0x3C] | ((u32)buf[0x3D] << 8) |
                     ((u32)buf[0x3E] << 16) | ((u32)buf[0x3F] << 24);
        if (pe_off + 24 > got || buf[pe_off] != 'P' || buf[pe_off + 1] != 'E')
            return;
        u32 opt_off = pe_off + 24;
        if (opt_off + 64 > got) return;
        u16 magic = (u16)(buf[opt_off] | (buf[opt_off + 1] << 8));
        if (magic != 0x20B) return;
        const u8 *opt = buf + opt_off;
        u32 entry_rva = (u32)opt[16] | ((u32)opt[17] << 8) |
                        ((u32)opt[18] << 16) | ((u32)opt[19] << 24);
        u64 image_base = (u64)((u32)opt[24] | ((u32)opt[25] << 8) |
                               ((u32)opt[26] << 16) | ((u32)opt[27] << 24)) |
                         ((u64)((u32)opt[28] | ((u32)opt[29] << 8) |
                                ((u32)opt[30] << 16) | ((u32)opt[31] << 24)) << 32);
        set_format(info, "PE32+");
        info->entry = image_base + entry_rva;
        info->segments = 1;
    }
}

static int row_count(void) {
    int rows = (gui_height() - TITLE_H - STATUS_H - LIST_TOP - 4) / ROW_H;
    return rows < 1 ? 1 : rows;
}

static void sync_target(void) {
    if (g_count <= 0) {
        g_target[0] = 0;
        return;
    }
    char tmp[160];
    if (default_output(g_list[g_sel].path, tmp, sizeof(tmp)) == 0)
        snprintf(g_target, sizeof(g_target), "%s", tmp);
    else
        snprintf(g_target, sizeof(g_target), "%s.lgy", g_list[g_sel].path);
}

static void set_status(int ok, const char *msg) {
    g_status_ok = ok;
    snprintf(g_status, sizeof(g_status), "%s", msg);
    g_need_redraw = 1;
}

static void select_index(int idx) {
    if (g_count <= 0) return;
    if (idx < 0) idx = 0;
    if (idx >= g_count) idx = g_count - 1;
    g_sel = idx;
    sync_target();
    probe_file(g_list[g_sel].path, &g_sel_info);
    g_need_redraw = 1;
}

static void rescan(void) {
    char keep_path[128];
    keep_path[0] = 0;
    if (g_count > 0 && g_sel < g_count)
        snprintf(keep_path, sizeof(keep_path), "%s", g_list[g_sel].path);

    g_count = 0;
    scan_dir("/");
    scan_dir("/bin");

    int idx = 0;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_list[i].path, keep_path) == 0) {
            idx = i;
            break;
        }
    }
    if (idx >= g_count) idx = g_count > 0 ? g_count - 1 : 0;
    g_sel = idx;

    if (g_count == 0) {
        g_target[0] = 0;
        memset(&g_sel_info, 0, sizeof(g_sel_info));
    } else {
        if (!(keep_path[0] && strcmp(g_list[g_sel].path, keep_path) == 0))
            sync_target();
        probe_file(g_list[g_sel].path, &g_sel_info);
    }
    g_need_redraw = 1;
}

static void convert_selected(void) {
    if (g_count <= 0) {
        set_status(0, "No file selected");
        return;
    }

    const char *in = g_list[g_sel].path;
    char out[160];
    if (g_target[0]) {
        snprintf(out, sizeof(out), "%s", g_target);
    } else if (default_output(in, out, sizeof(out)) != 0) {
        set_status(0, "Target path is empty");
        return;
    }

    lgy_info_t info;
    char why[192];
    int rc = lgy_convert(in, out, &info, why, (int)sizeof(why));
    if (rc != 0) {
        set_status(0, why[0] ? why : "conversion failed");
        return;
    }

    if (info.already) {
        set_status(1, "input is already an LGY file, nothing written");
        return;
    }

    char msg[200];
    snprintf(msg, sizeof(msg), "wrote %s, %u segments, %lu bytes", out,
             (unsigned)info.segments, (unsigned long)info.out_size);
    set_status(1, msg);
    rescan();
}

static void draw(void) {
    int w = gui_width(), h = gui_height();
    int rows = row_count();
    int px = LIST_W + 16;
    int pw = w - px - 12;
    if (pw < 180) pw = 180;

    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + rows) g_top = g_sel - rows + 1;
    if (g_top < 0) g_top = 0;

    gui_clear(C_BG);

    /* Title strip */
    gui_fill(0, 0, w, TITLE_H, C_PANEL);
    gui_text(12, 4, C_ACCENT, "LGY Builder");
    gui_text(116, 4, C_DIM,
             "Convert ELF64 or PE32+ files into .lgy containers");

    /* Left pane: candidate files */
    gui_fill(0, TITLE_H, LIST_W, h - TITLE_H - STATUS_H, C_PANEL);
    gui_text(12, TITLE_H + 6, C_DIM, "Files in / and /bin");

    for (int r = 0; r < rows; r++) {
        int idx = g_top + r;
        if (idx >= g_count) break;
        int y = LIST_TOP + r * ROW_H;
        int sel = idx == g_sel;
        if (sel) gui_fill(6, y - 2, LIST_W - 12, ROW_H, C_ACCENT);

        char sz[24];
        snprintf(sz, sizeof(sz), "%lu", (unsigned long)g_list[idx].size);
        int sx = LIST_W - 12 - (int)strlen(sz) * 8;
        int maxc = (sx - 16) / 8;
        if (maxc < 0) maxc = 0;
        gui_textn(12, y, sel ? C_WHITE : C_TEXT, g_list[idx].name, maxc);
        gui_text(sx, y, sel ? C_WHITE : C_DIM, sz);
    }

    /* Right pane: details, target path, actions */
    if (g_count == 0) {
        gui_text(px, TITLE_H + 12, C_TEXT,
                 "No .elf, .exe, .dll or .lgy files found.");
    } else {
        int y = TITLE_H + 12;
        char line[128];
        gui_textn(px, y, C_ACCENT, g_list[g_sel].path, pw / 8);
        y += 22;
        snprintf(line, sizeof(line), "Format:   %s", g_sel_info.format);
        gui_text(px, y, C_TEXT, line); y += 18;
        snprintf(line, sizeof(line), "Entry:    0x%lx",
                 (unsigned long)g_sel_info.entry);
        gui_text(px, y, C_TEXT, line); y += 18;
        snprintf(line, sizeof(line), "Segments: %u",
                 (unsigned)g_sel_info.segments);
        gui_text(px, y, C_TEXT, line); y += 18;
        snprintf(line, sizeof(line), "Size:     %lu bytes",
                 (unsigned long)g_sel_info.size);
        gui_text(px, y, C_TEXT, line); y += 24;

        gui_text(px, y, C_DIM, "Target path"); y += 18;
        int fw = pw - 8;
        gui_fill(px, y, fw, 20, C_FIELD);
        gui_rect(px, y, fw, 20, C_ACCENT);
        gui_textn(px + 4, y + 2, C_TEXT, g_target, (fw - 12) / 8);
        y += 30;

        g_btn_conv_x = px; g_btn_conv_y = y;
        g_btn_conv_w = 96; g_btn_conv_h = 22;
        gui_fill(px, y, 96, 22, C_ACCENT);
        gui_text(px + 20, y + 3, C_WHITE, "Convert");

        g_btn_files_x = px + 108; g_btn_files_y = y;
        g_btn_files_w = 176; g_btn_files_h = 22;
        gui_fill(g_btn_files_x, y, 176, 22, C_PANEL);
        gui_rect(g_btn_files_x, y, 176, 22, C_ACCENT);
        gui_text(g_btn_files_x + 12, y + 3, C_TEXT, "Open folder (Files)");

        y += 26;
        gui_text(px, y, C_DIM,
                 "PE output is a container only, it cannot run yet.");
    }

    /* Status bar */
    gui_fill(0, h - STATUS_H, w, STATUS_H, C_PANEL);
    gui_textn(10, h - STATUS_H + 3, g_status_ok ? C_TEXT : C_ERR, g_status,
              (w - 20) / 8);
    gui_flush();
    g_need_redraw = 0;
}

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_KEY: {
        u32 k = ev->code;
        int rows = row_count();
        if (k == INPUT_KEY_UP) {
            if (g_sel > 0) select_index(g_sel - 1);
        } else if (k == INPUT_KEY_DOWN) {
            if (g_sel + 1 < g_count) select_index(g_sel + 1);
        } else if (k == INPUT_KEY_PGUP || k == INPUT_KEY_PGDN) {
            int t = g_sel + (k == INPUT_KEY_PGUP ? -rows : rows);
            if (t < 0) t = 0;
            if (t >= g_count) t = g_count > 0 ? g_count - 1 : 0;
            if (t != g_sel) select_index(t);
        } else if (k == INPUT_KEY_HOME) {
            select_index(0);
        } else if (k == INPUT_KEY_END) {
            select_index(g_count - 1);
        } else if (k == '\n' || k == '\r') {
            convert_selected();
        } else if (k == '\b') {
            size_t n = strlen(g_target);
            if (n > 0) g_target[n - 1] = 0;
            g_need_redraw = 1;
        } else if (k == 0x1B) {
            gui_close();
            exit(0);
        } else if (k == INPUT_KEY_F5) {
            rescan();
        } else if (k >= 0x20 && k < 0x7F) {
            size_t n = strlen(g_target);
            if (n + 1 < sizeof(g_target)) {
                g_target[n] = (char)k;
                g_target[n + 1] = 0;
            }
            g_need_redraw = 1;
        }
        break;
    }

    case GUI_EVT_MOUSE: {
        if (!(ev->buttons & 1)) break;
        int hit = 0;
        if (ev->x >= g_btn_conv_x && ev->x < g_btn_conv_x + g_btn_conv_w &&
            ev->y >= g_btn_conv_y && ev->y < g_btn_conv_y + g_btn_conv_h) {
            convert_selected();
            hit = 1;
        } else if (ev->x >= g_btn_files_x &&
                   ev->x < g_btn_files_x + g_btn_files_w &&
                   ev->y >= g_btn_files_y &&
                   ev->y < g_btn_files_y + g_btn_files_h) {
            gui_launch("/bin/files.lgy");
            hit = 1;
        }
        if (!hit && ev->x < LIST_W && ev->y >= LIST_TOP) {
            int idx = g_top + (ev->y - LIST_TOP) / ROW_H;
            if (idx >= 0 && idx < g_count) select_index(idx);
        }
        break;
    }

    case GUI_EVT_TICK: {
        u64 now = now_ms();
        if (now - g_last_scan_ms >= 2000) {
            g_last_scan_ms = now;
            rescan();
        }
        break;
    }

    case GUI_EVT_RESIZE:
        g_need_redraw = 1;
        break;

    case GUI_EVT_CLOSE:
        exit(0);

    default:
        break;
    }
}

static void gui_main(void) {
    g_quiet = 1;
    snprintf(g_status, sizeof(g_status), "%s",
             "Up/Down select  PgUp/PgDn scroll  Enter convert  F5 rescan  Esc exit");
    g_status_ok = 1;

    rescan();
    g_last_scan_ms = now_ms();
    draw();

    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 100);
        if (rc < 0) exit(0);
        if (rc > 0) handle_event(&ev);
        if (g_need_redraw) draw();
    }
}

int main(int argc, char **argv) {
    /* gui_init() only succeeds when the desktop is on the other end of fd 3,
     * so it doubles as the GUI/CLI detector: without the desktop pipe pair
     * (a shell invocation) the console path below runs unchanged. */
    if (gui_init("LGY Builder", 720, 470) == 0) {
        printf("USER_LGYCONV_OK: gui ready\n");
        gui_main();
    }
    return cli_main(argc, argv);
}
