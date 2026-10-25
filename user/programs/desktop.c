/* MCubeAIOS desktop: compositor, window manager, taskbar and launcher.
 *
 * Every application renders into its own off-screen surface through the pipe
 * protocol in <mcube/gui.h>. The desktop owns the framebuffer, composites
 * free-floating windows in z-order, and implements the interaction model users
 * expect from a desktop OS: drag, resize, minimize/maximize, taskbar,
 * Alt+Tab, Alt+F4 and a Start menu.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>
#include <mcube/fb.h>
#include <mcube/input.h>

#define MAX_WINS      10
#define TASKBAR_H     42
#define TITLE_H       28
#define WIN_BORDER    1
#define MIN_W         220
#define MIN_H         140
#define CORNER_R      8
#define TRAY_W        214

#define START_X       8
#define START_Y       4
#define START_W       96
#define START_H       (TASKBAR_H - 8)
#define TASK_BTN_H    (TASKBAR_H - 8)

#define MENU_W        300
#define MENU_HEADER_H 76
#define MENU_ITEM_H   40
#define MENU_PAD      8

#define CURSOR_W      12
#define CURSOR_H      16

#define C_DESK_TOP    0x05080F
#define C_DESK_BOTTOM 0x0C1728
#define C_TASKBAR     0x0B111B
#define C_TASKBAR_TOP 0x0E1622
#define C_TASKBAR_BOT 0x070B12
#define C_TASKBAR_HI  0x1A2536
#define C_TASKBAR_HOV 0x27374F
#define C_TASKBAR_ON  0x20344F
#define C_LINE        0x2A3A55
#define C_TEXT        0xE8ECF4
#define C_TEXT_DIM    0x93A2B8
#define C_ACCENT      0x3B82F6
#define C_ACCENT_2    0x22D3EE
#define C_ACCENT_DARK 0x1D4ED8
#define C_FRAME       0x0E1420
#define C_BORDER      0x3E4E68
#define C_TITLE_ON_1  0x3A76E8
#define C_TITLE_ON_2  0x1B4AA6
#define C_TITLE_OFF_1 0x283142
#define C_TITLE_OFF_2 0x1B2230
#define C_MENU        0x111926
#define C_MENU_HOVER  0x2458B8
#define C_SHADOW      0x03050A
#define C_CLOSE_HOVER 0xD91B2B
#define C_SWITCH      0x0B1220

#define MOD_SHIFT 0x01u
#define MOD_CTRL  0x02u
#define MOD_ALT   0x04u
#define MOD_WIN   0x08u

enum { BTN_MIN = 1, BTN_MAX = 2, BTN_CLOSE = 3 };
enum { RZ_LEFT = 1, RZ_RIGHT = 2, RZ_TOP = 4, RZ_BOTTOM = 8 };

typedef struct {
    int used;
    pid_t pid;
    int cmd_fd;
    int evt_fd;
    char title[64];
    int ready;
    int x, y, w, h;        /* content rectangle in screen coordinates */
    u32 *surface;
    int sw, sh;            /* allocated surface (stride, rows) */
    int dirty;
    int flushed;
    int focused;
    int minimized;
    int maximized;
    int desk_hidden;       /* minimized by the show-desktop shortcut */
    int rx, ry, rw, rh;    /* geometry to restore after maximize */
} win_t;

static u32 *g_fb;
static int g_fbw, g_fbh, g_stride, g_fmt;
static u32 *g_bg;

static win_t g_wins[MAX_WINS];
static int g_zorder[MAX_WINS];   /* bottom -> top */
static int g_zcount;
static int g_focus = -1;
static int g_spawn_seq;

static int g_menu_open;
static int g_menu_sel;

static int g_switcher_open;
static int g_switcher_sel;
static int g_switcher_list[MAX_WINS];
static int g_switcher_n;

static u32 g_mods;

static int g_mx, g_my;
static u32 g_buttons;
static int g_press_win = -1;

static int g_drag_slot = -1;
static int g_drag_dx, g_drag_dy;

static int g_resize_slot = -1;
static int g_resize_zone;
static int g_resize_x0, g_resize_y0, g_resize_w0, g_resize_h0;
static int g_resize_mx0, g_resize_my0;

static int g_hover_slot = -1;
static int g_hover_btn;
static int g_hover_task = -1;    /* -2 = Start button, -1 = none, else slot */

static int g_desk_sel = -1;
static int g_desk_hover = -1;
static u64 g_desk_last_ms;
static int g_desk_last_idx = -1;

static u64 g_last_click_ms;
static int g_last_click_slot = -1;

static int g_need_compose;
static int g_need_taskbar;
static int g_need_menu;
static int g_cursor_moved;

static int g_cursor_saved;
static int g_cursor_x, g_cursor_y, g_cursor_w, g_cursor_h;
static u32 g_cursor_save[CURSOR_H][CURSOR_W];

static int g_ready_count;
static int g_flushed_count;
static int g_gui_ok_printed;
static u64 g_last_clock;

/* Desktop system-status card. Data is refreshed once per second. */
static u64 g_sys_uptime_s;
static int g_sys_procs;
static int g_sys_second;
static int g_sys_dirty;

static int g_task_slot[MAX_WINS];
static int g_task_x[MAX_WINS];
static int g_task_w[MAX_WINS];
static int g_task_n;

static const char *const g_app_names[] = {
    "Terminal", "Files", "Editor", "Calculator",
    "Image Viewer", "Settings", "Task Manager", "System Info",
};
static const char *const g_app_paths[] = {
    "/bin/terminal.lgy", "/bin/files.lgy", "/bin/editor.lgy",
    "/bin/calc.lgy", "/bin/image.lgy", "/bin/settings.lgy",
    "/bin/taskman.lgy", "/bin/sysinfo.lgy",
};
#define APP_COUNT ((int)(sizeof(g_app_paths) / sizeof(g_app_paths[0])))
#define BOOT_APP_COUNT 7

static const u32 g_icon_colors[] = {
    0x2AA9E0, 0xF59E0B, 0x10B981, 0xEC4899,
    0x8B5CF6, 0x22D3EE, 0xEF4444, 0x22C55E,
};

typedef struct {
    const char *name;
    const char *path;
} desk_app_t;

static const desk_app_t g_desk_apps[] = {
    { "Terminal", "/bin/terminal.lgy" },
    { "Files",    "/bin/files.lgy" },
    { "Editor",   "/bin/editor.lgy /TEST.TXT" },
    { "Tasks",    "/bin/taskman.lgy" },
    { "Settings", "/bin/settings.lgy" },
    { "System",   "/bin/sysinfo.lgy" },
};
#define DESK_COUNT ((int)(sizeof(g_desk_apps) / sizeof(g_desk_apps[0])))

#define DESK_BOX_X    18
#define DESK_BOX_Y    18
#define DESK_BOX_W    88
#define DESK_BOX_H    90
#define DESK_BOX_STEP 96
#define DESK_ICON_SZ  46

/* ---- pixel helpers -------------------------------------------------- */

/* Screen-space clip for region composition. Primitives writing into the
 * desktop framebuffer intersect against it; window surfaces and the cached
 * wallpaper are unaffected. */
static int g_clip_x0, g_clip_y0, g_clip_x1, g_clip_y1;

static void clip_reset(void) {
    g_clip_x0 = 0;
    g_clip_y0 = 0;
    g_clip_x1 = g_fbw;
    g_clip_y1 = g_fbh;
}

static void clip_set(int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_fbw) x1 = g_fbw;
    if (y1 > g_fbh) y1 = g_fbh;
    g_clip_x0 = x;
    g_clip_y0 = y;
    g_clip_x1 = x1;
    g_clip_y1 = y1;
}

static u32 pixel(u32 rgb) {
    u8 r = (u8)(rgb >> 16), g = (u8)(rgb >> 8), b = (u8)rgb;
    if (g_fmt == 1) { /* BOOT_PIXEL_BGRA8888 */
        return 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | b;
    }
    return 0xFF000000u | ((u32)b << 16) | ((u32)g << 8) | r;
}

static u32 lerp_color(u32 a, u32 b, int t) {
    int ar = (int)(a >> 16) & 0xFF, ag = (int)(a >> 8) & 0xFF, ab = (int)a & 0xFF;
    int br = (int)(b >> 16) & 0xFF, bg = (int)(b >> 8) & 0xFF, bb = (int)b & 0xFF;
    int r = ar + (br - ar) * t / 255;
    int g = ag + (bg - ag) * t / 255;
    int bl = ab + (bb - ab) * t / 255;
    return ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
}

static void gfx_fill(u32 *buf, int stride, int bw, int bh,
                     int x, int y, int w, int h, u32 rgb) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    int cx0 = 0, cy0 = 0, cx1 = bw, cy1 = bh;
    if (buf == g_fb) {
        cx0 = g_clip_x0;
        cy0 = g_clip_y0;
        cx1 = g_clip_x1;
        cy1 = g_clip_y1;
    }
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 >= x1 || y0 >= y1) return;
    u32 c = pixel(rgb);
    for (int yy = y0; yy < y1; yy++) {
        u32 *line = buf + (u64)yy * stride;
        for (int xx = x0; xx < x1; xx++) line[xx] = c;
    }
}

static void gfx_rect(u32 *buf, int stride, int bw, int bh,
                     int x, int y, int w, int h, u32 rgb) {
    gfx_fill(buf, stride, bw, bh, x, y, w, 1, rgb);
    gfx_fill(buf, stride, bw, bh, x, y + h - 1, w, 1, rgb);
    gfx_fill(buf, stride, bw, bh, x, y, 1, h, rgb);
    gfx_fill(buf, stride, bw, bh, x + w - 1, y, 1, h, rgb);
}

