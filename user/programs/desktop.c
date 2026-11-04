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

/* Anti-aliased UI typeface (8-bit coverage, regular + bold). Pulled in directly
 * so the data lands in the shell only, not in every user program; user/lib/
 * font.c uses the same trick for the 1 bpp console font. */
#include "../../kernel/font/ui_font.c"

#define MAX_WINS      10
#define TASKBAR_H     48
#define TITLE_H       28
#define WIN_BORDER    1
#define MIN_W         220
#define MIN_H         140
#define CORNER_R      8

/* Windows 11 style taskbar: square buttons in a centred cluster, clock and
 * tray pinned to the right edge. */
#define TRAY_W        236
#define TRAY_CLOCK_W  108
#define TB_BTN        40
#define TB_GAP        2
#define TB_ICON       22

#define MENU_W        300
#define MENU_HEADER_H 76
#define MENU_ITEM_H   40
#define MENU_PAD      8

#define CURSOR_W      16
#define CURSOR_H      16

/* Cursor shapes. The arrow keeps its hot spot at the tip; the resize cursors
 * are centred so they point at the exact pixel under the pointer. */
enum {
    CUR_ARROW = 0,
    CUR_SIZE_WE,
    CUR_SIZE_NS,
    CUR_SIZE_NWSE,
    CUR_SIZE_NESW,
};

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

/* Live accent colours: the Personalisation page writes them to THEME.CFG and
 * the shell re-reads that file, so the choice is a real system setting rather
 * than a per-window preview. */
static u32 g_accent = C_ACCENT;
static u32 g_accent_2 = C_ACCENT_2;
static u32 g_accent_dark = C_ACCENT_DARK;
static int g_wallpaper;
static int g_accent_idx;

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

/* Wallpaper palettes. Index 0 is the shipping default. */
typedef struct {
    u32 top, bottom, grid, glow;
} wallpaper_t;

static const wallpaper_t g_walls[4] = {
    { 0x05080F, 0x0C1728, 0x1B304C, 0x17405F },   /* Nebula  */
    { 0x03100C, 0x08202A, 0x14403C, 0x0E554E },   /* Aurora  */
    { 0x120A06, 0x231209, 0x462A16, 0x63300F },   /* Ember   */
    { 0x0A0C10, 0x131820, 0x232A34, 0x141A22 },   /* Slate   */
};
static u32 g_wall_top = C_DESK_TOP;
static u32 g_wall_bot = C_DESK_BOTTOM;
static u32 g_title_1 = C_TITLE_ON_1;
static u32 g_title_2 = C_TITLE_ON_2;

/* Options the shell honours itself, mirrored from the Settings app through
 * /CONFIG.CFG: 12/24-hour clock and high contrast chrome. They are polled
 * once a second, so flipping the switch changes the desktop without a restart. */
static int g_cfg_24h = 1;
static int g_cfg_contrast;

#define MOD_SHIFT 0x01u
#define MOD_CTRL  0x02u
#define MOD_ALT   0x04u
#define MOD_WIN   0x08u

enum { BTN_MIN = 1, BTN_MAX = 2, BTN_CLOSE = 3 };
enum { RZ_LEFT = 1, RZ_RIGHT = 2, RZ_TOP = 4, RZ_BOTTOM = 8 };

typedef struct {
    int used;
    int w, h;
    u32 *px;               /* GUI_IMG_MAX_PX pixels, allocated on first upload */
} win_img_t;

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
    win_img_t imgs[GUI_IMG_SLOTS];
    int dirty;
    int flushed;
    int focused;
    int minimized;
    int maximized;
    int desk_hidden;       /* minimized by the show-desktop shortcut */
    int rx, ry, rw, rh;    /* geometry to restore after maximize */
} win_t;

/* g_fb is the composition target: a RAM back buffer, not the framebuffer.
 * Writing pixels into video memory costs tens of nanoseconds each, so a drag
 * that repaints a 640x400 window straight into VRAM stuttered. Everything is
 * composed in cached memory and present() copies just the changed rows out. */
static u32 *g_fb;
static u32 *g_vram;
static int g_fbw, g_fbh, g_stride, g_fmt;
static u32 *g_bg;

static win_t g_wins[MAX_WINS];
static int g_zorder[MAX_WINS];   /* bottom -> top */
static int g_zcount;
static int g_focus = -1;
static int g_spawn_seq;

static int g_menu_open;
static int g_menu_sel;

/* Power control in the Start footer. One "Shut down" button whose right edge
 * carries a caret; the caret drops a flyout with Sleep / Shut down / Restart,
 * the same shape Windows puts there, drawn entirely from our own primitives. */
static int g_menu_power_sel = -1;    /* 1 = pointer over the button, -1 = none */
static int g_power_flyout;           /* 1 = the caret flyout is open */
static int g_power_sel = -1;         /* 0 = Sleep, 1 = Shut down, 2 = Restart */
static int g_sleeping;               /* screen blanked; any input wakes us */
static u64 g_sleep_since_ms;         /* uptime at the moment Sleep was picked */
/* Set when the button is still held as Sleep starts: the release that follows
 * belongs to that click and must not be mistaken for a wake-up request. */
static int g_sleep_need_release;

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

/* Taskbar flyouts (declared before the painters that reference them). */
static int g_taskmenu_open;
static int g_taskmenu_slot;
static int g_taskmenu_sel;
static int g_cal_open;
/* 0 = nothing, 1 = clock, 2 = show-desktop strip. */
static int g_tray_hover;
static int g_preview_shown;
static u64 g_hover_task_ms;

static u64 now_ms(void);

/* Defined further down but needed by the snap-layout code above it. */
static void resize_reply(int slot);

/* Defined with the control centre, but the Wi-Fi tile handler repaints it. */
static void netf_dirty(void);

/* ---- notification inbox ---------------------------------------------- */

/* There is no notification service in user space yet, so the shell keeps a
 * small inbox for the events it already knows about: an application was
 * launched, a window was closed. The list is newest first and the oldest entry
 * falls off the end, the same shape Windows puts behind the tray clock. */
#define NOTIF_MAX    6
#define NOTIF_TITLE  40
#define NOTIF_BODY   64

typedef struct {
    int used;
    u64 stamp;
    char title[NOTIF_TITLE];
    char body[NOTIF_BODY];
} notif_t;

static notif_t g_notifs[NOTIF_MAX];
static int g_notif_n;
static int g_dnd;                 /* do not disturb: drop new entries */

/* Control centre state. Volume has no mixer to drive yet and brightness is a
 * software dim applied when the frame reaches video memory, so both controls
 * are honest about what the shell can actually do. */
static int g_cc_open;
static int g_cc_hover = -1;
static int g_cc_drag;             /* 0 idle, 1 volume thumb, 2 brightness */
static int g_qs_wifi = 1;
static int g_qs_bt;
static int g_qs_night;
static int g_qs_eco;
static int g_qs_vol = 60;
static int g_qs_bri = 100;
static int g_bri_saved = 100;

/* The Wi-Fi quick setting drops a network list to its left. The kernel ships a
 * wired e1000 driver only, so the list shows the interfaces that really exist
 * and says so plainly instead of drawing access points that cannot be joined. */
static int g_net_flyout;

/* Battery telemetry, refreshed once per second alongside the clock. */
static mcube_battery_info_t g_bat;
static int g_have_bat;

/* USB input telemetry. Without a serial console this card row is the only way
 * to see whether the controller came up and whether any keyboard / mouse /
 * touch panel actually enumerated. */
static mcube_usb_info_t g_usb;

static int g_snap_open;           /* snap layouts flyout */
static int g_snap_slot = -1;
static int g_snap_sel = -1;
static u64 g_snap_hover_ms;

static void notif_push(const char *title, const char *body) {
    if (g_dnd) return;
    for (int i = NOTIF_MAX - 1; i > 0; i--) g_notifs[i] = g_notifs[i - 1];
    notif_t *n = &g_notifs[0];
    memset(n, 0, sizeof(*n));
    n->used = 1;
    n->stamp = now_ms();
    if (title) strncpy(n->title, title, NOTIF_TITLE - 1);
    if (body) strncpy(n->body, body, NOTIF_BODY - 1);
    if (g_notif_n < NOTIF_MAX) g_notif_n++;
}

static int g_desk_sel = -1;
static int g_desk_hover = -1;
static u64 g_desk_last_ms;
static int g_desk_last_idx = -1;

static u64 g_last_click_ms;
static int g_last_click_slot = -1;

static int g_need_compose;
static int g_need_taskbar;
static int g_need_tray;
static int g_need_menu;
static int g_cursor_moved;

static int g_cursor_saved;
static int g_cursor_ox, g_cursor_oy;   /* bitmap origin in screen space */
static int g_cursor_kind = CUR_ARROW;
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
    "Image Viewer", "Browser", "Settings", "LGY Builder",
    "Task Manager", "System Info", "Slides",
};
static const char *const g_app_paths[] = {
    "/bin/terminal.lgy", "/bin/files.lgy", "/bin/editor.lgy",
    "/bin/calc.lgy", "/bin/image.lgy", "/bin/browser.lgy",
    "/bin/settings.lgy", "/bin/lgyconv.lgy",
    "/bin/taskman.lgy", "/bin/sysinfo.lgy",
    "/bin/slides.lgy",
};
#define APP_COUNT ((int)(sizeof(g_app_paths) / sizeof(g_app_paths[0])))
#define BOOT_APP_COUNT 7

static const u32 g_icon_colors[] = {
    0x2AA9E0, 0xF59E0B, 0x10B981, 0xEC4899,
    0x8B5CF6, 0x22D3EE, 0x06B6D4, 0xA3E635,
    0xEF4444, 0x22C55E,
};

typedef struct {
    const char *name;
    const char *path;
} desk_app_t;

static const desk_app_t g_desk_apps[] = {
    { "Terminal", "/bin/terminal.lgy" },
    { "Files",    "/bin/files.lgy" },
    { "Editor",   "/bin/editor.lgy /TEST.TXT" },
    { "Slides",   "/bin/slides.lgy" },
    { "Browser",  "/bin/browser.lgy" },
    { "Tasks",    "/bin/taskman.lgy" },
    { "Settings", "/bin/settings.lgy" },
    { "System",   "/bin/sysinfo.lgy" },
    { "LGY",      "/bin/lgyconv.lgy" },
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

/* Copy an application bitmap (0x00RRGGBB, tightly packed) into a surface,
 * converting to the framebuffer pixel format exactly like gfx_fill does. */
static void gfx_blit(u32 *buf, int stride, int bw, int bh,
                     int x, int y, int w, int h, const u32 *src) {
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
    for (int yy = y0; yy < y1; yy++) {
        u32 *line = buf + (u64)yy * stride;
        const u32 *s = src + (u64)(yy - y) * (u64)w + (u64)(x0 - x);
        for (int xx = x0; xx < x1; xx++) line[xx] = pixel(*s++);
    }
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

/* Address of one surface pixel, or NULL when it falls outside the surface or,
 * for the desktop framebuffer, outside the current region clip. */
static u32 *px_at(u32 *buf, int stride, int bw, int bh, int x, int y) {
    if (x < 0 || y < 0) return 0;
    if (buf == g_fb) {
        if (x < g_clip_x0 || x >= g_clip_x1 || y < g_clip_y0 || y >= g_clip_y1)
            return 0;
    } else if (x >= bw || y >= bh) {
        return 0;
    }
    return buf + (u64)y * stride + x;
}

/* Composite a source colour over the destination at 0..255 coverage. Text
 * glyphs and rounded corners go through here, which is what gives the shell
 * smooth edges instead of the stair-stepped 1 bpp look it had before. The back
 * buffer stores R and B in an order set by the boot pixel format, so the blend
 * is done in the surface's own channel order and written back the same way. */
static void blend_px(u32 *buf, int stride, int bw, int bh,
                     int x, int y, u32 rgb, int a) {
    if (a <= 0) return;
    u32 *d = px_at(buf, stride, bw, bh, x, y);
    if (!d) return;
    if (a >= 255) { *d = pixel(rgb); return; }
    int rsh = (g_fmt == 1) ? 16 : 0;
    int bsh = (g_fmt == 1) ? 0 : 16;
    u32 dv = *d;
    int sr = (int)(rgb >> 16) & 0xFF;
    int sg = (int)(rgb >> 8) & 0xFF;
    int sb = (int)rgb & 0xFF;
    int dr = (int)((dv >> rsh) & 0xFF);
    int dg = (int)((dv >> 8) & 0xFF);
    int db = (int)((dv >> bsh) & 0xFF);
    int nr = dr + (sr - dr) * a / 255;
    int ng = dg + (sg - dg) * a / 255;
    int nb = db + (sb - db) * a / 255;
    *d = 0xFF000000u | ((u32)nr << rsh) | ((u32)ng << 8) | ((u32)nb << bsh);
}

/* Text from a coverage table: 8 bits of alpha per pixel rather than one bit.
 * mc_ui_font is the regular weight, mc_ui_font_bold the heading weight, so the
 * window titles and the launcher can establish a real type hierarchy while
 * keeping the same 8 px advance and therefore the same layout. */
static void gfx_text_tab(u32 *buf, int stride, int bw, int bh,
                         int x, int y, u32 rgb, const char *s, int n,
                         const u8 (*tab)[128]) {
    if (!s) return;
    u32 solid = pixel(rgb);
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
            const u8 *glyph = tab[ch];
            for (int row = 0; row < 16; row++) {
                int py = y + row;
                if (py < cy0 || py >= cy1) continue;
                const u8 *src = glyph + row * 8;
                for (int col = 0; col < 8; col++) {
                    u8 a = src[col];
                    if (!a) continue;
                    int gx = px + col;
                    if (gx < cx0 || gx >= cx1) continue;
                    if (a >= 255) buf[(u64)py * stride + gx] = solid;
                    else blend_px(buf, stride, bw, bh, gx, py, rgb, a);
                }
            }
        }
        px += 8;
    }
}

static void gfx_text(u32 *buf, int stride, int bw, int bh,
                     int x, int y, u32 rgb, const char *s, int n) {
    gfx_text_tab(buf, stride, bw, bh, x, y, rgb, s, n, mc_ui_font);
}

static void gfx_text_bold(u32 *buf, int stride, int bw, int bh,
                          int x, int y, u32 rgb, const char *s, int n) {
    gfx_text_tab(buf, stride, bw, bh, x, y, rgb, s, n, mc_ui_font_bold);
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

/* Inset of a rounded-rect row, in Q4 (sixteenths of a pixel).
 * `row_from_edge` is 0 on the outermost scanline of the corner. The exact edge
 * position is r - sqrt(r*r - dy*dy), which lands between pixels, and the
 * fractional part is what round_row turns into an alpha edge. */
static int inset_q4(int row_from_edge, int r) {
    if (r <= 0 || row_from_edge < 0 || row_from_edge >= r) return 0;
    int r4 = r * 16;
    /* Distance from the corner circle's centre to this scanline's centre. */
    int d4 = (r - row_from_edge) * 16 - 8;
    if (d4 <= 0) return 0;
    int q = r4 * r4 - d4 * d4;
    if (q < 0) q = 0;
    int ins = r4 - isqrt_i(q);
    return ins < 0 ? 0 : ins;
}

static int round_inset_q4(int row, int h, int r) {
    if (row < r) return inset_q4(row, r);
    if (row >= h - r) return inset_q4(h - 1 - row, r);
    return 0;
}

/* Top corners only; the bottom square edge is hidden under the content. */
static int round_inset_top_q4(int row, int r) {
    return inset_q4(row, r);
}

/* One scanline of a rounded rectangle with anti-aliased ends. */
static void round_row(u32 *buf, int stride, int bw, int bh,
                      int x, int y, int w, int ins4, u32 rgb) {
    int i0 = ins4 >> 4;
    int cov = (16 - (ins4 & 15)) * 255 / 16;
    int l = x + i0, rt = x + w - 1 - i0;
    if (l > rt) return;
    blend_px(buf, stride, bw, bh, l, y, rgb, cov);
    if (l == rt) return;
    blend_px(buf, stride, bw, bh, rt, y, rgb, cov);
    if (rt - l > 1) gfx_fill(buf, stride, bw, bh, l + 1, y, rt - l - 1, 1, rgb);
}

static void gfx_round_fill(u32 *buf, int stride, int bw, int bh,
                           int x, int y, int w, int h, int r, u32 rgb) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int yy = 0; yy < h; yy++)
        round_row(buf, stride, bw, bh, x, y + yy, w,
                  round_inset_q4(yy, h, r), rgb);
}

