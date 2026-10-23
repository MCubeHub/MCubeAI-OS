/* MCubeAIOS desktop: compositor, window manager, taskbar and launcher.
 *
 * Every application renders into its own off-screen surface through the pipe
 * protocol in <mcube/gui.h>. The desktop owns the framebuffer, clips all
 * drawing to window rectangles, composites the taskbar/menu and draws the
 * mouse cursor on top. Input events from SYS_INPUT_POLL are routed to the
 * focused window.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>
#include <mcube/fb.h>
#include <mcube/input.h>

#define MAX_WINS      10
#define TASKBAR_H     30
#define WIN_TITLE_H   22
#define WIN_BORDER    1
#define MENU_W        240
#define MENU_ITEM_H   26
#define MENU_PAD      8
#define CURSOR_W      12
#define CURSOR_H      16

#define C_DESK_TOP    0x0A1020
#define C_DESK_BOTTOM 0x22345A
#define C_TASKBAR     0x141A26
#define C_TASKBAR_HI  0x1E2738
#define C_LINE        0x2C3A55
#define C_TEXT        0xE8ECF4
#define C_TEXT_DIM    0x9AA6BC
#define C_ACCENT      0x3B82F6
#define C_ACCENT_DARK 0x1D4ED8
#define C_FRAME       0x0E1420
#define C_BORDER      0x46536B
#define C_TITLE_ON    0x2D6CDF
#define C_TITLE_OFF   0x2A3242
#define C_MENU        0x1A2231
#define C_MENU_HOVER  0x2D6CDF
#define C_SHADOW      0x04060A
#define C_BLACK       0x000000

typedef struct {
    int used;
    pid_t pid;
    int cmd_fd;
    int evt_fd;
    char title[64];
    int want_w, want_h;
    int ready;
    int x, y, w, h;        /* content rectangle in screen coordinates */
    u32 *surface;
    int sw, sh;
    int dirty;
    int flushed;
    int focused;
} win_t;

static u32 *g_fb;
static int g_fbw, g_fbh, g_stride, g_fmt;

static win_t g_wins[MAX_WINS];
static int g_focus = -1;

static int g_menu_open;
static int g_menu_sel;

static int g_mx, g_my;
static u32 g_buttons;
static int g_press_win = -1;

static int g_cursor_saved;
static int g_cursor_x, g_cursor_y, g_cursor_w, g_cursor_h;
static u32 g_cursor_save[CURSOR_H][CURSOR_W];

static int g_ready_count;
static int g_flushed_count;
static int g_gui_ok_printed;
static u64 g_last_clock;

static const char *const g_app_names[] = {
    "Terminal", "Files", "Editor", "Calculator",
    "Image Viewer", "Settings", "Task Manager",
};
static const char *const g_app_paths[] = {
    "/bin/terminal.elf", "/bin/files.elf", "/bin/editor.elf",
    "/bin/calc.elf", "/bin/image.elf", "/bin/settings.elf",
    "/bin/taskman.elf",
};
#define APP_COUNT ((int)(sizeof(g_app_paths) / sizeof(g_app_paths[0])))

/* ---- pixel helpers -------------------------------------------------- */

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
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bw) x1 = bw;
    if (y1 > bh) y1 = bh;
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

static void gfx_text(u32 *buf, int stride, int bw, int bh,
                     int x, int y, u32 rgb, const char *s, int n) {
    if (!s) return;
    u32 c = pixel(rgb);
    int px = x;
    for (int i = 0; (n < 0 || i < n) && s[i]; i++) {
        u8 ch = (u8)s[i];
        if (ch == '\n') { px = x; y += 16; continue; }
        if (px + 8 > 0 && px < bw && y + 16 > 0 && y < bh) {
            const u8 *glyph = mc_font8x16[ch];
            for (int row = 0; row < 16; row++) {
                int py = y + row;
                if (py < 0 || py >= bh) continue;
                u8 bits = glyph[row];
                for (int col = 0; col < 8; col++) {
                    if (!(bits & (0x80 >> col))) continue;
                    int gx = px + col;
                    if (gx < 0 || gx >= bw) continue;
                    buf[(u64)py * stride + gx] = c;
                }
            }
        }
        px += 8;
    }
}