static void gfx_line(u32 *buf, int stride, int bw, int bh,
                     int x0, int y0, int x1, int y1, u32 rgb) {
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    int cx0 = 0, cy0 = 0, cx1 = bw, cy1 = bh;
    if (buf == g_fb) {
        cx0 = g_clip_x0;
        cy0 = g_clip_y0;
        cx1 = g_clip_x1;
        cy1 = g_clip_y1;
    }
    u32 c = pixel(rgb);
    for (;;) {
        if (x0 >= cx0 && x0 < cx1 && y0 >= cy0 && y0 < cy1) {
            buf[(u64)y0 * stride + x0] = c;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void gfx_vgradient(u32 *buf, int stride, int bw, int bh,
                          int x, int y, int w, int h, u32 top, u32 bottom) {
    for (int yy = 0; yy < h; yy++) {
        u32 c = lerp_color(top, bottom, h > 1 ? yy * 255 / (h - 1) : 0);
        gfx_fill(buf, stride, bw, bh, x, y + yy, w, 1, c);
    }
}

static void gfx_text(u32 *buf, int stride, int bw, int bh,
                     int x, int y, u32 rgb, const char *s, int n) {
    if (!s) return;
    u32 c = pixel(rgb);
    int cx0 = 0, cy0 = 0, cx1 = bw, cy1 = bh;
    if (buf == g_fb) {
        cx0 = g_clip_x0;
        cy0 = g_clip_y0;
        cx1 = g_clip_x1;
        cy1 = g_clip_y1;
    }
    int px = x;
    for (int i = 0; (n < 0 || i < n) && s[i]; i++) {
        u8 ch = (u8)s[i];
        if (ch == '\n') { px = x; y += 16; continue; }
        if (px + 8 > cx0 && px < cx1 && y + 16 > cy0 && y < cy1) {
            const u8 *glyph = mc_font8x16[ch];
            for (int row = 0; row < 16; row++) {
                int py = y + row;
                if (py < cy0 || py >= cy1) continue;
                u8 bits = glyph[row];
                for (int col = 0; col < 8; col++) {
                    if (!(bits & (0x80 >> col))) continue;
                    int gx = px + col;
                    if (gx < cx0 || gx >= cx1) continue;
                    buf[(u64)py * stride + gx] = c;
                }
            }
        }
        px += 8;
    }
}

static int isqrt_i(int v) {
    if (v <= 0) return 0;
    int r = 0;
    int bit = 1 << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static int corner_inset(int row, int h, int r) {
    int d;
    if (r <= 0 || row >= h) return 0;
    if (row < r) d = r - 1 - row;
    else if (row >= h - r) d = row - (h - r);
    else return 0;
    return r - isqrt_i(r * r - d * d);
}

static void gfx_round_fill(u32 *buf, int stride, int bw, int bh,
                           int x, int y, int w, int h, int r, u32 rgb) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int yy = 0; yy < h; yy++) {
        int ins = corner_inset(yy, h, r);
        gfx_fill(buf, stride, bw, bh, x + ins, y + yy, w - 2 * ins, 1, rgb);
    }
}

static void gfx_round_top_gradient(u32 *buf, int stride, int bw, int bh,
                                   int x, int y, int w, int h, int r,
                                   u32 top, u32 bottom) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    for (int yy = 0; yy < h; yy++) {
        int ins = (yy < r) ? r - isqrt_i(r * r - (r - 1 - yy) * (r - 1 - yy)) : 0;
        u32 c = lerp_color(top, bottom, h > 1 ? yy * 255 / (h - 1) : 0);
        gfx_fill(buf, stride, bw, bh, x + ins, y + yy, w - 2 * ins, 1, c);
    }
}

static void gfx_text_scale(u32 *buf, int stride, int bw, int bh,
                           int x, int y, int scale, u32 rgb,
                           const char *s, int n) {
    if (!s || scale < 1) return;
    u32 c = pixel(rgb);
    for (int i = 0; (n < 0 || i < n) && s[i]; i++) {
        u8 ch = (u8)s[i];
        if (ch == '\n') { x += 0; y += 16 * scale; continue; }
        const u8 *glyph = mc_font8x16[ch];
        int gx0 = x + i * 8 * scale;
        if (gx0 + 8 * scale > 0 && gx0 < bw) {
            for (int row = 0; row < 16; row++) {
                u8 bits = glyph[row];
                for (int col = 0; col < 8; col++) {
                    if (!(bits & (0x80 >> col))) continue;
                    gfx_fill(buf, stride, bw, bh, gx0 + col * scale,
                             y + row * scale, scale, scale, c);
                }
            }
        }
    }
}

static void gfx_dim(u32 *buf, int stride, int bw, int bh,
                    int x, int y, int w, int h, int amount) {
    int cx0 = 0, cy0 = 0, cx1 = bw, cy1 = bh;
    if (buf == g_fb) {
        cx0 = g_clip_x0;
        cy0 = g_clip_y0;
        cx1 = g_clip_x1;
        cy1 = g_clip_y1;
    }
    if (x < cx0) { w -= cx0 - x; x = cx0; }
    if (y < cy0) { h -= cy0 - y; y = cy0; }
    if (x + w > cx1) w = cx1 - x;
    if (y + h > cy1) h = cy1 - y;
    if (w <= 0 || h <= 0) return;
    u32 keep = (u32)(255 - amount);
    for (int yy = 0; yy < h; yy++) {
        u32 *line = buf + (u64)(y + yy) * stride;
        for (int xx = 0; xx < w; xx++) {
            u32 p = line[x + xx];
            u32 r = ((p >> 16) & 0xFF) * keep / 255;
            u32 g = ((p >> 8) & 0xFF) * keep / 255;
            u32 b = (p & 0xFF) * keep / 255;
            line[x + xx] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

static void gfx_text_right(u32 *buf, int stride, int bw, int bh,
                           int right, int y, u32 rgb, const char *s) {
    int w = (int)strlen(s) * 8;
    gfx_text(buf, stride, bw, bh, right - w, y, rgb, s, -1);
}

/* Draw a string inside max_chars cells; append "..." when it does not fit. */
static void gfx_text_clip(u32 *buf, int stride, int bw, int bh,
                          int x, int y, u32 rgb, const char *s, int max_chars) {
    if (!s || max_chars <= 3) return;
    int len = (int)strlen(s);
    if (len <= max_chars) {
        gfx_text(buf, stride, bw, bh, x, y, rgb, s, -1);
        return;
    }
    char tmp[64];
    int keep = max_chars - 3;
    if (keep > 59) keep = 59;
    memcpy(tmp, s, (size_t)keep);
    tmp[keep] = '.';
    tmp[keep + 1] = '.';
    tmp[keep + 2] = '.';
    tmp[keep + 3] = 0;
    gfx_text(buf, stride, bw, bh, x, y, rgb, tmp, -1);
}

static void draw_icon(u32 *buf, int stride, int bw, int bh,
                      int x, int y, int size, u32 rgb, const char *label) {
    char letter = (label && label[0]) ? label[0] : '?';
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    char s[2] = { letter, 0 };
    int r = size / 4;

    gfx_round_fill(buf, stride, bw, bh, x, y, size, size, r, rgb);
    gfx_fill(buf, stride, bw, bh, x + r / 2, y + 1, size - r, 1,
             lerp_color(rgb, 0xFFFFFF, 60));
    gfx_fill(buf, stride, bw, bh, x + r / 2, y + size - 2, size - r, 1,
             lerp_color(rgb, 0x000000, 55));

    int scale = size >= 40 ? 2 : 1;
    gfx_text_scale(buf, stride, bw, bh,
                   x + (size - 8 * scale) / 2,
                   y + (size - 16 * scale) / 2,
                   scale, 0xFFFFFF, s, 1);
}

/* ---- clipping and cursor -------------------------------------------- */

static void blit_surface(const win_t *w) {
    if (!w->surface || w->w <= 0 || w->h <= 0) return;

    int cw = w->w < w->sw ? w->w : w->sw;
    int ch = w->h < w->sh ? w->h : w->sh;
    int frame_bottom = w->y + ch;

    for (int yy = w->y; yy < w->y + ch; yy++) {
        if (yy < g_clip_y0 || yy >= g_clip_y1) continue;

        /* Clip the content rectangle to the rounded bottom corners so the
         * desktop (or the window underneath) shows through. */
        int ins = 0;
        int dist = frame_bottom - yy;
        if (dist <= CORNER_R) {
            int d = CORNER_R - dist;
            ins = CORNER_R - isqrt_i(CORNER_R * CORNER_R - d * d);
        }

        int vx0 = w->x + ins;
        int vx1 = w->x + cw - ins;
        if (vx0 < g_clip_x0) vx0 = g_clip_x0;
        if (vx1 > g_clip_x1) vx1 = g_clip_x1;
        if (vx0 >= vx1) continue;

        const u32 *src = w->surface + (u64)(yy - w->y) * w->sw + (vx0 - w->x);
        memcpy(g_fb + (u64)yy * g_stride + vx0, src, (size_t)(vx1 - vx0) * 4);
    }
}

static const u16 g_cursor[CURSOR_H] = {
    0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xFE00, 0xD800, 0x8C00, 0x0C00, 0x0600, 0x0600,
};

static void cursor_restore(void) {
    if (!g_cursor_saved) return;
    for (int yy = 0; yy < g_cursor_h; yy++) {
        u32 *dst = g_fb + (u64)(g_cursor_y + yy) * g_stride + g_cursor_x;
        for (int xx = 0; xx < g_cursor_w; xx++) dst[xx] = g_cursor_save[yy][xx];
    }
    g_cursor_saved = 0;
}

static int cursor_bit(int x, int y) {
    if (x < 0 || x >= CURSOR_W || y < 0 || y >= CURSOR_H) return 0;
    return (g_cursor[y] >> (CURSOR_W - 1 - x)) & 1;
}

static void cursor_draw(void) {
    cursor_restore();

    int x = g_mx, y = g_my;
    if (x < 0 || y < 0 || x >= g_fbw || y >= g_fbh) return;

    int w = g_fbw - x < CURSOR_W ? g_fbw - x : CURSOR_W;
    int h = g_fbh - y < CURSOR_H ? g_fbh - y : CURSOR_H;

    for (int yy = 0; yy < h; yy++) {
        u32 *src = g_fb + (u64)(y + yy) * g_stride + x;
        for (int xx = 0; xx < w; xx++) g_cursor_save[yy][xx] = src[xx];
    }
    g_cursor_x = x;
    g_cursor_y = y;
    g_cursor_w = w;
    g_cursor_h = h;
    g_cursor_saved = 1;

    u32 white = pixel(0xFFFFFF);
    u32 black = pixel(0x000000);
    for (int yy = 0; yy < h; yy++) {
        u32 *line = g_fb + (u64)(y + yy) * g_stride + x;
        for (int xx = 0; xx < w; xx++) {
            int on = cursor_bit(xx, yy);
            if (on) {
                line[xx] = white;
            } else if (cursor_bit(xx - 1, yy) || cursor_bit(xx + 1, yy) ||
                       cursor_bit(xx, yy - 1) || cursor_bit(xx, yy + 1)) {
                line[xx] = black;
            }
        }
    }
}

/* ---- geometry -------------------------------------------------------- */

static int clamp_int(int v, int lo, int hi) {
    if (hi < lo) hi = lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int work_h(void) {
    return g_fbh - TASKBAR_H;
}

static void frame_box(const win_t *w, int *fx, int *fy, int *fw, int *fh) {
    *fx = w->x - WIN_BORDER;
    *fy = w->y - TITLE_H - WIN_BORDER;
    *fw = w->w + 2 * WIN_BORDER;
    *fh = w->h + TITLE_H + 2 * WIN_BORDER;
}

/* ---- region dirty tracking ------------------------------------------ */

static int g_dirty_valid;
static int g_dirty_x0, g_dirty_y0, g_dirty_x1, g_dirty_y1;

static void dirty_add(int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_fbw) x1 = g_fbw;
    if (y1 > g_fbh) y1 = g_fbh;
    if (x >= x1 || y >= y1) return;
    if (!g_dirty_valid) {
        g_dirty_x0 = x;
        g_dirty_y0 = y;
        g_dirty_x1 = x1;
        g_dirty_y1 = y1;
        g_dirty_valid = 1;
        return;
    }
    if (x < g_dirty_x0) g_dirty_x0 = x;
    if (y < g_dirty_y0) g_dirty_y0 = y;
    if (x1 > g_dirty_x1) g_dirty_x1 = x1;
    if (y1 > g_dirty_y1) g_dirty_y1 = y1;
}

/* Window chrome plus the drop shadow, which frame_box() does not include. */
static void dirty_frame(const win_t *w) {
    int fx, fy, fw, fh;
    frame_box(w, &fx, &fy, &fw, &fh);
    dirty_add(fx, fy, fw + 10, fh + 12);
}

static int inside(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static int rects_intersect(int ax, int ay, int aw, int ah,
                           int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static void clamp_win_rect(int *x, int *y, int *w, int *h) {
    if (*w < MIN_W) *w = MIN_W;
    if (*h < MIN_H) *h = MIN_H;
    if (*w > g_fbw - 2 * WIN_BORDER) *w = g_fbw - 2 * WIN_BORDER;
    int maxh = work_h() - TITLE_H - 2 * WIN_BORDER;
    if (maxh < MIN_H) maxh = MIN_H;
    if (*h > maxh) *h = maxh;

    *x = clamp_int(*x, WIN_BORDER, g_fbw - *w - WIN_BORDER);
    *y = clamp_int(*y, TITLE_H + WIN_BORDER, work_h() - *h - WIN_BORDER);
}

static void surface_ensure(win_t *w) {
    if (w->w <= 0 || w->h <= 0) return;
    if (w->surface && w->sw >= w->w && w->sh >= w->h) return;

    int cw = (w->w + 127) & ~127;
    int ch = (w->h + 127) & ~127;
    u32 *s = (u32 *)malloc((size_t)cw * (size_t)ch * 4);
    if (!s) return;
    w->surface = s;
    w->sw = cw;
    w->sh = ch;
    gfx_fill(s, cw, cw, ch, 0, 0, cw, ch, C_FRAME);
}

static void send_event(int slot, u32 type, u32 code, int x, int y,
                       int dx, int dy, u32 buttons, int w, int h) {
    win_t *win = &g_wins[slot];
    if (!win->used || win->evt_fd < 0) return;

    gui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.x = x;
    ev.y = y;
    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = buttons;
    ev.w = (u32)(w > 0 ? w : 0);
    ev.h = (u32)(h > 0 ? h : 0);

    const u8 *p = (const u8 *)&ev;
    size_t left = sizeof(ev);
    while (left) {
        ssize_t n = write(win->evt_fd, p, left);
        if (n <= 0) return;
        p += n;
        left -= (size_t)n;
    }
}

/* ---- z-order and focus ----------------------------------------------- */

static void z_remove(int slot) {
    int out = 0;
    for (int i = 0; i < g_zcount; i++) {
        if (g_zorder[i] != slot) g_zorder[out++] = g_zorder[i];
    }
    g_zcount = out;
}

static void z_push(int slot) {
    z_remove(slot);
    if (g_zcount < MAX_WINS) g_zorder[g_zcount++] = slot;
}

static void broadcast_focus(void) {
    for (int i = 0; i < MAX_WINS; i++) {
        if (!g_wins[i].used) continue;
        g_wins[i].focused = (i == g_focus);
        send_event(i, GUI_EVT_FOCUS, i == g_focus, 0, 0, 0, 0, 0, 0, 0);
    }
}

static void choose_focus(void) {
    g_focus = -1;
    for (int i = g_zcount - 1; i >= 0; i--) {
        int slot = g_zorder[i];
        if (g_wins[slot].used && !g_wins[slot].minimized) {
            g_focus = slot;
            break;
        }
    }
    broadcast_focus();
}

static void focus_window(int slot) {
    if (slot < 0 || slot >= MAX_WINS || !g_wins[slot].used) return;
    win_t *w = &g_wins[slot];
    int changed = (g_focus != slot) || w->minimized || w->desk_hidden ||
                  g_zcount == 0 || g_zorder[g_zcount - 1] != slot;
    w->minimized = 0;
    w->desk_hidden = 0;
    z_push(slot);
    g_focus = slot;
    if (changed) {
        broadcast_focus();
        g_need_compose = 1;
    }
}

static void minimize_window(int slot) {
    win_t *w = &g_wins[slot];
    if (!w->used || w->minimized) return;
    w->minimized = 1;
    w->desk_hidden = 0;
    if (g_focus == slot) choose_focus();
    else broadcast_focus();
    g_need_compose = 1;
}

static void close_window(int slot) {
    win_t *w = &g_wins[slot];
    if (!w->used) return;

    int pid = w->pid;
    int cfd = w->cmd_fd, efd = w->evt_fd;
    w->used = 0;
    w->surface = NULL;
    w->pid = 0;
    w->cmd_fd = -1;
    w->evt_fd = -1;
    w->minimized = 0;
    w->maximized = 0;

    z_remove(slot);
    close(cfd);
    close(efd);
    if (pid > 0) {
        kill(pid, 15);
        int status = 0;
        waitpid(pid, &status, 0);
    }

    if (g_drag_slot == slot) g_drag_slot = -1;
    if (g_resize_slot == slot) g_resize_slot = -1;
    if (g_press_win == slot) g_press_win = -1;
    if (g_hover_slot == slot) { g_hover_slot = -1; g_hover_btn = 0; }
    if (g_last_click_slot == slot) g_last_click_slot = -1;

    choose_focus();
    g_need_compose = 1;
}

/* ---- drawing --------------------------------------------------------- */

static void draw_background(void) {
    if (g_bg) {
        for (int y = g_clip_y0; y < g_clip_y1; y++) {
            memcpy(g_fb + (u64)y * g_stride + g_clip_x0,
                   g_bg + (u64)y * g_fbw + g_clip_x0,
                   (size_t)(g_clip_x1 - g_clip_x0) * 4);
        }
        return;
    }
    for (int y = 0; y < g_fbh; y++) {
        u32 c = lerp_color(C_DESK_TOP, C_DESK_BOTTOM,
                           y * 255 / (g_fbh > 1 ? g_fbh - 1 : 1));
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, 0, y, g_fbw, 1, c);
    }
}

static void desk_box(int idx, int *bx, int *by) {
    *bx = DESK_BOX_X;
    *by = DESK_BOX_Y + idx * DESK_BOX_STEP;
}

static void draw_desktop_icons(void) {
    for (int i = 0; i < DESK_COUNT; i++) {
        int bx, by;
        desk_box(i, &bx, &by);
        if (by + DESK_BOX_H > work_h()) break;

        int sel = (i == g_desk_sel);
        int hov = (i == g_desk_hover);
        if (sel || hov) {
            gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx, by,
                           DESK_BOX_W, DESK_BOX_H, 6,
                           sel ? C_ACCENT : 0x16253C);
            if (sel) {
                gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx + 1, by + 1,
                               DESK_BOX_W - 2, DESK_BOX_H - 2, 5, 0x183A6E);
            }
        }

        int ix = bx + (DESK_BOX_W - DESK_ICON_SZ) / 2;
        draw_icon(g_fb, g_stride, g_fbw, g_fbh, ix, by + 9, DESK_ICON_SZ,
                  g_icon_colors[i % (int)(sizeof(g_icon_colors) /
                                         sizeof(g_icon_colors[0]))],
                  g_desk_apps[i].name);

        int tw = (int)strlen(g_desk_apps[i].name) * 8;
        int tx = bx + (DESK_BOX_W - tw) / 2;
        int ty = by + 64;
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, tx + 1, ty + 1,
                 0x04070C, g_desk_apps[i].name, -1);
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, tx, ty,
                 sel ? 0xFFFFFF : 0xD7E2F2, g_desk_apps[i].name, -1);
    }
}

static int desktop_icon_at(int x, int y) {
    if (y >= work_h()) return -1;
    for (int i = 0; i < DESK_COUNT; i++) {
        int bx, by;
        desk_box(i, &bx, &by);
        if (inside(x, y, bx, by, DESK_BOX_W, DESK_BOX_H)) return i;
    }
    return -1;
}

/* ---- system status card --------------------------------------------- */

#define SYS_CARD_W 236
#define SYS_CARD_H 134

static void sys_card_box(int *x, int *y) {
    *x = g_fbw - SYS_CARD_W - 24;
    *y = 22;
}

static void draw_sys_card(void) {
    int x, y;
    sys_card_box(&x, &y);

    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x + 5, y + 7,
                   SYS_CARD_W, SYS_CARD_H, 12, 0x02050A);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x, y,
                   SYS_CARD_W, SYS_CARD_H, 10, 0x1B3E60);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x + 1, y + 1,
                   SYS_CARD_W - 2, SYS_CARD_H - 2, 9, 0x091524);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x + 16, y + 1,
             SYS_CARD_W - 32, 2, C_ACCENT_2);

    gfx_text(g_fb, g_stride, g_fbw, g_fbh, x + 14, y + 10,
             0x66D9F7, "SYSTEM // STATUS", -1);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, x + SYS_CARD_W - 14,
                   y + 10, 0x2E5C7E, "0x01");
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x + 14, y + 30,
             SYS_CARD_W - 28, 1, 0x16344F);

    int rx = x + 14, vx = x + SYS_CARD_W - 14;
    int ry = y + 40;

    int pulse = ((g_sys_second & 1) == 0) ? 0x4ADE80 : 0x1F7A46;
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, rx, ry + 5, 6, 6, pulse);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, rx + 14, ry,
             0x5B7191, "NEURAL CORE", -1);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, vx, ry,
                   0x4ADE80, "ONLINE");

    char up[16];
    snprintf(up, sizeof(up), "%02u:%02u:%02u",
             (u32)(g_sys_uptime_s / 3600), (u32)(g_sys_uptime_s / 60 % 60),
             (u32)(g_sys_uptime_s % 60));
    ry += 22;
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, rx, ry, 0x5B7191, "UPTIME", -1);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, vx, ry, 0xD7E6FA, up);

    char procs[24];
    snprintf(procs, sizeof(procs), "%d TASKS", g_sys_procs);
    ry += 22;
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, rx, ry, 0x5B7191, "PROCESSES", -1);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, vx, ry, 0xD7E6FA, procs);

    ry += 22;
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, rx, ry, 0x5B7191, "KERNEL", -1);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, vx, ry,
                   0x7FA8C9, "0.1.0-GENESIS");
}