static void gfx_round_top_gradient(u32 *buf, int stride, int bw, int bh,
                                   int x, int y, int w, int h, int r,
                                   u32 top, u32 bottom) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    for (int yy = 0; yy < h; yy++) {
        u32 c = lerp_color(top, bottom, h > 1 ? yy * 255 / (h - 1) : 0);
        round_row(buf, stride, bw, bh, x, y + yy, w,
                  round_inset_top_q4(yy, r), c);
    }
}

/* Filled disc with a one-pixel anti-aliased rim. The power glyphs in the
 * launcher are only 12-14 px across, which is exactly the size where a hard
 * circle looks like a Lego brick, so the rim is blended. */
static void gfx_disc(u32 *buf, int stride, int bw, int bh,
                     int cx, int cy, int r, u32 rgb) {
    if (r <= 0) return;
    int ri = r * 16;                            /* radius in Q4 */
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d = isqrt_i((dx * dx + dy * dy) * 256);   /* Q4 distance */
            int a = (ri - d) * 16 + 128;
            if (a <= 0) continue;
            if (a > 255) a = 255;
            blend_px(buf, stride, bw, bh, cx + dx, cy + dy, rgb, a);
        }
    }
}

/* Standing power mark: a ring open at the top with a stem through the gap.
 * `bg` is the tile colour, punched out so the mark reads on any background. */
static void gfx_power_glyph(u32 *buf, int stride, int bw, int bh,
                            int cx, int cy, int r, u32 rgb, u32 bg) {
    if (r < 4) return;
    gfx_disc(buf, stride, bw, bh, cx, cy, r, rgb);
    gfx_disc(buf, stride, bw, bh, cx, cy, r - 2, bg);
    int gw = r / 2 + 1;
    gfx_fill(buf, stride, bw, bh, cx - gw / 2, cy - r - 1, gw, r + 1, bg);
    gfx_fill(buf, stride, bw, bh, cx - (r >= 6 ? 1 : 0), cy - r,
             (r >= 6 ? 2 : 1), r + 1, rgb);
}

/* Two-stroke caret. `up` points at the button the flyout belongs to. */
static void gfx_chevron(u32 *buf, int stride, int bw, int bh,
                        int cx, int cy, int up, u32 rgb) {
    for (int i = 0; i < 4; i++) {
        int y = up ? cy - 2 + i : cy + 1 - i;
        gfx_fill(buf, stride, bw, bh, cx - i - 1, y, 2, 1, rgb);
        gfx_fill(buf, stride, bw, bh, cx + i, y, 2, 1, rgb);
    }
}

static void gfx_text_scale(u32 *buf, int stride, int bw, int bh,
                           int x, int y, int scale, u32 rgb,
                           const char *s, int n) {
    if (!s || scale < 1) return;
    int px = x;
    for (int i = 0; (n < 0 || i < n) && s[i]; i++) {
        u8 ch = (u8)s[i];
        if (ch == '\n') { px = x; y += 16 * scale; continue; }
        if (px + 8 * scale > 0 && px < bw) {
            const u8 *glyph = mc_ui_font[ch];
            for (int row = 0; row < 16; row++) {
                const u8 *src = glyph + row * 8;
                for (int col = 0; col < 8; col++) {
                    u8 a = src[col];
                    if (!a) continue;
                    int gx = px + col * scale, gy = y + row * scale;
                    if (a >= 255 || scale > 2) {
                        gfx_fill(buf, stride, bw, bh, gx, gy, scale, scale, rgb);
                        continue;
                    }
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            blend_px(buf, stride, bw, bh, gx + sx, gy + sy,
                                     rgb, a);
                }
            }
        }
        px += 8 * scale;
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

/* Even-odd scanline fill for a convex quad. Used by the shell mark below. */
static void gfx_poly(u32 *buf, int stride, int bw, int bh,
                     const int *px, const int *py, int n, u32 rgb) {
    int y0 = py[0], y1 = py[0];
    for (int i = 1; i < n; i++) {
        if (py[i] < y0) y0 = py[i];
        if (py[i] > y1) y1 = py[i];
    }
    for (int y = y0; y <= y1; y++) {
        int xs[8], cnt = 0;
        for (int i = 0; i < n && cnt < 8; i++) {
            int j = (i + 1) % n;
            int ay = py[i], by = py[j];
            if (ay == by) continue;
            if ((y >= ay && y < by) || (y >= by && y < ay)) {
                int ax = px[i], bx = px[j];
                xs[cnt++] = ax + (int)((i64)(y - ay) * (bx - ax) / (by - ay));
            }
        }
        for (int a = 0; a < cnt; a++) {
            for (int b = a + 1; b < cnt; b++) {
                if (xs[b] < xs[a]) { int t = xs[a]; xs[a] = xs[b]; xs[b] = t; }
            }
        }
        for (int a = 0; a + 1 < cnt; a += 2) {
            gfx_fill(buf, stride, bw, bh, xs[a], y, xs[a + 1] - xs[a], 1, rgb);
        }
    }
}

/* The MCube mark: an isometric cube with three shaded faces. This is the
 * shell's own identity, drawn from scratch rather than borrowing another
 * vendor's logo. */
static void draw_mcube_mark(u32 *buf, int stride, int bw, int bh,
                            int cx, int cy, int s, u32 base) {
    int hw = s / 2, qh = s / 4;
    u32 top = lerp_color(base, 0xFFFFFF, 105);
    u32 left = base;
    u32 right = lerp_color(base, 0x000000, 80);

    int tx[4] = { cx, cx + hw, cx, cx - hw };
    int ty[4] = { cy - 2 * qh, cy - qh, cy, cy - qh };
    gfx_poly(buf, stride, bw, bh, tx, ty, 4, top);

    int lx[4] = { cx - hw, cx, cx, cx - hw };
    int ly[4] = { cy - qh, cy, cy + 2 * qh, cy + qh };
    gfx_poly(buf, stride, bw, bh, lx, ly, 4, left);

    int rx[4] = { cx, cx + hw, cx + hw, cx };
    int ry[4] = { cy, cy - qh, cy + qh, cy + 2 * qh };
    gfx_poly(buf, stride, bw, bh, rx, ry, 4, right);
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

/* ---- presenting the back buffer to video memory --------------------- */

/* Rectangles the current frame has to copy out, as a bounding box. */
static int g_pr_x0, g_pr_y0 = 0, g_pr_x1, g_pr_y1;

static void pres_add(int x, int y, int w, int h) {
    if (!g_vram || w <= 0 || h <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_fbw) x1 = g_fbw;
    if (y1 > g_fbh) y1 = g_fbh;
    if (x >= x1 || y >= y1) return;
    if (g_pr_x0 >= g_pr_x1 || g_pr_y0 >= g_pr_y1) {
        g_pr_x0 = x; g_pr_y0 = y; g_pr_x1 = x1; g_pr_y1 = y1;
        return;
    }
    if (x < g_pr_x0) g_pr_x0 = x;
    if (y < g_pr_y0) g_pr_y0 = y;
    if (x1 > g_pr_x1) g_pr_x1 = x1;
    if (y1 > g_pr_y1) g_pr_y1 = y1;
}

/* One row copy per scanline: the video memory is slow for one pixel at a time
 * but reasonably fast when whole cache lines are written back to back.
 *
 * Brightness is applied here rather than in the compositor: the back buffer
 * always holds the true colours, so dragging the slider only has to re-present
 * the same frame through a different scale factor. Night light leans the same
 * pixels warm (less blue, slightly less green). */
static void present(void) {
    if (!g_vram || g_pr_x0 >= g_pr_x1 || g_pr_y0 >= g_pr_y1) return;

    int x0 = g_pr_x0, x1 = g_pr_x1;
    int bytes = (x1 - x0) * 4;
    int dim = (g_qs_bri < 100) || g_qs_night;

    for (int y = g_pr_y0; y < g_pr_y1; y++) {
        const u32 *src = g_fb + (u64)y * g_stride + x0;
        u32 *dst = g_vram + (u64)y * g_stride + x0;
        if (!dim) {
            memcpy(dst, src, (size_t)bytes);
            continue;
        }
        for (int i = 0; i < x1 - x0; i++) {
            u32 p = src[i];
            u32 r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            r = r * (u32)g_qs_bri / 100u;
            g = g * (u32)g_qs_bri / 100u;
            b = b * (u32)g_qs_bri / 100u;
            if (g_qs_night) {
                g = g * 96u / 100u;
                b = b * 78u / 100u;
            }
            dst[i] = (p & 0xFF000000u) | (r << 16) | (g << 8) | b;
        }
    }

    g_pr_x0 = g_pr_x1 = 0;
    g_pr_y0 = g_pr_y1 = 0;
}

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

/* 16x16 arrow; bit 15 is the leftmost column, so the tip sits at (0,0). */
static const u16 g_cursor_arrow[CURSOR_H] = {
    0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xF800, 0xD800, 0x8C00, 0x0C00, 0x0600, 0x0000,
};

/* One-pixel-wide segment test; the resize cursors are composed from a few of
 * these so every direction stays crisp without a table per shape. */
static int seg_bit(int px, int py, int ax, int ay, int bx, int by) {
    int dx = bx - ax, dy = by - ay;
    int l2 = dx * dx + dy * dy;
    if (l2 == 0) return px == ax && py == ay;
    int t = (px - ax) * dx + (py - ay) * dy;
    if (t <= 0) return (px - ax) * (px - ax) + (py - ay) * (py - ay) <= 1;
    if (t >= l2) {
        int qx = px - bx, qy = py - by;
        return qx * qx + qy * qy <= 1;
    }
    int cross = (px - ax) * dy - (py - ay) * dx;
    return cross * cross <= l2;
}

static int cursor_shape(int kind, int x, int y) {
    if (x < 0 || y < 0 || x >= CURSOR_W || y >= CURSOR_H) return 0;
    switch (kind) {
    case CUR_SIZE_WE:
        if (y == 7 && x >= 2 && x <= 13) return 1;
        if (seg_bit(x, y, 0, 7, 5, 3) || seg_bit(x, y, 0, 7, 5, 11)) return 1;
        if (seg_bit(x, y, 15, 7, 10, 3) || seg_bit(x, y, 15, 7, 10, 11)) return 1;
        return 0;
    case CUR_SIZE_NS:
        if (x == 7 && y >= 2 && y <= 13) return 1;
        if (seg_bit(x, y, 7, 0, 3, 5) || seg_bit(x, y, 7, 0, 11, 5)) return 1;
        if (seg_bit(x, y, 7, 15, 3, 10) || seg_bit(x, y, 7, 15, 11, 10)) return 1;
        return 0;
    case CUR_SIZE_NWSE:
        if (seg_bit(x, y, 4, 4, 11, 11)) return 1;
        if (seg_bit(x, y, 0, 0, 6, 2) || seg_bit(x, y, 0, 0, 2, 6)) return 1;
        if (seg_bit(x, y, 15, 15, 9, 13) || seg_bit(x, y, 15, 15, 13, 9)) return 1;
        return 0;
    case CUR_SIZE_NESW:
        if (seg_bit(x, y, 11, 4, 4, 11)) return 1;
        if (seg_bit(x, y, 15, 0, 9, 2) || seg_bit(x, y, 15, 0, 13, 6)) return 1;
        if (seg_bit(x, y, 0, 15, 6, 13) || seg_bit(x, y, 0, 15, 2, 9)) return 1;
        return 0;
    default:
        return (int)((g_cursor_arrow[y] >> (15 - x)) & 1);
    }
}

static int cursor_kind_at(int x, int y);

static void cursor_restore(void) {
    if (!g_cursor_saved) return;
    pres_add(g_cursor_ox, g_cursor_oy, CURSOR_W, CURSOR_H);
    for (int yy = 0; yy < CURSOR_H; yy++) {
        int sy = g_cursor_oy + yy;
        if (sy < 0 || sy >= g_fbh) continue;
        for (int xx = 0; xx < CURSOR_W; xx++) {
            int sx = g_cursor_ox + xx;
            if (sx < 0 || sx >= g_fbw) continue;
            g_fb[(u64)sy * g_stride + sx] = g_cursor_save[yy][xx];
        }
    }
    g_cursor_saved = 0;
}

static void cursor_draw(void) {
    cursor_restore();

    int kind = cursor_kind_at(g_mx, g_my);
    g_cursor_kind = kind;

    /* The arrow points with its tip, the resize cursors with their centre. */
    int hx = kind == CUR_ARROW ? 0 : 7;
    int hy = hx;
    int ox = g_mx - hx, oy = g_my - hy;

    g_cursor_ox = ox;
    g_cursor_oy = oy;
    g_cursor_saved = 1;
    pres_add(ox, oy, CURSOR_W, CURSOR_H);

    for (int yy = 0; yy < CURSOR_H; yy++) {
        int sy = oy + yy;
        if (sy < 0 || sy >= g_fbh) continue;
        u32 *line = g_fb + (u64)sy * g_stride;
        for (int xx = 0; xx < CURSOR_W; xx++) {
            int sx = ox + xx;
            if (sx < 0 || sx >= g_fbw) continue;
            g_cursor_save[yy][xx] = line[sx];
        }
    }

    u32 white = pixel(0xFFFFFF);
    u32 black = pixel(0x000000);
    for (int yy = 0; yy < CURSOR_H; yy++) {
        int sy = oy + yy;
        if (sy < 0 || sy >= g_fbh) continue;
        u32 *line = g_fb + (u64)sy * g_stride;
        for (int xx = 0; xx < CURSOR_W; xx++) {
            int sx = ox + xx;
            if (sx < 0 || sx >= g_fbw) continue;
            if (cursor_shape(kind, xx, yy)) {
                line[sx] = white;
            } else if (cursor_shape(kind, xx - 1, yy) ||
                       cursor_shape(kind, xx + 1, yy) ||
                       cursor_shape(kind, xx, yy - 1) ||
                       cursor_shape(kind, xx, yy + 1)) {
                line[sx] = black;
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

/* Rectangles here are {x0, y0, x1, y1} in screen space. */
#define DIRTY_RECT_MAX 12

static int rect_push(int *rects, int n, int x0, int y0, int x1, int y1) {
    if (x0 >= x1 || y0 >= y1) return n;
    if (n >= DIRTY_RECT_MAX) return -1;
    rects[n * 4 + 0] = x0;
    rects[n * 4 + 1] = y0;
    rects[n * 4 + 2] = x1;
    rects[n * 4 + 3] = y1;
    return n + 1;
}

static int rects_subtract(const int *in, int n, int ax0, int ay0, int ax1,
                          int ay1, int *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        int x0 = in[i * 4 + 0], y0 = in[i * 4 + 1];
        int x1 = in[i * 4 + 2], y1 = in[i * 4 + 3];
        if (ax0 >= x1 || ax1 <= x0 || ay0 >= y1 || ay1 <= y0) {
            m = rect_push(out, m, x0, y0, x1, y1);
            if (m < 0) return -1;
            continue;
        }
        int my0 = ay0 > y0 ? ay0 : y0;
        int my1 = ay1 < y1 ? ay1 : y1;
        if (ay0 > y0) {
            m = rect_push(out, m, x0, y0, x1, ay0);
            if (m < 0) return -1;
        }
        if (ay1 < y1) {
            m = rect_push(out, m, x0, ay1, x1, y1);
            if (m < 0) return -1;
        }
        if (ax0 > x0) {
            m = rect_push(out, m, x0, my0, ax0, my1);
            if (m < 0) return -1;
        }
        if (ax1 < x1) {
            m = rect_push(out, m, ax1, my0, x1, my1);
            if (m < 0) return -1;
        }
    }
    return m;
}

/* Queue the exposed parts of a window for region composition. A surface
 * update that arrives while the window is (partly) covered cannot be blitted
 * straight to the screen without painting over the windows above it, and
 * skipping it would leave the exposed strip showing stale pixels forever. */
static void dirty_window_exposed(int slot) {
    const win_t *w = &g_wins[slot];
    int fx, fy, fw, fh;
    frame_box(w, &fx, &fy, &fw, &fh);

    int rects[DIRTY_RECT_MAX * 4];
    int next[DIRTY_RECT_MAX * 4];
    int n = rect_push(rects, 0, fx, fy, fx + fw + 10, fy + fh + 12);
    if (n < 0) return;

    for (int i = g_zcount - 1; i >= 0 && n > 0; i--) {
        int above = g_zorder[i];
        if (above == slot) break;
        const win_t *a = &g_wins[above];
        if (!a->used || a->minimized) continue;
        int ax, ay, aw, ah;
        frame_box(a, &ax, &ay, &aw, &ah);
        n = rects_subtract(rects, n, ax, ay, ax + aw, ay + ah, next);
        if (n < 0) {
            dirty_add(fx, fy, fw + 10, fh + 12);
            return;
        }
        memcpy(rects, next, (size_t)n * 4 * sizeof(int));
    }

    for (int i = 0; i < n; i++) {
        dirty_add(rects[i * 4], rects[i * 4 + 1],
                  rects[i * 4 + 2] - rects[i * 4],
                  rects[i * 4 + 3] - rects[i * 4 + 1]);
    }
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
    notif_push("Window closed", w->title);
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
        u32 c = lerp_color(g_wall_top, g_wall_bot,
                           y * 255 / (g_fbh > 1 ? g_fbh - 1 : 1));
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, 0, y, g_fbw, 1, c);
    }
}

static void desk_box(int idx, int *bx, int *by) {
    /* Icons fill a column downwards and then wrap into a second column, so a
     * short screen (768 lines) still shows every shortcut. */
    int rows = (work_h() - DESK_BOX_Y) / DESK_BOX_STEP;
    if (rows < 1) rows = 1;
    int col = idx / rows;
    int row = idx % rows;
    *bx = DESK_BOX_X + col * DESK_BOX_STEP;
    *by = DESK_BOX_Y + row * DESK_BOX_STEP;
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
                           sel ? g_accent : 0x16253C);
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
#define SYS_CARD_H 156

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
             SYS_CARD_W - 32, 2, g_accent_2);

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

    /* USB input health: green as soon as one HID device is being polled, red
     * when the controller exists but nothing enumerated, and "NO CONTROLLER"
     * when not even a host controller was claimed. */
    char usb[24];
    u32 usb_col;
    if (g_usb.devs > 0) {
        snprintf(usb, sizeof(usb), "%u HID", (u32)g_usb.devs);
        usb_col = 0x4ADE80;
    } else if (g_usb.ports > 0) {
        snprintf(usb, sizeof(usb), "0 HID (%u P)", (u32)g_usb.ports);
        usb_col = 0xF87171;
    } else {
        snprintf(usb, sizeof(usb), "NO CONTROLLER");
        usb_col = 0xF87171;
    }
    ry += 22;
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, rx, ry, 0x5B7191, "USB INPUT", -1);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, vx, ry, usb_col, usb);
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

    int cx, cy;
    sys_card_box(&cx, &cy);
    pres_add(cx - 4, cy - 4, SYS_CARD_W + 8, SYS_CARD_H + 8);
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
    u32 border;
    if (g_cfg_contrast) {
        border = w->focused ? 0xFFFFFF : 0x9FB4CC;
    } else {
        border = w->focused ? lerp_color(C_BORDER, g_accent, 90) : C_BORDER;
    }
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, fx, fy, fw, fh,
                   CORNER_R, border);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, fx + 1, fy + 1, fw - 2,
                   fh - 2, CORNER_R - 1, C_FRAME);

    if (w->focused) {
        gfx_round_top_gradient(g_fb, g_stride, g_fbw, g_fbh, fx + 1, fy + 1,
                               fw - 2, TITLE_H, CORNER_R - 1,
                               g_title_1, g_title_2);
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

static int g_start_x, g_start_y, g_start_w, g_start_h;
static int g_tray_x;

/* Pinned launcher. The browser is the one app people go looking for and then
 * cannot find on a desktop grid that scrolls off the bottom of a 1366x768
 * panel, so it keeps a permanent button beside Start. -3 is the sentinel the
 * taskbar hit test returns for it (-2 is Start, -1 is empty bar). */
#define PIN_BROWSER  (-3)
static int g_pin_x;

static int is_browser_window(const win_t *w) {
    return w->used && w->title[0] && strstr(w->title, "Browser") != NULL;
}

static int browser_running(void) {
    for (int i = 0; i < MAX_WINS; i++) {
        if (is_browser_window(&g_wins[i])) return i;
    }
    return -1;
}

static int task_button_y(void) {
    return work_h() + (TASKBAR_H - TB_BTN) / 2;
}

/* Windows 11 keeps the app icons in a centred cluster: Start at the left edge
 * of that cluster, running apps to its right, clock pinned to the far right. */
static void taskbar_layout(void) {
    g_task_n = 0;
    for (int i = 0; i < MAX_WINS; i++) {
        if (!g_wins[i].used) continue;
        /* A running browser is already represented by its pinned button;
         * listing it twice would only make the bar noisier. */
        if (is_browser_window(&g_wins[i])) continue;
        g_task_slot[g_task_n++] = i;
    }

    g_tray_x = g_fbw - TRAY_W;

    /* The launcher sits at the left edge of the bar and the running apps grow
     * to its right; this is the shell's own arrangement, not a clone of any
     * particular desktop's centred dock. */
    int x = 10;
    int by = task_button_y();
    g_start_x = x;
    g_start_y = by;
    g_start_w = TB_BTN;
    g_start_h = TB_BTN;
    x += TB_BTN + TB_GAP + 8;

    g_pin_x = x;
    x += TB_BTN + TB_GAP + 8;

    int limit = g_tray_x - 12;
    for (int i = 0; i < g_task_n; i++) {
        if (x + TB_BTN > limit) { g_task_n = i; break; }
        g_task_x[i] = x;
        g_task_w[i] = TB_BTN;
        x += TB_BTN + TB_GAP;
    }
}

static void draw_start_button(void) {
    int open = g_menu_open;
    int hover = (g_hover_task == -2);

    if (open || hover) {
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, g_start_x, g_start_y,
                       g_start_w, g_start_h, 8, open ? 0x1C3350 : 0x1B2740);
    }

    /* The shell mark: our cube, never a copy of another vendor's button. */
    u32 base = (open || hover) ? 0xEAF6FF : g_accent;
    draw_mcube_mark(g_fb, g_stride, g_fbw, g_fbh,
                    g_start_x + g_start_w / 2, g_start_y + g_start_h / 2,
                    22, base);
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

/* Power source glyph, 24x11, drawn in the tray and again in the control centre
 * so both places read the same way. Three states, because a pack that firmware
 * cannot measure must not be drawn as if it were full:
 *   - measured pack  -> battery shell with a charge bar, red under 20 %
 *   - pack, no level -> empty shell with a bolt, meaning "on mains, unknown"
 *   - no pack        -> a mains plug, meaning "this machine runs on wall power" */
static void draw_power_glyph(int x, int y) {
    const int bw = 22, bh = 11;

    if (!g_have_bat || !g_bat.present) {
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x + 5, y, 2, 4, 0x8FA3BE);
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x + 15, y, 2, 4, 0x8FA3BE);
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x, y + 3, bw, bh - 3, 3,
                       0x8FA3BE);
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x + 1, y + 4, bw - 2,
                       bh - 5, 2, 0x0B111B);
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x + 6, y + 7, bw - 12, 2,
                 0x8FA3BE);
        return;
    }

    int level = g_bat.has_percent ? (int)g_bat.percent : 0;
    u32 col;
    if (g_bat.has_percent && level < 20) col = 0xE5484D;
    else if (g_bat.charging)             col = 0x34D399;
    else                                 col = 0xE8ECF4;

    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x, y, bw, bh, 3, 0x7A8CA6);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x + 1, y + 1, bw - 2, bh - 2,
                   2, 0x0B111B);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x + bw, y + 3, 2, 5, 1,
                   0x7A8CA6);

    if (g_bat.has_percent) {
        int inner = bw - 4;
        int fill = inner * level / 100;
        if (fill > 0)
            gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x + 2, y + 2, fill,
                           bh - 4, 1, col);
    } else {
        /* Unmeasured charge: never paint a bar, paint the bolt instead. */
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x + 11, y + 2, 2, 3, col);
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x + 9, y + 4, 3, 2, col);
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x + 9, y + 6, 2, 3, col);
    }
}