static void blit_surface(const win_t *w) {
    if (!w->surface || w->w <= 0 || w->h <= 0) return;
    int x0 = w->x, y0 = w->y;
    int x1 = x0 + w->w, y1 = y0 + w->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_fbw) x1 = g_fbw;
    if (y1 > g_fbh) y1 = g_fbh;
    if (x0 >= x1 || y0 >= y1) return;

    int cols = x1 - x0;
    for (int yy = y0; yy < y1; yy++) {
        const u32 *src = w->surface + (u64)(yy - w->y) * w->sw + (x0 - w->x);
        memcpy(g_fb + (u64)yy * g_stride + x0, src, (size_t)cols * 4);
    }
}

/* ---- mouse cursor --------------------------------------------------- */

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

/* ---- window geometry ------------------------------------------------ */

static void frame_box(const win_t *w, int *fx, int *fy, int *fw, int *fh) {
    *fx = w->x - WIN_BORDER;
    *fy = w->y - WIN_TITLE_H - WIN_BORDER;
    *fw = w->w + 2 * WIN_BORDER;
    *fh = w->h + WIN_TITLE_H + 2 * WIN_BORDER;
}

static int inside(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static int window_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_WINS; i++) if (g_wins[i].used) n++;
    return n;
}

static void ensure_surface(win_t *w) {
    if (w->w <= 0 || w->h <= 0) return;
    if (w->surface && w->sw == w->w && w->sh == w->h) return;

    u32 *s = (u32 *)malloc((size_t)w->w * (size_t)w->h * 4);
    if (!s) return;
    w->surface = s;
    w->sw = w->w;
    w->sh = w->h;
    gfx_fill(s, w->sw, w->sw, w->sh, 0, 0, w->sw, w->sh, C_FRAME);
}

static void send_event(int slot, u32 type, u32 code, int x, int y,
                       int dx, int dy, u32 buttons, int w, int h) {
    win_t *win = &g_wins[slot];
    if (!win->used) return;

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

static void relayout(void) {
    int n = window_count();
    if (n == 0) {
        g_focus = -1;
        return;
    }

    int rows = n <= 2 ? 1 : (n <= 8 ? 2 : 3);
    int cols = (n + rows - 1) / rows;
    int area_x = 0, area_y = TASKBAR_H;
    int area_w = g_fbw, area_h = g_fbh - TASKBAR_H;
    int tw = area_w / cols, th = area_h / rows;

    int idx = 0;
    for (int i = 0; i < MAX_WINS; i++) {
        win_t *w = &g_wins[i];
        if (!w->used) continue;

        int cx = idx % cols, cy = idx / cols;
        int fx = area_x + cx * tw + 4;
        int fy = area_y + cy * th + 4;
        int fw = tw - 8;
        int fh = th - 8;

        w->x = fx + WIN_BORDER;
        w->y = fy + WIN_TITLE_H + WIN_BORDER;
        w->w = fw - 2 * WIN_BORDER;
        w->h = fh - WIN_TITLE_H - 2 * WIN_BORDER;
        if (w->w < 32) w->w = 32;
        if (w->h < 24) w->h = 24;

        ensure_surface(w);
        send_event(i, GUI_EVT_RESIZE, 0, 0, 0, 0, 0, 0, w->w, w->h);
        idx++;
    }
}

/* ---- drawing -------------------------------------------------------- */

static void draw_background(void) {
    for (int y = 0; y < g_fbh; y++) {
        u32 c = lerp_color(C_DESK_TOP, C_DESK_BOTTOM, y * 255 / (g_fbh - 1));
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, 0, y, g_fbw, 1, c);
    }
}

