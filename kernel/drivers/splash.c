/* MCubeAIOS boot splash: "terminal genesis".
 *
 * A secure-boot screen in the language of a hacker terminal, held to the
 * restraint a hardware vendor would ship: near-black glass, one phosphor
 * emerald accent, a boot trace that types itself out behind a block cursor,
 * a scan head passing over the mark while the modules load. Every string is
 * rendered from the anti-aliased UI face, so the text is crisp rather than
 * blocky. Only small regions repaint: the hero area is restored from a saved
 * snapshot and each mote carries its own 3x3 patch, which keeps the splash
 * cheap on an uncached linear framebuffer while moving every frame.
 */

#include <mcube/kernel.h>
#include <mcube/fb.h>
#include <mcube/mm.h>
#include <mcube/timer.h>

#define FONT_W 8
#define FONT_H 16
#define SPLASH_MS 3600

extern const u8 mc_ui_font[256][128];
extern const u8 mc_ui_font_bold[256][128];

static struct {
    u8 *base;
    int w, h, pitch;
    int fmt;
} g_s;

/* Near-black glass with a phosphor emerald primary, one cool accent and one
 * warm alert. Saturation is kept low so it reads as a boot loader from a
 * hardware vendor, not as a screensaver. */
#define C_BG_TOP 0x03060A
#define C_BG_BOT 0x050E12
#define C_GRID   0x123240
#define C_MINT   0x3DE58C
#define C_CYAN   0x49E2FF
#define C_VIOLET 0xA07CFF
#define C_AMBER  0xFFB454
#define C_WHITE  0xEAF6F2
#define C_TEXT   0xC9DED8
#define C_TRACE  0x86B7A8
#define C_DIM    0x4A6E6A
#define C_FAINT  0x1D3438

static int g_big;
static int g_cube_cx, g_cube_cy, g_cube_size, g_cube_box;
static int g_wm_x, g_wm_y, g_wm_scale, g_wm_w;
static int g_ul_x, g_ul_y, g_ul_w;
static int g_hero_x, g_hero_y, g_hero_w, g_hero_h;
static int g_comp_cx, g_comp_cy;
static int g_log_x, g_log_y, g_log_step;
static int g_tel_x, g_tel_y, g_tel_w;
static int g_prog_x, g_prog_y, g_prog_w, g_prog_h;

static u32 *g_hero_bg;
static u32 g_rng = 0x4D435542u;

/* ---- fixed-point trig ---------------------------------------------- */

static const int g_sin[91] = {
    0, 71, 143, 214, 286, 357, 428, 499, 570, 641,
    711, 782, 852, 921, 991, 1060, 1129, 1198, 1266, 1334,
    1401, 1468, 1534, 1600, 1666, 1731, 1796, 1860, 1923, 1986,
    2048, 2110, 2171, 2231, 2290, 2349, 2408, 2465, 2522, 2578,
    2633, 2687, 2741, 2793, 2845, 2896, 2946, 2996, 3044, 3091,
    3138, 3183, 3228, 3271, 3314, 3355, 3396, 3435, 3474, 3511,
    3547, 3582, 3617, 3650, 3681, 3712, 3742, 3770, 3798, 3824,
    3849, 3873, 3896, 3917, 3937, 3956, 3974, 3991, 4006, 4021,
    4034, 4046, 4056, 4065, 4074, 4080, 4086, 4090, 4094, 4095,
    4096,
};

static int isin(int deg) {
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg <= 90) return g_sin[deg];
    if (deg <= 180) return g_sin[180 - deg];
    if (deg <= 270) return -g_sin[deg - 180];
    return -g_sin[360 - deg];
}

static int icos(int deg) { return isin(deg + 90); }

/* ---- pixels --------------------------------------------------------- */

static u32 rgb(u32 v) {
    /* GOP reports the byte order; for BGRA the packed 0xRRGGBB value is
     * already in the memory order the hardware expects. */
    if (g_s.fmt == (int)BOOT_PIXEL_BGRA8888) {
        return 0xFF000000u | (v & 0xFFFFFFu);
    }
    return 0xFF000000u | ((v & 0xFFu) << 16) | (v & 0x00FF00u) |
           ((v >> 16) & 0xFFu);
}

static u32 mix(u32 a, u32 b, int t) {
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    int ar = (int)(a >> 16) & 0xFF, ag = (int)(a >> 8) & 0xFF;
    int ab = (int)a & 0xFF;
    int br = (int)(b >> 16) & 0xFF, bg = (int)(b >> 8) & 0xFF;
    int bb = (int)b & 0xFF;
    int r = ar + (br - ar) * t / 255;
    int g = ag + (bg - ag) * t / 255;
    int bl = ab + (bb - ab) * t / 255;
    return ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
}

static u32 rgb_mix(u32 a, u32 b, int t) { return rgb(mix(a, b, t)); }

static u32 px_read(int x, int y) {
    if (x < 0 || y < 0 || x >= g_s.w || y >= g_s.h) return 0;
    return *((u32 *)(g_s.base + (u64)y * (u64)g_s.pitch) + x);
}

static void px_write(int x, int y, u32 c) {
    if (x < 0 || y < 0 || x >= g_s.w || y >= g_s.h) return;
    *((u32 *)(g_s.base + (u64)y * (u64)g_s.pitch) + x) = c;
}

/* Alpha blend a packed 0xRRGGBB colour over the framebuffer. Coverage comes
 * from the anti-aliased glyph tables, so text is resampled instead of being
 * thresholded into hard blocks. */