static int sys_card_covered(void) {
    if (g_switcher_open) return 1;
    int x, y;
    sys_card_box(&x, &y);
    for (int i = 0; i < MAX_WINS; i++) {
        win_t *w = &g_wins[i];
        if (!w->used || w->minimized) continue;
        int fx, fy, fw, fh;
        frame_box(w, &fx, &fy, &fw, &fh);
        if (fx < x + SYS_CARD_W + 6 && fx + fw + 16 > x - 6 &&
            fy < y + SYS_CARD_H + 10 && fy + fh + 18 > y - 6) {
            return 1;
        }
    }
    return 0;
}

static void refresh_sys_card(void) {
    if (g_need_compose || sys_card_covered()) return;
    /* The card repaints every pixel inside its own outline, so updating it in
     * place needs no background restore and never shows a half-erased card. */
    draw_sys_card();
    cursor_draw();
}

static void button_box(const win_t *w, int btn, int *bx, int *by,
                       int *bw, int *bh) {
    int fx, fy, fw, fh;
    frame_box(w, &fx, &fy, &fw, &fh);
    (void)fh;
    *bw = 28;
    *bh = TITLE_H - 6;
    *by = fy + 3;
    *bx = fx + fw - 2 - (*bw) * (4 - btn);
}

static void draw_title_button(const win_t *w, int slot, int btn) {
    int bx, by, bw, bh;
    button_box(w, btn, &bx, &by, &bw, &bh);

    int hover = (g_hover_slot == slot && g_hover_btn == btn);
    if (hover) {
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx, by, bw, bh, 4,
                       btn == BTN_CLOSE ? C_CLOSE_HOVER : 0x42587A);
    }

    u32 fg = (w->focused || hover) ? C_TEXT : C_TEXT_DIM;
    int cx = bx + bw / 2, cy = by + bh / 2;
    switch (btn) {
    case BTN_MIN:
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, cx - 5, cy + 4, 10, 1, fg);
        break;
    case BTN_MAX:
        if (w->maximized) {
            gfx_rect(g_fb, g_stride, g_fbw, g_fbh, cx - 2, cy - 6, 8, 8, fg);
            gfx_rect(g_fb, g_stride, g_fbw, g_fbh, cx - 6, cy - 2, 8, 8, fg);
        } else {
            gfx_rect(g_fb, g_stride, g_fbw, g_fbh, cx - 5, cy - 5, 10, 9, fg);
        }
        break;
    default:
        gfx_line(g_fb, g_stride, g_fbw, g_fbh, cx - 4, cy - 4, cx + 4, cy + 4, fg);
        gfx_line(g_fb, g_stride, g_fbw, g_fbh, cx + 4, cy - 4, cx - 4, cy + 4, fg);
        break;
    }
}

