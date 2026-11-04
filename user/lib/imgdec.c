/* Shared image decoder: PNG (with a compact DEFLATE), BMP and PPM.
 *
 * This used to live inside the browser. It is a library now so the picture
 * viewer can decode files from /pictures without duplicating a codec. The
 * caller passes all buffers, which keeps memory use explicit on a system with
 * a bump allocator and no swap.
 */

#include <mcube/libc.h>
#include <mcube/imgdec.h>

static u32 rd16le(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 rd32le(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 rd32be(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/* Last failure reason, so callers can tell "too large" apart from "corrupt".
 * Decoders reset this to CORRUPT on entry and overwrite it at the few points
 * where the input is valid but simply outside the supported subset. */
static int g_last_error;

/* ---- DEFLATE --------------------------------------------------------- */

typedef struct {
    const u8 *in;
    int len;
    int pos;
    u32 bits;
    int nbits;
} bitbr_t;

static int br_get(bitbr_t *br, int need) {
    while (br->nbits < need) {
        if (br->pos >= br->len) return -1;
        br->bits |= (u32)br->in[br->pos++] << br->nbits;
        br->nbits += 8;
    }
    int v = (int)(br->bits & ((1u << need) - 1u));
    br->bits >>= need;
    br->nbits -= need;
    return v;
}

typedef struct {
    u16 count[16];
    u16 sym[288];
} huff_t;

static void huff_make(huff_t *h, const u8 *lens, int n) {
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;

    u16 off[16];
    off[0] = 0;
    for (int i = 1; i < 16; i++) off[i] = (u16)(off[i - 1] + h->count[i - 1]);
    for (int i = 0; i < n; i++) {
        if (lens[i]) h->sym[off[lens[i]]++] = (u16)i;
    }
}

static int huff_get(bitbr_t *br, const huff_t *h) {
    int code = 0, first = 0, index = 0;

    for (int len = 1; len <= 15; len++) {
        int b = br_get(br, 1);
        if (b < 0) return -1;
        code |= b;
        int cnt = h->count[len];
        if (code - first < cnt) return h->sym[index + (code - first)];
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return -1;
}

static int inflate_raw(const u8 *in, int inlen, u8 *out, int outcap,
                       int *outlen) {
    static const int l_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
        35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const int l_ext[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static const int d_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
        257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static const int d_ext[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };

    bitbr_t br;
    br.in = in;
    br.len = inlen;
    br.pos = 0;
    br.bits = 0;
    br.nbits = 0;

    int total = 0;
    int last = 0;

    while (!last) {
        last = br_get(&br, 1);
        if (last < 0) return -1;
        int type = br_get(&br, 2);
        if (type < 0) return -1;

        if (type == 0) {                    /* stored block */
            br.bits = 0;
            br.nbits = 0;
            if (br.pos + 4 > br.len) return -1;
            int n = (int)br.in[br.pos] | ((int)br.in[br.pos + 1] << 8);
            int nn = (int)br.in[br.pos + 2] | ((int)br.in[br.pos + 3] << 8);
            br.pos += 4;
            if ((n ^ 0xFFFF) != nn) return -1;
            if (br.pos + n > br.len || total + n > outcap) return -1;
            memcpy(out + total, br.in + br.pos, (size_t)n);
            br.pos += n;
            total += n;
            continue;
        }

        huff_t lit, dist;
        u8 lens[320];

        if (type == 1) {                    /* fixed Huffman */
            int i = 0;
            for (; i < 144; i++) lens[i] = 8;
            for (; i < 256; i++) lens[i] = 9;
            for (; i < 280; i++) lens[i] = 7;
            for (; i < 288; i++) lens[i] = 8;
            huff_make(&lit, lens, 288);
            for (i = 0; i < 30; i++) lens[i] = 5;
            huff_make(&dist, lens, 30);
        } else if (type == 2) {              /* dynamic Huffman */
            static const int order[19] = {
                16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
            };
            int hlit = br_get(&br, 5);
            int hdist = br_get(&br, 5);
            int hclen = br_get(&br, 4);
            if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
            hlit += 257;
            hdist += 1;
            hclen += 4;
            if (hlit > 286 || hdist > 30) return -1;

            memset(lens, 0, sizeof(lens));
            for (int i = 0; i < hclen; i++) {
                int v = br_get(&br, 3);
                if (v < 0) return -1;
                lens[order[i]] = (u8)v;
            }
            huff_t cl;
            huff_make(&cl, lens, 19);

            u8 codes[320];
            int idx = 0;
            while (idx < hlit + hdist) {
                int sym = huff_get(&br, &cl);
                if (sym < 0) return -1;
                if (sym < 16) {
                    codes[idx++] = (u8)sym;
                    continue;
                }
                int rep;
                u8 val = 0;
                if (sym == 16) {
                    if (idx == 0) return -1;
                    int v = br_get(&br, 2);
                    if (v < 0) return -1;
                    rep = 3 + v;
                    val = codes[idx - 1];
                } else if (sym == 17) {
                    int v = br_get(&br, 3);
                    if (v < 0) return -1;
                    rep = 3 + v;
                } else {
                    int v = br_get(&br, 7);
                    if (v < 0) return -1;
                    rep = 11 + v;
                }
                if (idx + rep > hlit + hdist) return -1;
                for (int k = 0; k < rep; k++) codes[idx++] = val;
            }
            huff_make(&lit, codes, hlit);
            huff_make(&dist, codes + hlit, hdist);
        } else {
            return -1;
        }

        for (;;) {
            int sym = huff_get(&br, &lit);
            if (sym < 0) return -1;
            if (sym < 256) {
                if (total >= outcap) return -1;
                out[total++] = (u8)sym;
                continue;
            }
            if (sym == 256) break;

            sym -= 257;
            if (sym >= 29) return -1;
            int e = br_get(&br, l_ext[sym]);
            if (e < 0) return -1;
            int len = l_base[sym] + e;

            int dsym = huff_get(&br, &dist);
            if (dsym < 0 || dsym >= 30) return -1;
            int e2 = br_get(&br, d_ext[dsym]);
            if (e2 < 0) return -1;
            int back = d_base[dsym] + e2;

            if (back > total || total + len > outcap) return -1;
            for (int k = 0; k < len; k++) {
                out[total] = out[total - back];
                total++;
            }
        }
    }

    *outlen = total;
    return 0;
}

/* ---- PNG ------------------------------------------------------------- */

static int png_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static int png_channels(int ctype) {
    switch (ctype) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
    default: return 0;
    }
}

static int decode_png(const u8 *d, int len, u32 *out, int *ow, int *oh,
                      u8 *aux, int aux_cap, u8 *rows, int rows_cap) {
    g_last_error = IMGDEC_ERR_CORRUPT;
    if (len < 8 || memcmp(d, "\x89PNG\r\n\x1a\n", 8) != 0) {
        g_last_error = IMGDEC_ERR_UNSUPPORTED;
        return -1;
    }
    if (!aux || !rows) return -1;

    int w = 0, h = 0, depth = 0, ctype = -1, inter = 0;
    u8 pal[768];
    int npal = 0;
    int idat = 0;

    int pos = 8;
    while (pos + 12 <= len) {
        u32 clen = rd32be(d + pos);
        const u8 *tag = d + pos + 4;
        if (clen > (u32)(len - pos - 12)) return -1;

        if (memcmp(tag, "IHDR", 4) == 0 && clen >= 13) {
            w = (int)rd32be(d + pos + 8);
            h = (int)rd32be(d + pos + 12);
            depth = d[pos + 16];
            ctype = d[pos + 17];
            inter = d[pos + 20];
        } else if (memcmp(tag, "PLTE", 4) == 0) {
            int n = (int)(clen / 3);
            if (n > 256) n = 256;
            memcpy(pal, d + pos + 8, (size_t)n * 3);
            npal = n;
        } else if (memcmp(tag, "IDAT", 4) == 0) {
            if (idat + (int)clen > aux_cap) {
                g_last_error = IMGDEC_ERR_TOO_LARGE;
                return -1;
            }
            memcpy(aux + idat, d + pos + 8, clen);
            idat += (int)clen;
        } else if (memcmp(tag, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + (int)clen;
    }

    int ch = png_channels(ctype);
    if (w <= 0 || h <= 0 || ch == 0) return -1;
    if (depth != 8 || inter != 0) {
        g_last_error = IMGDEC_ERR_UNSUPPORTED;
        return -1;
    }
    if ((long long)w * h > IMGDEC_MAX_PX) {
        g_last_error = IMGDEC_ERR_TOO_LARGE;
        return -1;
    }
    if (idat < 3) return -1;

    /* zlib stream: two-byte header, raw DEFLATE, Adler-32 trailer. */
    const u8 *z = (const u8 *)aux;
    if ((z[0] & 0x0F) != 8 || ((int)z[0] * 256 + z[1]) % 31 != 0) return -1;

    long long rowbytes = (long long)w * ch;
    long long need = (long long)h * (rowbytes + 1);
    if (need > (long long)rows_cap) {
        g_last_error = IMGDEC_ERR_TOO_LARGE;
        return -1;
    }

    int got = 0;
    if (inflate_raw(z + 2, idat - 2, rows, (int)need, &got) != 0) return -1;
    if (got < (int)need) return -1;

    /* Undo the row filters in place: each row depends only on the previous row,
     * which is already reconstructed when we reach it. */
    for (int y = 0; y < h; y++) {
        u8 *row = rows + (long long)y * (rowbytes + 1);
        u8 ft = row[0];
        u8 *cur = row + 1;
        const u8 *prev = (y > 0) ? row - rowbytes : NULL;
        for (long long x = 0; x < rowbytes; x++) {
            int a = (x >= ch) ? cur[x - ch] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= ch) ? prev[x - ch] : 0;
            int v = cur[x];
            switch (ft) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) >> 1; break;
            case 4: v += png_paeth(a, b, c); break;
            default: return -1;
            }
            cur[x] = (u8)v;
        }
    }

    for (int y = 0; y < h; y++) {
        const u8 *row = rows + (long long)y * (rowbytes + 1) + 1;
        u32 *o = out + (size_t)y * (size_t)w;
        if (ctype == 0) {
            for (int x = 0; x < w; x++) {
                u32 v = row[x];
                o[x] = (v << 16) | (v << 8) | v;
            }
        } else if (ctype == 2) {
            for (int x = 0; x < w; x++) {
                const u8 *p = row + x * 3;
                o[x] = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
            }
        } else if (ctype == 3) {
            for (int x = 0; x < w; x++) {
                int idx = row[x];
                if (idx >= npal) idx = 0;
                const u8 *p = pal + idx * 3;
                o[x] = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
            }
        } else if (ctype == 4) {
            for (int x = 0; x < w; x++) {
                u32 v = row[x * 2];
                o[x] = (v << 16) | (v << 8) | v;
            }
        } else {
            for (int x = 0; x < w; x++) {
                const u8 *p = row + x * 4;
                o[x] = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
            }
        }
    }

    *ow = w;
    *oh = h;
    return 0;
}

/* ---- BMP / PPM -------------------------------------------------------- */

static int decode_bmp(const u8 *d, int len, u32 *out, int *ow, int *oh) {
    g_last_error = IMGDEC_ERR_CORRUPT;
    if (len < 54 || d[0] != 'B' || d[1] != 'M') {
        g_last_error = IMGDEC_ERR_UNSUPPORTED;
        return -1;
    }

    u32 off = rd32le(d + 10);
    int w = (int)rd32le(d + 18);
    int hraw = (int)rd32le(d + 22);
    u32 bpp = rd16le(d + 28);
    u32 comp = rd32le(d + 30);

    if (w <= 0 || hraw == 0) return -1;
    if (bpp != 24 && bpp != 32) {
        g_last_error = IMGDEC_ERR_UNSUPPORTED;
        return -1;
    }
    if (comp != 0) {
        g_last_error = IMGDEC_ERR_UNSUPPORTED;     /* RLE / bitfields */
        return -1;
    }

    int h = hraw < 0 ? -hraw : hraw;
    if ((long long)w * h > IMGDEC_MAX_PX) {
        g_last_error = IMGDEC_ERR_TOO_LARGE;
        return -1;
    }

    u32 rowbytes = ((bpp * (u32)w) + 31u) / 32u * 4u;
    if ((long long)off + (long long)rowbytes * h > len) return -1;

    int step = (int)(bpp / 8u);
    for (int y = 0; y < h; y++) {
        int srow = hraw < 0 ? y : (h - 1 - y);
        const u8 *s = d + off + (long long)srow * rowbytes;
        u32 *o = out + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            const u8 *p = s + (long long)x * step;
            o[x] = ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0];
        }
    }
    *ow = w;
    *oh = h;
    return 0;
}

static int ppm_uint(const u8 *d, int len, int *pos, int *val) {
    while (*pos < len) {
        u8 c = d[*pos];
        if (c == '#') {
            while (*pos < len && d[*pos] != '\n') (*pos)++;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            (*pos)++;
        } else {
            break;
        }
    }
    if (*pos >= len) return -1;

    int v = 0, digits = 0;
    while (*pos < len && d[*pos] >= '0' && d[*pos] <= '9') {
        v = v * 10 + (d[*pos] - '0');
        if (v > 100000) return -1;
        (*pos)++;
        digits++;
    }
    if (!digits) return -1;
    *val = v;
    return 0;
}

static int decode_ppm(const u8 *d, int len, u32 *out, int *ow, int *oh) {
    g_last_error = IMGDEC_ERR_CORRUPT;
    if (len < 3 || d[0] != 'P') {
        g_last_error = IMGDEC_ERR_UNSUPPORTED;
        return -1;
    }
    int fmt = d[1];
    if (fmt != '3' && fmt != '5' && fmt != '6') {
        g_last_error = IMGDEC_ERR_UNSUPPORTED;
        return -1;
    }

    int pos = 2;
    int w = 0, h = 0, maxv = 0;
    if (ppm_uint(d, len, &pos, &w) != 0 ||
        ppm_uint(d, len, &pos, &h) != 0 ||
        ppm_uint(d, len, &pos, &maxv) != 0) return -1;
    if (w <= 0 || h <= 0 || maxv <= 0 || maxv > 255) return -1;
    if ((long long)w * h > IMGDEC_MAX_PX) {
        g_last_error = IMGDEC_ERR_TOO_LARGE;
        return -1;
    }

    if (fmt != '3') {
        /* Exactly one whitespace separates the header from the samples; accept
         * the CRLF pair some writers emit as well. */
        if (pos >= len) return -1;
        if (d[pos] == '\r' && pos + 1 < len && d[pos + 1] == '\n') pos += 2;
        else pos++;
    }

    for (int y = 0; y < h; y++) {
        u32 *o = out + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            int r, g, b;
            if (fmt == '3') {
                if (ppm_uint(d, len, &pos, &r) != 0 ||
                    ppm_uint(d, len, &pos, &g) != 0 ||
                    ppm_uint(d, len, &pos, &b) != 0) return -1;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
            } else if (fmt == '5') {
                if (pos >= len) return -1;
                r = g = b = d[pos++];
            } else {
                if (pos + 3 > len) return -1;
                r = d[pos];
                g = d[pos + 1];
                b = d[pos + 2];
                pos += 3;
            }
            o[x] = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
    }

    *ow = w;
    *oh = h;
    return 0;
}

/* ---- public API ------------------------------------------------------- */

int img_decode(const u8 *d, int len, u32 *out, int *ow, int *oh,
               u8 *aux, int aux_cap, u8 *rows, int rows_cap) {
    g_last_error = IMGDEC_ERR_UNSUPPORTED;
    if (!d || len < 2 || !out || !ow || !oh) return -1;

    int rc;
    if (len >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
        rc = decode_png(d, len, out, ow, oh, aux, aux_cap, rows, rows_cap);
    } else if (d[0] == 'B' && d[1] == 'M') {
        rc = decode_bmp(d, len, out, ow, oh);
    } else if (d[0] == 'P') {
        rc = decode_ppm(d, len, out, ow, oh);
    } else {
        return -1;
    }
    if (rc == 0) g_last_error = IMGDEC_ERR_NONE;
    return rc;
}

int img_last_error(void) {
    return g_last_error;
}

static int ext_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

int img_name_supported(const char *name) {
    if (!name) return 0;
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return 0;
    return ext_eq(dot + 1, "png") || ext_eq(dot + 1, "bmp") ||
           ext_eq(dot + 1, "ppm");
}