static void blend(int x, int y, u32 c, int a) {
    if (a <= 0) return;
    if (x < 0 || y < 0 || x >= g_s.w || y >= g_s.h) return;
    if (a >= 255) { px_write(x, y, rgb(c)); return; }
    u32 p = *((u32 *)(g_s.base + (u64)y * (u64)g_s.pitch) + x);
    int pr, pg, pb;
    if (g_s.fmt == (int)BOOT_PIXEL_BGRA8888) {
        pr = (int)(p >> 16) & 0xFF;
        pg = (int)(p >> 8) & 0xFF;
        pb = (int)p & 0xFF;
    } else {
        pb = (int)(p >> 16) & 0xFF;
        pg = (int)(p >> 8) & 0xFF;
        pr = (int)p & 0xFF;
    }
    int cr = (int)(c >> 16) & 0xFF, cg = (int)(c >> 8) & 0xFF, cb = (int)c & 0xFF;
    int r = pr + (cr - pr) * a / 255;
    int g = pg + (cg - pg) * a / 255;
    int b = pb + (cb - pb) * a / 255;
    px_write(x, y, rgb(((u32)r << 16) | ((u32)g << 8) | (u32)b));
}

static void fill(int x, int y, int w, int h, u32 c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_s.w) w = g_s.w - x;
    if (y + h > g_s.h) h = g_s.h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        u32 *row = (u32 *)(g_s.base + (u64)(y + yy) * (u64)g_s.pitch) + x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}

static void line(int x0, int y0, int x1, int y1, u32 c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px_write(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void ellipse_ring(int cx, int cy, int rx, int ry, u32 c) {
    int px = 0x7FFFFFFF, py = 0x7FFFFFFF;
    for (int a = 0; a < 360; a += 2) {
        int x = cx + icos(a) * rx / 4096;
        int y = cy + isin(a) * ry / 4096;
        if (x == px && y == py) continue;
        px_write(x, y, c);
        px = x;
        py = y;
    }
}

/* ---- text ----------------------------------------------------------- */

/* The UI faces carry 8-bit coverage per cell (16 rows x 8 columns). Blitting
 * coverage rather than a bitmap is what removes the stair-stepped edges. */
static void glyph_aa(int x, int y, int scale, u32 c, u8 ch, const u8 (*tab)[128]) {
    const u8 *cov = tab[ch];
    for (int row = 0; row < FONT_H; row++) {
        for (int col = 0; col < FONT_W; col++) {
            int a = cov[row * FONT_W + col];
            if (!a) continue;
            if (scale == 1) { blend(x + col, y + row, c, a); continue; }
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    blend(x + col * scale + sx, y + row * scale + sy, c, a);
                }
            }
        }
    }
}

static void glyph(int x, int y, int scale, u32 c, u8 ch) {
    glyph_aa(x, y, scale, c, ch, mc_ui_font);
}

static void glyph_b(int x, int y, int scale, u32 c, u8 ch) {
    glyph_aa(x, y, scale, c, ch, mc_ui_font_bold);
}

static void text(int x, int y, int scale, u32 c, const char *s) {
    while (*s) {
        glyph(x, y, scale, c, (u8)*s++);
        x += FONT_W * scale;
    }
}

static void text_b(int x, int y, int scale, u32 c, const char *s) {
    while (*s) {
        glyph_b(x, y, scale, c, (u8)*s++);
        x += FONT_W * scale;
    }
}

static int text_w(const char *s, int scale) {
    return (int)strlen(s) * FONT_W * scale;
}

static void text_right(int right, int y, int scale, u32 c, const char *s) {
    text(right - text_w(s, scale), y, scale, c, s);
}

/* ---- background ----------------------------------------------------- */

static u32 rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

static u32 bg_color(int x, int y) {
    int t = y * 255 / (g_s.h > 1 ? g_s.h - 1 : 1);
    u32 c = mix(rgb(C_BG_TOP), rgb(C_BG_BOT), t);
    if ((x & 15) == 0 || (y & 15) == 0) c = mix(c, rgb(C_GRID), 34);
    if ((y & 3) == 2) c = mix(c, 0, 24);     /* CRT scanline, very shallow */
    return c;
}

static void bg_rect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_s.w) w = g_s.w - x;
    if (y + h > g_s.h) h = g_s.h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        u32 *row = (u32 *)(g_s.base + (u64)(y + yy) * (u64)g_s.pitch) + x;
        for (int xx = 0; xx < w; xx++) row[xx] = bg_color(x + xx, y + yy);
    }
}

static void draw_background(void) {
    for (int y = 0; y < g_s.h; y++) {
        u32 *row = (u32 *)(g_s.base + (u64)y * (u64)g_s.pitch);
        int t = y * 255 / (g_s.h > 1 ? g_s.h - 1 : 1);
        u32 c0 = mix(rgb(C_BG_TOP), rgb(C_BG_BOT), t);
        u32 cg = mix(c0, rgb(C_GRID), 34);
        int scan = (y & 3) == 2;
        if (scan) {
            c0 = mix(c0, 0, 24);
            cg = mix(cg, 0, 24);
        }
        int grid_row = (y & 15) == 0;
        for (int x = 0; x < g_s.w; x++) {
            row[x] = (grid_row || (x & 15) == 0) ? cg : c0;
        }
    }

    /* Static circuit traces, kept very dim so they read as texture. */
    u32 trace = rgb_mix(C_BG_TOP, C_FAINT, 90);
    line(0, g_s.h / 5, g_s.w / 4, g_s.h / 5, trace);
    line(g_s.w / 4, g_s.h / 5, g_s.w / 4 + 60, g_s.h / 5 + 60, trace);
    line(g_s.w, g_s.h / 3, g_s.w - g_s.w / 5, g_s.h / 3, trace);
    line(g_s.w - g_s.w / 5, g_s.h / 3, g_s.w - g_s.w / 5 - 40, g_s.h / 3 + 40, trace);
    line(0, g_s.h * 3 / 4, g_s.w / 6, g_s.h * 3 / 4, trace);
    line(g_s.w, g_s.h * 4 / 5, g_s.w - g_s.w / 6, g_s.h * 4 / 5, trace);
}