static void draw_window(const win_t *w, int slot) {
    if (w->minimized) return;

    int fx, fy, fw, fh;
    frame_box(w, &fx, &fy, &fw, &fh);

    if (!w->maximized) {
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, fx + 10, fy + 12, fw, fh,
                       CORNER_R + 4, 0x020409);
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, fx + 5, fy + 6, fw, fh,
                       CORNER_R + 2, 0x050A13);
    }

    u32 border = w->focused ? lerp_color(C_BORDER, C_ACCENT, 90) : C_BORDER;
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, fx, fy, fw, fh,
                   CORNER_R, border);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, fx + 1, fy + 1, fw - 2,
                   fh - 2, CORNER_R - 1, C_FRAME);

    if (w->focused) {
        gfx_round_top_gradient(g_fb, g_stride, g_fbw, g_fbh, fx + 1, fy + 1,
                               fw - 2, TITLE_H, CORNER_R - 1,
                               C_TITLE_ON_1, C_TITLE_ON_2);
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, fx + 1 + CORNER_R / 2, fy + 1,
                 fw - 2 - CORNER_R, 1, 0x79B4FF);
    } else {
        gfx_round_top_gradient(g_fb, g_stride, g_fbw, g_fbh, fx + 1, fy + 1,
                               fw - 2, TITLE_H, CORNER_R - 1,
                               C_TITLE_OFF_1, C_TITLE_OFF_2);
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, fx + 1 + CORNER_R / 2, fy + 1,
                 fw - 2 - CORNER_R, 1, 0x46536B);
    }
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, fx + 1, fy + TITLE_H,
             fw - 2, 1, 0x0A1018);

    draw_icon(g_fb, g_stride, g_fbw, g_fbh, fx + 7, fy + 6, 16,
              g_icon_colors[slot % (int)(sizeof(g_icon_colors) /
                                          sizeof(g_icon_colors[0]))],
              w->title);

    int text_x = fx + 31;
    int text_end = fx + fw - 2 - 28 * 3 - 8;
    int max_chars = (text_end - text_x) / 8;
    if (max_chars < 0) max_chars = 0;
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, text_x + 1, fy + 7,
             0x0A1B33, w->title, max_chars);
    gfx_text_clip(g_fb, g_stride, g_fbw, g_fbh, text_x, fy + 6,
                  w->focused ? C_TEXT : C_TEXT_DIM, w->title, max_chars);

    draw_title_button(w, slot, BTN_MIN);
    draw_title_button(w, slot, BTN_MAX);
    draw_title_button(w, slot, BTN_CLOSE);

    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, w->x, w->y,
             w->w, w->h, C_FRAME);
    blit_surface(w);
}

/* ---- taskbar --------------------------------------------------------- */

static void taskbar_layout(void) {
    g_task_n = 0;
    for (int i = 0; i < MAX_WINS; i++) {
        if (!g_wins[i].used) continue;
        g_task_slot[g_task_n++] = i;
    }
    if (g_task_n == 0) return;

    int left = START_X + START_W + 10;
    int right = g_fbw - TRAY_W - 8;
    int avail = right - left;
    int bw = g_task_n ? (avail - 4 * (g_task_n - 1)) / g_task_n : 150;
    if (bw > 168) bw = 168;
    if (bw < 84) bw = 84;

    int x = left;
    for (int i = 0; i < g_task_n; i++) {
        g_task_x[i] = x;
        g_task_w[i] = bw;
        x += bw + 4;
    }
}

static int taskbar_item_y(void) {
    return work_h() + START_Y;
}

static void draw_start_button(void) {
    int ty = taskbar_item_y();
    int open = g_menu_open;
    int hover = (g_hover_task == -2);

    u32 bg = open ? C_ACCENT : (hover ? 0x24416B : C_TASKBAR_HI);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, START_X, ty,
                   START_W, START_H, 6, bg);
    if (open || hover) {
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, START_X + 8, ty + 1,
                 START_W - 16, 1, lerp_color(bg, 0xFFFFFF, 70));
    }

    u32 logo = (open || hover) ? 0xFFFFFF : 0x6CB6FF;
    int lx = START_X + 10, ly = ty + 11;
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, lx, ly, 6, 6, logo);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, lx + 7, ly, 6, 6, logo);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, lx, ly + 7, 6, 6, logo);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, lx + 7, ly + 7, 6, 6, logo);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, START_X + 32, ty + 9,
             C_TEXT, "Start", -1);
}

static void civil_from_days(i64 z, int *year, int *month, int *day) {
    z += 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    u32 doe = (u32)(z - era * 146097);
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = (i64)yoe + era * 400;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    u32 mp = (5 * doy + 2) / 153;
    u32 d = doy - (153 * mp + 2) / 5 + 1;
    u32 m = mp < 10 ? mp + 3 : mp - 9;
    *year = (int)(y + (m <= 2));
    *month = (int)m;
    *day = (int)d;
}

static void draw_tray(void) {
    int ty = taskbar_item_y();
    int x0 = g_fbw - TRAY_W;

    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x0, work_h() + 8,
             1, TASKBAR_H - 16, 0x223149);

    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x0 + 14, ty + 14, 6, 6, 0x34D399);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x0 + 13, ty + 13, 8, 1, 0x86EFC0);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, x0 + 26, ty + 9, C_TEXT_DIM,
             "AI", -1);

    int nx = x0 + 62, ny = ty + 22;
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, nx, ny - 4, 3, 6, C_TEXT_DIM);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, nx + 5, ny - 8, 3, 10, C_TEXT_DIM);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, nx + 10, ny - 12, 3, 14, 0x5EEAD4);

    mcube_time_t ts;
    if (mc_time(&ts) != 0) return;

    u32 secs = (u32)(ts.epoch % 86400);
    i64 days = (i64)(ts.epoch / 86400);
    int year, month, day;
    civil_from_days(days, &year, &month, &day);

    char clock[16];
    snprintf(clock, sizeof(clock), "%02u:%02u", secs / 3600,
             (secs / 60) % 60);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, g_fbw - 14, ty + 2,
                   C_TEXT, clock);

    char date[16];
    snprintf(date, sizeof(date), "%02d/%02d", month, day);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, g_fbw - 14, ty + 19,
                   C_TEXT_DIM, date);
}

static void draw_taskbar(void) {
    taskbar_layout();

    gfx_vgradient(g_fb, g_stride, g_fbw, g_fbh, 0, work_h(), g_fbw, TASKBAR_H,
                  C_TASKBAR_TOP, C_TASKBAR_BOT);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, 0, work_h(), g_fbw, 1, 0x223149);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, 0, g_fbh - 1, g_fbw, 1, 0x05080E);

    draw_start_button();

    int ty = taskbar_item_y();
    for (int i = 0; i < g_task_n; i++) {
        int slot = g_task_slot[i];
        win_t *w = &g_wins[slot];
        int bx = g_task_x[i], bw = g_task_w[i];

        u32 bg = w->focused ? C_TASKBAR_ON
                            : (g_hover_task == slot ? C_TASKBAR_HOV
                                                    : C_TASKBAR_HI);
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx, ty, bw,
                       TASK_BTN_H, 6, bg);
        if (w->focused) {
            gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx, ty, bw,
                           TASK_BTN_H, 6, lerp_color(bg, C_ACCENT, 70));
            gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx + 1, ty + 1,
                           bw - 2, TASK_BTN_H - 2, 5, bg);
            gfx_fill(g_fb, g_stride, g_fbw, g_fbh, bx + 10,
                     work_h() + TASKBAR_H - 4, bw - 20, 2, C_ACCENT_2);
        }

        draw_icon(g_fb, g_stride, g_fbw, g_fbh, bx + 7, ty + 9, 16,
                  g_icon_colors[slot % (int)(sizeof(g_icon_colors) /
                                             sizeof(g_icon_colors[0]))],
                  w->title);

        int max_chars = (bw - 30) / 8;
        if (max_chars < 0) max_chars = 0;
        gfx_text_clip(g_fb, g_stride, g_fbw, g_fbh, bx + 29, ty + 9,
                      w->minimized ? C_TEXT_DIM : C_TEXT, w->title, max_chars);
    }

    draw_tray();
}