/* Battery / mains badge, drawn between the system icons and the clock. The
 * kernel reports whether a pack exists and whether we are on mains; the charge
 * level only when it can measure it, so an unmeasurable machine is labelled AC
 * rather than shown a percentage nobody measured. */
static void draw_tray_battery(int x) {
    int by = work_h() + (TASKBAR_H - 11) / 2;
    draw_power_glyph(x, by);

    char txt[8];
    if (g_have_bat && g_bat.present && g_bat.has_percent)
        snprintf(txt, sizeof(txt), "%u%%", (unsigned)g_bat.percent);
    else
        snprintf(txt, sizeof(txt), "AC");
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, x + 28, work_h() + 16, C_TEXT_DIM,
             txt, -1);
}

static void draw_tray(void) {
    int ty = task_button_y();
    int icons_x = g_tray_x;
    int clock_x = g_fbw - TRAY_CLOCK_W;

    /* Hover is per-target: the icon strip lights up for the control centre, the
     * clock block for the calendar. */
    if (g_tray_hover == 1) {
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, icons_x + 3,
                       work_h() + 5, clock_x - icons_x - 6, TASKBAR_H - 10, 8,
                       0x1A2B44);
    } else if (g_tray_hover == 3) {
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, clock_x + 3,
                       work_h() + 5, g_fbw - 8 - clock_x, TASKBAR_H - 10, 8,
                       0x1A2B44);
    }

    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, clock_x, work_h() + 12, 1,
             TASKBAR_H - 24, 0x223149);

    /* Status of the AI layer, kept in the tray like a system icon. */
    int ax = icons_x + 14, ay = ty + 13;
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, ax, ay, 6, 6, 0x34D399);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, ax + 12, ty + 10, C_TEXT_DIM,
             "AI", -1);

    /* Unread badge: the count of entries sitting in the control-centre inbox. */
    if (g_notif_n > 0) {
        int bx = icons_x + 46, by = ty + 7, d = 16;
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx, by, d, d, d / 2,
                       0xE5484D);
        char cnt[4];
        snprintf(cnt, sizeof(cnt), "%d", g_notif_n > 9 ? 9 : g_notif_n);
        int cw = (int)strlen(cnt) * 8;
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, bx + (d - cw) / 2, by + 0,
                0xFFFFFF, cnt, -1);
    }

    /* Power state sits between the notification badge and the clock. */
    draw_tray_battery(icons_x + 64);

    mcube_time_t ts;
    if (mc_time(&ts) != 0) return;

    u32 secs = (u32)(ts.epoch % 86400);
    i64 days = (i64)(ts.epoch / 86400);
    int year, month, day;
    civil_from_days(days, &year, &month, &day);

    char clock[20];
    if (g_cfg_24h) {
        snprintf(clock, sizeof(clock), "%02u:%02u", secs / 3600,
                 (secs / 60) % 60);
    } else {
        u32 h12 = (secs / 3600) % 12;
        if (!h12) h12 = 12;
        snprintf(clock, sizeof(clock), "%u:%02u %s", h12, (secs / 60) % 60,
                 secs < 43200 ? "AM" : "PM");
    }
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, g_fbw - 12, work_h() + 7,
                   C_TEXT, clock);

    char date[16];
    snprintf(date, sizeof(date), "%04d/%02d/%02d", year, month, day);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, g_fbw - 12, work_h() + 25,
                   C_TEXT_DIM, date);
}

/* Windows keeps a sliver at the far right of the taskbar for peek-at-desktop;
 * this is the same affordance on a 5 px strip. */
static void draw_show_desktop(void) {
    int x = g_fbw - 5;
    if (g_tray_hover == 2) {
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x - 2, work_h() + 10, 7,
                 TASKBAR_H - 20, 0x1A2B44);
    }
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x, work_h() + 10, 1,
             TASKBAR_H - 20, g_tray_hover == 2 ? 0x7FB2FF : 0x223149);
}

static void draw_pin_button(void) {
    int ty = task_button_y();
    int hover = (g_hover_task == PIN_BROWSER);
    int slot = browser_running();
    int focused = slot >= 0 && g_wins[slot].focused && !g_wins[slot].minimized;

    if (hover || focused) {
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, g_pin_x, ty, TB_BTN,
                       TB_BTN, 8, hover ? 0x1E2C42 : 0x16202F);
    }

    draw_icon(g_fb, g_stride, g_fbw, g_fbh, g_pin_x + (TB_BTN - TB_ICON) / 2,
              ty + (TB_BTN - TB_ICON) / 2, TB_ICON, 0x22D3EE, "Browser");

    if (slot >= 0) {
        int bar = focused ? 16 : 8;
        u32 pc = focused ? g_accent_2 : (hover ? 0x4C6E9A : 0x2B4568);
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh,
                       g_pin_x + 3, ty + (TB_BTN - bar) / 2, 3, bar, 1, pc);
    }
}

static void draw_taskbar(void) {
    taskbar_layout();

    u32 tb_top = g_cfg_contrast ? 0x000000 : C_TASKBAR_TOP;
    u32 tb_bot = g_cfg_contrast ? 0x000000 : C_TASKBAR_BOT;

    gfx_vgradient(g_fb, g_stride, g_fbw, g_fbh, 0, work_h(), g_fbw, TASKBAR_H,
                  tb_top, tb_bot);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, 0, work_h(), g_fbw, 1, 0x223149);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, 0, g_fbh - 1, g_fbw, 1, 0x05080E);

    draw_start_button();
    draw_pin_button();

    int ty = task_button_y();
    for (int i = 0; i < g_task_n; i++) {
        int slot = g_task_slot[i];
        win_t *w = &g_wins[slot];
        int bx = g_task_x[i];
        int focused = w->focused && !w->minimized;
        int hovered = (g_hover_task == slot);

        if (focused || hovered) {
            gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx, ty, TB_BTN,
                           TB_BTN, 8, hovered ? 0x1E2C42 : 0x16202F);
        }

        u32 icol = g_icon_colors[slot % (int)(sizeof(g_icon_colors) /
                                             sizeof(g_icon_colors[0]))];
        if (w->minimized) icol = lerp_color(icol, C_TASKBAR_BOT, 130);
        draw_icon(g_fb, g_stride, g_fbw, g_fbh, bx + (TB_BTN - TB_ICON) / 2,
                  ty + (TB_BTN - TB_ICON) / 2, TB_ICON, icol, w->title);

        /* Running/focused marker: a short bar hugging the left edge of the
         * button. Deliberately not the underline pill of another desktop. */
        int bar = focused ? 16 : 8;
        u32 pc = focused ? g_accent_2 : (hovered ? 0x4C6E9A : 0x2B4568);
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh,
                       bx + 3, ty + (TB_BTN - bar) / 2, 3, bar, 1, pc);
    }

    draw_tray();
    draw_show_desktop();
}

/* ---- taskbar popups: hover preview, context menu, calendar ----------- */

#define PREVIEW_W 252
#define PREVIEW_H 160
#define TASK_MENU_W 184
#define TASK_MENU_ITEM_H 28
#define TASK_MENU_PAD 6
#define CAL_W 304
#define CAL_H 262

static int g_pv_x = -1, g_pv_y = -1;

static int task_button_index(int slot) {
    for (int i = 0; i < g_task_n; i++) {
        if (g_task_slot[i] == slot) return i;
    }
    return -1;
}

static int preview_box(int *x, int *y) {
    int idx = task_button_index(g_hover_task);
    if (idx < 0) return 0;
    int cx = g_task_x[idx] + g_task_w[idx] / 2;
    *x = cx - PREVIEW_W / 2;
    *y = work_h() - PREVIEW_H - 8;
    if (*x < 8) *x = 8;
    if (*x + PREVIEW_W > g_fbw - 8) *x = g_fbw - 8 - PREVIEW_W;
    return 1;
}

static int taskmenu_box(int *x, int *y) {
    int idx = task_button_index(g_taskmenu_slot);
    if (idx < 0) return 0;
    *x = g_task_x[idx];
    *y = work_h() - (3 * TASK_MENU_ITEM_H + 2 * TASK_MENU_PAD) - 8;
    if (*x + TASK_MENU_W > g_fbw - 8) *x = g_fbw - 8 - TASK_MENU_W;
    return 1;
}

static void calendar_box(int *x, int *y) {
    *x = g_fbw - CAL_W - 10;
    *y = work_h() - CAL_H - 10;
}

static int taskmenu_item_at(int x, int y) {
    int bx, by;
    if (!taskmenu_box(&bx, &by)) return -1;
    if (x < bx + 4 || x >= bx + TASK_MENU_W - 4) return -1;
    int y0 = by + TASK_MENU_PAD;
    if (y < y0 || y >= y0 + 3 * TASK_MENU_ITEM_H) return -1;
    return (y - y0) / TASK_MENU_ITEM_H;
}

/* Three separate targets share the right end of the bar: the peek sliver at the
 * very edge, the clock/date block, and the icon strip (which opens the control
 * centre). Splitting them is what makes the tray behave like Windows 11. */
static int tray_hit(int x, int y) {
    if (y < work_h() || y >= g_fbh) return 0;
    taskbar_layout();
    if (x >= g_fbw - 5) return 2;
    if (x >= g_fbw - TRAY_CLOCK_W) return 3;
    if (x >= g_tray_x) return 1;
    return 0;
}