/* ---- hero snapshot -------------------------------------------------- */

static void hero_capture(void) {
    if (!g_hero_bg) return;
    for (int y = 0; y < g_hero_h; y++) {
        u32 *src = (u32 *)(g_s.base + (u64)(g_hero_y + y) * (u64)g_s.pitch) + g_hero_x;
        memcpy(g_hero_bg + (usize)y * g_hero_w, src, (usize)g_hero_w * 4);
    }
}

static void hero_restore(int x, int y, int w, int h) {
    if (!g_hero_bg) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < g_hero_x) x0 = g_hero_x;
    if (y0 < g_hero_y) y0 = g_hero_y;
    if (x1 > g_hero_x + g_hero_w) x1 = g_hero_x + g_hero_w;
    if (y1 > g_hero_y + g_hero_h) y1 = g_hero_y + g_hero_h;
    if (x1 <= x0 || y1 <= y0) return;
    for (int yy = y0; yy < y1; yy++) {
        u32 *dst = (u32 *)(g_s.base + (u64)yy * (u64)g_s.pitch) + x0;
        const u32 *src = g_hero_bg +
            (usize)(yy - g_hero_y) * g_hero_w + (x0 - g_hero_x);
        memcpy(dst, src, (usize)(x1 - x0) * 4);
    }
}

/* ---- boot trace copy ------------------------------------------------ */

/* Short, factual lines: a boot loader from a vendor prints what it proves,
 * not paragraphs. The address is synthesised but stable per line. */
static const char *const g_log_lines[] = {
    "cpu  x86_64 long mode / apic + tsc",
    "mem  4-level paging / slab ready",
    "disk ahci link up / fat32 mounted",
    "net  e1000 gigabit / dns online",
    "hid  ps2 keyboard + pointer",
    "ai   gguf runtime / tools bound",
    "sec  ring3 isolation / syscall gate",
    "gui  compositor handoff / neural ui",
};
#define LOG_COUNT ((int)(sizeof(g_log_lines) / sizeof(g_log_lines[0])))

/* ---- layout --------------------------------------------------------- */

/* The lockup is sized from the panel height, not from a fixed pixel count:
 * the mark carries the same visual weight on a 1200-row panel as on an
 * 800-row one. 4x coverage cells is the floor (32 px per glyph), 9x the
 * ceiling (72 px). */
static void layout(void) {
    g_big = g_s.w >= 1100;

    int scale = g_s.h / 170;
    if (scale < 4) scale = 4;
    if (scale > 9) scale = 9;

    int cube = scale * 30;
    int gap = scale * 7;
    int comp = cube + gap + 9 * FONT_W * scale;
    if (comp > g_s.w * 3 / 4) {          /* narrow panel: fall back to 4x */
        scale = 4;
        cube = 120;
        gap = 26;
        comp = cube + gap + 9 * FONT_W * scale;
    }
    int start = (g_s.w - comp) / 2;

    /* Anchor the progress block to the bottom and stack the trace above it;
     * whatever is left belongs to the hero, which is what keeps the sheet
     * from showing a dead band under the mark. */
    g_prog_x = g_big ? 96 : 36;
    g_prog_w = g_s.w - 2 * g_prog_x;
    g_prog_h = 10;
    g_prog_y = g_s.h - (g_big ? 86 : 76);

    g_log_x = g_big ? 96 : 36;
    g_log_step = g_big ? 24 : 18;

    int trace_h = (LOG_COUNT - 1) * g_log_step + FONT_H;
    g_log_y = g_prog_y - (g_big ? 72 : 30) - trace_h;
    if (g_log_y < g_s.h / 2) {           /* short panel: tighten the pitch */
        g_log_step = 16;
        trace_h = (LOG_COUNT - 1) * g_log_step + FONT_H;
        g_log_y = g_prog_y - (g_big ? 48 : 26) - trace_h;
    }

    /* The hero owns the band between the header and the trace. It is centred
     * inside that band rather than pinned to its top, otherwise a tall panel
     * grows all of its slack underneath the mark and the sheet looks top
     * heavy. 900 rows is the capture ceiling (~7 MiB of snapshot). */
    int band_top = g_big ? 78 : 56;
    int band_bot = g_log_y - (g_big ? 74 : 50);
    int band_h = band_bot - band_top;
    if (band_h < 190) band_h = 190;
    if (band_h > 900) {
        int mid = (band_top + band_bot) / 2;
        band_top = mid - 450;
        band_h = 900;
    }
    g_hero_y = band_top;
    g_hero_h = band_h;

    g_cube_size = cube;
    g_cube_cx = start + cube / 2;
    g_cube_cy = band_top + band_h / 2 - (g_big ? 4 : 0);
    g_cube_box = cube + cube / 3;

    g_wm_scale = scale;
    g_wm_w = 9 * FONT_W * scale;
    g_wm_x = start + cube + gap;
    g_wm_y = g_cube_cy - 3 * FONT_H * scale / 4;
    g_ul_x = g_wm_x;
    g_ul_y = g_wm_y + FONT_H * scale + 14;
    g_ul_w = g_wm_w;

    g_comp_cx = start + comp / 2;
    g_comp_cy = g_cube_cy;

    g_hero_x = start - cube;
    if (g_hero_x < 0) g_hero_x = 0;
    g_hero_w = comp + cube * 2;
    if (g_hero_x + g_hero_w > g_s.w) g_hero_w = g_s.w - g_hero_x;

    g_tel_w = g_big ? 360 : 300;
    g_tel_x = g_s.w - g_log_x - g_tel_w;
    g_tel_y = g_log_y;
}