/* ---- start menu ------------------------------------------------------ */

static int menu_items_y(int my) {
    return my + MENU_HEADER_H + 26;
}

static void menu_box(int *mx, int *my, int *mw, int *mh) {
    *mw = MENU_W;
    *mh = MENU_HEADER_H + 26 + APP_COUNT * MENU_ITEM_H + 40;
    *mx = START_X;
    *my = work_h() - *mh - 10;
    if (*my < 8) *my = 8;
}

static int menu_item_at(int x, int y) {
    int mx, my, mw, mh;
    menu_box(&mx, &my, &mw, &mh);
    int y0 = menu_items_y(my);
    if (x < mx + 6 || x >= mx + mw - 6) return -1;
    if (y < y0 || y >= y0 + APP_COUNT * MENU_ITEM_H) return -1;
    return (y - y0) / MENU_ITEM_H;
}

static void draw_menu(void) {
    if (!g_menu_open) return;

    int mx, my, mw, mh;
    menu_box(&mx, &my, &mw, &mh);

    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, mx + 9, my + 10, mw, mh,
                   12, 0x020409);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, mx, my, mw, mh, 10, 0x2A3A55);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, mx + 1, my + 1, mw - 2,
                   mh - 2, 9, C_MENU);

    gfx_round_top_gradient(g_fb, g_stride, g_fbw, g_fbh, mx + 1, my + 1,
                           mw - 2, MENU_HEADER_H, 9, 0x2864C4, 0x12345F);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, mx + 12, my + 1, mw - 24, 1,
             lerp_color(0x2864C4, 0xFFFFFF, 55));

    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, mx + 18, my + 16, 42, 42,
                   21, 0x0F2B58);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, mx + 19, my + 17, 40, 40,
                   20, C_ACCENT);
    gfx_text_scale(g_fb, g_stride, g_fbw, g_fbh, mx + 31, my + 25, 2,
                   0xFFFFFF, "M", 1);

    gfx_text(g_fb, g_stride, g_fbw, g_fbh, mx + 72, my + 15, 0xFFFFFF,
             "MCubeAIOS", -1);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, mx + 72, my + 35,
             0xBFD8FF, "SECURE AI SESSION", -1);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, mx + mw - 16, my + 57,
                   0x8FC2FF, "v0.1.0");

    gfx_text(g_fb, g_stride, g_fbw, g_fbh, mx + 18, my + MENU_HEADER_H + 8,
             C_TEXT_DIM, "ALL APPS", -1);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, mx + mw - 18,
                   my + MENU_HEADER_H + 8, 0x5B7191, "PINNED");

    int y0 = menu_items_y(my);
    for (int i = 0; i < APP_COUNT; i++) {
        int iy = y0 + i * MENU_ITEM_H;
        if (i == g_menu_sel) {
            gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, mx + 8, iy + 1,
                           mw - 16, MENU_ITEM_H - 4, 6, C_MENU_HOVER);
            gfx_fill(g_fb, g_stride, g_fbw, g_fbh, mx + 8, iy + 7,
                     3, MENU_ITEM_H - 16, C_ACCENT_2);
        }
        draw_icon(g_fb, g_stride, g_fbw, g_fbh, mx + 20, iy + 10, 20,
                  g_icon_colors[i % (int)(sizeof(g_icon_colors) /
                                          sizeof(g_icon_colors[0]))],
                  g_app_names[i]);
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, mx + 52, iy + 12,
                 i == g_menu_sel ? 0xFFFFFF : C_TEXT, g_app_names[i], -1);
    }

    int fy = my + mh - 34;
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, mx + 14, fy, mw - 28, 1, 0x23324A);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, mx + 18, fy + 10,
             0x7E90AC, "F1 menu  Alt+Tab  Win+D", -1);
}

/* ---- Alt+Tab switcher ------------------------------------------------ */

static void switcher_build(void) {
    g_switcher_n = 0;
    for (int i = g_zcount - 1; i >= 0 && g_switcher_n < MAX_WINS; i--) {
        int slot = g_zorder[i];
        if (g_wins[slot].used) g_switcher_list[g_switcher_n++] = slot;
    }
}

static void switcher_open(int reverse) {
    switcher_build();
    if (g_switcher_n == 0) return;
    g_switcher_open = 1;
    if (g_switcher_n > 1) g_switcher_sel = reverse ? g_switcher_n - 1 : 1;
    else g_switcher_sel = 0;
    g_menu_open = 0;
    g_need_compose = 1;
}

static void switcher_step(int dir) {
    if (!g_switcher_open || g_switcher_n == 0) return;
    g_switcher_sel = (g_switcher_sel + dir + g_switcher_n) % g_switcher_n;
    g_need_compose = 1;
}

static void switcher_confirm(void) {
    if (!g_switcher_open) return;
    if (g_switcher_n > 0 && g_switcher_sel >= 0 &&
        g_switcher_sel < g_switcher_n) {
        focus_window(g_switcher_list[g_switcher_sel]);
    }
    g_switcher_open = 0;
    g_switcher_n = 0;
    g_need_compose = 1;
}

static void draw_switcher(void) {
    if (!g_switcher_open || g_switcher_n == 0) return;

    gfx_dim(g_fb, g_stride, g_fbw, g_fbh, 0, 0, g_fbw, work_h(), 112);

    int pw = 430;
    if (pw > g_fbw - 40) pw = g_fbw - 40;
    int item_h = 54;
    int ph = 48 + g_switcher_n * item_h + 34;
    int px = (g_fbw - pw) / 2;
    int py = (work_h() - ph) / 2;
    if (py < 8) py = 8;

    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, px + 10, py + 12, pw, ph,
                   14, 0x020409);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, px, py, pw, ph, 12,
                   lerp_color(C_SWITCH, C_ACCENT, 90));
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, px + 1, py + 1, pw - 2,
                   ph - 2, 11, C_SWITCH);
    gfx_round_top_gradient(g_fb, g_stride, g_fbw, g_fbh, px + 1, py + 1,
                           pw - 2, 46, 11, 0x1B3763, 0x101B30);

    gfx_text(g_fb, g_stride, g_fbw, g_fbh, px + 20, py + 15, C_TEXT,
             "SWITCH WINDOWS", -1);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, px + pw - 20, py + 15,
                   0x8FA8CC, "TAB / SHIFT+TAB");

    int y = py + 48;
    for (int i = 0; i < g_switcher_n; i++) {
        int slot = g_switcher_list[i];
        win_t *w = &g_wins[slot];
        if (i == g_switcher_sel) {
            gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, px + 10, y + 3,
                           pw - 20, item_h - 8, 7, C_ACCENT_DARK);
            gfx_fill(g_fb, g_stride, g_fbw, g_fbh, px + 10, y + 11,
                     3, item_h - 24, C_ACCENT_2);
        }
        draw_icon(g_fb, g_stride, g_fbw, g_fbh, px + 22, y + 11, 30,
                  g_icon_colors[slot % (int)(sizeof(g_icon_colors) /
                                             sizeof(g_icon_colors[0]))],
                  w->title);
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, px + 64, y + 18,
                 i == g_switcher_sel ? 0xFFFFFF : C_TEXT, w->title, 40);
        y += item_h;
    }

    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, px + pw - 20,
                   py + ph - 20, 0x5E7699, "RELEASE ALT TO SWITCH");
}

/* ---- composition ----------------------------------------------------- */

static void compose(void) {
    cursor_restore();
    draw_background();
    draw_desktop_icons();
    draw_sys_card();
    for (int i = 0; i < g_zcount; i++) {
        int slot = g_zorder[i];
        win_t *w = &g_wins[slot];
        if (!w->used) continue;
        draw_window(w, slot);
        w->dirty = 0;
    }
    draw_taskbar();
    if (g_menu_open) draw_menu();
    if (g_switcher_open) draw_switcher();
    cursor_draw();

    g_need_compose = 0;
    g_need_taskbar = 0;
    g_need_menu = 0;
    g_sys_dirty = 0;
    g_cursor_moved = 0;
}

/* Repaint just one rectangle of the desktop. The scene is still walked in
 * full z-order, but every write is clipped, so a window drag no longer
 * rewrites the whole framebuffer (and the host window) on every mouse move. */
static void compose_region(int x, int y, int w, int h) {
    if (g_menu_open || g_switcher_open) {
        compose();
        return;
    }
    int tb = g_need_taskbar;
    int menu = g_need_menu;
    int sys = g_sys_dirty;
    clip_set(x, y, w, h);
    compose();
    clip_reset();
    /* compose() clears the pending flags, but clipped draws do not cover
     * updates outside the region (taskbar clock, status card). */
    g_need_taskbar = tb;
    g_need_menu = menu;
    g_sys_dirty = sys;
}

static int window_is_covered(int slot) {
    int fx, fy, fw, fh;
    frame_box(&g_wins[slot], &fx, &fy, &fw, &fh);

    /* Every entry visited before hitting `slot` is stacked above it. */
    for (int i = g_zcount - 1; i >= 0; i--) {
        int above = g_zorder[i];
        if (above == slot) break;
        win_t *w = &g_wins[above];
        if (!w->used || w->minimized) continue;
        int ax, ay, aw, ah;
        frame_box(w, &ax, &ay, &aw, &ah);
        if (rects_intersect(fx, fy, fw, fh, ax, ay, aw, ah)) return 1;
    }
    return 0;
}

static void flush_dirty(void) {
    if (g_menu_open || g_switcher_open) return;

    int any = 0;
    for (int i = 0; i < g_zcount; i++) {
        int slot = g_zorder[i];
        win_t *w = &g_wins[slot];
        if (!w->used || !w->dirty || w->minimized) continue;
        if (window_is_covered(slot)) continue;
        if (!any) {
            cursor_restore();
            any = 1;
        }
        blit_surface(w);
        w->dirty = 0;
    }
    if (any) cursor_draw();
}

static void redraw_taskbar(void) {
    cursor_restore();
    draw_taskbar();
    cursor_draw();
}

static void redraw_menu(void) {
    if (!g_menu_open) return;
    cursor_restore();
    draw_menu();
    cursor_draw();
}

/* ---- application lifecycle ------------------------------------------ */