static void draw_popup_panel(int x, int y, int w, int h) {
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x + 6, y + 9, w, h, 10,
                   C_SHADOW);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x - 1, y - 1, w + 2, h + 2,
                   12, 0x2C3E5C);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x, y, w, h, 11, 0x101826);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x + 2, y + 1, w - 4, 1, 0x2B4668);
}

static void draw_preview(void) {
    if (!g_preview_shown || g_hover_task < 0) return;
    if (g_menu_open || g_switcher_open || g_taskmenu_open || g_cal_open ||
        g_cc_open || g_snap_open)
        return;
    if (g_hover_task >= MAX_WINS || !g_wins[g_hover_task].used) return;

    int x, y;
    if (!preview_box(&x, &y)) return;
    win_t *w = &g_wins[g_hover_task];

    const int pad = 10;
    int tw = PREVIEW_W - 2 * pad;
    int th = PREVIEW_H - 52;
    draw_popup_panel(x, y, PREVIEW_W, PREVIEW_H);

    int ix = x + pad, iy = y + pad;
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, ix, iy, tw, th, 0x060A12);
    if (w->surface && w->w > 0 && w->h > 0) {
        int cw = w->w, ch = w->h;
        int dw, dh;
        if ((i64)tw * ch <= (i64)th * cw) {
            dw = tw;
            dh = ch * tw / cw;
        } else {
            dh = th;
            dw = cw * th / ch;
        }
        int ox = ix + (tw - dw) / 2, oy = iy + (th - dh) / 2;
        for (int yy = 0; yy < dh; yy++) {
            int py = oy + yy;
            if (py < g_clip_y0 || py >= g_clip_y1) continue;
            const u32 *row = w->surface + (u64)(yy * ch / dh) * w->sw;
            for (int xx = 0; xx < dw; xx++) {
                int px = ox + xx;
                if (px < g_clip_x0 || px >= g_clip_x1) continue;
                g_fb[(u64)py * g_stride + px] = row[xx * cw / dw];
            }
        }
    }
    gfx_rect(g_fb, g_stride, g_fbw, g_fbh, ix - 1, iy - 1, tw + 2, th + 2,
             0x24344D);

    u32 tint = g_icon_colors[g_hover_task %
                             (int)(sizeof(g_icon_colors) /
                                   sizeof(g_icon_colors[0]))];
    draw_icon(g_fb, g_stride, g_fbw, g_fbh, x + pad, y + PREVIEW_H - 34, 18,
              tint, w->title);
    gfx_text_clip(g_fb, g_stride, g_fbw, g_fbh, x + pad + 26,
                  y + PREVIEW_H - 34, C_TEXT, w->title, 18);

    const char *state = w->minimized ? "MINIMIZED"
                                     : (w->focused ? "ACTIVE" : "RUNNING");
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, x + PREVIEW_W - pad,
                   y + PREVIEW_H - 32, w->minimized ? C_TEXT_DIM : 0x5EEAD4,
                   state);

    g_pv_x = x;
    g_pv_y = y;
}

static void draw_task_menu(void) {
    if (!g_taskmenu_open) return;
    int x, y;
    if (!taskmenu_box(&x, &y)) return;

    int h = 3 * TASK_MENU_ITEM_H + 2 * TASK_MENU_PAD;
    draw_popup_panel(x, y, TASK_MENU_W, h);

    static const char *const items[3] = {"Restore", "Minimize", "Close"};
    for (int i = 0; i < 3; i++) {
        int iy = y + TASK_MENU_PAD + i * TASK_MENU_ITEM_H;
        if (i == g_taskmenu_sel) {
            gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x + 4, iy + 1,
                           TASK_MENU_W - 8, TASK_MENU_ITEM_H - 2, 6,
                           C_MENU_HOVER);
        }
        u32 dot = i == 2 ? 0xE2566B : (i == 0 ? 0x5EEAD4 : g_accent);
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x + 14, iy + 11, 6, 6, 2,
                       dot);
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, x + 30, iy + 6, C_TEXT,
                 items[i], -1);
    }
}

static int days_in_month(int year, int month) {
    static const int days[12] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return days[month - 1];
}

static void draw_calendar(void) {
    if (!g_cal_open) return;

    int x, y;
    calendar_box(&x, &y);
    draw_popup_panel(x, y, CAL_W, CAL_H);

    mcube_time_t ts;
    if (mc_time(&ts) != 0) return;
    i64 epoch_days = (i64)(ts.epoch / 86400);
    int year, month, day;
    civil_from_days(epoch_days, &year, &month, &day);

    static const char *const mname[12] = {
        "January", "February", "March",     "April",   "May",      "June",
        "July",    "August",   "September", "October", "November", "December"};
    char head[32];
    snprintf(head, sizeof(head), "%s %d", mname[month - 1], year);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, x + 18, y + 16, C_TEXT, head, -1);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x + 16, y + 42, CAL_W - 32, 1,
             0x24344D);

    static const char *const wd[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    int colw = (CAL_W - 32) / 7;
    for (int i = 0; i < 7; i++) {
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, x + 16 + i * colw + colw / 2 - 8,
                 y + 52, C_TEXT_DIM, wd[i], -1);
    }

    /* 1970-01-01 was a Thursday, so Monday-first indexing is days + 3. */
    i64 first = epoch_days - (day - 1);
    int lead = (int)((first + 3) % 7);
    int dim = days_in_month(year, month);
    int grid_y = y + 76;

    for (int d = 1; d <= dim; d++) {
        int cell = lead + d - 1;
        int cx = x + 16 + (cell % 7) * colw;
        int cy = grid_y + (cell / 7) * 26;
        if (d == day) {
            gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, cx + colw / 2 - 11,
                           cy, 22, 22, 11, g_accent);
        }
        char num[8];
        snprintf(num, sizeof(num), "%d", d);
        int nx = cx + colw / 2 - (int)strlen(num) * 4;
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, nx, cy + 3,
                 d == day ? 0xFFFFFF : C_TEXT, num, -1);
    }

    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x + 16, y + CAL_H - 30, CAL_W - 32,
             1, 0x24344D);
    char today[32];
    snprintf(today, sizeof(today), "Today %02d/%02d/%04d", day, month, year);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, x + 18, y + CAL_H - 22,
             C_TEXT_DIM, today, -1);
}

/* ---- control centre: notifications and quick settings ----------------- */

/* One panel under the tray carries both halves of what Windows splits into the
 * notification centre and quick settings: the inbox on top, the toggles and
 * the two sliders underneath. The layout is anchored to the far right so it
 * opens under the same spot every time. */
#define CC_W        344
#define CC_H        520
#define CC_PAD      14
#define CC_TILE_H   48
#define CC_ROW_H    54

#define CC_HIT_NONE  -1
#define CC_HIT_CLEAR 10
#define CC_HIT_DND   11
#define CC_HIT_VOL   12
#define CC_HIT_BRI   13

static const char *const g_qs_names[4] = {
    "Wi-Fi", "Bluetooth", "Night light", "Battery saver",
};

static u32 qs_color(int i) {
    switch (i) {
    case 0:  return 0x3B82F6;
    case 1:  return 0x2AA9E0;
    case 2:  return 0xF59E0B;
    default: return 0x22C55E;
    }
}

static int qs_on(int i) {
    switch (i) {
    case 0:  return g_qs_wifi;
    case 1:  return g_qs_bt;
    case 2:  return g_qs_night;
    default: return g_qs_eco;
    }
}

static void cc_box(int *x, int *y) {
    *x = g_fbw - CC_W - 12;
    if (*x < 8) *x = 8;
    *y = work_h() - CC_H - 10;
    if (*y < 8) *y = 8;
}

static void cc_clear_rect(int px, int py, int *x, int *y, int *w, int *h) {
    *w = 72;
    *h = 20;
    *x = px + CC_W - CC_PAD - *w;
    *y = py + 10;
}

static void cc_dnd_rect(int px, int py, int *x, int *y, int *w, int *h) {
    *x = px + CC_PAD;
    *y = py + 36;
    *w = CC_W - 2 * CC_PAD;
    *h = 30;
}

static void cc_tile_rect(int px, int py, int i, int *x, int *y, int *w,
                         int *h) {
    int tw = (CC_W - 2 * CC_PAD - 10) / 2;
    *x = px + CC_PAD + (i & 1) * (tw + 10);
    *y = py + 280 + (i >> 1) * (CC_TILE_H + 10);
    *w = tw;
    *h = CC_TILE_H;
}

static void cc_slider_rect(int px, int py, int i, int *x, int *y, int *w,
                           int *h) {
    *x = px + CC_PAD;
    *y = py + 402 + i * 32;
    *w = CC_W - 2 * CC_PAD;
    *h = 28;
}

/* The track leaves room for the label on the left and the percentage on the
 * right, so no text ever sits on top of it. */
static void cc_track_rect(int px, int py, int i, int *x, int *w) {
    (void)py;
    (void)i;
    *x = px + CC_PAD + 76;
    *w = CC_W - 2 * CC_PAD - 76 - 52;
}

static void cc_dirty(void) {
    int x, y;
    cc_box(&x, &y);
    dirty_add(x - 3, y - 3, CC_W + 18, CC_H + 20);
}

static void cc_clear_notifs(void) {
    memset(g_notifs, 0, sizeof(g_notifs));
    g_notif_n = 0;
    cc_dirty();
}

static void cc_toggle_tile(int i) {
    switch (i) {
    case 0:
        /* The tile opens the network list; flipping the radio is the switch
         * inside it, so a stray click here cannot drop a working link. */
        g_net_flyout = !g_net_flyout;
        cc_dirty();
        netf_dirty();
        return;
    case 1:
        g_qs_bt = !g_qs_bt;
        break;
    case 2:
        g_qs_night = !g_qs_night;
        g_need_compose = 1;         /* the whole screen is filtered */
        return;
    default:
        /* Battery saver caps the backlight; turning it off restores whatever
         * the slider was set to before. */
        g_qs_eco = !g_qs_eco;
        if (g_qs_eco) {
            g_bri_saved = g_qs_bri;
            g_qs_bri = 65;
        } else {
            g_qs_bri = g_bri_saved;
        }
        g_need_compose = 1;
        return;
    }
    cc_dirty();
}

/* Map a pointer position to a slider value; used for the initial click and for
 * every move while the thumb is held. */
static void cc_set_slider(int which, int x) {
    int px, py;
    cc_box(&px, &py);
    int tx, tw;
    cc_track_rect(px, py, which == 1 ? 0 : 1, &tx, &tw);
    int v = (x - tx) * 100 / tw;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (which == 1) g_qs_vol = v;
    else g_qs_bri = v;
    g_need_compose = 1;
}

static int cc_at(int x, int y) {
    if (!g_cc_open) return CC_HIT_NONE;
    int px, py, bx, by, bw, bh;
    cc_box(&px, &py);
    if (!inside(x, y, px, py, CC_W, CC_H)) return CC_HIT_NONE;

    cc_clear_rect(px, py, &bx, &by, &bw, &bh);
    if (inside(x, y, bx, by, bw, bh)) return CC_HIT_CLEAR;
    cc_dnd_rect(px, py, &bx, &by, &bw, &bh);
    if (inside(x, y, bx, by, bw, bh)) return CC_HIT_DND;
    for (int i = 0; i < 4; i++) {
        cc_tile_rect(px, py, i, &bx, &by, &bw, &bh);
        if (inside(x, y, bx, by, bw, bh)) return i;
    }
    for (int i = 0; i < 2; i++) {
        cc_slider_rect(px, py, i, &bx, &by, &bw, &bh);
        if (inside(x, y, bx, by, bw, bh))
            return i == 0 ? CC_HIT_VOL : CC_HIT_BRI;
    }
    return CC_HIT_NONE;
}

static void draw_cc_tile(int x, int y, int w, int h, int i, int hot) {
    int on = qs_on(i);
    u32 col = qs_color(i);
    u32 face = on ? col : 0x18233A;
    if (on && hot) face = lerp_color(col, 0xFFFFFF, 45);
    if (!on && hot) face = 0x1E2C42;

    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x, y, w, h, 8,
                   on ? lerp_color(col, 0xFFFFFF, 55) : 0x24344D);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x + 1, y + 1, w - 2, h - 2,
                   7, face);

    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x + 11, y + 11, 8, 8, 4,
                   on ? 0xFFFFFF : 0x5B7191);
    gfx_text_clip(g_fb, g_stride, g_fbw, g_fbh, x + 26, y + 8,
                  on ? 0xFFFFFF : C_TEXT, g_qs_names[i], (w - 34) / 8);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, x + 11, y + 27,
             on ? 0xDCEBFF : C_TEXT_DIM, on ? "On" : "Off", -1);
}

static void draw_cc_slider(int px, int py, int i, const char *label,
                           int value, u32 col) {
    int x, y, w, h;
    cc_slider_rect(px, py, i, &x, &y, &w, &h);

    int code = i == 0 ? CC_HIT_VOL : CC_HIT_BRI;
    int hot = (g_cc_hover == code) || (g_cc_drag == i + 1);

    gfx_text(g_fb, g_stride, g_fbw, g_fbh, x, y + 6,
             hot ? C_TEXT : C_TEXT_DIM, label, -1);

    int tx, tw;
    cc_track_rect(px, py, i, &tx, &tw);
    int ty = y + h / 2 - 2;
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, tx, ty, tw, 4, 2, 0x2A3A55);
    int fill = tw * value / 100;
    if (fill > 0)
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, tx, ty, fill, 4, 2, col);
    int kx = tx + fill - 6;
    if (kx < tx - 6) kx = tx - 6;
    if (kx > tx + tw - 6) kx = tx + tw - 6;
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, kx, y + h / 2 - 7, 12, 14, 7,
                   hot ? 0xFFFFFF : 0xE8ECF4);

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", value);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, x + w, y + 6, C_TEXT_DIM,
                   pct);
}

/* Power footer: the same glyph the tray uses, plus the sentence a user wants
 * when a pack exists but firmware will not tell us how full it is. */
static void draw_cc_power_row(int px, int py) {
    int x = px + CC_PAD, y = py + 478, w = CC_W - 2 * CC_PAD, h = 32;

    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x, y, w, h, 8, 0x16202F);
    draw_power_glyph(x + 12, y + 12);

    char head[32];
    char sub[64];
    if (g_have_bat && g_bat.present) {
        if (g_bat.has_percent) {
            snprintf(head, sizeof(head), "Battery %u%%",
                     (unsigned)g_bat.percent);
            snprintf(sub, sizeof(sub), "%s",
                     g_bat.charging ? "Charging" : "On battery");
        } else {
            snprintf(head, sizeof(head), "%s", "Battery installed");
            snprintf(sub, sizeof(sub), "%s",
                     g_bat.ac_online ? "On AC, charge unavailable"
                                     : "Charge unavailable");
        }
    } else {
        snprintf(head, sizeof(head), "%s", "AC power");
        snprintf(sub, sizeof(sub), "%s", "No battery detected");
    }
    gfx_text_clip(g_fb, g_stride, g_fbw, g_fbh, x + 44, y + 5, C_TEXT, head,
                  (w - 52) / 8);
    gfx_text_clip(g_fb, g_stride, g_fbw, g_fbh, x + 44, y + 19, 0x5B7191, sub,
                  (w - 52) / 8);
}