/* ---- hero background (static before capture) ------------------------ */

static void draw_hero_background(void) {
    int x1 = g_hero_x + g_hero_w;
    int y1 = g_hero_y + g_hero_h;

    /* Glow and rings are derived from the mark size instead of fixed pixels,
     * so the halo keeps the same proportion whether the panel is 800 or 1200
     * rows tall. The 36000/90000 thresholds and the 330x118 ring were tuned
     * against a 176 px cube, hence the 176 px reference below. */
    int k = g_cube_size > 0 ? g_cube_size : 176;
    int t1 = 36000 * k / 176;
    int t2 = 90000 * k / 176;
    int t1k = t1 > 0 ? t1 : 1;
    int t2k = t2 > t1k ? t2 : t1k + 1;

    for (int y = g_hero_y; y < y1; y++) {
        u32 *row = (u32 *)(g_s.base + (u64)y * (u64)g_s.pitch);
        int dy = y - g_comp_cy;
        for (int x = g_hero_x; x < x1; x++) {
            int dx = x - g_comp_cx;
            int d2 = dx * dx / 3 + dy * dy;
            u32 c = bg_color(x, y);
            if (d2 < t1k) c = mix(c, rgb(0x0A3A2A), (t1k - d2) * 110 / t1k);
            else if (d2 < t2k) c = mix(c, rgb(0x08241C), (t2k - d2) * 55 / (t2k - t1k));
            row[x] = c;
        }
    }

    /* Faint orbital rings behind the composition. */
    ellipse_ring(g_comp_cx, g_comp_cy, 330 * k / 176, 118 * k / 176,
                 rgb_mix(C_CYAN, 0x000000, 88));
    ellipse_ring(g_comp_cx, g_comp_cy, 250 * k / 176, 86 * k / 176,
                 rgb_mix(C_MINT, 0x000000, 96));

    /* Reticle around the cube. */
    int bx = g_cube_cx - g_cube_box / 2;
    int by = g_cube_cy - g_cube_box / 2;
    int L = 18;
    u32 rc = rgb_mix(C_CYAN, 0x000000, 110);
    line(bx, by, bx + L, by, rc);
    line(bx, by, bx, by + L, rc);
    line(bx + g_cube_box - L, by, bx + g_cube_box, by, rc);
    line(bx + g_cube_box, by, bx + g_cube_box, by + L, rc);
    line(bx, by + g_cube_box, bx + L, by + g_cube_box, rc);
    line(bx, by + g_cube_box - L, bx, by + g_cube_box, rc);
    line(bx + g_cube_box - L, by + g_cube_box, bx + g_cube_box, by + g_cube_box, rc);
    line(bx + g_cube_box, by + g_cube_box - L, bx + g_cube_box, by + g_cube_box, rc);

    /* Wordmark: crisp MCubeAIOS, "AI" carrying the accent. */
    static const char wm[] = "MCubeAIOS";
    text(g_wm_x - 2, g_wm_y, g_wm_scale, rgb(0x0C3A2C), wm);
    text(g_wm_x + 2, g_wm_y, g_wm_scale, rgb(0x241A38), wm);
    text(g_wm_x, g_wm_y + 3, g_wm_scale, rgb(0x081E18), wm);
    for (int i = 0; i < 9; i++) {
        u32 c = (i == 5 || i == 6) ? rgb(C_MINT) : rgb(C_WHITE);
        glyph(g_wm_x + i * FONT_W * g_wm_scale, g_wm_y, g_wm_scale, c, (u8)wm[i]);
    }

    line(g_ul_x, g_ul_y, g_ul_x + g_ul_w - 1, g_ul_y, rgb(0x1B5C42));
    fill(g_ul_x, g_ul_y - 2, 1, 5, rgb(C_MINT));
    fill(g_ul_x + g_ul_w - 1, g_ul_y - 2, 1, 5, rgb(C_CYAN));

    const char *tag = g_big
        ? "[ NEURAL KERNEL 0.1.0 // AI-NATIVE OPERATING SYSTEM ]"
        : "NEURAL KERNEL 0.1.0 // AI-NATIVE OS";
    int tx = g_wm_x + (g_wm_w - text_w(tag, 1)) / 2;
    text(tx, g_ul_y + 18, 1, rgb_mix(C_DIM, C_MINT, 70), tag);

    hero_capture();
}

/* ---- drifting motes ------------------------------------------------- */

#define MAX_MOTES 120

typedef struct {
    int x, y, vx, vy;
    u8 phase;
    u8 live;
    u32 patch[9];
} mote_t;

static mote_t g_motes[MAX_MOTES];

static int in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int mote_blocked(int x, int y) {
    if (y < 54 || y > g_s.h - 132) return 1;
    if (x < 20 || x > g_s.w - 20) return 1;
    if (in_rect(x, y, g_hero_x, g_hero_y, g_hero_w, g_hero_h)) return 1;
    if (in_rect(x, y, g_log_x - 24, g_log_y - 26, 540, 8 * g_log_step + 34)) return 1;
    if (in_rect(x, y, g_tel_x - 14, g_tel_y - 26, g_tel_w + 28, 146)) return 1;
    return 0;
}