static int free_slot(void) {
    for (int i = 0; i < MAX_WINS; i++) if (!g_wins[i].used) return i;
    return -1;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void spawn_app(const char *spec) {
    if (!spec || !*spec) return;

    char path[128];
    char arg[128];
    int has_arg = 0;

    const char *space = strchr(spec, ' ');
    usize plen = space ? (usize)(space - spec) : strlen(spec);
    if (plen >= sizeof(path)) plen = sizeof(path) - 1;
    memcpy(path, spec, plen);
    path[plen] = 0;
    if (space && space[1]) {
        strncpy(arg, space + 1, sizeof(arg) - 1);
        arg[sizeof(arg) - 1] = 0;
        has_arg = 1;
    }

    int slot = free_slot();
    if (slot < 0) return;

    int cmd[2] = { -1, -1 };
    int evt[2] = { -1, -1 };
    if (pipe(cmd) != 0) return;
    if (pipe(evt) != 0) {
        close(cmd[0]);
        close(cmd[1]);
        return;
    }

    char *argv[3];
    argv[0] = path;
    argv[1] = has_arg ? arg : NULL;
    argv[2] = NULL;

    /* spawn() creates a fresh address space and installs the two pipe ends as
     * fd 3/4, so the desktop heap is never copied. */
    pid_t pid = spawn(path, argv, cmd[1], evt[0]);
    close(cmd[1]);
    close(evt[0]);

    if (pid < 0) {
        close(cmd[0]);
        close(evt[1]);
        return;
    }

    win_t *w = &g_wins[slot];
    memset(w, 0, sizeof(*w));
    w->used = 1;
    w->pid = pid;
    w->cmd_fd = cmd[0];
    w->evt_fd = evt[1];
    w->w = 640;
    w->h = 400;

    int step = g_spawn_seq++ % 7;
    w->x = 70 + step * 36;
    w->y = 60 + step * 32;
    clamp_win_rect(&w->x, &w->y, &w->w, &w->h);
    surface_ensure(w);

    strncpy(w->title, base_name(path), sizeof(w->title) - 1);
    w->title[sizeof(w->title) - 1] = 0;
    w->ready = 0;
    w->dirty = 1;

    z_push(slot);
    focus_window(slot);
}

/* ---- commands from applications ------------------------------------- */

static int read_cmd(win_t *w, gui_cmd_t *cmd) {
    u8 *p = (u8 *)cmd;
    size_t left = sizeof(*cmd);
    while (left) {
        ssize_t n = read(w->cmd_fd, p, left);
        if (n > 0) {
            p += n;
            left -= (size_t)n;
            continue;
        }
        if (n == 0) return -1;
        if (errno != EAGAIN && errno != EINTR) return -1;
        sleep_ms(1);
    }
    return 0;
}

static void set_title_from_cmd(win_t *w, const gui_cmd_t *cmd) {
    u32 len = cmd->len;
    if (len >= sizeof(w->title)) len = sizeof(w->title) - 1;
    memcpy(w->title, cmd->text, len);
    w->title[len] = 0;
}

static void resize_reply(int slot) {
    win_t *w = &g_wins[slot];
    surface_ensure(w);
    if (!w->surface || w->sw < w->w || w->sh < w->h) return;
    send_event(slot, GUI_EVT_RESIZE, 0, 0, 0, 0, 0, 0, w->w, w->h);
}

static void handle_cmd(int slot, const gui_cmd_t *cmd) {
    win_t *w = &g_wins[slot];
    if (!w->used) return;

    switch (cmd->type) {
    case GUI_CMD_INIT: {
        set_title_from_cmd(w, cmd);
        int nw = cmd->x > 0 ? cmd->x : w->w;
        int nh = cmd->y > 0 ? cmd->y : w->h;
        clamp_win_rect(&w->x, &w->y, &nw, &nh);
        w->w = nw;
        w->h = nh;
        resize_reply(slot);
        if (!w->ready) {
            w->ready = 1;
            g_ready_count++;
        }
        g_need_compose = 1;
        break;
    }

    case GUI_CMD_CLEAR:
        if (w->surface) {
            gfx_fill(w->surface, w->sw, w->sw, w->sh, 0, 0, w->sw, w->sh,
                     cmd->color);
            w->dirty = 1;
        }
        break;

    case GUI_CMD_FILL:
        if (w->surface) {
            gfx_fill(w->surface, w->sw, w->sw, w->sh,
                     cmd->x, cmd->y, cmd->w, cmd->h, cmd->color);
            w->dirty = 1;
        }
        break;

    case GUI_CMD_RECT:
        if (w->surface) {
            gfx_rect(w->surface, w->sw, w->sw, w->sh,
                     cmd->x, cmd->y, cmd->w, cmd->h, cmd->color);
            w->dirty = 1;
        }
        break;

    case GUI_CMD_TEXT: {
        if (!w->surface) break;
        char tmp[GUI_TEXT_MAX + 1];
        u32 len = cmd->len;
        if (len > GUI_TEXT_MAX) len = GUI_TEXT_MAX;
        memcpy(tmp, cmd->text, len);
        tmp[len] = 0;
        gfx_text(w->surface, w->sw, w->sw, w->sh,
                 cmd->x, cmd->y, cmd->color, tmp, (int)len);
        w->dirty = 1;
        break;
    }

    case GUI_CMD_FLUSH:
        w->dirty = 1;
        if (!w->flushed) {
            w->flushed = 1;
            g_flushed_count++;
        }
        break;

    case GUI_CMD_TITLE:
        set_title_from_cmd(w, cmd);
        g_need_compose = 1;
        break;

    case GUI_CMD_CLOSE:
        close_window(slot);
        break;

    case GUI_CMD_LAUNCH: {
        char path[GUI_TEXT_MAX + 1];
        u32 len = cmd->len;
        if (len > GUI_TEXT_MAX) len = GUI_TEXT_MAX;
        memcpy(path, cmd->text, len);
        path[len] = 0;
        spawn_app(path);
        break;
    }

    case GUI_CMD_KILL:
        if (cmd->x > 0) kill((pid_t)cmd->x, 15);
        break;

    default:
        break;
    }
}

/* ---- window interactions -------------------------------------------- */

static int window_at(int x, int y) {
    for (int i = g_zcount - 1; i >= 0; i--) {
        int slot = g_zorder[i];
        win_t *w = &g_wins[slot];
        if (!w->used || w->minimized) continue;
        int fx, fy, fw, fh;
        frame_box(w, &fx, &fy, &fw, &fh);
        if (inside(x, y, fx, fy, fw, fh)) return slot;
    }
    return -1;
}

static int title_button_at(const win_t *w, int x, int y) {
    int fx, fy, fw, fh;
    frame_box(w, &fx, &fy, &fw, &fh);
    if (!inside(x, y, fx + 1, fy + 1, fw - 2, TITLE_H)) return 0;
    for (int btn = BTN_MIN; btn <= BTN_CLOSE; btn++) {
        int bx, by, bw, bh;
        button_box(w, btn, &bx, &by, &bw, &bh);
        if (inside(x, y, bx, by, bw, bh)) return btn;
    }
    return 0;
}

static int resize_zone(const win_t *w, int x, int y) {
    if (w->maximized) return 0;
    int fx, fy, fw, fh;
    frame_box(w, &fx, &fy, &fw, &fh);
    if (!inside(x, y, fx - 2, fy - 2, fw + 4, fh + 4)) return 0;

    int zone = 0;
    if (x >= fx - 2 && x < fx + 5) zone |= RZ_LEFT;
    else if (x <= fx + fw + 1 && x >= fx + fw - 5) zone |= RZ_RIGHT;
    if (y >= fy - 2 && y < fy + 5) zone |= RZ_TOP;
    else if (y <= fy + fh + 1 && y >= fy + fh - 5) zone |= RZ_BOTTOM;
    return zone;
}

static void maximize_toggle(int slot) {
    win_t *w = &g_wins[slot];
    if (!w->used) return;

    if (!w->maximized) {
        w->rx = w->x;
        w->ry = w->y;
        w->rw = w->w;
        w->rh = w->h;
        w->maximized = 1;
        w->x = WIN_BORDER;
        w->y = TITLE_H + WIN_BORDER;
        w->w = g_fbw - 2 * WIN_BORDER;
        w->h = work_h() - TITLE_H - 2 * WIN_BORDER;
    } else {
        w->maximized = 0;
        w->x = w->rx;
        w->y = w->ry;
        w->w = w->rw;
        w->h = w->rh;
    }
    clamp_win_rect(&w->x, &w->y, &w->w, &w->h);
    resize_reply(slot);
    focus_window(slot);
    g_need_compose = 1;
}

static void start_drag(int slot, int x, int y) {
    win_t *w = &g_wins[slot];
    int expanded = 0;
    if (w->maximized) {
        int rw = w->rw, rh = w->rh;
        w->maximized = 0;
        w->w = rw;
        w->h = rh;
        w->x = x - rw / 2;
        w->y = y - TITLE_H - WIN_BORDER;
        clamp_win_rect(&w->x, &w->y, &w->w, &w->h);
        resize_reply(slot);
        expanded = 1;
    }
    g_drag_slot = slot;
    g_drag_dx = x - w->x;
    g_drag_dy = y - w->y;
    g_press_win = -1;
    /* Unmaximizing changes far more than the window rectangle; otherwise the
     * press-time focus/stacking compose() already covers this. */
    if (expanded) g_need_compose = 1;
}

static void drag_move(int x, int y) {
    win_t *w = &g_wins[g_drag_slot];
    if (g_drag_slot < 0 || !w->used) { g_drag_slot = -1; return; }
    dirty_frame(w);
    w->x = clamp_int(x - g_drag_dx, WIN_BORDER,
                     g_fbw - w->w - WIN_BORDER);
    w->y = clamp_int(y - g_drag_dy, TITLE_H + WIN_BORDER,
                     work_h() - w->h - WIN_BORDER);
    dirty_frame(w);
}

static void start_resize(int slot, int zone, int x, int y) {
    win_t *w = &g_wins[slot];
    g_resize_slot = slot;
    g_resize_zone = zone;
    g_resize_x0 = w->x;
    g_resize_y0 = w->y;
    g_resize_w0 = w->w;
    g_resize_h0 = w->h;
    g_resize_mx0 = x;
    g_resize_my0 = y;
    g_press_win = -1;
}

static void resize_move(int x, int y) {
    int slot = g_resize_slot;
    if (slot < 0 || !g_wins[slot].used) { g_resize_slot = -1; return; }
    win_t *w = &g_wins[slot];

    int dx = x - g_resize_mx0;
    int dy = y - g_resize_my0;

    int left = g_resize_x0 - WIN_BORDER;
    int top = g_resize_y0 - TITLE_H - WIN_BORDER;
    int right = left + g_resize_w0 + 2 * WIN_BORDER;
    int bottom = top + g_resize_h0 + TITLE_H + 2 * WIN_BORDER;

    if (g_resize_zone & RZ_LEFT) left += dx;
    if (g_resize_zone & RZ_RIGHT) right += dx;
    if (g_resize_zone & RZ_TOP) top += dy;
    if (g_resize_zone & RZ_BOTTOM) bottom += dy;

    int minw = MIN_W + 2 * WIN_BORDER;
    int minh = MIN_H + TITLE_H + 2 * WIN_BORDER;
    if (right - left < minw) {
        if (g_resize_zone & RZ_LEFT) left = right - minw;
        else right = left + minw;
    }
    if (bottom - top < minh) {
        if (g_resize_zone & RZ_TOP) top = bottom - minh;
        else bottom = top + minh;
    }

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > g_fbw) right = g_fbw;
    if (bottom > work_h()) bottom = work_h();

    dirty_frame(w);
    w->x = left + WIN_BORDER;
    w->y = top + TITLE_H + WIN_BORDER;
    w->w = right - left - 2 * WIN_BORDER;
    w->h = bottom - top - TITLE_H - 2 * WIN_BORDER;
    dirty_frame(w);
}