static void draw_control_center(void) {
    if (!g_cc_open) return;

    int px, py;
    cc_box(&px, &py);
    draw_popup_panel(px, py, CC_W, CC_H);

    gfx_text_bold(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD, py + 12, C_TEXT,
                  "Notifications", -1);

    int bx, by, bw, bh;
    cc_clear_rect(px, py, &bx, &by, &bw, &bh);
    int chot = (g_cc_hover == CC_HIT_CLEAR) && g_notif_n > 0;
    if (chot)
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx, by, bw, bh, 6,
                       0x1E2C42);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, bx + bw, by + 2,
                   g_notif_n == 0 ? 0x4A5B73
                                  : (chot ? 0xFFFFFF : g_accent_2),
                   "Clear all");

    /* Do not disturb: a pill with its own switch, so the state reads at a
     * glance even when the list below is empty. */
    cc_dnd_rect(px, py, &bx, &by, &bw, &bh);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx, by, bw, bh, 15,
                   g_dnd ? 0x1D3C7A : 0x16202F);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, bx + 12, by + 7,
             g_dnd ? 0xDCEBFF : C_TEXT_DIM, "Do not disturb", -1);
    int sw = 34, sh = 16;
    int sx = bx + bw - sw - 8, sy = by + (bh - sh) / 2;
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, sx, sy, sw, sh, sh / 2,
                   g_dnd ? g_accent : 0x2A3A55);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh,
                   g_dnd ? sx + sw - sh + 2 : sx + 2, sy + 2, sh - 4, sh - 4,
                   (sh - 4) / 2, 0xFFFFFF);

    int ny = py + 74;
    if (g_notif_n == 0) {
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD, ny,
                       CC_W - 2 * CC_PAD, 54, 8, 0x131C2A);
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD + 14, ny + 12,
                 C_TEXT_DIM, "No new notifications", -1);
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD + 14, ny + 30,
                 0x5B7191, "Activity from apps shows up here.", -1);
    } else {
        int rows = g_notif_n > 3 ? 3 : g_notif_n;
        for (int i = 0; i < rows; i++) {
            const notif_t *n = &g_notifs[i];
            int cy = ny + i * CC_ROW_H;
            gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD, cy,
                           CC_W - 2 * CC_PAD, 48, 8, 0x16202F);
            gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD, cy + 8,
                           4, 32, 2, g_accent_2);
            draw_icon(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD + 12, cy + 14,
                      20, g_accent, n->title);
            gfx_text_clip(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD + 42,
                          cy + 8, C_TEXT, n->title,
                          (CC_W - 2 * CC_PAD - 52) / 8);
            gfx_text_clip(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD + 42,
                          cy + 26, C_TEXT_DIM, n->body,
                          (CC_W - 2 * CC_PAD - 52) / 8);
        }
    }

    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD, py + 244,
             CC_W - 2 * CC_PAD, 1, 0x24344D);
    gfx_text_bold(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD, py + 254, C_TEXT,
                  "Quick settings", -1);

    for (int i = 0; i < 4; i++) {
        int x, y, w, h;
        cc_tile_rect(px, py, i, &x, &y, &w, &h);
        draw_cc_tile(x, y, w, h, i, g_cc_hover == i);
    }

    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD, py + 394,
             CC_W - 2 * CC_PAD, 1, 0x24344D);
    draw_cc_slider(px, py, 0, "Volume", g_qs_vol, g_accent);
    draw_cc_slider(px, py, 1, "Brightness", g_qs_bri, 0xF5C451);

    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD, py + 470,
             CC_W - 2 * CC_PAD, 1, 0x24344D);
    draw_cc_power_row(px, py);
}

/* ---- control centre: network list ------------------------------------ */

/* The Wi-Fi tile used to be a dead on/off pill, which is why clicking it never
 * showed anything to connect to. It now drops this list: the radio switch, a
 * truthful note about the missing wireless driver, and the state of the wired
 * adapter the kernel actually has. */

#define NETF_W 300
#define NETF_H 232

#define NET_HIT_NONE     (-1)
#define NET_HIT_WIFI     0
#define NET_HIT_SETTINGS 1

static void netf_box(int *x, int *y) {
    int cx, cy;
    cc_box(&cx, &cy);
    *x = cx - NETF_W - 10;
    if (*x < 8) *x = 8;
    *y = cy;
    if (*y + NETF_H > work_h() - 10) *y = work_h() - 10 - NETF_H;
    if (*y < 8) *y = 8;
}

static void netf_dirty(void) {
    int x, y;
    netf_box(&x, &y);
    dirty_add(x - 3, y - 3, NETF_W + 18, NETF_H + 18);
}

static int netf_at(int x, int y) {
    if (!g_net_flyout) return NET_HIT_NONE;
    int bx, by;
    netf_box(&bx, &by);
    if (!inside(x, y, bx, by, NETF_W, NETF_H)) return NET_HIT_NONE;
    if (inside(x, y, bx + 12, by + 40, NETF_W - 24, 34)) return NET_HIT_WIFI;
    if (inside(x, y, bx + 12, by + NETF_H - 44, NETF_W - 24, 30))
        return NET_HIT_SETTINGS;
    return NET_HIT_NONE;
}

static void draw_net_flyout(void) {
    if (!g_cc_open || !g_net_flyout) return;

    int bx, by;
    netf_box(&bx, &by);
    draw_popup_panel(bx, by, NETF_W, NETF_H);

    gfx_text_bold(g_fb, g_stride, g_fbw, g_fbh, bx + 14, by + 12, C_TEXT,
                  "Network", -1);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, bx + NETF_W - 14, by + 12,
                   0x5B7191, g_qs_wifi ? "Wi-Fi on" : "Wi-Fi off");

    /* Wi-Fi row: same switch shape as the Do-not-disturb pill, so the state
     * reads the same way in both places. */
    int rx = bx + 12, ry = by + 40, rw = NETF_W - 24, rh = 34;
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, rx, ry, rw, rh, 8, 0x16202F);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, rx + 10, ry + 13, 8, 8, 4,
                   g_qs_wifi ? g_accent : 0x5B7191);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, rx + 26, ry + 9, C_TEXT, "Wi-Fi",
             -1);
    int sw = 34, sh = 16;
    int sx = rx + rw - sw - 10, sy = ry + (rh - sh) / 2;
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, sx, sy, sw, sh, sh / 2,
                   g_qs_wifi ? g_accent : 0x2A3A55);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh,
                   g_qs_wifi ? sx + sw - sh + 2 : sx + 2, sy + 2, sh - 4,
                   sh - 4, (sh - 4) / 2, 0xFFFFFF);

    gfx_text_clip(g_fb, g_stride, g_fbw, g_fbh, bx + 14, by + 82, C_TEXT_DIM,
                  "No wireless adapter detected", (NETF_W - 28) / 8);
    gfx_text_clip(g_fb, g_stride, g_fbw, g_fbh, bx + 14, by + 98, 0x5B7191,
                  "This build ships a wired driver only.", (NETF_W - 28) / 8);

    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, bx + 12, by + 122, NETF_W - 24, 1,
             0x24344D);

    mcube_net_info_t ni;
    int have = (net_info(&ni) == 0);
    int up = have && ni.link_up;

    gfx_text_bold(g_fb, g_stride, g_fbw, g_fbh, bx + 14, by + 132, C_TEXT,
                  "Ethernet", -1);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx + NETF_W - 90, by + 137, 8,
                   8, 4, up ? 0x22C55E : 0x5B7191);
    gfx_text_right(g_fb, g_stride, g_fbw, g_fbh, bx + NETF_W - 14, by + 132,
                   up ? 0x34D399 : C_TEXT_DIM,
                   up ? "Connected" : "Unplugged");

    char line[64];
    if (up)
        snprintf(line, sizeof(line), "%u.%u.%u.%u",
                 (ni.ip >> 24) & 0xFFu, (ni.ip >> 16) & 0xFFu,
                 (ni.ip >> 8) & 0xFFu, ni.ip & 0xFFu);
    else
        snprintf(line, sizeof(line), "No carrier");
    gfx_text_clip(g_fb, g_stride, g_fbw, g_fbh, bx + 14, by + 152, C_TEXT_DIM,
                  line, (NETF_W - 28) / 8);

    snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned)ni.mac[0], (unsigned)ni.mac[1], (unsigned)ni.mac[2],
             (unsigned)ni.mac[3], (unsigned)ni.mac[4], (unsigned)ni.mac[5]);
    gfx_text_clip(g_fb, g_stride, g_fbw, g_fbh, bx + 14, by + 170, 0x5B7191,
                  line, (NETF_W - 28) / 8);

    /* Footer shortcut into the full Network & internet page. */
    int fy = by + NETF_H - 44;
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx + 12, fy, NETF_W - 24, 30,
                   8, 0x1B2B44);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, bx + 24, fy + 7, g_accent_2,
             "Open network settings", -1);
}

static void cc_toggle(void) {
    if (g_cc_open && g_net_flyout) {
        g_net_flyout = 0;
        netf_dirty();
    }
    g_cc_open = !g_cc_open;
    if (g_cc_open) {
        g_cal_open = 0;
        g_taskmenu_open = 0;
        g_taskmenu_slot = -1;
        g_preview_shown = 0;
        g_menu_open = 0;
        g_cc_hover = -1;
    }
    cc_dirty();
    g_need_taskbar = 1;
}

/* ---- snap layouts ----------------------------------------------------- */

/* Hovering the maximise button drops this picker; each tile sets the window
 * geometry directly. Snapped windows keep the restore geometry the maximise
 * button already saves, so un-maximising returns to the old size. */
#define SNAP_W 196
#define SNAP_H 120

static int snap_box(int *x, int *y) {
    if (g_snap_slot < 0 || g_snap_slot >= MAX_WINS ||
        !g_wins[g_snap_slot].used)
        return 0;
    int bx, by, bw, bh;
    button_box(&g_wins[g_snap_slot], BTN_MAX, &bx, &by, &bw, &bh);
    *x = bx + bw / 2 - SNAP_W / 2;
    *y = by + bh + 8;
    if (*x < 8) *x = 8;
    if (*x + SNAP_W > g_fbw - 8) *x = g_fbw - 8 - SNAP_W;
    if (*y + SNAP_H > work_h() - 8) *y = work_h() - 8 - SNAP_H;
    return 1;
}

static void snap_zone_rect(int px, int py, int zone, int *x, int *y, int *w,
                           int *h) {
    if (zone < 2) {
        *w = 83;
        *h = 32;
        *x = px + CC_PAD - 4 + zone * (*w + 10);
        *y = py + 32;
        return;
    }
    int z = zone - 2;
    *w = 40;
    *h = 32;
    *x = px + CC_PAD - 4 + z * (*w + 5);
    *y = py + 72;
}

static int snap_zone_at(int x, int y) {
    if (!g_snap_open) return -1;
    int px, py;
    if (!snap_box(&px, &py)) return -1;
    for (int z = 0; z < 6; z++) {
        int bx, by, bw, bh;
        snap_zone_rect(px, py, z, &bx, &by, &bw, &bh);
        if (inside(x, y, bx, by, bw, bh)) return z;
    }
    return -1;
}

static void draw_snap_flyout(void) {
    if (!g_snap_open) return;
    int px, py;
    if (!snap_box(&px, &py)) return;

    draw_popup_panel(px, py, SNAP_W, SNAP_H);
    gfx_text_bold(g_fb, g_stride, g_fbw, g_fbh, px + CC_PAD, py + 10, C_TEXT,
                  "Snap layout", -1);

    for (int z = 0; z < 6; z++) {
        int x, y, w, h;
        snap_zone_rect(px, py, z, &x, &y, &w, &h);
        int hot = (g_snap_sel == z);
        gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, x, y, w, h, 6,
                       hot ? 0x2A5CA8 : 0x1B2740);

        int ix = x + 8, iy = y + 7, iw = w - 16, ih = h - 14;
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, ix, iy, iw, ih, 0x0B1220);
        if (z < 2) {
            int hw = iw / 2;
            if (z == 0)
                gfx_fill(g_fb, g_stride, g_fbw, g_fbh, ix, iy, hw, ih,
                         hot ? g_accent_2 : 0x3B82F6);
            else
                gfx_fill(g_fb, g_stride, g_fbw, g_fbh, ix + hw, iy, iw - hw,
                         ih, hot ? g_accent_2 : 0x3B82F6);
        } else {
            int q = z - 2;
            int qw = (iw - 2) / 2, qh = (ih - 2) / 2;
            int cx = ix + (q & 1) * (qw + 2);
            int cy = iy + (q >> 1) * (qh + 2);
            gfx_fill(g_fb, g_stride, g_fbw, g_fbh, ix, iy, iw, ih, 0x111A28);
            gfx_fill(g_fb, g_stride, g_fbw, g_fbh, cx, cy, qw, qh,
                     hot ? g_accent_2 : 0x3B82F6);
        }
        gfx_rect(g_fb, g_stride, g_fbw, g_fbh, x, y, w, h,
                 hot ? 0x7FB2FF : 0x2C3E5C);
    }
}

static void snap_close(void) {
    if (!g_snap_open) return;
    int x, y;
    if (snap_box(&x, &y)) dirty_add(x - 4, y - 4, SNAP_W + 20, SNAP_H + 20);
    g_snap_open = 0;
    g_snap_sel = -1;
    g_snap_slot = -1;
    g_snap_hover_ms = 0;
}

static void snap_open_for(int slot) {
    if (slot < 0 || slot >= MAX_WINS || !g_wins[slot].used) return;
    if (g_snap_open && g_snap_slot == slot) return;
    snap_close();
    g_snap_slot = slot;
    g_snap_sel = -1;
    g_snap_open = 1;
    int x, y;
    if (snap_box(&x, &y)) dirty_add(x - 4, y - 4, SNAP_W + 20, SNAP_H + 20);
}

static void snap_apply(int slot, int zone) {
    if (slot < 0 || slot >= MAX_WINS || !g_wins[slot].used) return;
    win_t *w = &g_wins[slot];

    if (!w->maximized) {
        w->rx = w->x;
        w->ry = w->y;
        w->rw = w->w;
        w->rh = w->h;
    }

    int left = WIN_BORDER;
    int right = g_fbw - WIN_BORDER;
    int top = TITLE_H + WIN_BORDER;
    int bottom = work_h() - WIN_BORDER;
    int mx = (left + right) / 2, my = (top + bottom) / 2;
    int x0 = left, x1 = right, y0 = top, y1 = bottom;

    switch (zone) {
    case 0: x1 = mx; break;
    case 1: x0 = mx; break;
    case 2: x1 = mx; y1 = my; break;
    case 3: x0 = mx; y1 = my; break;
    case 4: x1 = mx; y0 = my; break;
    default: x0 = mx; y0 = my; break;
    }

    w->maximized = 1;
    w->x = x0;
    w->y = y0;
    w->w = x1 - x0;
    w->h = y1 - y0;
    clamp_win_rect(&w->x, &w->y, &w->w, &w->h);
    resize_reply(slot);
    focus_window(slot);
    g_need_compose = 1;
}

static void draw_taskbar_popups(void) {
    draw_preview();
    draw_task_menu();
    draw_calendar();
    draw_control_center();
    draw_net_flyout();
    draw_snap_flyout();
}

/* Repaint the areas the popups occupy (old and new). */
static void dirty_preview(void) {
    if (g_pv_x >= 0) {
        dirty_add(g_pv_x - 2, g_pv_y - 2, PREVIEW_W + 18, PREVIEW_H + 16);
        g_pv_x = -1;
        g_pv_y = -1;
    }
    int x, y;
    if (g_preview_shown && g_hover_task >= 0 && preview_box(&x, &y)) {
        dirty_add(x - 2, y - 2, PREVIEW_W + 18, PREVIEW_H + 16);
    }
}

static void dirty_popups(void) {
    dirty_preview();
    int x, y;
    if (g_taskmenu_open && taskmenu_box(&x, &y)) {
        dirty_add(x - 2, y - 2, TASK_MENU_W + 16,
                  3 * TASK_MENU_ITEM_H + 2 * TASK_MENU_PAD + 16);
    }
    if (g_cal_open) {
        calendar_box(&x, &y);
        dirty_add(x - 2, y - 2, CAL_W + 16, CAL_H + 16);
    }
    if (g_cc_open) cc_dirty();
    if (g_cc_open && g_net_flyout) netf_dirty();
}

static void close_taskbar_popups(void) {
    g_cal_open = 0;
    g_taskmenu_open = 0;
    g_taskmenu_slot = -1;
    g_taskmenu_sel = -1;
    g_preview_shown = 0;
    g_hover_task_ms = 0;
    g_cc_open = 0;
    g_cc_hover = -1;
    g_cc_drag = 0;
    g_net_flyout = 0;
    snap_close();
    dirty_popups();
}

/* ---- start menu ------------------------------------------------------ */

static int menu_items_y(int my) {
    return my + MENU_HEADER_H + 26;
}

#define MENU_POWER_H 28

static void menu_box(int *mx, int *my, int *mw, int *mh) {
    taskbar_layout();
    *mw = MENU_W;
    *mh = MENU_HEADER_H + 26 + APP_COUNT * MENU_ITEM_H + 40 + MENU_POWER_H;
    /* Windows 11 opens Start centred over its taskbar button. */
    *mx = g_start_x + g_start_w / 2 - *mw / 2;
    if (*mx + *mw > g_fbw - 8) *mx = g_fbw - 8 - *mw;
    if (*mx < 8) *mx = 8;
    *my = work_h() - *mh - 10;
    if (*my < 8) *my = 8;
}

/* Power control in the Start footer: one action button plus a caret, sitting
 * where Windows puts its power glyph. The artwork and the flyout are ours. */
#define PWR_BTN_W    150
#define PWR_BTN_H    26
#define PWR_ARROW_W  24
#define PWR_ITEM_H   30
#define PWR_FLYOUT_W 176