static void mote_init(void) {
    for (int i = 0; i < MAX_MOTES; i++) {
        mote_t *m = &g_motes[i];
        memset(m, 0, sizeof(*m));
        for (int tries = 0; tries < 200; tries++) {
            int x = 24 + (int)(rnd() % (u32)(g_s.w - 48));
            int y = 60 + (int)(rnd() % (u32)(g_s.h - 196));
            if (mote_blocked(x, y)) continue;
            m->x = x;
            m->y = y;
            break;
        }
        m->vx = (int)(rnd() % 3) - 1;
        m->vy = -1 - (int)(rnd() % 2);
        m->phase = (u8)(rnd() & 0xFF);
    }
}

static void mote_patch_save(mote_t *m) {
    for (int dy = 0; dy < 3; dy++) {
        for (int dx = 0; dx < 3; dx++) {
            m->patch[dy * 3 + dx] = px_read(m->x + dx - 1, m->y + dy - 1);
        }
    }
}

static void mote_patch_restore(mote_t *m) {
    for (int dy = 0; dy < 3; dy++) {
        for (int dx = 0; dx < 3; dx++) {
            px_write(m->x + dx - 1, m->y + dy - 1, m->patch[dy * 3 + dx]);
        }
    }
}

static void motes_step(void) {
    for (int i = 0; i < MAX_MOTES; i++) {
        mote_t *m = &g_motes[i];
        if (m->live) mote_patch_restore(m);

        m->x += m->vx;
        m->y += m->vy;
        m->phase++;
        if (m->y < 60) { m->y = g_s.h - 140; m->x = 24 + (int)(rnd() % (u32)(g_s.w - 48)); }
        if (m->x < 20) m->x = g_s.w - 24;
        if (m->x > g_s.w - 20) m->x = 24;

        if (mote_blocked(m->x, m->y)) {
            m->live = 0;
            continue;
        }

        mote_patch_save(m);
        m->live = 1;

        u32 c = rgb_mix(C_FAINT, C_MINT, 60 + (m->phase % 64));
        px_write(m->x, m->y, c);
        if ((m->phase % 97) < 20) {
            px_write(m->x - 1, m->y, rgb_mix(C_FAINT, C_CYAN, 50));
            px_write(m->x + 1, m->y, rgb_mix(C_FAINT, C_CYAN, 50));
            px_write(m->x, m->y - 1, rgb_mix(C_FAINT, C_CYAN, 50));
            px_write(m->x, m->y + 1, rgb_mix(C_FAINT, C_CYAN, 50));
        }
        if ((m->phase % 211) == 0) fill(m->x, m->y, 2, 2, rgb(C_AMBER));
    }
}

/* ---- MCube wireframe + orbital particles ---------------------------- */

static const int g_cube_v[8][3] = {
    {-1024, -1024, -1024}, {1024, -1024, -1024},
    {1024, 1024, -1024}, {-1024, 1024, -1024},
    {-1024, -1024, 1024}, {1024, -1024, 1024},
    {1024, 1024, 1024}, {-1024, 1024, 1024},
};

static const u8 g_cube_e[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

static void draw_cube(u64 elapsed, int hot) {
    if (!g_hero_bg) return;

    hero_restore(g_cube_cx - g_cube_box / 2, g_cube_cy - g_cube_box / 2,
                 g_cube_box, g_cube_box);

    int ay = (int)((elapsed * 45 / 1000 + 30) % 360);
    int ax = 26 + 20 * isin((int)(elapsed / 18)) / 4096;
    int ca = icos(ay), sa = isin(ay);
    int cb = icos(ax), sb = isin(ax);
    int vs = g_cube_size * 3 / 8;

    int px[8], py[8], pz[8];
    for (int i = 0; i < 8; i++) {
        int x = g_cube_v[i][0], y = g_cube_v[i][1], z = g_cube_v[i][2];
        /* Vertices are Q10, the trig table is Q12: divide by 4096 so the
         * rotated coordinates stay in the same Q10 space. */
        int x1 = (x * ca + z * sa) / 4096;
        int z1 = (-x * sa + z * ca) / 4096;
        int y1 = (y * cb - z1 * sb) / 4096;
        int z2 = (y * sb + z1 * cb) / 4096;
        px[i] = g_cube_cx + x1 * vs / 1024;
        py[i] = g_cube_cy + y1 * vs / 1024;
        pz[i] = z2;
    }

    int order[12];
    int key[12];
    for (int i = 0; i < 12; i++) {
        order[i] = i;
        key[i] = pz[g_cube_e[i][0]] + pz[g_cube_e[i][1]];
    }
    for (int i = 0; i < 11; i++) {
        for (int j = i + 1; j < 12; j++) {
            if (key[order[j]] < key[order[i]]) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }

    for (int k = 0; k < 12; k++) {
        int e = order[k];
        int a = g_cube_e[e][0], b = g_cube_e[e][1];
        int t = (key[e] + 3584) * 255 / 7168;
        u32 c = hot ? rgb_mix(0x1E5C46, C_MINT, t)
                    : rgb_mix(0x14505C, C_CYAN, t);
        line(px[a], py[a], px[b], py[b], c);
        if (t > 205) line(px[a] + 1, py[a], px[b] + 1, py[b], mix(c, rgb(C_WHITE), 60));
    }

    for (int i = 0; i < 8; i++) {
        u32 c = pz[i] > 0 ? rgb(C_WHITE) : rgb(0x21564A);
        fill(px[i] - 1, py[i] - 1, 3, 3, c);
    }
}

#define MAX_ORBS 36

typedef struct {
    int ring, ang, speed;
    u8 live;
} orb_t;

static orb_t g_orbs[MAX_ORBS];

static void orbs_init(void) {
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 12; i++) {
            orb_t *o = &g_orbs[r * 12 + i];
            o->ring = r;
            o->ang = i * 30 + r * 10;
            o->speed = 2 + r;
            o->live = 0;
        }
    }
}