static void finish_resize(int slot) {
    if (slot < 0 || slot >= MAX_WINS || !g_wins[slot].used) return;
    resize_reply(slot);
    dirty_frame(&g_wins[slot]);
}

/* ---- taskbar and menu actions --------------------------------------- */

static int taskbar_hit(int x, int y) {
    if (y < work_h() || y >= g_fbh) return -1;
    int ty = taskbar_item_y();
    if (inside(x, y, START_X, ty, START_W, START_H)) return -2;

    taskbar_layout();
    for (int i = 0; i < g_task_n; i++) {
        if (inside(x, y, g_task_x[i], ty, g_task_w[i], TASK_BTN_H)) {
            return g_task_slot[i];
        }
    }
    return -1;
}

static void toggle_menu(void) {
    g_menu_open = !g_menu_open;
    if (g_menu_open) g_menu_sel = 0;
    g_need_compose = 1;
}

static void launch_selected(void) {
    if (g_menu_sel < 0) g_menu_sel = 0;
    if (g_menu_sel >= APP_COUNT) g_menu_sel = APP_COUNT - 1;
    int idx = g_menu_sel;
    g_menu_open = 0;
    spawn_app(g_app_paths[idx]);
    g_need_compose = 1;
}

static void taskbar_click(int slot) {
    win_t *w = &g_wins[slot];
    if (!w->used) return;
    if (!w->minimized && g_focus == slot) minimize_window(slot);
    else focus_window(slot);
}

static void show_desktop(void) {
    int any_visible = 0;
    for (int i = 0; i < MAX_WINS; i++) {
        if (g_wins[i].used && !g_wins[i].minimized) any_visible = 1;
    }

    if (any_visible) {
        for (int i = 0; i < MAX_WINS; i++) {
            win_t *w = &g_wins[i];
            if (!w->used || w->minimized) continue;
            w->minimized = 1;
            w->desk_hidden = 1;
        }
        g_focus = -1;
        broadcast_focus();
    } else {
        for (int i = 0; i < MAX_WINS; i++) {
            win_t *w = &g_wins[i];
            if (w->used && w->desk_hidden) {
                w->minimized = 0;
                w->desk_hidden = 0;
            }
        }
        choose_focus();
    }
    g_menu_open = 0;
    g_need_compose = 1;
}

/* ---- input ----------------------------------------------------------- */

static u64 now_ms(void) {
    mcube_time_t ts;
    return mc_time(&ts) == 0 ? ts.uptime_ms : 0;
}

static void update_hover(void) {
    int new_task = taskbar_hit(g_mx, g_my);
    int new_slot = -1, new_btn = 0;

    int new_desk = -1;
    if (!g_menu_open && !g_switcher_open && g_my < work_h() &&
        window_at(g_mx, g_my) < 0) {
        new_desk = desktop_icon_at(g_mx, g_my);
    }
    if (new_desk != g_desk_hover) {
        g_desk_hover = new_desk;
        g_need_compose = 1;
    }

    if (g_menu_open) {
        int idx = menu_item_at(g_mx, g_my);
        if (idx >= 0 && idx < APP_COUNT && idx != g_menu_sel) {
            g_menu_sel = idx;
            g_need_menu = 1;
        }
    }

    int slot = window_at(g_mx, g_my);
    if (slot >= 0) {
        int btn = title_button_at(&g_wins[slot], g_mx, g_my);
        if (btn) {
            new_slot = slot;
            new_btn = btn;
        }
    }

    if (new_task != g_hover_task) {
        g_hover_task = new_task;
        g_need_taskbar = 1;
    }
    if (new_slot != g_hover_slot || new_btn != g_hover_btn) {
        g_hover_slot = new_slot;
        g_hover_btn = new_btn;
        g_need_compose = 1;
    }
}

static void mouse_press(int x, int y) {
    if (g_switcher_open) {
        g_switcher_open = 0;
        g_need_compose = 1;
        return;
    }

    if (g_menu_open) {
        if (inside(x, y, START_X, taskbar_item_y(), START_W, START_H)) {
            g_menu_open = 0;
            g_need_compose = 1;
            return;
        }
        int idx = menu_item_at(x, y);
        if (idx >= 0 && idx < APP_COUNT) {
            g_menu_sel = idx;
            launch_selected();
            return;
        }
        int mx, my, mw, mh;
        menu_box(&mx, &my, &mw, &mh);
        if (inside(x, y, mx, my, mw, mh)) return;
        g_menu_open = 0;
        g_need_compose = 1;
    }

    int task = taskbar_hit(x, y);
    if (task == -2) {
        toggle_menu();
        return;
    }
    if (task >= 0) {
        taskbar_click(task);
        return;
    }
    if (y >= work_h()) return;

    for (int i = g_zcount - 1; i >= 0; i--) {
        int slot = g_zorder[i];
        win_t *w = &g_wins[slot];
        if (!w->used || w->minimized) continue;

        int fx, fy, fw, fh;
        frame_box(w, &fx, &fy, &fw, &fh);
        if (!inside(x, y, fx, fy, fw, fh)) continue;

        focus_window(slot);

        if (inside(x, y, fx + 1, fy + 1, fw - 2, TITLE_H)) {
            int btn = title_button_at(w, x, y);
            if (btn == BTN_CLOSE) {
                close_window(slot);
                return;
            }
            if (btn == BTN_MIN) {
                minimize_window(slot);
                return;
            }
            if (btn == BTN_MAX) {
                g_last_click_slot = -1;
                maximize_toggle(slot);
                return;
            }

            u64 now = now_ms();
            if (g_last_click_slot == slot && now >= g_last_click_ms &&
                now - g_last_click_ms < 420) {
                g_last_click_slot = -1;
                maximize_toggle(slot);
                return;
            }
            g_last_click_ms = now;
            g_last_click_slot = slot;
            start_drag(slot, x, y);
            return;
        }

        int zone = resize_zone(w, x, y);
        if (zone) {
            start_resize(slot, zone, x, y);
            return;
        }

        if (inside(x, y, w->x, w->y, w->w, w->h)) {
            g_last_click_slot = -1;
            g_press_win = slot;
            send_event(slot, GUI_EVT_MOUSE, g_buttons, x - w->x, y - w->y,
                       0, 0, g_buttons, 0, 0);
            return;
        }
        return;
    }

    int di = desktop_icon_at(x, y);
    if (di >= 0) {
        u64 now = now_ms();
        int dbl = g_desk_last_idx == di && now >= g_desk_last_ms &&
                  now - g_desk_last_ms < 420;
        g_desk_sel = di;
        g_desk_last_idx = di;
        g_desk_last_ms = now;
        g_last_click_slot = -1;
        if (dbl) {
            g_desk_last_idx = -1;
            spawn_app(g_desk_apps[di].path);
        } else {
            g_need_compose = 1;
        }
        return;
    }

    if (g_desk_sel != -1) {
        g_desk_sel = -1;
        g_need_compose = 1;
    }
    g_focus = -1;
    broadcast_focus();
    g_need_compose = 1;
}

static void mouse_release(int x, int y) {
    int slot = g_press_win;
    g_press_win = -1;
    if (slot < 0 || slot >= MAX_WINS || !g_wins[slot].used) return;
    win_t *w = &g_wins[slot];
    send_event(slot, GUI_EVT_MOUSE, g_buttons, x - w->x, y - w->y,
               0, 0, g_buttons, 0, 0);
}

static void cycle_focus(void) {
    int start = g_focus < 0 ? 0 : g_focus;
    for (int n = 1; n <= MAX_WINS; n++) {
        int i = (start + n) % MAX_WINS;
        if (g_wins[i].used && !g_wins[i].minimized) {
            focus_window(i);
            return;
        }
    }
}

static void handle_key(u32 raw) {
    int down = !(raw & INPUT_KEY_RELEASE);
    u32 code = raw & ~INPUT_KEY_RELEASE;

    switch (code) {
    case INPUT_KEY_LSHIFT:
    case INPUT_KEY_RSHIFT:
        if (down) g_mods |= MOD_SHIFT;
        else g_mods &= ~MOD_SHIFT;
        return;
    case INPUT_KEY_LCTRL:
    case INPUT_KEY_RCTRL:
        if (down) g_mods |= MOD_CTRL;
        else g_mods &= ~MOD_CTRL;
        return;
    case INPUT_KEY_LALT:
    case INPUT_KEY_RALT: {
        int was_down = (g_mods & MOD_ALT) != 0;
        if (down) g_mods |= MOD_ALT;
        else g_mods &= ~MOD_ALT;
        if (!down && was_down && g_switcher_open) switcher_confirm();
        return;
    }
    case INPUT_KEY_LWIN:
    case INPUT_KEY_RWIN: {
        int was_down = (g_mods & MOD_WIN) != 0;
        if (down) g_mods |= MOD_WIN;
        else g_mods &= ~MOD_WIN;
        if (down && !was_down) toggle_menu();
        return;
    }
    default:
        break;
    }

    if (!down) return;

    if (g_switcher_open) {
        if (code == '\t') {
            switcher_step((g_mods & MOD_SHIFT) ? -1 : 1);
        } else if (code == 27) {
            g_switcher_open = 0;
            g_need_compose = 1;
        }
        return;
    }

    if ((g_mods & MOD_ALT) && code == '\t') {
        switcher_open((g_mods & MOD_SHIFT) != 0);
        return;
    }
    if ((g_mods & MOD_ALT) && code == INPUT_KEY_F4) {
        if (g_focus >= 0 && g_wins[g_focus].used) close_window(g_focus);
        return;
    }
    if ((g_mods & MOD_WIN) && (code == 'd' || code == 'D')) {
        show_desktop();
        return;
    }

    if (code == INPUT_KEY_F1) {
        toggle_menu();
        return;
    }

    if (g_menu_open) {
        if (code == INPUT_KEY_UP) {
            g_menu_sel = (g_menu_sel + APP_COUNT - 1) % APP_COUNT;
            g_need_menu = 1;
        } else if (code == INPUT_KEY_DOWN) {
            g_menu_sel = (g_menu_sel + 1) % APP_COUNT;
            g_need_menu = 1;
        } else if (code == INPUT_KEY_HOME) {
            g_menu_sel = 0;
            g_need_menu = 1;
        } else if (code == INPUT_KEY_END) {
            g_menu_sel = APP_COUNT - 1;
            g_need_menu = 1;
        } else if (code == '\n') {
            launch_selected();
        } else if (code == 27) {
            g_menu_open = 0;
            g_need_compose = 1;
        }
        return;
    }

    if (code == INPUT_KEY_F9) {
        cycle_focus();
        return;
    }

    if (g_mods & (MOD_ALT | MOD_CTRL | MOD_WIN)) return;

    if (g_focus >= 0 && g_focus < MAX_WINS && g_wins[g_focus].used &&
        !g_wins[g_focus].minimized) {
        send_event(g_focus, GUI_EVT_KEY, code, 0, 0, 0, 0, 0, 0, 0);
    }
}