static void menu_power_box(int *bx, int *by, int *bw, int *bh) {
    int mx, my, mw, mh;
    menu_box(&mx, &my, &mw, &mh);
    *bw = PWR_BTN_W;
    *bh = PWR_BTN_H;
    *by = my + mh - 34 - MENU_POWER_H + 3;
    *bx = mx + mw - 16 - *bw;
}

/* Right-hand slice of the same button that opens the flyout. */
static void menu_power_arrow_box(int *bx, int *by, int *bw, int *bh) {
    int x, y, w, h;
    menu_power_box(&x, &y, &w, &h);
    *bx = x + w - PWR_ARROW_W;
    *by = y;
    *bw = PWR_ARROW_W;
    *bh = h;
}

/* The flyout grows upward out of the button and keeps its right edge aligned,
 * so it never spills past the Start panel. */
static void power_flyout_box(int *fx, int *fy, int *fw, int *fh) {
    int bx, by, bw, bh;
    menu_power_box(&bx, &by, &bw, &bh);
    *fw = PWR_FLYOUT_W;
    *fh = 8 + 3 * PWR_ITEM_H + 8;
    *fx = bx + bw - *fw;
    *fy = by - 6 - *fh;
}

static int power_item_at(int x, int y) {
    if (!g_power_flyout) return -1;
    int fx, fy, fw, fh;
    power_flyout_box(&fx, &fy, &fw, &fh);
    if (x < fx + 6 || x >= fx + fw - 6) return -1;
    int y0 = fy + 8;
    if (y < y0 || y >= y0 + 3 * PWR_ITEM_H) return -1;
    return (y - y0) / PWR_ITEM_H;
}

static void draw_power_flyout(void) {
    int fx, fy, fw, fh;
    power_flyout_box(&fx, &fy, &fw, &fh);

    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, fx + 6, fy + 7, fw, fh,
                   10, 0x020409);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, fx, fy, fw, fh, 8, 0x33456A);
    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, fx + 1, fy + 1, fw - 2,
                   fh - 2, 7, 0x141D2C);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, fx + 12, fy + 1, fw - 24, 1,
             lerp_color(0x141D2C, 0xFFFFFF, 26));

    static const char *const items[3] = { "Sleep", "Shut down", "Restart" };
    int y0 = fy + 8;
    for (int i = 0; i < 3; i++) {
        int iy = y0 + i * PWR_ITEM_H;
        int hot = (g_power_sel == i);
        u32 tile = hot ? C_MENU_HOVER : 0x141D2C;
        u32 fg = hot ? 0xFFFFFF : 0xC5D3E6;
        if (hot)
            gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, fx + 5, iy + 1,
                           fw - 10, PWR_ITEM_H - 2, 5, C_MENU_HOVER);

        int icx = fx + 21, icy = iy + PWR_ITEM_H / 2;
        if (i == 0) {
            /* Crescent moon for Sleep. */
            gfx_disc(g_fb, g_stride, g_fbw, g_fbh, icx, icy, 6, fg);
            gfx_disc(g_fb, g_stride, g_fbw, g_fbh, icx + 4, icy - 2, 5, tile);
        } else {
            gfx_power_glyph(g_fb, g_stride, g_fbw, g_fbh, icx, icy, 6, fg,
                            tile);
        }
        gfx_text_bold(g_fb, g_stride, g_fbw, g_fbh, fx + 38, iy + 7, fg,
                      items[i], -1);
    }
}

static void enter_sleep(void);

/* 0 = Sleep, 1 = Shut down, 2 = Restart. Shut down and Restart are terminal:
 * the kernel resets or powers the machine off inside the syscall. */
static void power_exec(int sel) {
    g_power_flyout = 0;
    g_power_sel = -1;
    g_menu_power_sel = -1;
    g_menu_open = 0;
    g_need_menu = 0;

    if (sel == 0) { enter_sleep(); return; }
    mc_power(sel == 2 ? MCUBE_POWER_REBOOT : MCUBE_POWER_OFF);
}

/* Suspend to idle. The kernel has no ACPI S3 entry point yet, so Sleep parks
 * the shell behind a blank screen and leaves the timer running; any key or
 * pointer event resumes and repaints. */
static void enter_sleep(void) {
    mcube_time_t ts;
    g_sleep_since_ms = (mc_time(&ts) == 0) ? ts.uptime_ms : 0;
    g_sleep_need_release = (g_buttons != 0) ? 1 : 0;
    g_sleeping = 1;
    g_need_compose = 0;
    g_need_taskbar = 0;
    g_need_tray = 0;
    g_need_menu = 0;
    g_dirty_valid = 0;
    g_cursor_saved = 0;
    g_pr_x0 = g_pr_x1 = 0;
    g_pr_y0 = g_pr_y1 = 0;
    if (g_fb)
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, 0, 0, g_fbw, g_fbh, 0x000000);
    pres_add(0, 0, g_fbw, g_fbh);
    present();
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
                   20, g_accent);
    draw_mcube_mark(g_fb, g_stride, g_fbw, g_fbh, mx + 39, my + 37, 22,
                    0xEAF2FF);

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
                     3, MENU_ITEM_H - 16, g_accent_2);
        }
        draw_icon(g_fb, g_stride, g_fbw, g_fbh, mx + 20, iy + 10, 20,
                  g_icon_colors[i % (int)(sizeof(g_icon_colors) /
                                          sizeof(g_icon_colors[0]))],
                  g_app_names[i]);
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, mx + 52, iy + 12,
                 i == g_menu_sel ? 0xFFFFFF : C_TEXT, g_app_names[i], -1);
    }

    int fy = my + mh - 34 - MENU_POWER_H;
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, mx + 14, fy, mw - 28, 1, 0x23324A);

    gfx_text_bold(g_fb, g_stride, g_fbw, g_fbh, mx + 18, fy + 9, 0x7E90AC,
                  "POWER", -1);

    /* One action button with a caret on its right edge, as on Windows 11. */
    int bx, by, bw, bh;
    menu_power_box(&bx, &by, &bw, &bh);
    int hot = (g_menu_power_sel == 1);
    u32 tile = hot ? 0x2A3F5C : 0x18233A;
    u32 fg = hot ? 0xFFFFFF : 0xC5D3E6;

    gfx_round_fill(g_fb, g_stride, g_fbw, g_fbh, bx, by, bw, bh, 6, tile);
    if (hot)
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, bx + 2, by + 2, bw - 4, 1,
                 0x3E5C86);

    int ax, ay, aw, ah;
    menu_power_arrow_box(&ax, &ay, &aw, &ah);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, ax, ay + 4, 1, ah - 8, 0x2A3A55);

    int gcx = bx + 19, gcy = by + bh / 2;
    gfx_power_glyph(g_fb, g_stride, g_fbw, g_fbh, gcx, gcy, 6, fg, tile);
    gfx_text_bold(g_fb, g_stride, g_fbw, g_fbh, gcx + 13, by + 5, fg,
                  "Shut down", -1);
    gfx_chevron(g_fb, g_stride, g_fbw, g_fbh, ax + aw / 2, ay + ah / 2,
                !g_power_flyout, fg);

    int hy = my + mh - 26;
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, mx + 18, hy,
             0x7E90AC, "F1 menu  Alt+Tab  Win+D", -1);

    if (g_power_flyout) draw_power_flyout();
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
                   lerp_color(C_SWITCH, g_accent, 90));
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
                           pw - 20, item_h - 8, 7, g_accent_dark);
            gfx_fill(g_fb, g_stride, g_fbw, g_fbh, px + 10, y + 11,
                     3, item_h - 24, g_accent_2);
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

/* Repaint the whole scene and copy it out to video memory. px/py/pw/ph bound
 * the rectangle that actually has to reach the screen. */
static void compose(int px, int py, int pw, int ph) {
    /* No cursor_restore() here: this pass repaints the scene itself, so a
     * clipped region covers the cursor within it. Restoring a saved block
     * would blit stale pixels for a cursor that sits outside the region. */
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
    draw_taskbar_popups();
    cursor_draw();

    pres_add(px, py, pw, ph);

    g_need_compose = 0;
    g_need_taskbar = 0;
    g_need_tray = 0;
    g_need_menu = 0;
    g_sys_dirty = 0;
    g_cursor_moved = 0;
}

/* Repaint just one rectangle of the desktop. The scene is still walked in
 * full z-order, but every write is clipped, so a window drag no longer
 * rewrites the whole framebuffer (and the host window) on every mouse move. */
static void compose_region(int x, int y, int w, int h) {
    if (g_menu_open || g_switcher_open) {
        clip_reset();
        compose(0, 0, g_fbw, g_fbh);
        return;
    }
    int tb = g_need_taskbar;
    int tr = g_need_tray;
    int menu = g_need_menu;
    int sys = g_sys_dirty;

    /* compose() clears every dirty flag, but a clipped pass only paints this
     * region; windows outside it still owe the screen their update. */
    int pending[MAX_WINS];
    for (int i = 0; i < MAX_WINS; i++) pending[i] = g_wins[i].dirty;

    clip_set(x, y, w, h);
    compose(x, y, w, h);
    clip_reset();

    for (int i = 0; i < MAX_WINS; i++) {
        if (!pending[i] || g_wins[i].dirty) continue;
        int fx, fy, fw, fh;
        frame_box(&g_wins[i], &fx, &fy, &fw, &fh);
        if (!rects_intersect(fx, fy, fw, fh, x, y, w, h)) g_wins[i].dirty = 1;
    }

    /* Clipped draws do not cover updates outside the region (taskbar clock,
     * status card), and the region compositor restores their flags. */
    g_need_taskbar = tb;
    g_need_tray = tr;
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
        if (window_is_covered(slot)) {
            dirty_window_exposed(slot);
            w->dirty = 0;
            continue;
        }
        if (!any) {
            cursor_restore();
            any = 1;
        }
        blit_surface(w);
        pres_add(w->x - 2, w->y - TITLE_H - 2, w->w + 4, w->h + TITLE_H + 4);
        w->dirty = 0;
    }
    if (any) cursor_draw();
}

static void redraw_taskbar(void) {
    cursor_restore();
    draw_taskbar();
    cursor_draw();
    pres_add(0, work_h(), g_fbw, TASKBAR_H);
}

/* Only the clock changed: repaint the tray strip instead of the whole bar. The
 * full-width version used to flash the bottom of the screen once per second. */
static void redraw_tray(void) {
    cursor_restore();
    int x = g_tray_x - 6;
    if (x < 0) x = 0;
    int w = g_fbw - x;
    int y = work_h();
    gfx_vgradient(g_fb, g_stride, g_fbw, g_fbh, x, y, w, TASKBAR_H,
                  C_TASKBAR_TOP, C_TASKBAR_BOT);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x, y, w, 1, 0x223149);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, x, g_fbh - 1, w, 1, 0x05080E);
    draw_tray();
    draw_show_desktop();
    cursor_draw();
    pres_add(x, y, w, TASKBAR_H);
}

static void redraw_menu(void) {
    if (!g_menu_open) return;
    cursor_restore();
    draw_menu();
    cursor_draw();

    int mx, my, mw, mh;
    menu_box(&mx, &my, &mw, &mh);
    pres_add(mx - 12, my - 12, mw + 24, mh + 24);
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
    notif_push("Application started", w->title);
}

/* ---- commands from applications ------------------------------------- */

static int read_bytes(int fd, void *buf, size_t len) {
    u8 *p = (u8 *)buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n == 0) return -1;
        if (errno != EAGAIN && errno != EINTR) return -1;
        sleep_ms(1);
    }
    return 0;
}

static int read_discard(int fd, size_t len) {
    u8 tmp[64];
    while (len) {
        size_t chunk = len > sizeof(tmp) ? sizeof(tmp) : len;
        ssize_t n = read(fd, tmp, chunk);
        if (n > 0) {
            len -= (size_t)n;
            continue;
        }
        if (n == 0) return -1;
        if (errno != EAGAIN && errno != EINTR) return -1;
        sleep_ms(1);
    }
    return 0;
}

/* Payload of the GUI_CMD_IMAGE currently being handled. */
static u32 *g_img_stage;
static u32 g_img_stage_px;