static void orbs_draw(void) {
    int rx0 = g_cube_size / 2 + 6;
    int ry0 = g_cube_size / 4 + 4;
    for (int i = 0; i < MAX_ORBS; i++) {
        orb_t *o = &g_orbs[i];
        int rx = rx0 + o->ring * 11;
        int ry = ry0 + o->ring * 10;
        int ox = g_cube_cx + icos(o->ang) * rx / 4096;
        int oy = g_cube_cy + 8 + isin(o->ang) * ry / 4096;

        if (o->live) hero_restore(ox - 1, oy - 1, 5, 5);

        o->ang += o->speed;
        if (o->ang >= 360) o->ang -= 360;
        o->live = 1;

        int x = g_cube_cx + icos(o->ang) * rx / 4096;
        int y = g_cube_cy + 8 + isin(o->ang) * ry / 4096;

        u32 c = o->ring == 0 ? rgb(C_CYAN)
              : o->ring == 1 ? rgb(C_VIOLET)
                             : rgb_mix(C_CYAN, C_MINT, 120);
        if (o->ring == 2 && (i % 5) == 0) c = rgb(C_AMBER);
        u32 halo = mix(c, rgb(C_BG_TOP), 170);
        fill(x - 1, y - 1, 3, 3, halo);
        fill(x, y, 2, 2, c);
    }
}

/* ---- HUD ------------------------------------------------------------ */

/* Trace lines are formatted once up front, then revealed a character at a
 * time so the block scans like a live console instead of popping in rows.
 * The caret parks at the head of whatever is still typing. */
static char g_log_text[LOG_COUNT][80];
static int g_log_chars[LOG_COUNT];
static int g_car_x = -1, g_car_y;

static int g_last_pct = -1;
static int g_last_phase = -1;
static int g_core_chip;
static int g_bars[24];
static int g_ul_prev = -1;

static void draw_header(void) {
    text_b(24, 12, 2, rgb(C_WHITE), "MCUBE");
    int x = 24 + text_w("MCUBE", 2) + 16;
    text(x, 22, 1, rgb(C_MINT), "//");
    text(x + 28, 22, 1, rgb(C_TRACE), "SECURE BOOT");

    const char *right = "CHAIN VERIFIED";
    int rw = text_w(right, 1);
    text_right(g_s.w - 24, 22, 1, rgb(C_DIM), right);
    fill(g_s.w - 24 - rw - 14, 27, 4, 4, rgb(C_MINT));

    line(24, 46, g_s.w - 24, 46, rgb(0x123A3A));
    fill(24, 46, 12, 1, rgb(C_MINT));
    fill(g_s.w - 36, 46, 12, 1, rgb(C_MINT));
}

static void draw_footer(u32 mem_mib) {
    u32 c = rgb(C_DIM);
    line(24, g_s.h - 40, g_s.w - 24, g_s.h - 40, rgb(0x102A2C));
    fill(24, g_s.h - 40, 12, 1, rgb(0x1E5C4A));
    fill(g_s.w - 36, g_s.h - 40, 12, 1, rgb(0x1E5C4A));

    text(24, g_s.h - 28, 1, c, "MCUBEAIOS // AI-NATIVE OPERATING SYSTEM");
    char buf[56];
    ksnprintf(buf, sizeof(buf), "BUILD 0.1.0-GENESIS // MEM %u MiB", mem_mib);
    text_right(g_s.w - 24, g_s.h - 28, 1, c, buf);
}

static void draw_log_header(void) {
    fill(g_log_x, g_log_y - 26, 3, 8, rgb(C_MINT));
    text_b(g_log_x + 12, g_log_y - 30, 1, rgb(C_MINT), "BOOT TRACE");
    line(g_log_x + text_w("BOOT TRACE", 1) + 24, g_log_y - 22,
         g_log_x + 400, g_log_y - 22, rgb(0x123A34));
}

static void logs_init(void) {
    for (int i = 0; i < LOG_COUNT; i++) {
        ksnprintf(g_log_text[i], sizeof(g_log_text[i]), "0x%06X  OK  %s",
                  0x00100000u + (u32)i * 0x2C400u, g_log_lines[i]);
        g_log_chars[i] = 0;
    }
}

/* Columns 0..7 are the address, 10..11 the verdict, the rest the detail. */
static void draw_log_line(int i, int from, int to) {
    int y = g_log_y + i * g_log_step;
    for (int c = from; c < to; c++) {
        u32 col = c < 8 ? rgb(0x2E6A5A)
               : c < 12 ? rgb(C_MINT)
                        : rgb(C_TEXT);
        glyph(g_log_x + c * FONT_W, y, 1, col, (u8)g_log_text[i][c]);
    }
}

static void logs_step(u64 elapsed) {
    /* About two hundred characters a second, sampled once per frame. */
    int budget = elapsed < 140 ? 0 : (int)((elapsed - 140) / 5);
    int k = 0;
    for (; k < LOG_COUNT; k++) {
        int len = (int)strlen(g_log_text[k]);
        if (budget >= len + 3) { budget -= len + 3; continue; }
        break;
    }
    for (int j = 0; j < LOG_COUNT; j++) {
        int len = (int)strlen(g_log_text[j]);
        int want = j < k ? len : (j == k ? budget : 0);
        if (want > len) want = len;
        if (want <= g_log_chars[j]) continue;
        draw_log_line(j, g_log_chars[j], want);
        g_log_chars[j] = want;
    }
}

