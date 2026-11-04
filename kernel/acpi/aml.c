/* Minimal AML reader for battery telemetry.
 *
 * A full AML interpreter is not worth the risk this early in the boot: every
 * opcode that is mis-decoded becomes a wild read. What the shell actually
 * needs is four numbers -- state, discharge rate, remaining capacity and full
 * capacity -- and those live in _BST / _BIF / _BIX, which firmware writes as a
 * small, very repetitive subset of AML:
 *
 *   Method (_BST, 0) { Return (Package (0x04) { B0ST, B0RA, B0RC, B0VO }) }
 *   Method (_BIF, 0) { ... Store (...) ... Return (PKG) }
 *
 * So this module builds a deliberately flat namespace (leaf names only) by
 * scanning the DSDT for Field, OperationRegion and Method definitions, then
 * evaluates only the battery methods with a bounded subset of the term
 * grammar. Anything outside that subset aborts the evaluation and the caller
 * reports "no measurement" instead of inventing a number. Step and depth
 * budgets turn a malformed method into a non-event: it stops, it never hangs.
 */

#include <mcube/acpi.h>
#include <mcube/kernel.h>
#include <mcube/io.h>
#include <mcube/mm.h>

#define AML_MAX_FIELDS   192
#define AML_MAX_REGIONS  24
#define AML_MAX_METHODS  160
#define AML_ARENA        128
#define AML_PKG_ELEMS    32
#define AML_MAX_SCRATCH  8
#define AML_MAX_DEPTH    6
#define AML_MAX_STEPS    200000

#define AML_SPACE_MEM    0
#define AML_SPACE_IO     1
#define AML_SPACE_EC     3

#define V_INT 0
#define V_PKG 1
#define V_BUF 2

typedef struct {
    u8 type;
    u8 n;       /* package element count */
    u32 pi;     /* package arena index */
    u64 i;      /* integer value / buffer length */
} aval_t;

typedef struct {
    char name[5];
    char region[5];
    u64 bit_off;
    u32 bit_len;
} aml_field_t;

typedef struct {
    char name[5];
    u8 space;
    u64 base;
    u64 len;
} aml_region_t;

typedef struct {
    char name[5];
    u32 body;
    u32 end;
    u8 argc;
} aml_method_t;

typedef struct {
    const u8 *p;
    u32 pos;
    u32 end;
    aval_t args[7];
    aval_t locals[8];
} ev_t;

static const u8 *g_dsdt;
static u32 g_len;
static int g_ready;

static aml_field_t g_fields[AML_MAX_FIELDS];
static int g_field_n;
static aml_region_t g_regions[AML_MAX_REGIONS];
static int g_region_n;
static aml_method_t g_methods[AML_MAX_METHODS];
static int g_method_n;

static aval_t g_arena[AML_ARENA];
static u32 g_arena_n;
static char g_scratch_name[AML_MAX_SCRATCH][5];
static aval_t g_scratch_value[AML_MAX_SCRATCH];
static int g_scratch_n;

static int g_steps;
static int g_depth;
static int g_suspect;

static int eval_term(ev_t *e, aval_t *out);
static int exec_list(ev_t *e, u32 end, aval_t *ret, int *returned);
static int call_method(const char *name, const aval_t *args, int argc,
                       aval_t *out);
static int store_value(ev_t *e, const aval_t *v);

/* ---- byte helpers ----------------------------------------------------- */

static u32 rd32(const u8 *p, u32 off) {
    return (u32)p[off] | ((u32)p[off + 1] << 8) | ((u32)p[off + 2] << 16) |
           ((u32)p[off + 3] << 24);
}

static u64 rd64(const u8 *p, u32 off) {
    return (u64)rd32(p, off) | ((u64)rd32(p, off + 4) << 32);
}