static void handle_input(const input_event_t *ev) {
    if (ev->type == INPUT_MOUSE) {
        int moved = ev->x != g_mx || ev->y != g_my;
        g_mx = ev->x;
        g_my = ev->y;

        u32 pressed = ev->buttons & ~g_buttons;
        u32 released = g_buttons & ~ev->buttons;
        g_buttons = ev->buttons;

        if (g_drag_slot >= 0) {
            if (released & 1) {
                dirty_frame(&g_wins[g_drag_slot]);
                g_drag_slot = -1;
            } else if (moved) {
                drag_move(g_mx, g_my);
            }
        } else if (g_resize_slot >= 0) {
            if (released & 1) {
                int slot = g_resize_slot;
                g_resize_slot = -1;
                finish_resize(slot);
            } else if (moved) {
                resize_move(g_mx, g_my);
            }
        } else {
            if (moved) update_hover();
            if (pressed & 1) mouse_press(g_mx, g_my);
            if (released & 1) mouse_release(g_mx, g_my);
        }

        if (moved) g_cursor_moved = 1;
        return;
    }

    if (ev->type == INPUT_KEY) handle_key(ev->code);
}

/* ---- main loop ------------------------------------------------------- */

static u32 bg_next(u32 *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void init_background(void) {
    int w = g_fbw, h = g_fbh;
    g_bg = (u32 *)malloc((size_t)w * (size_t)h * 4);
    if (!g_bg) return;

    i64 glow_x = w * 3 / 4, glow_y = h / 5;
    i64 glow_r2 = (i64)(w / 2) * (w / 2);
    i64 vig_r2 = (i64)(w / 2) * (w / 2) + (i64)(h / 2) * (h / 2);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            u32 c = lerp_color(C_DESK_TOP, C_DESK_BOTTOM,
                               y * 255 / (h > 1 ? h - 1 : 1));

            if ((x & 63) == 0 || (y & 63) == 0) {
                c = lerp_color(c, 0x1B304C, 38);
            }

            i64 dx = x - glow_x, dy = y - glow_y;
            i64 d2 = dx * dx + dy * dy;
            if (d2 < glow_r2) {
                int f = (int)((glow_r2 - d2) * 120 / glow_r2);
                c = lerp_color(c, 0x17405F, f);
            }

            i64 vx = x - w / 2, vy = y - h / 2;
            int vf = (int)((vx * vx + vy * vy) * 130 / vig_r2);
            if (vf > 255) vf = 255;
            c = lerp_color(c, 0x02040A, vf);

            g_bg[(u64)y * w + x] = pixel(c);
        }
    }

    /* Faint binary columns. */
    u32 seed = 0xC0FFEE11u;
    for (int col = 0; col < 46; col++) {
        int x = 4 + (int)(bg_next(&seed) % (u32)(w - 16));
        int y = (int)(bg_next(&seed) % (u32)(h / 2));
        int len = 3 + (int)(bg_next(&seed) % 9u);
        for (int k = 0; k < len; k++) {
            char s[2];
            s[0] = (bg_next(&seed) & 1) ? '1' : '0';
            s[1] = 0;
            gfx_text(g_bg, w, w, h, x, y + k * 16, 0x0D1E33, s, 1);
        }
    }

    /* Neural mesh in the upper right; very low contrast. */
    enum { NN = 14 };
    int nx[NN], ny[NN];
    for (int i = 0; i < NN; i++) {
        nx[i] = w * 45 / 100 + (int)(bg_next(&seed) % (u32)(w * 50 / 100));
        ny[i] = 60 + (int)(bg_next(&seed) % (u32)(h - 220));
    }
    for (int i = 0; i < NN; i++) {
        for (int j = i + 1; j < NN; j++) {
            int dx = nx[i] - nx[j], dy = ny[i] - ny[j];
            if (dx * dx + dy * dy < 240 * 240 && ((i + j) & 3) == 0) {
                gfx_line(g_bg, w, w, h, nx[i], ny[i], nx[j], ny[j],
                         0x102A44);
            }
        }
    }
    for (int i = 0; i < NN; i++) {
        gfx_fill(g_bg, w, w, h, nx[i] - 3, ny[i] - 3, 7, 7, 0x0A1B2E);
        gfx_fill(g_bg, w, w, h, nx[i] - 1, ny[i] - 1, 3, 3,
                 (i % 5 == 0) ? 0x2A7FA8 : 0x1B4566);
    }

    int wm_y = work_h() - 150;
    if (wm_y < 40) wm_y = 40;
    gfx_text_scale(g_bg, w, w, h, w - 208, wm_y, 4, 0x0D1A2B, "MCUBE", -1);
    gfx_text_scale(g_bg, w, w, h, w - 176, wm_y + 70, 4, 0x0A1523, "AIOS", -1);
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    mcube_fb_info_t info;
    if (fb_info(&info) != 0 || info.width == 0 || info.height == 0) {
        printf("USER_DESKTOP_FAIL: no framebuffer\n");
        return 1;
    }

    g_fb = (u32 *)fb_map();
    if (!g_fb) {
        printf("USER_DESKTOP_FAIL: fb_map\n");
        return 1;
    }

    g_fbw = (int)info.width;
    g_fbh = (int)info.height;
    g_stride = (int)(info.pitch / 4);
    g_fmt = (int)info.format;
    g_mx = g_fbw / 2;
    g_my = g_fbh / 2;
    clip_reset();

    printf("USER_DESKTOP_OK: %dx%d %s\n", g_fbw, g_fbh,
           g_fmt == 1 ? "BGRA" : "RGBA");

    for (int i = 0; i < MAX_WINS; i++) {
        g_wins[i].cmd_fd = -1;
        g_wins[i].evt_fd = -1;
    }

    init_background();

    mcube_time_t boot_ts;
    if (mc_time(&boot_ts) == 0) {
        g_sys_uptime_s = boot_ts.uptime_ms / 1000;
        g_sys_second = (int)g_sys_uptime_s;
    }
    mcube_task_info_t boot_tasks[24];
    int boot_procs = taskinfo(boot_tasks, 24);
    g_sys_procs = boot_procs > 0 ? boot_procs : 0;

    compose();

    /* Boot with a useful set of windows so every subsystem is exercised.
     * The editor is handed the sample file installed on the ESP. */
    spawn_app("/bin/terminal.lgy");
    spawn_app("/bin/files.lgy");
    spawn_app("/bin/editor.lgy /TEST.TXT");
    spawn_app("/bin/calc.lgy");
    spawn_app("/bin/image.lgy");
    spawn_app("/bin/settings.lgy");
    spawn_app("/bin/taskman.lgy");

    for (;;) {
        mcube_pollfd_t pfds[MAX_WINS];
        int slots[MAX_WINS];
        int nfds = 0;

        for (int i = 0; i < MAX_WINS; i++) {
            if (!g_wins[i].used) continue;
            pfds[nfds].fd = g_wins[i].cmd_fd;
            pfds[nfds].events = MCUBE_POLLIN;
            pfds[nfds].revents = 0;
            slots[nfds] = i;
            nfds++;
        }

        int ready = poll(pfds, nfds, 16);
        if (ready < 0) ready = 0;
        if (nfds == 0) sleep_ms(16);

        input_event_t iev;
        while (input_poll(&iev) == 1) handle_input(&iev);

        for (int i = 0; i < nfds; i++) {
            if (!(pfds[i].revents & MCUBE_POLLIN)) continue;
            int slot = slots[i];

            /* Drain a burst of commands: a full redraw can be hundreds of
             * primitives and waiting one per frame would be visibly slow. */
            for (int burst = 0; burst < 256; burst++) {
                if (slot < 0 || slot >= MAX_WINS || !g_wins[slot].used) break;

                mcube_pollfd_t probe;
                probe.fd = g_wins[slot].cmd_fd;
                probe.events = MCUBE_POLLIN;
                probe.revents = 0;
                if (poll(&probe, 1, 0) <= 0) break;

                gui_cmd_t cmd;
                if (read_cmd(&g_wins[slot], &cmd) != 0) {
                    close_window(slot);
                    break;
                }
                handle_cmd(slot, &cmd);
            }
        }

        mcube_time_t ts;
        if (mc_time(&ts) == 0 && ts.uptime_ms / 1000 != g_last_clock) {
            g_last_clock = ts.uptime_ms / 1000;
            g_sys_uptime_s = g_last_clock;
            g_sys_second = (int)g_last_clock;
            mcube_task_info_t tasks[24];
            int ntasks = taskinfo(tasks, 24);
            g_sys_procs = ntasks > 0 ? ntasks : 0;
            g_sys_dirty = 1;
            g_need_taskbar = 1;
            for (int i = 0; i < MAX_WINS; i++) {
                if (g_wins[i].used) {
                    send_event(i, GUI_EVT_TICK, (u32)ts.epoch,
                               0, 0, 0, 0, 0, 0, 0);
                }
            }
        }

        if (g_need_compose) {
            compose();
        } else {
            int did = 0;
            if (g_dirty_valid) {
                compose_region(g_dirty_x0, g_dirty_y0,
                               g_dirty_x1 - g_dirty_x0,
                               g_dirty_y1 - g_dirty_y0);
                g_dirty_valid = 0;
                did = 1;
            }
            int tb = g_need_taskbar;
            int menu = g_need_menu;
            if (tb) redraw_taskbar();
            if (menu) redraw_menu();
            int card = 0;
            if (g_sys_dirty) {
                refresh_sys_card();
                g_sys_dirty = 0;
                card = 1;
            }
            if (g_cursor_moved && !did && !tb && !menu && !card) {
                cursor_draw();
            }
        }
        g_need_taskbar = 0;
        g_need_menu = 0;
        g_cursor_moved = 0;

        flush_dirty();

        if (!g_gui_ok_printed && g_ready_count >= BOOT_APP_COUNT &&
            g_flushed_count >= BOOT_APP_COUNT) {
            g_gui_ok_printed = 1;
            printf("USER_GUI_OK: %d windows ready\n", g_ready_count);
        }
    }
}