/* Erase and redraw around logs_step so an advancing caret never wipes a
 * glyph that landed in the same frame. */
static void caret_erase(void) {
    if (g_car_x < 0) return;
    bg_rect(g_car_x, g_car_y + 3, FONT_W - 2, FONT_H - 6);
    g_car_x = -1;
}

static void caret_draw(u64 elapsed) {
    int line = 0, col = 0;
    for (int j = 0; j < LOG_COUNT; j++) {
        int len = (int)strlen(g_log_text[j]);
        line = j;
        col = g_log_chars[j];
        if (col < len) break;
    }
    g_car_x = g_log_x + col * FONT_W + 1;
    g_car_y = g_log_y + line * g_log_step;
    if ((elapsed / 400) & 1) {
        fill(g_car_x, g_car_y + 3, FONT_W - 2, FONT_H - 6, rgb(C_MINT));
    }
}

static void tel_init(void) {
    for (int i = 0; i < 24; i++) g_bars[i] = 8 + (int)(rnd() % 40u);
    fill(g_tel_x, g_tel_y + 24, g_tel_w, 82, rgb(0x04100E));
    text_b(g_tel_x, g_tel_y - 2, 1, rgb(C_TRACE), "NEURAL ACTIVITY");
}

static void tel_draw(u64 elapsed) {
    text_right(g_tel_x + g_tel_w, g_tel_y - 2, 1, rgb(0x2E6A56), "LIVE");
    fill(g_tel_x + g_tel_w - 7, g_tel_y, 4, 4,
         ((elapsed / 500) & 1) ? rgb(C_MINT) : rgb(0x1C4A38));

    fill(g_tel_x, g_tel_y + 24, g_tel_w, 70, rgb(0x04100E));

    int n = 24;
    int gap = 3;
    int bw = (g_tel_w - (n - 1) * gap) / n;
    if (bw < 4) bw = 4;
    int base = g_tel_y + 92;
    int sum = 0;

    for (int i = 0; i < n; i++) {
        int target = 8 + (int)(rnd() % 58u);
        g_bars[i] += (target - g_bars[i]) / 6;
        if (g_bars[i] < 4) g_bars[i] = 4;
        if (g_bars[i] > 66) g_bars[i] = 66;
        int h = g_bars[i];
        sum += h;

        int x = g_tel_x + i * (bw + gap);
        fill(x, base - 66, bw, 66, rgb(0x051614));
        u32 c = rgb_mix(0x145148, C_MINT, h * 255 / 66);
        fill(x, base - h, bw, h, c);
        fill(x, base - h, bw, 2, rgb_mix(c, C_WHITE, 90));
    }

    int pct = sum * 100 / (n * 66);
    char buf[48];
    ksnprintf(buf, sizeof(buf), "CORE %3d%%", pct);
    fill(g_tel_x, g_tel_y + 100, 160, 16, rgb(0x04100E));
    text(g_tel_x, g_tel_y + 100, 1, rgb(C_MINT), buf);

    u64 ms = elapsed % 1000;
    ksnprintf(buf, sizeof(buf), "T+ %u.%03u s",
              (u32)(elapsed / 1000), (u32)ms);
    fill(g_tel_x + 170, g_tel_y + 100, g_tel_w - 170, 16, rgb(0x04100E));
    text_right(g_tel_x + g_tel_w - 16, g_tel_y + 100, 1, rgb(C_DIM), buf);

    static const char spin[] = "|/-\\";
    fill(g_tel_x + g_tel_w - 10, g_tel_y + 100, 10, 16, rgb(0x04100E));
    glyph(g_tel_x + g_tel_w - 8, g_tel_y + 100, 1, rgb(C_MINT),
          (u8)spin[(elapsed / 80) & 3]);
}

static int progress_pct(u64 elapsed) {
    int pct = (int)(elapsed * 100 / 2400);
    if (pct > 100) pct = 100;
    return pct;
}

static const char *const g_phases[] = {
    "INITIALIZING NEURAL LATTICE",
    "LINKING CORTEX MODULES",
    "SYNCHRONIZING AI CORE",
    "CALIBRATING DESKTOP",
    "NEURAL CORE ONLINE",
    "HANDING OFF TO DESKTOP",
};

static int progress_phase_id(int pct, u64 elapsed) {
    if (elapsed >= 3150) return 5;
    if (pct >= 100) return 4;
    if (pct < 25) return 0;
    if (pct < 55) return 1;
    if (pct < 85) return 2;
    return 3;
}