static int read_cmd(win_t *w, gui_cmd_t *cmd) {
    if (read_bytes(w->cmd_fd, cmd, sizeof(*cmd)) != 0) return -1;

    /* GUI_CMD_IMAGE and GUI_CMD_IMAGE_PUT send their pixels as a payload right
     * after the fixed-size header. Consume them here so the command stream
     * stays framed; the pixels are then copied into the window's cache slot by
     * handle_cmd. */
    if (cmd->type == GUI_CMD_IMAGE || cmd->type == GUI_CMD_IMAGE_PUT) {
        u32 bytes = cmd->len;
        if (!g_img_stage) {
            g_img_stage = (u32 *)malloc((size_t)GUI_IMG_MAX_PX * 4u);
            if (g_img_stage) g_img_stage_px = GUI_IMG_MAX_PX;
        }
        if (g_img_stage && bytes <= g_img_stage_px * 4u) {
            if (read_bytes(w->cmd_fd, g_img_stage, bytes) != 0) return -1;
        } else {
            if (read_discard(w->cmd_fd, bytes) != 0) return -1;
            cmd->len = 0;               /* too large: handle_cmd drops it */
        }
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

    case GUI_CMD_IMAGE: {
        /* Copy the staged payload into the window cache slot. Nothing is
         * painted here; gui_image_draw() paints it on each repaint. */
        int id = cmd->x;
        if (!g_img_stage || cmd->len == 0) break;
        if (id < 0 || id >= GUI_IMG_SLOTS) break;
        if (cmd->w <= 0 || cmd->h <= 0) break;
        if ((u64)cmd->w * (u64)cmd->h > (u64)GUI_IMG_MAX_PX) break;
        if (cmd->len < (u32)((u64)cmd->w * (u64)cmd->h * 4u)) break;

        win_img_t *im = &w->imgs[id];
        if (!im->px) im->px = (u32 *)malloc((size_t)GUI_IMG_MAX_PX * 4u);
        if (!im->px) break;
        memcpy(im->px, g_img_stage, (size_t)((u64)cmd->w * (u64)cmd->h * 4u));
        im->w = cmd->w;
        im->h = cmd->h;
        im->used = 1;
        break;
    }

    case GUI_CMD_IMAGE_DRAW: {
        int id = cmd->w;
        if (!w->surface || id < 0 || id >= GUI_IMG_SLOTS) break;
        win_img_t *im = &w->imgs[id];
        if (!im->used || !im->px) break;

        int src_y = cmd->h;
        if (src_y < 0) src_y = 0;
        if (src_y >= im->h) break;
        int rows = (int)cmd->flags;
        if (rows <= 0 || rows > im->h - src_y) rows = im->h - src_y;

        gfx_blit(w->surface, w->sw, w->sw, w->sh, cmd->x, cmd->y,
                 im->w, rows, im->px + (u64)src_y * (u64)im->w);
        w->dirty = 1;
        break;
    }

    case GUI_CMD_IMAGE_PUT: {
        /* Copy a w*h region into the slot at (color=x, y=y). The slot grows to
         * cover the written area so an atlas can be filled piece by piece. */
        int id = cmd->x;
        if (!g_img_stage || id < 0 || id >= GUI_IMG_SLOTS) break;
        if (cmd->len == 0 || cmd->w <= 0 || cmd->h <= 0) break;
        int rx = (int)cmd->color;
        int ry = cmd->y;
        if (rx < 0 || ry < 0) break;
        if (cmd->len < (u32)((u64)cmd->w * (u64)cmd->h * 4u)) break;

        int nw = rx + cmd->w;
        int nh = ry + cmd->h;
        if ((u64)nw * (u64)nh > (u64)GUI_IMG_MAX_PX) break;

        win_img_t *im = &w->imgs[id];
        if (!im->px) im->px = (u32 *)malloc((size_t)GUI_IMG_MAX_PX * 4u);
        if (!im->px) break;

        /* Widen the slot first if needed; existing rows must shift right by the
         * difference, and the source rows are copied bottom-up to avoid
         * overwriting data that has not moved yet. */
        if (nw > im->w && im->used) {
            for (int y = im->h - 1; y >= 0; y--) {
                memmove(im->px + (u64)y * (u64)nw,
                        im->px + (u64)y * (u64)im->w, (size_t)im->w * 4u);
            }
        }
        im->w = nw;
        im->h = nh;
        im->used = 1;

        const u32 *src = g_img_stage;
        for (int y = 0; y < cmd->h; y++) {
            memcpy(im->px + (u64)(ry + y) * (u64)im->w + (u64)rx,
                   src + (u64)y * (u64)cmd->w, (size_t)cmd->w * 4u);
        }
        break;
    }

    case GUI_CMD_IMAGE_BLIT: {
        /* Paint the sub-rectangle (color=sx, h=sy, len=sw, flags=sh) of slot w
         * at (x,y). One row at a time keeps clipping in gfx_blit. */
        int id = cmd->w;
        if (!w->surface || id < 0 || id >= GUI_IMG_SLOTS) break;
        win_img_t *im = &w->imgs[id];
        if (!im->used || !im->px) break;

        int sx = (int)cmd->color;
        int sy = (int)cmd->h;
        int sw = (int)cmd->len;
        int sh = (int)cmd->flags;
        if (sx < 0 || sy < 0 || sw <= 0 || sh <= 0) break;
        if (sx + sw > im->w || sy + sh > im->h) break;

        for (int r = 0; r < sh; r++) {
            gfx_blit(w->surface, w->sw, w->sw, w->sh, cmd->x, cmd->y + r,
                     sw, 1, im->px + (u64)(sy + r) * (u64)im->w + (u64)sx);
        }
        w->dirty = 1;
        break;
    }

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

    case GUI_CMD_KILL: {
        int pid = cmd->x;
        if (pid <= 0) break;
        /* Kill the window through the normal close path so its pipes are
         * closed and the child is reaped; otherwise the window, its taskbar
         * button and the task-list row all survive as zombies. */
        int target = -1;
        for (int i = 0; i < MAX_WINS; i++) {
            if (g_wins[i].used && (int)g_wins[i].pid == pid) { target = i; break; }
        }
        if (target >= 0) {
            close_window(target);
        } else {
            kill((pid_t)pid, 15);
            int status = 0;
            waitpid((pid_t)pid, &status, 0);
        }
        break;
    }

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

/* Map a resize border hit to the cursor Windows would show there. */
static int zone_cursor(int zone) {
    int left = (zone & RZ_LEFT) != 0, right = (zone & RZ_RIGHT) != 0;
    int top = (zone & RZ_TOP) != 0, bottom = (zone & RZ_BOTTOM) != 0;
    if ((left && top) || (right && bottom)) return CUR_SIZE_NWSE;
    if ((right && top) || (left && bottom)) return CUR_SIZE_NESW;
    if (left || right) return CUR_SIZE_WE;
    if (top || bottom) return CUR_SIZE_NS;
    return CUR_ARROW;
}

/* Which pointer shape belongs under (x, y) right now. Windows only changes the
 * arrow into a resize cursor over a border the user can actually drag. */
static int cursor_kind_at(int x, int y) {
    if (g_resize_slot >= 0) return zone_cursor(g_resize_zone);
    if (g_drag_slot >= 0) return CUR_ARROW;
    if (g_menu_open || g_switcher_open || g_taskmenu_open || g_cal_open ||
        g_cc_open || g_snap_open)
        return CUR_ARROW;
    if (y >= work_h()) return CUR_ARROW;

    for (int i = g_zcount - 1; i >= 0; i--) {
        int slot = g_zorder[i];
        win_t *w = &g_wins[slot];
        if (!w->used || w->minimized) continue;
        int fx, fy, fw, fh;
        frame_box(w, &fx, &fy, &fw, &fh);
        if (!inside(x, y, fx, fy, fw, fh)) continue;
        return zone_cursor(resize_zone(w, x, y));
    }
    return CUR_ARROW;
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
    taskbar_layout();
    if (inside(x, y, g_start_x, g_start_y, g_start_w, g_start_h)) return -2;

    int ty = task_button_y();
    if (inside(x, y, g_pin_x, ty, TB_BTN, TB_BTN)) return PIN_BROWSER;
    for (int i = 0; i < g_task_n; i++) {
        if (inside(x, y, g_task_x[i], ty, g_task_w[i], TB_BTN)) {
            return g_task_slot[i];
        }
    }
    return -1;
}

/* Start and its power flyout always close together; leaving the flyout flag
 * set would make the next open come back with a stale submenu. */
static void close_menu(void) {
    g_menu_open = 0;
    g_power_flyout = 0;
    g_power_sel = -1;
    g_menu_power_sel = -1;
    g_need_compose = 1;
}

static void toggle_menu(void) {
    if (!g_menu_open) close_taskbar_popups();
    g_menu_open = !g_menu_open;
    if (g_menu_open) g_menu_sel = 0;
    g_power_flyout = 0;
    g_power_sel = -1;
    g_menu_power_sel = -1;
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
    g_preview_shown = 0;
    g_hover_task_ms = now_ms();
    if (!w->minimized && g_focus == slot) minimize_window(slot);
    else focus_window(slot);
}

/* Pinned browser button: bring the existing window forward instead of starting
 * a second copy, which is what a pinned taskbar entry means everywhere else. */
static void open_pinned_browser(void) {
    int slot = browser_running();
    if (slot >= 0) {
        win_t *w = &g_wins[slot];
        w->minimized = 0;
        w->desk_hidden = 0;
        focus_window(slot);
    } else {
        spawn_app("/bin/browser.lgy");
    }
    g_menu_open = 0;
    close_taskbar_popups();
    g_need_compose = 1;
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
    close_taskbar_popups();
    g_need_compose = 1;
}

/* ---- input ----------------------------------------------------------- */

/* Hover feedback only touches the widget itself, so mark exactly that area
 * instead of asking for a full-screen compose on every pointer move. */
static void dirty_task_button(int task) {
    taskbar_layout();
    if (task == -2) {
        dirty_add(g_start_x, g_start_y, g_start_w, g_start_h);
        return;
    }
    if (task == PIN_BROWSER) {
        dirty_add(g_pin_x, task_button_y(), TB_BTN, g_fbh - task_button_y());
        return;
    }
    if (task < 0) return;
    int ty = task_button_y();
    for (int i = 0; i < g_task_n; i++) {
        if (g_task_slot[i] != task) continue;
        /* Include the running-app pill drawn just below the icon. */
        dirty_add(g_task_x[i], ty, g_task_w[i], g_fbh - ty);
        return;
    }
}

static void dirty_desk_icon(int idx) {
    if (idx < 0 || idx >= DESK_COUNT) return;
    int bx, by;
    desk_box(idx, &bx, &by);
    dirty_add(bx, by, DESK_BOX_W, DESK_BOX_H);
}

static u64 now_ms(void) {
    mcube_time_t ts;
    return mc_time(&ts) == 0 ? ts.uptime_ms : 0;
}

/* Painted-frame statistics, reported to the serial log so repaint cost is
 * measurable from the outside. */
static int g_paint_n;
static u64 g_paint_sum, g_paint_max;

static void paint_stat(u64 ms) {
    g_paint_n++;
    g_paint_sum += ms;
    if (ms > g_paint_max) g_paint_max = ms;
    if (g_paint_n < 300) return;
    /* Counted only when something was actually painted (see main loop). */
    u64 tenths = g_paint_sum * 10 / (u64)g_paint_n;
    printf("USER_DESKTOP_PAINT: %d frames, avg %u.%u ms, max %u ms\n",
           g_paint_n, (unsigned)(tenths / 10), (unsigned)(tenths % 10),
           (unsigned)g_paint_max);
    g_paint_n = 0;
    g_paint_sum = 0;
    g_paint_max = 0;
}

static void update_hover(void) {
    int new_task = taskbar_hit(g_mx, g_my);
    int new_slot = -1, new_btn = 0;

    int new_tray = tray_hit(g_mx, g_my);
    if (new_tray != g_tray_hover) {
        g_tray_hover = new_tray;
        g_need_taskbar = 1;
    }

    if (g_taskmenu_open) {
        int sel = taskmenu_item_at(g_mx, g_my);
        if (sel != g_taskmenu_sel) {
            g_taskmenu_sel = sel;
            dirty_popups();
        }
    }

    int new_desk = -1;
    if (!g_menu_open && !g_switcher_open && g_my < work_h() &&
        window_at(g_mx, g_my) < 0) {
        new_desk = desktop_icon_at(g_mx, g_my);
    }
    if (new_desk != g_desk_hover) {
        dirty_desk_icon(g_desk_hover);
        dirty_desk_icon(new_desk);
        g_desk_hover = new_desk;
    }

    if (g_menu_open) {
        /* The power flyout floats above the app list; while the pointer is over
         * it the list must not keep claiming a hover, or the highlight jumps
         * between the two layers. */
        int in_flyout = 0;
        if (g_power_flyout) {
            int fx, fy, fw, fh;
            power_flyout_box(&fx, &fy, &fw, &fh);
            in_flyout = inside(g_mx, g_my, fx, fy, fw, fh);
        }
        int idx = in_flyout ? -1 : menu_item_at(g_mx, g_my);
        if (idx >= 0 && idx < APP_COUNT && idx != g_menu_sel) {
            g_menu_sel = idx;
            g_menu_power_sel = -1;
            g_need_menu = 1;
        }

        int psel = -1;
        int bx, by, bw, bh;
        menu_power_box(&bx, &by, &bw, &bh);
        if (inside(g_mx, g_my, bx, by, bw, bh)) psel = 1;
        if (psel != g_menu_power_sel) {
            g_menu_power_sel = psel;
            g_need_menu = 1;
        }

        if (g_power_flyout) {
            int fsel = power_item_at(g_mx, g_my);
            if (fsel != g_power_sel) {
                g_power_sel = fsel;
                g_need_menu = 1;
            }
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
        dirty_task_button(g_hover_task);
        dirty_task_button(new_task);
        g_hover_task = new_task;
        g_hover_task_ms = now_ms();
        if (g_preview_shown) {
            g_preview_shown = 0;
            dirty_preview();
        }
    }
    if (new_slot != g_hover_slot || new_btn != g_hover_btn) {
        if (g_hover_slot >= 0 && g_hover_slot < MAX_WINS)
            dirty_frame(&g_wins[g_hover_slot]);
        if (new_slot >= 0) dirty_frame(&g_wins[new_slot]);
        g_hover_slot = new_slot;
        g_hover_btn = new_btn;
    }

    /* Control-centre hover: one repaint of the panel when the target changes. */
    if (g_cc_open) {
        int h = cc_at(g_mx, g_my);
        if (h != g_cc_hover) {
            g_cc_hover = h;
            cc_dirty();
        }
    } else if (g_cc_hover != -1) {
        g_cc_hover = -1;
    }

    /* Snap layout picker: highlight the tile under the pointer and drop the
     * flyout as soon as the pointer leaves both it and the maximise button. */
    if (g_snap_open) {
        int px, py;
        int over_flyout = snap_box(&px, &py) &&
                          inside(g_mx, g_my, px, py, SNAP_W, SNAP_H);
        int over_btn = (g_hover_btn == BTN_MAX && g_hover_slot == g_snap_slot);
        int z = snap_zone_at(g_mx, g_my);
        if (z != g_snap_sel) {
            g_snap_sel = z;
            if (snap_box(&px, &py))
                dirty_add(px - 4, py - 4, SNAP_W + 20, SNAP_H + 20);
        }
        if (!over_flyout && !over_btn) snap_close();
    }

    /* Arm the 400 ms dwell timer that opens the picker. */
    if (!g_snap_open && g_hover_btn == BTN_MAX && g_hover_slot >= 0) {
        if (g_snap_hover_ms == 0) g_snap_hover_ms = now_ms();
    } else if (g_hover_btn != BTN_MAX) {
        g_snap_hover_ms = 0;
    }
}

static void mouse_press(int x, int y) {
    taskbar_layout();

    if (g_switcher_open) {
        g_switcher_open = 0;
        g_need_compose = 1;
        return;
    }

    if (g_menu_open) {
        if (inside(x, y, g_start_x, g_start_y, g_start_w, g_start_h)) {
            close_menu();
            return;
        }

        /* The flyout floats over the app list, so it gets first refusal. */
        if (g_power_flyout) {
            int sel = power_item_at(x, y);
            if (sel >= 0) {
                power_exec(sel);
                return;
            }
            int fx, fy, fw, fh;
            power_flyout_box(&fx, &fy, &fw, &fh);
            if (!inside(x, y, fx, fy, fw, fh)) {
                g_power_flyout = 0;
                g_power_sel = -1;
                g_need_menu = 1;
            }
            return;
        }

        int ax, ay, aw, ah;
        menu_power_arrow_box(&ax, &ay, &aw, &ah);
        if (inside(x, y, ax, ay, aw, ah)) {
            g_power_flyout = 1;
            g_power_sel = 1;
            g_menu_sel = -1;
            g_need_menu = 1;
            return;
        }

        /* Main face of the button is the action itself: Shut down. */
        int bx, by, bw, bh;
        menu_power_box(&bx, &by, &bw, &bh);
        if (inside(x, y, bx, by, bw, bh)) {
            power_exec(1);
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
        close_menu();
    }

    /* The control centre floats over everything, so it takes the click first. */
    if (g_cc_open) {
        /* The network list hangs off its tile and gets first refusal. */
        if (g_net_flyout) {
            int nh = netf_at(x, y);
            if (nh == NET_HIT_WIFI) {
                g_qs_wifi = !g_qs_wifi;
                cc_dirty();
                netf_dirty();
                g_need_taskbar = 1;
                return;
            }
            if (nh == NET_HIT_SETTINGS) {
                spawn_app("/bin/settings.lgy");
                return;
            }
            int nx, ny;
            netf_box(&nx, &ny);
            if (inside(x, y, nx, ny, NETF_W, NETF_H)) return;

            /* Any other click dismisses the list. The same click still lands on
             * the control centre, except on the Wi-Fi tile where pressing it
             * again is what closes the list. */
            int px0, py0, tx, ty0, tw, th;
            cc_box(&px0, &py0);
            cc_tile_rect(px0, py0, 0, &tx, &ty0, &tw, &th);
            g_net_flyout = 0;
            netf_dirty();
            if (inside(x, y, tx, ty0, tw, th)) return;
        }

        int hit = cc_at(x, y);
        if (hit == CC_HIT_CLEAR) {
            cc_clear_notifs();
            g_need_taskbar = 1;
            return;
        }
        if (hit == CC_HIT_DND) {
            g_dnd = !g_dnd;
            cc_dirty();
            return;
        }
        if (hit >= 0 && hit < 4) {
            cc_toggle_tile(hit);
            g_need_taskbar = 1;
            return;
        }
        if (hit == CC_HIT_VOL) {
            g_cc_drag = 1;
            cc_set_slider(1, x);
            cc_dirty();
            return;
        }
        if (hit == CC_HIT_BRI) {
            g_cc_drag = 2;
            cc_set_slider(2, x);
            cc_dirty();
            return;
        }
        int px, py;
        cc_box(&px, &py);
        if (inside(x, y, px, py, CC_W, CC_H)) return;
        if (tray_hit(x, y) != 1) {
            g_cc_open = 0;
            g_cc_hover = -1;
            cc_dirty();
            g_need_taskbar = 1;
            return;
        }
    }

    /* Snap layouts sit above the window they belong to. */
    if (g_snap_open) {
        int zone = snap_zone_at(x, y);
        int slot = g_snap_slot;
        if (zone >= 0) {
            snap_close();
            snap_apply(slot, zone);
            return;
        }
        int px, py;
        if (snap_box(&px, &py) && inside(x, y, px, py, SNAP_W, SNAP_H))
            return;
        snap_close();
    }

    /* Flyouts own the next click: a hit runs its action, anything else just
     * dismisses the flyout (as on Windows). */
    if (g_taskmenu_open) {
        int sel = taskmenu_item_at(x, y);
        int slot = g_taskmenu_slot;
        dirty_popups();
        g_taskmenu_open = 0;
        g_taskmenu_sel = -1;
        g_taskmenu_slot = -1;
        if (sel >= 0 && slot >= 0 && slot < MAX_WINS && g_wins[slot].used) {
            if (sel == 0) focus_window(slot);
            else if (sel == 1) minimize_window(slot);
            else close_window(slot);
        }
        return;
    }

    if (g_cal_open) {
        int bx, by;
        calendar_box(&bx, &by);
        int in_cal = inside(x, y, bx, by, CAL_W, CAL_H);
        int on_clock = tray_hit(x, y) == 3;
        dirty_popups();
        g_cal_open = 0;
        if (in_cal || on_clock) {
            g_need_taskbar = 1;
            return;
        }
    }

    if (g_preview_shown) {
        int px, py;
        if (preview_box(&px, &py) &&
            inside(x, y, px, py, PREVIEW_W, PREVIEW_H)) {
            int slot = g_hover_task;
            dirty_preview();
            g_preview_shown = 0;
            if (slot >= 0 && slot < MAX_WINS && g_wins[slot].used)
                focus_window(slot);
            return;
        }
        dirty_preview();
        g_preview_shown = 0;
    }

    int task = taskbar_hit(x, y);
    if (task == -2) {
        toggle_menu();
        return;
    }
    if (task == PIN_BROWSER) {
        open_pinned_browser();
        return;
    }
    if (task >= 0) {
        taskbar_click(task);
        return;
    }

    int tray = tray_hit(x, y);
    if (tray == 1) {
        cc_toggle();
        return;
    }
    if (tray == 3) {
        g_cal_open = !g_cal_open;
        if (g_cal_open) {
            g_taskmenu_open = 0;
            g_taskmenu_slot = -1;
            g_preview_shown = 0;
            g_menu_open = 0;
            dirty_popups();
        } else {
            dirty_popups();
        }
        g_need_taskbar = 1;
        return;
    }
    if (tray == 2) {
        show_desktop();
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

static void mouse_right_press(int x, int y) {
    if (g_drag_slot >= 0 || g_resize_slot >= 0) return;

    int task = taskbar_hit(x, y);
    if (task >= 0) {
        if (g_cal_open) dirty_popups();
        g_cal_open = 0;
        g_menu_open = 0;
        g_preview_shown = 0;
        g_taskmenu_open = 1;
        g_taskmenu_slot = task;
        g_taskmenu_sel = -1;
        dirty_popups();
        return;
    }

    if (g_taskmenu_open) {
        dirty_popups();
        g_taskmenu_open = 0;
        g_taskmenu_slot = -1;
        g_taskmenu_sel = -1;
    }
    if (g_cal_open) {
        dirty_popups();
        g_cal_open = 0;
    }
    if (g_menu_open) {
        g_menu_open = 0;
        g_need_compose = 1;
    }
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

    /* Escape backs out of the shell-level popups before a focused app sees it. */
    if (code == 27) {
        if (g_snap_open) {
            snap_close();
            g_need_compose = 1;
            return;
        }
        if (g_cc_open) {
            if (g_net_flyout) {
                g_net_flyout = 0;
                netf_dirty();
                return;
            }
            g_cc_open = 0;
            g_cc_hover = -1;
            g_need_taskbar = 1;
            cc_dirty();
            return;
        }
    }

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
    if ((g_mods & MOD_WIN) && (code == 'a' || code == 'A')) {
        cc_toggle();
        return;
    }

    if (code == INPUT_KEY_F1) {
        toggle_menu();
        return;
    }

    if (g_menu_open) {
        if (g_power_flyout) {
            if (code == INPUT_KEY_UP) {
                g_power_sel = (g_power_sel <= 0) ? 2 : g_power_sel - 1;
                g_need_menu = 1;
            } else if (code == INPUT_KEY_DOWN) {
                g_power_sel = (g_power_sel < 0 || g_power_sel >= 2) ? 0
                                                                    : g_power_sel + 1;
                g_need_menu = 1;
            } else if (code == '\n') {
                power_exec(g_power_sel < 0 ? 1 : g_power_sel);
            } else if (code == 27) {
                g_power_flyout = 0;
                g_power_sel = -1;
                g_need_menu = 1;
            }
            return;
        }
        if (code == INPUT_KEY_UP) {
            if (g_menu_sel <= 0) {
                g_menu_sel = -1;            /* step down into the footer */
                g_menu_power_sel = 1;
            } else {
                g_menu_sel--;
            }
            g_need_menu = 1;
        } else if (code == INPUT_KEY_DOWN) {
            if (g_menu_sel < 0) g_menu_sel = 0;
            else g_menu_sel = (g_menu_sel + 1) % APP_COUNT;
            g_menu_power_sel = -1;
            g_need_menu = 1;
        } else if (code == INPUT_KEY_HOME) {
            g_menu_sel = 0;
            g_menu_power_sel = -1;
            g_need_menu = 1;
        } else if (code == INPUT_KEY_END) {
            g_menu_sel = APP_COUNT - 1;
            g_menu_power_sel = -1;
            g_need_menu = 1;
        } else if (code == '\t') {
            /* Tab walks between the app list and the power button. */
            int on_power = g_menu_power_sel < 0;
            g_menu_power_sel = on_power ? 1 : -1;
            g_menu_sel = on_power ? -1 : 0;
            g_need_menu = 1;
        } else if (code == '\n') {
            if (g_menu_power_sel == 1) {
                g_power_flyout = 1;
                g_power_sel = 1;
                g_need_menu = 1;
            } else {
                launch_selected();
            }
        } else if (code == 27) {
            close_menu();
        }
        return;
    }

    if (code == INPUT_KEY_F9) {
        cycle_focus();
        return;
    }

    /* Alt and Win chords belong to the shell; Ctrl chords pass through so
     * applications can implement their own shortcuts (browser tabs, editing). */
    if (g_mods & (MOD_ALT | MOD_WIN)) return;

    if (g_focus >= 0 && g_focus < MAX_WINS && g_wins[g_focus].used &&
        !g_wins[g_focus].minimized) {
        send_event(g_focus, GUI_EVT_KEY, code, 0, 0, 0, 0, 0, 0, 0);
    }
}

static void handle_input(const input_event_t *ev) {
    /* Any input resumes from Sleep; the event that wakes us is consumed so the
     * screen comes back exactly as it was, without a stray click landing. */
    if (g_sleeping) {
        mcube_time_t ts;
        u64 now = (mc_time(&ts) == 0) ? ts.uptime_ms : 0;

        /* Keep the pointer bookkeeping coherent while blank: if the button is
         * still held (the click that chose Sleep) the later release must not
         * look like a fresh press once we are awake again. */
        if (ev->type == INPUT_MOUSE) {
            g_mx = ev->x;
            g_my = ev->y;
            g_buttons = ev->buttons;
        }

        /* The click that selected Sleep is followed by its own release a few
         * milliseconds later; swallow anything in that window, and never let a
         * release on its own wake the machine. Only a fresh press or a key
         * press brings the screen back. */
        if (g_sleep_need_release) {
            if (ev->type == INPUT_MOUSE && ev->buttons == 0)
                g_sleep_need_release = 0;
            return;
        }
        if (now < g_sleep_since_ms + 500) return;
        if (ev->type == INPUT_MOUSE && ev->buttons == 0) return;
        if (ev->type != INPUT_MOUSE && ev->type != INPUT_KEY) return;

        g_sleeping = 0;
        /* The blank frame replaced the whole screen, so nothing on it can be
         * reused: drop the clip rectangle, mark every window for a re-blit and
         * repaint the scene from scratch. */
        clip_reset();
        g_cursor_saved = 0;
        g_dirty_valid = 0;
        for (int i = 0; i < MAX_WINS; i++)
            if (g_wins[i].used) g_wins[i].dirty = 1;
        g_need_compose = 1;
        g_need_taskbar = 1;
        g_need_tray = 1;
        g_need_menu = 1;
        g_sys_dirty = 1;
        g_cursor_moved = 1;
        return;
    }

    if (ev->type == INPUT_MOUSE) {
        int moved = ev->x != g_mx || ev->y != g_my;
        g_mx = ev->x;
        g_my = ev->y;

        u32 pressed = ev->buttons & ~g_buttons;
        u32 released = g_buttons & ~ev->buttons;
        g_buttons = ev->buttons;

        if (g_cc_drag) {
            if (released & 1) {
                g_cc_drag = 0;
                cc_dirty();
            } else if (moved) {
                cc_set_slider(g_cc_drag, g_mx);
                cc_dirty();
            }
        } else if (g_drag_slot >= 0) {
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
            if (pressed & 2) mouse_right_press(g_mx, g_my);
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

/* Paint the desktop backdrop into the already allocated g_bg buffer. Kept
 * apart from the allocation so switching wallpaper at runtime is just a
 * repaint, not a re-malloc. */
static void build_background(void) {
    int w = g_fbw, h = g_fbh;
    if (!g_bg) return;
    const wallpaper_t *wp = &g_walls[g_wallpaper & 3];

    i64 glow_x = w * 3 / 4, glow_y = h / 5;
    i64 glow_r2 = (i64)(w / 2) * (w / 2);
    i64 vig_r2 = (i64)(w / 2) * (w / 2) + (i64)(h / 2) * (h / 2);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            u32 c = lerp_color(wp->top, wp->bottom,
                               y * 255 / (h > 1 ? h - 1 : 1));

            if ((x & 63) == 0 || (y & 63) == 0) {
                c = lerp_color(c, wp->grid, 38);
            }

            i64 dx = x - glow_x, dy = y - glow_y;
            i64 d2 = dx * dx + dy * dy;
            if (d2 < glow_r2) {
                int f = (int)((glow_r2 - d2) * 120 / glow_r2);
                c = lerp_color(c, wp->glow, f);
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

static void init_background(void) {
    int w = g_fbw, h = g_fbh;
    g_bg = (u32 *)malloc((size_t)w * (size_t)h * 4);
    if (!g_bg) return;
    build_background();
}

/* ---- theme: wallpaper + accent -------------------------------------- */

#define THEME_PATH "/THEME.CFG"
#define CONFIG_PATH "/CONFIG.CFG"
#define ACCENT_COUNT 5

static const u32 g_accent_table[ACCENT_COUNT] = {
    0x3B82F6, 0x14B8A6, 0x8B5CF6, 0xF59E0B, 0xF43F5E,
};

static void theme_set_accent(int idx) {
    if (idx < 0 || idx >= ACCENT_COUNT) idx = 0;
    g_accent_idx = idx;
    g_accent = g_accent_table[idx];
    g_accent_2 = lerp_color(g_accent, 0xFFFFFF, 45);
    g_accent_dark = lerp_color(g_accent, 0x000000, 95);
    g_title_1 = lerp_color(g_accent, 0xFFFFFF, 55);
    g_title_2 = lerp_color(g_accent, 0x000000, 120);
}

static void theme_set_wallpaper(int idx) {
    if (idx < 0 || idx >= 4) idx = 0;
    g_wallpaper = idx;
    g_wall_top = g_walls[idx].top;
    g_wall_bot = g_walls[idx].bottom;
}

/* Read the file Settings > Personalisation writes. */
static int theme_read(int *accent, int *wall) {
    int fd = open(THEME_PATH, VFS_O_RDONLY);
    if (fd < 0) return 0;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *p = strstr(buf, "accent=");
    *accent = p ? atoi(p + 7) : 0;
    p = strstr(buf, "wall=");
    *wall = p ? atoi(p + 5) : 0;
    return 1;
}

/* Applied live: Settings writes the file, the shell repaints within a tick. */
static void theme_poll(void) {
    int a, w;
    if (!theme_read(&a, &w)) return;
    int changed = 0;
    if (a != g_accent_idx) { theme_set_accent(a); changed = 1; }
    if (w != g_wallpaper) {
        theme_set_wallpaper(w);
        build_background();
        changed = 1;
    }
    if (changed) {
        g_need_compose = 1;
        printf("USER_THEME_OK: accent %d wallpaper %d\n", g_accent_idx,
               g_wallpaper);
    }
}

/* Mirror the two switches in /CONFIG.CFG that the shell itself acts on. The
 * indices match the Settings feature table: f7 is the 24-hour clock, f10 the
 * high-contrast chrome. Everything else in that file belongs to Settings. */
static void config_poll(void) {
    int fd = open(CONFIG_PATH, VFS_O_RDONLY);
    char buf[256];
    int n;
    int h24 = 1;
    int hc = 0;
    const char *p;

    if (fd < 0) return;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;

    p = strstr(buf, "f7=");
    if (p) h24 = atoi(p + 3) ? 1 : 0;
    p = strstr(buf, "f10=");
    if (p) hc = atoi(p + 4) ? 1 : 0;

    if (h24 == g_cfg_24h && hc == g_cfg_contrast) return;
    g_cfg_24h = h24;
    g_cfg_contrast = hc;
    g_need_taskbar = 1;
    g_need_tray = 1;
    g_need_compose = 1;
    printf("USER_CONFIG_OK: 24h %d contrast %d\n", h24, hc);
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

    g_vram = (u32 *)fb_map();
    if (!g_vram) {
        printf("USER_DESKTOP_FAIL: fb_map\n");
        return 1;
    }

    g_fbw = (int)info.width;
    g_fbh = (int)info.height;
    g_stride = (int)(info.pitch / 4);

    /* Compose in RAM, blit to VRAM once per frame. */
    g_fb = (u32 *)malloc((size_t)g_stride * (size_t)g_fbh * 4);
    if (!g_fb) {
        printf("USER_DESKTOP_FAIL: back buffer\n");
        return 1;
    }
    memset(g_fb, 0, (size_t)g_stride * (size_t)g_fbh * 4);

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

    theme_set_accent(0);
    theme_set_wallpaper(0);
    {
        int a, w;
        if (theme_read(&a, &w)) {
            theme_set_accent(a);
            theme_set_wallpaper(w);
        }
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

    u64 t0 = now_ms();
    compose(0, 0, g_fbw, g_fbh);
    present();
    printf("USER_DESKTOP_PAINT: first compose %u ms\n",
           (unsigned)(now_ms() - t0));

    /* Boot with a useful set of windows so every subsystem is exercised.
     * The editor is handed the sample file installed on the ESP. */
    spawn_app("/bin/terminal.lgy");
    spawn_app("/bin/files.lgy");
    spawn_app("/bin/editor.lgy /TEST.TXT");
    spawn_app("/bin/calc.lgy");
    spawn_app("/bin/image.lgy");
    spawn_app("/bin/settings.lgy");
    spawn_app("/bin/taskman.lgy");
    spawn_app("/bin/browser.lgy");

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

        /* Asleep: the framebuffer is black and nothing needs composing. The
         * poll above already rate-limits the loop to one pass per 16 ms. */
        if (g_sleeping) {
            g_need_compose = 0;
            g_need_taskbar = 0;
            g_need_tray = 0;
            g_need_menu = 0;
            g_dirty_valid = 0;
            g_cursor_moved = 0;
            continue;
        }

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
            /* Power source changes slowly; once a second keeps the tray glyph
             * in step with the clock repaint that follows. */
            if (battery_info(&g_bat) == 0) g_have_bat = 1;
            memset(&g_usb, 0, sizeof(g_usb));
            if (usb_info(&g_usb) != 0) memset(&g_usb, 0, sizeof(g_usb));
            g_need_tray = 1;
            theme_poll();
            config_poll();
            for (int i = 0; i < MAX_WINS; i++) {
                if (g_wins[i].used) {
                    send_event(i, GUI_EVT_TICK, (u32)ts.epoch,
                               0, 0, 0, 0, 0, 0, 0);
                }
            }
        }

        /* Windows-style thumbnail flyout after a short hover. */
        if (!g_preview_shown && g_hover_task >= 0 && g_hover_task_ms != 0 &&
            !g_menu_open && !g_switcher_open && !g_taskmenu_open &&
            !g_cal_open && now_ms() >= g_hover_task_ms + 350 &&
            g_hover_task < MAX_WINS && g_wins[g_hover_task].used) {
            g_preview_shown = 1;
            dirty_preview();
        }

        /* Snap picker: opens once the pointer rests on the maximise button. */
        if (!g_snap_open && g_hover_btn == BTN_MAX && g_hover_slot >= 0 &&
            g_snap_hover_ms != 0 && now_ms() >= g_snap_hover_ms + 400 &&
            !g_cc_open) {
            snap_open_for(g_hover_slot);
        }

        u64 paint_start = now_ms();
        int painted = 0;
        if (g_need_compose) {
            compose(0, 0, g_fbw, g_fbh);
            painted = 1;
        } else {
            int did = 0;
            if (g_dirty_valid) {
                compose_region(g_dirty_x0, g_dirty_y0,
                               g_dirty_x1 - g_dirty_x0,
                               g_dirty_y1 - g_dirty_y0);
                g_dirty_valid = 0;
                did = 1;
                painted = 1;
            }
            int tb = g_need_taskbar;
            int tr = g_need_tray;
            int menu = g_need_menu;
            if (tb) {
                redraw_taskbar();
                tr = 0;
                painted = 1;
            }
            if (tr) {
                redraw_tray();
                painted = 1;
            }
            if (menu) {
                redraw_menu();
                painted = 1;
            }
            int card = 0;
            if (g_sys_dirty) {
                refresh_sys_card();
                g_sys_dirty = 0;
                card = 1;
                painted = 1;
            }
            if (g_cursor_moved && !did && !tb && !tr && !menu && !card) {
                cursor_draw();
                painted = 1;
            }
        }
        flush_dirty();
        present();
        if (painted) paint_stat(now_ms() - paint_start);
        g_need_taskbar = 0;
        g_need_tray = 0;
        g_need_menu = 0;
        g_cursor_moved = 0;

        if (!g_gui_ok_printed && g_ready_count >= BOOT_APP_COUNT &&
            g_flushed_count >= BOOT_APP_COUNT) {
            g_gui_ok_printed = 1;
            printf("USER_GUI_OK: %d windows ready\n", g_ready_count);
        }
    }
}