static void draw_window(const win_t *w) {
    int fx, fy, fw, fh;
    frame_box(w, &fx, &fy, &fw, &fh);

    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, fx + 4, fy + 4, fw, fh, C_SHADOW);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, fx, fy, fw, fh,
             w->focused ? C_TITLE_ON : C_TITLE_OFF);

    int max_title = (fw - 34) / 8;
    if (max_title < 0) max_title = 0;
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, fx + 8, fy + 3, C_TEXT,
             w->title, max_title);

    /* Close button: a small x in the top-right corner. */
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, fx + fw - 20, fy + 3,
             w->focused ? C_TEXT : C_TEXT_DIM, "x", 1);

    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, w->x, w->y, w->w, w->h, C_FRAME);
    blit_surface(w);

    gfx_rect(g_fb, g_stride, g_fbw, g_fbh, fx, fy, fw, fh, C_BORDER);
}

static void draw_taskbar(void) {
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, 0, 0, g_fbw, TASKBAR_H, C_TASKBAR);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, 0, TASKBAR_H - 1, g_fbw, 1, C_LINE);

    u32 start_bg = g_menu_open ? C_ACCENT : C_TASKBAR_HI;
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, 4, 3, 84, TASKBAR_H - 7, start_bg);
    gfx_rect(g_fb, g_stride, g_fbw, g_fbh, 4, 3, 84, TASKBAR_H - 7,
             g_menu_open ? C_ACCENT : C_LINE);
    gfx_text(g_fb, g_stride, g_fbw, g_fbh, 12, 7, C_TEXT, "MCube", -1);

    int bx = 96;
    for (int i = 0; i < MAX_WINS && bx + 112 < g_fbw - 100; i++) {
        win_t *w = &g_wins[i];
        if (!w->used) continue;
        u32 bg = w->focused ? C_TITLE_ON : C_TASKBAR_HI;
        gfx_fill(g_fb, g_stride, g_fbw, g_fbh, bx, 4, 108, TASKBAR_H - 8, bg);
        gfx_rect(g_fb, g_stride, g_fbw, g_fbh, bx, 4, 108, TASKBAR_H - 8,
                 w->focused ? C_ACCENT : C_LINE);
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, bx + 6, 7, C_TEXT, w->title, 12);
        bx += 114;
    }

    mcube_time_t ts;
    if (mc_time(&ts) == 0) {
        u32 secs = (u32)(ts.epoch % 86400);
        char clock[16];
        snprintf(clock, sizeof(clock), "%02u:%02u:%02u",
                 secs / 3600, (secs / 60) % 60, secs % 60);
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, g_fbw - 82, 7, C_TEXT, clock, -1);
    }
}

static void menu_box(int *mx, int *my, int *mw, int *mh) {
    *mx = 4;
    *my = TASKBAR_H + 4;
    *mw = MENU_W;
    *mh = MENU_PAD * 2 + APP_COUNT * MENU_ITEM_H;
}

static void draw_menu(void) {
    int mx, my, mw, mh;
    menu_box(&mx, &my, &mw, &mh);

    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, mx + 5, my + 5, mw, mh, C_SHADOW);
    gfx_fill(g_fb, g_stride, g_fbw, g_fbh, mx, my, mw, mh, C_MENU);
    gfx_rect(g_fb, g_stride, g_fbw, g_fbh, mx, my, mw, mh, C_BORDER);

    for (int i = 0; i < APP_COUNT; i++) {
        int iy = my + MENU_PAD + i * MENU_ITEM_H;
        if (i == g_menu_sel) {
            gfx_fill(g_fb, g_stride, g_fbw, g_fbh, mx + 2, iy, mw - 4,
                     MENU_ITEM_H - 2, C_MENU_HOVER);
        }
        gfx_text(g_fb, g_stride, g_fbw, g_fbh, mx + 12, iy + 4, C_TEXT,
                 g_app_names[i], -1);
    }
}

static void compose(void) {
    cursor_restore();
    draw_background();
    for (int i = 0; i < MAX_WINS; i++) {
        if (g_wins[i].used) {
            draw_window(&g_wins[i]);
            g_wins[i].dirty = 0;
        }
    }
    draw_taskbar();
    if (g_menu_open) draw_menu();
    cursor_draw();
}