static void draw_progress(u64 elapsed) {
    int pct = progress_pct(elapsed);
    int phase_id = progress_phase_id(pct, elapsed);

    if (pct != g_last_pct || g_last_phase != phase_id) {
        bg_rect(g_prog_x, g_prog_y - 26, 460, 18);
        text_b(g_prog_x, g_prog_y - 26, 1, rgb(C_MINT), g_phases[phase_id]);

        char pb[8];
        ksnprintf(pb, sizeof(pb), "%3d%%", pct);
        bg_rect(g_prog_x + g_prog_w - 64, g_prog_y - 28, 64, 20);
        text_b(g_prog_x + g_prog_w - text_w(pb, 2), g_prog_y - 32, 2,
               rgb_mix(C_CYAN, C_WHITE, pct == 100 ? 170 : 60), pb);
        /* A short rule under the label anchors the two hairlines. */
        fill(g_prog_x, g_prog_y - 8, 28, 1, rgb(0x1E5C4A));

        g_last_pct = pct;
        g_last_phase = phase_id;
    }

    int nseg = g_big ? 48 : 40;
    int gap = 2;
    int sw = (g_prog_w - (nseg - 1) * gap) / nseg;
    if (sw < 3) sw = 3;
    int filled = pct * nseg / 100;

    fill(g_prog_x, g_prog_y - 1, g_prog_w, g_prog_h + 2, rgb(0x04100E));
    for (int i = 0; i < nseg; i++) {
        int x = g_prog_x + i * (sw + gap);
        u32 c = rgb_mix(C_MINT, C_CYAN, i * 255 / (nseg - 1));
        if (i < filled) {
            fill(x, g_prog_y, sw, g_prog_h, c);
            fill(x, g_prog_y, sw, 2, rgb_mix(c, C_WHITE, 100));
        } else if (i == filled) {
            fill(x, g_prog_y, sw, g_prog_h, rgb_mix(c, C_WHITE, 150));
        } else {
            fill(x, g_prog_y, sw, 1, rgb(0x0F2C28));
            fill(x, g_prog_y + g_prog_h - 1, sw, 1, rgb(0x0A1E1A));
        }
    }
}

static void underline_anim(u64 elapsed) {
    if (!g_hero_bg) return;
    if (g_ul_prev >= 0) hero_restore(g_ul_x + g_ul_prev - 2, g_ul_y - 2, 40, 5);

    int span = g_ul_w + 80;
    int lx = (int)((elapsed / 8) % (u32)span) - 40;
    g_ul_prev = lx;

    for (int i = 0; i < 28; i++) {
        int x = g_ul_x + lx + i;
        if (x < g_ul_x || x >= g_ul_x + g_ul_w) continue;
        int t = 255 - i * 9;
        u32 c = rgb_mix(C_MINT, C_WHITE, t);
        fill(x, g_ul_y - 1, 1, 3, c);
    }
}

static void core_chip(u64 elapsed) {
    if (g_core_chip || elapsed < 2400) return;
    g_core_chip = 1;

    const char *s = "NEURAL CORE ONLINE";
    int w = text_w(s, 1) + 34;
    int x = g_comp_cx - w / 2;
    int y = g_hero_y + g_hero_h - 30;
    fill(x, y, w, 19, rgb_mix(C_BG_TOP, C_MINT, 24));
    fill(x, y, w, 1, rgb(0x1E5C48));
    fill(x, y + 18, w, 1, rgb(0x1E5C48));
    fill(x, y, 1, 19, rgb(0x1E5C48));
    fill(x + w - 1, y, 1, 19, rgb(0x1E5C48));
    fill(x + 8, y + 8, 3, 3, rgb(C_MINT));
    text_b(x + 18, y + 2, 1, rgb_mix(C_MINT, C_WHITE, 90), s);
}

/* ---- scan head ------------------------------------------------------ */

/* One pass of a soft scan band down the hero while the modules load. The
 * band is erased at the top of the next frame, before the cube and the
 * orbitals repaint, so it never leaves a trail in the snapshot. */
static int g_scan_y, g_scan_on;

static void scan_restore(void) {
    if (!g_scan_on) return;
    g_scan_on = 0;
    if (!g_hero_bg) return;
    hero_restore(g_hero_x, g_scan_y - 14, g_hero_w, 20);
}

static void scan_draw(u64 elapsed) {
    if (!g_hero_bg || elapsed >= 2400) return;
    int y = g_hero_y + (int)(elapsed * (u64)g_hero_h / 2400);
    g_scan_y = y;
    g_scan_on = 1;

    for (int i = -9; i < 0; i++) {
        int yy = y + i;
        if (yy < g_hero_y || yy >= g_hero_y + g_hero_h) continue;
        int a = (i + 9) * 13;
        for (int x = g_hero_x; x < g_hero_x + g_hero_w; x++) {
            blend(x, yy, C_MINT, a);
        }
    }
    if (y < g_hero_y || y >= g_hero_y + g_hero_h) return;
    for (int x = g_hero_x; x < g_hero_x + g_hero_w; x++) {
        blend(x, y, C_WHITE, 140);
    }
}

/* ---- entry ---------------------------------------------------------- */

void fb_boot_splash(u32 usable_mib) {
    mcube_fb_info_t info;
    if (fb_get_info(&info) != 0 || info.width < 320 || info.height < 200) return;

    g_s.base = (u8 *)phys_to_virt(info.phys);
    g_s.w = (int)info.width;
    g_s.h = (int)info.height;
    g_s.pitch = (int)info.pitch;
    g_s.fmt = (int)info.format;

    fb_console_disable();

    layout();
    g_hero_bg = kmalloc((usize)g_hero_w * (usize)g_hero_h * 4);
    if (g_hero_bg) {
        memset(g_hero_bg, 0, (usize)g_hero_w * (usize)g_hero_h * 4);
    }

    draw_background();
    draw_hero_background();

    mote_init();
    orbs_init();
    tel_init();
    logs_init();

    draw_header();
    draw_footer(usable_mib);
    draw_log_header();

    u64 t0 = timer_uptime_ms();
    for (;;) {
        u64 elapsed = timer_uptime_ms() - t0;
        if (elapsed >= SPLASH_MS) break;
        int hot = elapsed >= 2400;

        scan_restore();
        underline_anim(elapsed);
        motes_step();

        draw_cube(elapsed, hot);
        orbs_draw();
        scan_draw(elapsed);

        caret_erase();
        logs_step(elapsed);
        caret_draw(elapsed);

        tel_draw(elapsed);
        draw_progress(elapsed);
        core_chip(elapsed);

        timer_sleep_ms(40);
    }
}