static int name_char(u8 c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* One 4-character NameSeg. Returns 0 when the bytes are not a valid name,
 * which is how a false positive in the byte scan is rejected. */
static int name_seg(const u8 *p, u32 len, u32 *pos, char out[5]) {
    if (*pos + 4 > len) return 0;
    for (u32 k = 0; k < 4; k++) {
        if (!name_char(p[*pos + k])) return 0;
        out[k] = (char)p[*pos + k];
    }
    out[4] = 0;
    *pos += 4;
    return 1;
}

/* Parse a full NameString but keep only its last segment. A flat namespace is
 * sufficient here: battery field leaves (B0ST, B0RC, ...) are unique in
 * practice, and method names are always looked up by their leaf. */
static int name_string(const u8 *p, u32 len, u32 *pos, char out[5]) {
    while (*pos < len) {
        u8 c = p[*pos];
        if (c == 0x5C || c == 0x5E) {   /* root / parent prefix */
            *pos += 1;
            continue;
        }
        break;
    }
    if (*pos >= len) return 0;

    u8 c = p[*pos];
    if (c == 0x2E) {                    /* DualNamePrefix */
        char tmp[5];
        *pos += 1;
        if (!name_seg(p, len, pos, tmp)) return 0;
        return name_seg(p, len, pos, out);
    }
    if (c == 0x2F) {                    /* MultiNamePrefix */
        char tmp[5];
        if (*pos + 1 >= len) return 0;
        u8 count = p[*pos + 1];
        *pos += 2;
        if (count == 0) return 0;
        for (u8 k = 0; k < count; k++) {
            if (!name_seg(p, len, pos, (k + 1 == count) ? out : tmp)) return 0;
        }
        return 1;
    }
    return name_seg(p, len, pos, out);
}

/* AML PkgLength. The value counts from the lead byte through the end of the
 * object, which is what lets every caller skip an object it cannot decode. */
static int pkg_len(const u8 *p, u32 len, u32 *pos, u32 *total) {
    if (*pos >= len) return 0;
    u8 lead = p[*pos];
    u8 extra = (u8)(lead >> 6);
    if (*pos + 1u + extra > len) return 0;

    u32 val;
    if (extra == 0) {
        val = lead & 0x3Fu;
    } else {
        val = lead & 0x0Fu;
        for (u8 k = 0; k < extra; k++) {
            val |= (u32)p[*pos + 1 + k] << (4 + 8 * k);
        }
    }
    *pos += 1u + extra;
    *total = val;
    return val != 0;
}

/* Integer constant only (no names, no arithmetic). Used for region offsets. */
static int const_int(const u8 *p, u32 len, u32 *pos, u64 *out) {
    if (*pos >= len) return 0;
    u8 op = p[*pos];
    switch (op) {
    case 0x00: *out = 0; *pos += 1; return 1;
    case 0x01: *out = 1; *pos += 1; return 1;
    case 0x0A:
        if (*pos + 1 >= len) return 0;
        *out = p[*pos + 1];
        *pos += 2;
        return 1;
    case 0x0B:
        if (*pos + 2 >= len) return 0;
        *out = (u32)p[*pos + 1] | ((u32)p[*pos + 2] << 8);
        *pos += 3;
        return 1;
    case 0x0C:
        if (*pos + 4 >= len) return 0;
        *out = rd32(p, *pos + 1);
        *pos += 5;
        return 1;
    default:
        return 0;
    }
}

/* ---- namespace construction ------------------------------------------ */

static void scan_regions(void) {
    for (u32 i = 0; i + 4 < g_len && g_region_n < AML_MAX_REGIONS; i++) {
        if (g_dsdt[i] != 0x5B || g_dsdt[i + 1] != 0x80) continue;

        u32 pos = i + 2;
        char nm[5];
        if (!name_string(g_dsdt, g_len, &pos, nm)) continue;
        if (pos + 1 >= g_len) continue;

        u8 space = g_dsdt[pos++];
        u64 base = 0, size = 0;
        if (!const_int(g_dsdt, g_len, &pos, &base)) continue;
        if (!const_int(g_dsdt, g_len, &pos, &size)) continue;

        aml_region_t *r = &g_regions[g_region_n++];
        memcpy(r->name, nm, 5);
        r->space = space;
        r->base = base;
        r->len = size;
    }
}

/* Field lists are self-describing: reserved runs, access attributes and named
 * bit fields. Walking them is far safer than trying to decode the surrounding
 * device tree. */
static void scan_fields(void) {
    for (u32 i = 0; i + 8 < g_len && g_field_n < AML_MAX_FIELDS; i++) {
        if (g_dsdt[i] != 0x5B || g_dsdt[i + 1] != 0x81) continue;

        u32 pkg_start = i + 2;
        u32 pos = pkg_start;
        u32 total;
        if (!pkg_len(g_dsdt, g_len, &pos, &total)) continue;
        u32 end = pkg_start + total;
        if (end > g_len || end <= pos) continue;

        char region[5];
        if (!name_string(g_dsdt, end, &pos, region)) continue;
        if (pos >= end) continue;
        pos++;                          /* field flags */

        u64 bit = 0;
        while (pos < end && g_field_n < AML_MAX_FIELDS) {
            u8 op = g_dsdt[pos];
            if (op == 0x00) {            /* ReservedField */
                u32 bits;
                pos++;
                if (!pkg_len(g_dsdt, end, &pos, &bits)) break;
                bit += bits;
            } else if (op == 0x01) {     /* AccessField */
                if (pos + 3 > end) break;
                pos += 3;
            } else if (op == 0x02) {     /* ConnectField */
                pos++;
                if (pos >= end) break;
                if (g_dsdt[pos] == 0x11) {
                    u32 lead = pos + 1;
                    u32 bytes;
                    if (!pkg_len(g_dsdt, end, &lead, &bytes)) break;
                    u32 bend = pos + 1 + bytes;
                    if (bend > end) break;
                    pos = bend;
                } else {
                    char tmp[5];
                    if (!name_string(g_dsdt, end, &pos, tmp)) break;
                }
            } else if (op == 0x03) {     /* ExtendedAccessField */
                if (pos + 4 > end) break;
                pos += 4;
            } else {                     /* named bit field */
                char nm[5];
                u32 bits;
                if (!name_seg(g_dsdt, end, &pos, nm)) break;
                if (!pkg_len(g_dsdt, end, &pos, &bits)) break;
                aml_field_t *f = &g_fields[g_field_n++];
                memcpy(f->name, nm, 5);
                memcpy(f->region, region, 5);
                f->bit_off = bit;
                f->bit_len = bits;
                bit += bits;
            }
        }
    }
}

static void scan_methods(void) {
    for (u32 i = 0; i + 8 < g_len && g_method_n < AML_MAX_METHODS; i++) {
        if (g_dsdt[i] != 0x14) continue;

        u32 pkg_start = i + 1;
        u32 pos = pkg_start;
        u32 total;
        if (!pkg_len(g_dsdt, g_len, &pos, &total)) continue;
        u32 end = pkg_start + total;
        if (end > g_len || end <= pos + 5) continue;

        char nm[5];
        if (!name_string(g_dsdt, end, &pos, nm)) continue;
        if (pos >= end) continue;

        u8 flags = g_dsdt[pos++];
        aml_method_t *m = &g_methods[g_method_n++];
        memcpy(m->name, nm, 5);
        m->body = pos;
        m->end = end;
        m->argc = (u8)(flags & 0x07u);
    }
}

static aml_field_t *find_field(const char *name) {
    for (int i = 0; i < g_field_n; i++) {
        if (strcmp(g_fields[i].name, name) == 0) return &g_fields[i];
    }
    return NULL;
}

static aml_region_t *find_region(const char *name) {
    for (int i = 0; i < g_region_n; i++) {
        if (strcmp(g_regions[i].name, name) == 0) return &g_regions[i];
    }
    return NULL;
}

static aml_method_t *find_method(const char *name) {
    for (int i = 0; i < g_method_n; i++) {
        if (strcmp(g_methods[i].name, name) == 0) return &g_methods[i];
    }
    return NULL;
}

/* ---- field reads ------------------------------------------------------ */

static int region_byte(const aml_region_t *r, u32 off, u8 *out) {
    switch (r->space) {
    case AML_SPACE_EC: {
        u64 addr = r->base + off;
        if (addr > 0xFF) return -1;
        return ec_read((u8)addr, out);
    }
    case AML_SPACE_IO: {
        u64 port = r->base + off;
        if (port > 0xFFFF) return -1;
        *out = inb((u16)port);
        return 0;
    }
    case AML_SPACE_MEM: {
        const u8 *p = (const u8 *)phys_to_virt(r->base + off);
        if (!p) return -1;
        *out = *p;
        return 0;
    }
    default:
        return -1;
    }
}

static int field_read(const aml_field_t *f, u64 *out) {
    const aml_region_t *r = find_region(f->region);
    if (!r) return -1;

    u32 shift = (u32)(f->bit_off & 7u);
    u32 start = (u32)(f->bit_off >> 3);
    u32 bits = f->bit_len;
    if (bits > 64) bits = 64;

    u32 nbytes = (shift + bits + 7u) / 8u;
    if (nbytes == 0) nbytes = 1;
    if (nbytes > 8) nbytes = 8;

    u64 raw = 0;
    for (u32 k = 0; k < nbytes; k++) {
        u8 b;
        if (region_byte(r, start + k, &b) != 0) return -1;
        raw |= (u64)b << (8 * k);
    }
    raw >>= shift;
    if (bits < 64) raw &= (((u64)1 << bits) - 1u);
    *out = raw;
    return 0;
}

/* ---- evaluation ------------------------------------------------------- */

static int burn(void) {
    if (g_steps <= 0) return 0;
    g_steps--;
    return 1;
}

static aval_t val_int(u64 v) {
    aval_t a;
    a.type = V_INT;
    a.n = 0;
    a.pi = 0;
    a.i = v;
    return a;
}

static int scratch_set(const char *name, const aval_t *v) {
    for (int i = 0; i < g_scratch_n; i++) {
        if (strcmp(g_scratch_name[i], name) == 0) {
            g_scratch_value[i] = *v;
            return 0;
        }
    }
    if (g_scratch_n >= AML_MAX_SCRATCH) return -1;
    memcpy(g_scratch_name[g_scratch_n], name, 5);
    g_scratch_value[g_scratch_n] = *v;
    g_scratch_n++;
    return 0;
}

static int scratch_get(const char *name, aval_t *out) {
    for (int i = 0; i < g_scratch_n; i++) {
        if (strcmp(g_scratch_name[i], name) == 0) {
            *out = g_scratch_value[i];
            return 1;
        }
    }
    return 0;
}

static int invoke(ev_t *e, const aml_method_t *m, aval_t *out) {
    aval_t args[7];
    memset(args, 0, sizeof(args));
    for (u8 k = 0; k < m->argc; k++) {
        if (eval_term(e, &args[k]) < 0) return -1;
    }
    return call_method(m->name, args, m->argc, out);
}

static int eval_name(ev_t *e, aval_t *out) {
    char nm[5];
    if (!name_string(e->p, e->end, &e->pos, nm)) return -1;

    if (scratch_get(nm, out)) return 0;

    aml_field_t *f = find_field(nm);
    if (f) {
        u64 v = 0;
        if (field_read(f, &v) != 0) {
            g_suspect = 1;
            *out = val_int(0);
            return 0;
        }
        *out = val_int(v);
        return 0;
    }

    aml_method_t *m = find_method(nm);
    if (m) return invoke(e, m, out);

    /* Unresolved names are common in firmware methods (OSYS, conditions and
     * vendor globals). Treat them as zero but mark the reading unreliable so
     * a percentage is never published from a half-evaluated method. */
    g_suspect = 1;
    *out = val_int(0);
    return 0;
}

static int eval_term(ev_t *e, aval_t *out) {
    *out = val_int(0);
    if (!burn()) return -1;
    if (e->pos >= e->end) return -1;

    u8 op = e->p[e->pos];

    if (op >= 0x60 && op <= 0x67) {          /* Local0..7 */
        *out = e->locals[op - 0x60];
        e->pos++;
        return 0;
    }
    if (op >= 0x68 && op <= 0x6E) {          /* Arg0..6 */
        *out = e->args[op - 0x68];
        e->pos++;
        return 0;
    }

    switch (op) {
    case 0x00:
        *out = val_int(0);
        e->pos++;
        return 0;
    case 0x01:
        *out = val_int(1);
        e->pos++;
        return 0;
    case 0x0A:
        if (e->pos + 2 > e->end) return -1;
        *out = val_int(e->p[e->pos + 1]);
        e->pos += 2;
        return 0;
    case 0x0B:
        if (e->pos + 3 > e->end) return -1;
        *out = val_int((u32)e->p[e->pos + 1] |
                       ((u32)e->p[e->pos + 2] << 8));
        e->pos += 3;
        return 0;
    case 0x0C:
        if (e->pos + 5 > e->end) return -1;
        *out = val_int(rd32(e->p, e->pos + 1));
        e->pos += 5;
        return 0;
    case 0x0E:
        if (e->pos + 9 > e->end) return -1;
        *out = val_int(rd64(e->p, e->pos + 1));
        e->pos += 9;
        return 0;
    case 0x0D: {                             /* StringPrefix */
        u32 p = e->pos + 1;
        u32 n = 0;
        while (p + n < e->end && e->p[p + n]) n++;
        if (p + n >= e->end) return -1;
        e->pos = p + n + 1;
        out->type = V_BUF;
        out->i = n;
        return 0;
    }
    case 0x11: {                             /* Buffer */
        u32 lead = e->pos + 1;
        u32 total;
        if (!pkg_len(e->p, e->end, &lead, &total)) return -1;
        u32 bend = e->pos + 1 + total;
        if (bend > e->end) return -1;
        out->type = V_BUF;
        out->i = total;
        e->pos = bend;
        return 0;
    }
    case 0x12:                               /* Package */
    case 0x13: {                             /* VarPackage */
        u32 lead = e->pos + 1;
        u32 total;
        if (!pkg_len(e->p, e->end, &lead, &total)) return -1;
        u32 pend = e->pos + 1 + total;
        if (pend > e->end || pend <= lead) return -1;
        e->pos = lead;

        u32 count;
        if (op == 0x12) {
            if (e->pos >= pend) return -1;
            count = e->p[e->pos++];
        } else {
            aval_t nv;
            if (eval_term(e, &nv) < 0) return -1;
            count = (u32)nv.i;
        }
        if (count > AML_PKG_ELEMS) return -1;
        if (g_arena_n + count > AML_ARENA) return -1;

        u32 base = g_arena_n;
        for (u32 k = 0; k < count; k++) {
            if (e->pos >= pend) return -1;
            aval_t v;
            if (eval_term(e, &v) < 0) return -1;
            g_arena[base + k] = v;
        }
        g_arena_n = base + count;

        out->type = V_PKG;
        out->n = (u8)count;
        out->pi = base;
        out->i = count;
        e->pos = pend;
        return 0;
    }
    case 0x70: {                             /* Store */
        aval_t src;
        e->pos++;
        if (eval_term(e, &src) < 0) return -1;
        if (store_value(e, &src) < 0) return -1;
        *out = src;
        return 0;
    }
    case 0x72:                               /* Add */
    case 0x74:                               /* Subtract */
    case 0x77:                               /* Multiply */
    case 0x79:                               /* ShiftLeft */
    case 0x7A:                               /* ShiftRight */
    case 0x7B:                               /* And */
    case 0x7C:                               /* Nand */
    case 0x7D:                               /* Or */
    case 0x7E:                               /* Nor */
    case 0x7F:                               /* Xor */
    case 0x85: {                             /* Mod */
        aval_t a, b;
        e->pos++;
        if (eval_term(e, &a) < 0) return -1;
        if (eval_term(e, &b) < 0) return -1;
        u64 x = a.i, y = b.i, r = 0;
        switch (op) {
        case 0x72: r = x + y; break;
        case 0x74: r = x - y; break;
        case 0x77: r = x * y; break;
        case 0x79: r = (y > 63) ? 0 : (x << y); break;
        case 0x7A: r = (y > 63) ? 0 : (x >> y); break;
        case 0x7B: r = x & y; break;
        case 0x7C: r = ~(x & y); break;
        case 0x7D: r = x | y; break;
        case 0x7E: r = ~(x | y); break;
        case 0x7F: r = x ^ y; break;
        default:   r = (y == 0) ? 0 : (x % y); break;
        }
        *out = val_int(r);
        return 0;
    }
    case 0x78: {                             /* Divide: q,r targets */
        aval_t a, b;
        e->pos++;
        if (eval_term(e, &a) < 0) return -1;
        if (eval_term(e, &b) < 0) return -1;
        u64 q = (b.i == 0) ? 0 : (a.i / b.i);
        u64 rem = (b.i == 0) ? 0 : (a.i % b.i);
        aval_t qv = val_int(q);
        aval_t rv = val_int(rem);
        if (store_value(e, &qv) < 0) return -1;
        if (store_value(e, &rv) < 0) return -1;
        *out = qv;
        return 0;
    }
    case 0x80: {                             /* Not */
        aval_t a;
        e->pos++;
        if (eval_term(e, &a) < 0) return -1;
        *out = val_int(~a.i);
        return 0;
    }
    case 0x81:                               /* FindSetLeftBit */
    case 0x82: {                             /* FindSetRightBit */
        aval_t a;
        e->pos++;
        if (eval_term(e, &a) < 0) return -1;
        u64 r = 0;
        if (op == 0x81) {
            for (int k = 63; k >= 0; k--) {
                if (a.i & ((u64)1 << k)) {
                    r = (u64)(k + 1);
                    break;
                }
            }
        } else {
            for (int k = 0; k < 64; k++) {
                if (a.i & ((u64)1 << k)) {
                    r = (u64)(k + 1);
                    break;
                }
            }
        }
        *out = val_int(r);
        return 0;
    }
    case 0x83: {                             /* DerefOf */
        aval_t a;
        e->pos++;
        if (eval_term(e, &a) < 0) return -1;
        *out = a;
        return 0;
    }
    case 0x87: {                             /* SizeOf */
        aval_t a;
        e->pos++;
        if (eval_term(e, &a) < 0) return -1;
        if (a.type == V_PKG) *out = val_int(a.n);
        else if (a.type == V_BUF) *out = val_int(a.i);
        else *out = val_int(1);
        return 0;
    }
    case 0x88: {                             /* Index */
        aval_t pk, ix;
        e->pos++;
        if (eval_term(e, &pk) < 0) return -1;
        if (eval_term(e, &ix) < 0) return -1;
        if (pk.type == V_PKG && ix.i < pk.n) {
            *out = g_arena[pk.pi + (u32)ix.i];
        } else {
            *out = val_int(0);
        }
        return 0;
    }
    case 0x90:                               /* LAnd */
    case 0x91:                               /* LOr */
    case 0x93:                               /* LEqual */
    case 0x94:                               /* LGreater */
    case 0x95:                               /* LLess */
    case 0x96:                               /* LGreaterEqual */
    case 0x97: {                             /* LLessEqual */
        aval_t a, b;
        e->pos++;
        if (eval_term(e, &a) < 0) return -1;
        if (eval_term(e, &b) < 0) return -1;
        int r;
        switch (op) {
        case 0x90: r = (a.i != 0) && (b.i != 0); break;
        case 0x91: r = (a.i != 0) || (b.i != 0); break;
        case 0x93: r = a.i == b.i; break;
        case 0x94: r = a.i > b.i; break;
        case 0x95: r = a.i < b.i; break;
        case 0x96: r = a.i >= b.i; break;
        default:   r = a.i <= b.i; break;
        }
        *out = val_int(r ? 1u : 0u);
        return 0;
    }
    case 0x92: {                             /* LNot */
        aval_t a;
        e->pos++;
        if (eval_term(e, &a) < 0) return -1;
        *out = val_int(a.i ? 0u : 1u);
        return 0;
    }
    case 0x9D: {                             /* ToInteger */
        aval_t a;
        e->pos++;
        if (eval_term(e, &a) < 0) return -1;
        *out = val_int(a.i);
        return 0;
    }
    case 0x86: {                             /* Notify: two operands */
        e->pos++;
        aval_t a, b;
        if (eval_term(e, &a) < 0) return -1;
        if (eval_term(e, &b) < 0) return -1;
        *out = val_int(0);
        return 0;
    }
    case 0x5B: {                             /* extended opcode */
        if (e->pos + 1 >= e->end) return -1;
        u8 ext = e->p[e->pos + 1];
        if (ext == 0x30) {                   /* Revision */
            e->pos += 2;
            *out = val_int(2);
            return 0;
        }
        if (ext == 0x31) {                   /* Debug object */
            e->pos += 2;
            *out = val_int(0);
            return 0;
        }
        return -1;
    }
    default:
        return eval_name(e, out);
    }
}

static int store_value(ev_t *e, const aval_t *v) {
    if (!burn()) return -1;
    if (e->pos >= e->end) return -1;

    u8 t = e->p[e->pos];
    if (t >= 0x60 && t <= 0x67) {
        e->locals[t - 0x60] = *v;
        e->pos++;
        return 0;
    }
    if (t >= 0x68 && t <= 0x6E) {
        e->args[t - 0x68] = *v;
        e->pos++;
        return 0;
    }
    if (t == 0x88) {                         /* Index (pkg, i) */
        aval_t pk, ix;
        e->pos++;
        if (eval_term(e, &pk) < 0) return -1;
        if (eval_term(e, &ix) < 0) return -1;
        if (pk.type != V_PKG || ix.i >= pk.n) return -1;
        g_arena[pk.pi + (u32)ix.i] = *v;
        return 0;
    }
    if (t == 0x5C || t == 0x5E || t == 0x2E || t == 0x2F ||
        (t >= 'A' && t <= 'Z') || t == '_') {
        char nm[5];
        if (!name_string(e->p, e->end, &e->pos, nm)) return -1;
        if (find_field(nm)) return 0;        /* field writes are ignored */
        return scratch_set(nm, v);
    }
    return -1;
}

static int exec_list(ev_t *e, u32 end, aval_t *ret, int *returned) {
    while (e->pos < end) {
        if (!burn()) return -1;

        u8 op = e->p[e->pos];
        if (op == 0xA4) {                    /* Return */
            e->pos++;
            if (eval_term(e, ret) < 0) return -1;
            *returned = 1;
            return 0;
        }
        if (op == 0xA0) {                    /* If */
            u32 pkg_start = e->pos + 1;
            u32 total;
            u32 pos = pkg_start;
            if (!pkg_len(e->p, end, &pos, &total)) return -1;
            u32 bend = pkg_start + total;
            if (bend > end || bend <= pos) return -1;

            e->pos = pos;
            aval_t pred;
            if (eval_term(e, &pred) < 0) return -1;
            int taken = pred.i != 0;

            if (taken) {
                int sub_ret = 0;
                if (exec_list(e, bend, ret, &sub_ret) < 0) return -1;
                if (sub_ret) {
                    *returned = 1;
                    return 0;
                }
            }
            e->pos = bend;

            if (e->pos < end && e->p[e->pos] == 0xA1) {   /* Else */
                u32 estart = e->pos + 1;
                u32 etot;
                u32 epos = estart;
                if (!pkg_len(e->p, end, &epos, &etot)) return -1;
                u32 eend = estart + etot;
                if (eend > end || eend <= epos) return -1;
                e->pos = epos;
                if (!taken) {
                    int sub_ret = 0;
                    if (exec_list(e, eend, ret, &sub_ret) < 0) return -1;
                    if (sub_ret) {
                        *returned = 1;
                        return 0;
                    }
                }
                e->pos = eend;
            }
            continue;
        }
        if (op == 0x08) {                    /* Name inside a method */
            e->pos++;
            char nm[5];
            if (!name_string(e->p, end, &e->pos, nm)) return -1;
            aval_t v;
            if (eval_term(e, &v) < 0) return -1;
            if (scratch_set(nm, &v) < 0) return -1;
            continue;
        }

        aval_t ignored;
        if (eval_term(e, &ignored) < 0) return -1;
    }
    return 0;
}

/* Call a method by leaf name. Arguments are values, results are values; the
 * caller only ever asks for the battery methods, which take none. */
static int call_method(const char *name, const aval_t *args, int argc,
                       aval_t *out) {
    aml_method_t *m = find_method(name);
    if (!m || m->argc != argc) return -1;
    if (g_depth >= AML_MAX_DEPTH) return -1;

    ev_t e;
    memset(&e, 0, sizeof(e));
    e.p = g_dsdt;
    e.pos = m->body;
    e.end = m->end;
    for (int i = 0; i < argc && i < 7; i++) e.args[i] = args[i];

    g_depth++;
    aval_t r = val_int(0);
    int returned = 0;
    int rc = exec_list(&e, e.end, &r, &returned);
    g_depth--;
    if (rc < 0) return -1;
    *out = r;
    return 0;
}

/* ---- public interface ------------------------------------------------- */

int aml_init(const u8 *dsdt, u32 len) {
    g_dsdt = dsdt;
    g_len = len;
    g_ready = 0;
    g_field_n = 0;
    g_region_n = 0;
    g_method_n = 0;

    if (!dsdt || len < 8) return 0;

    scan_regions();
    scan_fields();
    scan_methods();

    g_ready = find_method("_BST") || find_method("_BIX") ||
              find_method("_BIF");

    kprintf("[aml] namespace: %u fields, %u regions, %u methods%s\n",
            (u32)g_field_n, (u32)g_region_n, (u32)g_method_n,
            g_ready ? ", battery methods found" : "");
    if (g_field_n >= AML_MAX_FIELDS || g_region_n >= AML_MAX_REGIONS ||
        g_method_n >= AML_MAX_METHODS) {
        kprintf("[aml] namespace table full; battery fields past the cap are ignored\n");
    }
    return g_field_n + g_region_n + g_method_n;
}

int aml_objects(void) {
    return g_field_n + g_region_n + g_method_n;
}

int aml_battery_query(acpi_battery_state_t *out) {
    if (!out || !g_ready) return -1;

    memset(out, 0, sizeof(*out));
    g_arena_n = 0;
    g_scratch_n = 0;
    g_steps = AML_MAX_STEPS;
    g_depth = 0;
    g_suspect = 0;

    out->present = 1;
    out->ac_online = 1;

    /* _PSR is the authoritative mains-presence flag when the firmware has it. */
    aval_t psr;
    if (call_method("_PSR", NULL, 0, &psr) == 0 && !g_suspect) {
        out->ac_online = psr.i != 0 ? 1 : 0;
    }

    aval_t bst;
    if (call_method("_BST", NULL, 0, &bst) < 0) return -1;
    if (bst.type != V_PKG || bst.n < 3) return -1;

    u64 state = g_arena[bst.pi + 0].i;
    u64 rate = g_arena[bst.pi + 1].i;
    u64 remain = g_arena[bst.pi + 2].i;

    out->charging = (state & 0x02u) ? 1 : 0;
    if (state & 0x01u) out->ac_online = 0;   /* discharging */

    /* Full capacity: _BIX element 3 (last full charge), else _BIF element 2. */
    u64 full = 0;
    aval_t info;
    if (call_method("_BIX", NULL, 0, &info) == 0 &&
        info.type == V_PKG && info.n >= 4) {
        full = g_arena[info.pi + 3].i;
    } else if (call_method("_BIF", NULL, 0, &info) == 0 &&
               info.type == V_PKG && info.n >= 3) {
        full = g_arena[info.pi + 2].i;
    }

    if (full == 0 || g_suspect) {
        out->has_percent = 0;
        return 0;
    }

    u64 pct = (remain * 100u) / full;
    if (pct > 100u) pct = 100u;
    out->percent = (u8)pct;
    out->has_percent = 1;

    /* Runtime estimate only makes sense while discharging with a real rate.
     * Capacity and rate share a unit (mWh/mW or mAh/mA), so the ratio is hours
     * either way. */
    if (rate > 0 && remain > 0 && (state & 0x01u)) {
        u64 mins = (remain * 60u) / rate;
        if (mins > 0xFFFFFFFFu) mins = 0xFFFFFFFFu;
        out->minutes_left = (u32)mins;
    }
    return 0;
}