static void flush_dirty(void) {
    int any = 0;
    for (int i = 0; i < MAX_WINS; i++) {
        win_t *w = &g_wins[i];
        if (!w->used || !w->dirty) continue;
        if (g_menu_open) continue;   /* keep the menu on top */
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

    close(cfd);
    close(efd);
    if (pid > 0) {
        kill(pid, 15);
        int status = 0;
        waitpid(pid, &status, 0);
    }

    if (g_focus == slot) {
        g_focus = -1;
        for (int i = 0; i < MAX_WINS; i++) {
            if (g_wins[i].used) { g_focus = i; break; }
        }
    }
    for (int i = 0; i < MAX_WINS; i++) {
        g_wins[i].focused = (i == g_focus && g_wins[i].used);
    }

    relayout();
    compose();
}

static void focus_window(int slot) {
    if (slot < 0 || slot >= MAX_WINS || !g_wins[slot].used) return;
    if (g_focus == slot) return;
    g_focus = slot;
    for (int i = 0; i < MAX_WINS; i++) {
        g_wins[i].focused = (i == g_focus && g_wins[i].used);
    }
    for (int i = 0; i < MAX_WINS; i++) {
        if (g_wins[i].used) send_event(i, GUI_EVT_FOCUS, i == g_focus, 0, 0, 0, 0, 0, 0, 0);
    }
    compose();
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
    w->want_w = 640;
    w->want_h = 480;
    strncpy(w->title, base_name(path), sizeof(w->title) - 1);
    w->title[sizeof(w->title) - 1] = 0;
    w->focused = 1;
    for (int i = 0; i < MAX_WINS; i++) {
        if (i != slot && g_wins[i].used) g_wins[i].focused = 0;
    }
    g_focus = slot;

    relayout();
    compose();
}

/* ---- command and event handling ------------------------------------- */

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

static void handle_cmd(int slot, const gui_cmd_t *cmd) {
    win_t *w = &g_wins[slot];
    if (!w->used) return;

    switch (cmd->type) {
    case GUI_CMD_INIT:
        set_title_from_cmd(w, cmd);
        w->want_w = cmd->x;
        w->want_h = cmd->y;
        if (!w->ready) {
            w->ready = 1;
            g_ready_count++;
        }
        /* Redraw the frame now that the real title is known. */
        compose();
        break;

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
        compose();
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

/* ---- input ---------------------------------------------------------- */

static void mouse_press(int x, int y) {
    int mx, my, mw, mh;
    menu_box(&mx, &my, &mw, &mh);

    if (g_menu_open) {
        if (inside(x, y, 4, 3, 84, TASKBAR_H - 7)) {
            g_menu_open = 0;
            compose();
            return;
        }
        if (inside(x, y, mx, my, mw, mh)) {
            int idx = (y - my - MENU_PAD) / MENU_ITEM_H;
            if (idx >= 0 && idx < APP_COUNT) {
                g_menu_open = 0;
                spawn_app(g_app_paths[idx]);
                compose();
            }
            return;
        }
        g_menu_open = 0;
        compose();
        return;
    }

    if (inside(x, y, 4, 3, 84, TASKBAR_H - 7)) {
        g_menu_open = 1;
        g_menu_sel = 0;
        compose();
        return;
    }

    int bx = 96;
    for (int i = 0; i < MAX_WINS && bx + 112 < g_fbw - 100; i++) {
        if (!g_wins[i].used) continue;
        if (inside(x, y, bx, 4, 108, TASKBAR_H - 8)) {
            focus_window(i);
            return;
        }
        bx += 114;
    }

    for (int i = MAX_WINS - 1; i >= 0; i--) {
        win_t *w = &g_wins[i];
        if (!w->used) continue;

        int fx, fy, fw, fh;
        frame_box(w, &fx, &fy, &fw, &fh);
        if (inside(x, y, fx + fw - 24, fy + 2, 22, WIN_TITLE_H - 4)) {
            close_window(i);
            return;
        }
        if (inside(x, y, w->x, w->y, w->w, w->h)) {
            focus_window(i);
            send_event(i, GUI_EVT_MOUSE, g_buttons, x - w->x, y - w->y,
                       0, 0, g_buttons, 0, 0);
            g_press_win = i;
            return;
        }
        if (inside(x, y, fx, fy, fw, fh)) {
            focus_window(i);
            return;
        }
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

static void launch_selected(void) {
    if (g_menu_sel < 0) g_menu_sel = 0;
    if (g_menu_sel >= APP_COUNT) g_menu_sel = APP_COUNT - 1;
    g_menu_open = 0;
    spawn_app(g_app_paths[g_menu_sel]);
    compose();
}

static void handle_key(u32 code) {
    if (code == INPUT_KEY_F1) {
        g_menu_open = !g_menu_open;
        g_menu_sel = 0;
        compose();
        return;
    }

    if (g_menu_open) {
        if (code == INPUT_KEY_UP) {
            g_menu_sel = (g_menu_sel + APP_COUNT - 1) % APP_COUNT;
            redraw_menu();
        } else if (code == INPUT_KEY_DOWN) {
            g_menu_sel = (g_menu_sel + 1) % APP_COUNT;
            redraw_menu();
        } else if (code == '\n') {
            launch_selected();
        } else if (code == 27) {
            g_menu_open = 0;
            compose();
        }
        return;
    }

    if (code == INPUT_KEY_F9) {
        int start = g_focus;
        for (int n = 1; n <= MAX_WINS; n++) {
            int i = (start + n) % MAX_WINS;
            if (g_wins[i].used) {
                focus_window(i);
                break;
            }
        }
        return;
    }

    if (g_focus >= 0 && g_wins[g_focus].used) {
        send_event(g_focus, GUI_EVT_KEY, code, 0, 0, 0, 0, 0, 0, 0);
    }
}

static void handle_input(const input_event_t *ev) {
    if (ev->type == INPUT_MOUSE) {
        g_mx = ev->x;
        g_my = ev->y;

        u32 pressed = ev->buttons & ~g_buttons;
        u32 released = g_buttons & ~ev->buttons;
        g_buttons = ev->buttons;

        if (g_menu_open) {
            int mx, my, mw, mh;
            menu_box(&mx, &my, &mw, &mh);
            if (inside(g_mx, g_my, mx, my, mw, mh)) {
                int idx = (g_my - my - MENU_PAD) / MENU_ITEM_H;
                if (idx >= 0 && idx < APP_COUNT && idx != g_menu_sel) {
                    g_menu_sel = idx;
                    redraw_menu();
                }
            }
        }

        if (pressed & 1) mouse_press(g_mx, g_my);
        if (released & 1) mouse_release(g_mx, g_my);
        cursor_draw();
        return;
    }

    if (ev->type == INPUT_KEY) handle_key(ev->code);
}

/* ---- main loop ------------------------------------------------------ */

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

    printf("USER_DESKTOP_OK: %dx%d %s\n", g_fbw, g_fbh,
           g_fmt == 1 ? "BGRA" : "RGBA");

    for (int i = 0; i < MAX_WINS; i++) {
        g_wins[i].cmd_fd = -1;
        g_wins[i].evt_fd = -1;
    }

    compose();

    /* Boot with a useful set of windows so every subsystem is exercised.
     * The editor is handed the sample file installed on the ESP. */
    spawn_app("/bin/terminal.elf");
    spawn_app("/bin/files.elf");
    spawn_app("/bin/editor.elf /TEST.TXT");
    spawn_app("/bin/calc.elf");
    spawn_app("/bin/image.elf");
    spawn_app("/bin/settings.elf");
    spawn_app("/bin/taskman.elf");

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
            redraw_taskbar();
            for (int i = 0; i < MAX_WINS; i++) {
                if (g_wins[i].used) send_event(i, GUI_EVT_TICK, (u32)ts.epoch,
                                               0, 0, 0, 0, 0, 0, 0);
            }
        }

        flush_dirty();

        if (!g_gui_ok_printed && g_ready_count >= APP_COUNT &&
            g_flushed_count >= APP_COUNT) {
            g_gui_ok_printed = 1;
            printf("USER_GUI_OK: %d windows ready\n", g_ready_count);
        }
    }
}
