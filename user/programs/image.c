/* Pictures: an album viewer for bitmaps stored in /pictures.
 *
 * The left pane lists every PNG/BMP/PPM file found by scanning the directory;
 * the right pane previews the selected one. Decoding uses the shared codec in
 * user/lib/imgdec.c, and the pixels are handed to the desktop once and then
 * painted from its cache slot, so scrolling the list costs nothing.
 *
 * Navigation: click a list row, or use the arrow keys / space. Clicking the
 * preview cycles fit -> 100% -> 200%. A resize re-scales from the decoded
 * pixels without touching the file again.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>
#include <mcube/input.h>
#include <mcube/imgdec.h>

#define WIN_W        760
#define WIN_H        520

#define TOOL_H       34
#define LIST_W       208
#define STATUS_H     24
#define ROW_H        28
#define MAX_PICS     96
#define PIC_MAX_BYTES (512 * 1024)

#define C_BG     0x12171F
#define C_PANEL  0x1A2029
#define C_LIST   0x161C24
#define C_LIST_ALT 0x19202A
#define C_TEXT   0xE8EEF7
#define C_DIM    0x93A1B5
#define C_SEL    0x1F4F86
#define C_SEL_TX 0xF2F7FF
#define C_ACCENT 0x4CC2FF
#define C_BORDER 0x2B3442
#define C_STATUS 0x0F141B
#define C_BTN    0x252D39
#define C_WARN   0xFF8A8A

typedef struct rect {
    int x, y, w, h;
} rect_t;

typedef struct {
    char name[64];
    char path[160];
    u32 size;
} pic_t;

static pic_t g_pics[MAX_PICS];
static int g_count;
static int g_sel = -1;
static int g_top;
static int g_need = 1;
static char g_status[180] = "";
static char g_dir[180] = "/pictures";
static char g_want[64] = "";      /* file name to select when launched with a path */
static char g_err[96] = "";       /* last failure, also painted in the preview */

/* Decoded/display buffers. The heap is a bump allocator, so they are reserved
 * once at startup and reused for every file. */
static u8  *g_raw;
static u8  *g_aux;
static u8  *g_rows;
static u32 *g_nat;
static u32 *g_disp;

static int g_cur_sel = -1;      /* file currently uploaded to slot 0 */
static int g_cur_ok;
static u32 g_nat_w, g_nat_h;    /* natural size */
static u32 g_cur_w, g_cur_h;    /* uploaded (scaled) size */
static int g_zoom;              /* 0 fit, 1 100%, 2 200% */
static char g_fmt[8] = "";

static rect_t g_btn_prev, g_btn_next;

/* ---- helpers -------------------------------------------------------- */

static void set_status(const char *s) {
    snprintf(g_status, sizeof(g_status), "%s", s ? s : "");
    g_need = 1;
}

/* Failure that must be visible even when the status bar is easy to miss:
 * kept in g_err and painted in the middle of the preview pane. */
static void set_error(const char *s) {
    snprintf(g_err, sizeof(g_err), "%s", s ? s : "");
    set_status(g_err);
}

static void clear_error(void) {
    g_err[0] = 0;
}

static void preview_area(int *x, int *y, int *w, int *h) {
    *x = LIST_W;
    *y = TOOL_H;
    *w = gui_width() - LIST_W;
    *h = gui_height() - TOOL_H - STATUS_H;
    if (*w < 8) *w = 8;
    if (*h < 8) *h = 8;
}

static int list_rows(void) {
    int rows = (gui_height() - TOOL_H - STATUS_H - 8) / ROW_H;
    return rows < 1 ? 1 : rows;
}

static void human_size(u32 n, char *out, size_t cap) {
    if (n < 1024) snprintf(out, cap, "%u B", n);
    else if (n < 1024u * 1024u)
        snprintf(out, cap, "%u KB", (n + 512u) / 1024u);
    else
        snprintf(out, cap, "%u MB", (n + 512u * 1024u) / (1024u * 1024u));
}

/* Format tag for the status bar, taken from the file extension. */
static void set_format(const char *name) {
    const char *dot = strrchr(name, '.');
    for (int i = 0; i < 7; i++) g_fmt[i] = 0;
    if (!dot || !dot[1]) {
        snprintf(g_fmt, sizeof(g_fmt), "IMG");
        return;
    }
    int n = (int)strlen(dot + 1);
    if (n > 3) n = 3;
    for (int i = 0; i < n; i++) {
        char c = dot[1 + i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        g_fmt[i] = c;
    }
    g_fmt[4] = 0;
}

/* ---- scaling -------------------------------------------------------- */

static void fit_size(int aw, int ah, int *dw, int *dh) {
    int w = (int)g_nat_w, h = (int)g_nat_h;
    if (w > aw || h > ah) {
        if ((long long)aw * h <= (long long)ah * w) {
            w = aw;
            h = (int)((long long)g_nat_h * aw / g_nat_w);
        } else {
            h = ah;
            w = (int)((long long)g_nat_w * ah / g_nat_h);
        }
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *dw = w;
    *dh = h;
}

/* Nearest-neighbour resample of g_nat into g_disp, then upload it to the
 * desktop cache slot. Returns 0 on success. */
static int build_display(void) {
    int ax, ay, aw, ah;
    preview_area(&ax, &ay, &aw, &ah);
    int dw, dh;

    if (g_zoom == 0) {
        fit_size(aw - 16, ah - 16, &dw, &dh);
    } else if (g_zoom == 1) {
        dw = (int)g_nat_w;
        dh = (int)g_nat_h;
    } else {
        dw = (int)g_nat_w * 2;
        dh = (int)g_nat_h * 2;
    }

    /* The desktop only accepts GUI_IMG_MAX_PX pixels per slot. When a zoom
     * level would exceed that, fall back to fit rather than showing nothing. */
    if ((long long)dw * dh > (long long)GUI_IMG_MAX_PX) {
        g_zoom = 0;
        fit_size(aw - 16, ah - 16, &dw, &dh);
    }
    if (dw < 1 || dh < 1) return -1;

    for (int y = 0; y < dh; y++) {
        int sy = (int)((long long)y * g_nat_h / dh);
        if (sy >= (int)g_nat_h) sy = (int)g_nat_h - 1;
        const u32 *srow = g_nat + (size_t)sy * (size_t)g_nat_w;
        u32 *drow = g_disp + (size_t)y * (size_t)dw;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long long)x * g_nat_w / dw);
            if (sx >= (int)g_nat_w) sx = (int)g_nat_w - 1;
            drow[x] = srow[sx];
        }
    }

    if (gui_image_load(0, dw, dh, g_disp) != 0) return -1;
    g_cur_w = (u32)dw;
    g_cur_h = (u32)dh;
    return 0;
}

/* ---- loading -------------------------------------------------------- */

static void update_status(void) {
    if (g_sel < 0 || g_sel >= g_count) {
        if (g_err[0]) return;
        set_status(g_count ? "" : "No pictures found");
        return;
    }
    char kb[20];
    char zoom[12];
    human_size(g_pics[g_sel].size, kb, sizeof(kb));
    if (g_zoom == 0) snprintf(zoom, sizeof(zoom), "fit");
    else snprintf(zoom, sizeof(zoom), "%d%%", g_zoom == 1 ? 100 : 200);

    snprintf(g_status, sizeof(g_status), "%d/%d  %s   -   %ux%u %s   -   %s   -   %s",
             g_sel + 1, g_count, g_pics[g_sel].name, g_nat_w, g_nat_h, g_fmt, kb, zoom);
    g_need = 1;
}

/* Precise reason for the last img_decode() failure. The viewer shows this
 * both in the status bar and in the preview area so a black pane never looks
 * like a hang. */
static const char *decode_error_text(void) {
    switch (img_last_error()) {
    case IMGDEC_ERR_TOO_LARGE:
        return "Image is too large (limit 1280x960)";
    case IMGDEC_ERR_UNSUPPORTED:
        return "Unsupported image format or variant";
    default:
        return "Image is corrupt or truncated";
    }
}

static void load_current(void) {
    g_cur_ok = 0;
    g_cur_sel = -1;
    g_nat_w = g_nat_h = 0;
    clear_error();
    if (g_sel < 0 || g_sel >= g_count) {
        if (g_count == 0) {
            char msg[96];
            snprintf(msg, sizeof(msg), "No pictures found in %s", g_dir);
            set_status(msg);
        }
        return;
    }

    pic_t *p = &g_pics[g_sel];
    int fd = open(p->path, VFS_O_RDONLY);
    if (fd < 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Cannot open %s", p->name);
        set_error(msg);
        return;
    }
    mcube_stat_t st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        set_error("Cannot read file information");
        return;
    }
    if (st.size == 0) {
        close(fd);
        set_error("File is empty");
        return;
    }
    if (st.size > PIC_MAX_BYTES) {
        close(fd);
        set_error("File is too large to preview (max 512 KB)");
        return;
    }

    int got = 0;
    while (got < (int)st.size) {
        ssize_t n = read(fd, g_raw + got, (size_t)((int)st.size - got));
        if (n <= 0) break;
        got += (int)n;
    }
    close(fd);
    if (got <= 0) {
        set_error("Cannot read file contents");
        return;
    }

    int nw = 0, nh = 0;
    if (img_decode(g_raw, got, g_nat, &nw, &nh, g_aux, PIC_MAX_BYTES, g_rows,
                   IMGDEC_MAX_PX * 4 + 4096) != 0) {
        set_error(decode_error_text());
        return;
    }
    g_nat_w = (u32)nw;
    g_nat_h = (u32)nh;
    g_cur_sel = g_sel;

    if (build_display() != 0) {
        set_error("Preview buffer is too small for this image");
        return;
    }
    g_cur_ok = 1;
    set_format(p->name);
    update_status();
}

static void select_pic(int i) {
    if (i < 0 || i >= g_count) return;
    if (i == g_sel && g_cur_ok) return;

    g_sel = i;
    int rows = list_rows();
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + rows) g_top = g_sel - rows + 1;
    if (g_top < 0) g_top = 0;

    load_current();
    g_need = 1;
}

static int name_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

static void scan_pictures(void) {
    g_count = 0;
    for (int i = 0; i < MAX_PICS; i++) {
        mcube_dirent_t e;
        if (mc_readdir(g_dir, i, &e) != 0) break;
        if (e.type == VFS_TYPE_DIR) continue;
        if (!img_name_supported(e.name)) continue;

        pic_t *p = &g_pics[g_count];
        snprintf(p->name, sizeof(p->name), "%s", e.name);
        if (strcmp(g_dir, "/") == 0) snprintf(p->path, sizeof(p->path), "/%s", e.name);
        else snprintf(p->path, sizeof(p->path), "%s/%s", g_dir, e.name);
        p->size = 0;
        mcube_stat_t st;
        if (stat(p->path, &st) == 0) p->size = (u32)st.size;
        g_count++;
    }

    /* Directory order differs between FAT32 and the initrd tmpfs, so sort by
     * name to keep the album stable across boot media. */
    for (int i = 1; i < g_count; i++) {
        pic_t key = g_pics[i];
        int j = i - 1;
        while (j >= 0 && name_cmp(g_pics[j].name, key.name) > 0) {
            g_pics[j + 1] = g_pics[j];
            j--;
        }
        g_pics[j + 1] = key;
    }
}

/* ---- drawing -------------------------------------------------------- */

static void arrow(int x, int y, int dir, u32 c) {
    for (int i = 0; i < 5; i++) {
        int px = dir > 0 ? x + i : x + 4 - i;
        gui_fill(px, y + i, 1, 1, c);
        gui_fill(px, y + 8 - i, 1, 1, c);
    }
}

static void draw(void) {
    int w = gui_width(), h = gui_height();
    if (w < 64 || h < 64) return;

    gui_clear(C_BG);

    /* Toolbar with the album title, item count and prev/next buttons. */
    gui_fill(0, 0, w, TOOL_H, C_PANEL);
    gui_fill(0, TOOL_H - 1, w, 1, C_BORDER);
    gui_text(14, 9, C_TEXT, "Pictures");

    char cnt[48];
    if (g_count == 0) snprintf(cnt, sizeof(cnt), "no images");
    else snprintf(cnt, sizeof(cnt), "%d image%s", g_count, g_count == 1 ? "" : "s");
    gui_text(14 + 8 * 8 + 14, 9, C_DIM, cnt);

    int bw = 26, bh = 20;
    int by = (TOOL_H - bh) / 2;
    g_btn_next.x = w - 14 - bw;
    g_btn_next.y = by;
    g_btn_next.w = bw;
    g_btn_next.h = bh;
    g_btn_prev.x = g_btn_next.x - bw - 8;
    g_btn_prev.y = by;
    g_btn_prev.w = bw;
    g_btn_prev.h = bh;

    gui_fill(g_btn_prev.x, by, bw, bh, C_BTN);
    gui_fill(g_btn_next.x, by, bw, bh, C_BTN);
    arrow(g_btn_prev.x + 9, by + 6, -1, C_ACCENT);
    arrow(g_btn_next.x + 9, by + 6, 1, C_ACCENT);

    int list_h = h - TOOL_H - STATUS_H;
    gui_fill(0, TOOL_H, LIST_W, list_h, C_LIST);
    gui_fill(LIST_W, TOOL_H, 1, list_h, C_BORDER);

    int rows = list_rows();
    for (int r = 0; r < rows; r++) {
        int i = g_top + r;
        if (i >= g_count) break;
        int y = TOOL_H + 4 + r * ROW_H;
        if (i == g_sel) gui_fill(6, y, LIST_W - 12, ROW_H - 2, C_SEL);
        else if (i % 2) gui_fill(6, y, LIST_W - 12, ROW_H - 2, C_LIST_ALT);

        char label[32];
        int maxc = (LIST_W - 24) / 8;
        snprintf(label, sizeof(label), "%s", g_pics[i].name);
        if ((int)strlen(label) > maxc) {
            label[maxc] = 0;
            if (maxc >= 3) {
                label[maxc - 3] = '.';
                label[maxc - 2] = '.';
                label[maxc - 1] = '.';
            }
        }
        gui_text(12, y + 6, i == g_sel ? C_SEL_TX : C_TEXT, label);
    }

    int ax, ay, aw, ah;
    preview_area(&ax, &ay, &aw, &ah);

    if (g_cur_ok && g_cur_sel == g_sel) {
        int ix = ax + (aw - (int)g_cur_w) / 2;
        int iy = ay + (ah - (int)g_cur_h) / 2;
        gui_rect(ix - 1, iy - 1, (int)g_cur_w + 2, (int)g_cur_h + 2, C_BORDER);
        gui_image_draw(0, ix, iy, 0, 0);
    } else if (g_err[0]) {
        /* Keep the failure in the middle of the pane, not just the status bar. */
        gui_text(ax + (aw - (int)strlen(g_err) * 8) / 2, ay + ah / 2 - 8,
                 C_WARN, g_err);
        gui_text(ax + (aw - 8 * 19) / 2, ay + ah / 2 + 14, C_DIM,
                 "Check the file, or try another one");
    } else if (g_count == 0) {
        int cx = ax + aw / 2;
        char msg[96];
        snprintf(msg, sizeof(msg), "No pictures found in %s", g_dir);
        gui_text(cx - (int)strlen(msg) * 8 / 2, ay + ah / 2 - 18, C_TEXT, msg);
        gui_text(cx - 8 * 20, ay + ah / 2 + 2, C_DIM,
                 "Put PNG, BMP or PPM files in this folder");
        gui_text(cx - 8 * 22, ay + ah / 2 + 22, C_DIM,
                 "(the directory is read at start-up)");
    }

    /* Status bar: index, name, dimensions, format, size and zoom. */
    gui_fill(0, h - STATUS_H, w, STATUS_H, C_STATUS);
    gui_fill(0, h - STATUS_H, w, 1, C_BORDER);
    gui_text(12, h - STATUS_H + 4, C_DIM, g_status);

    gui_flush();
    g_need = 0;
}

/* ---- events --------------------------------------------------------- */

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_MOUSE:
        if (!(ev->buttons & 1)) break;
        if (ev->y < TOOL_H) {
            if (ev->x >= g_btn_prev.x && ev->x < g_btn_prev.x + g_btn_prev.w &&
                ev->y >= g_btn_prev.y && ev->y < g_btn_prev.y + g_btn_prev.h) {
                select_pic(g_sel - 1);
            } else if (ev->x >= g_btn_next.x &&
                       ev->x < g_btn_next.x + g_btn_next.w &&
                       ev->y >= g_btn_next.y && ev->y < g_btn_next.y + g_btn_next.h) {
                select_pic(g_sel + 1);
            }
            break;
        }
        if (ev->y >= gui_height() - STATUS_H) break;
        if (ev->x < LIST_W) {
            if (ev->y >= TOOL_H + 4) {
                int idx = g_top + (ev->y - TOOL_H - 4) / ROW_H;
                select_pic(idx);
            }
        } else if (g_count > 0 && g_cur_ok) {
            g_zoom = (g_zoom + 1) % 3;
            if (build_display() == 0) update_status();
            else set_status("image too large for the preview");
            g_need = 1;
        }
        break;

    case GUI_EVT_KEY:
        switch (ev->code) {
        case INPUT_KEY_LEFT:
        case INPUT_KEY_UP:
        case INPUT_KEY_PGUP:
            select_pic(g_sel - 1);
            break;
        case INPUT_KEY_RIGHT:
        case INPUT_KEY_DOWN:
        case INPUT_KEY_PGDN:
        case ' ':
            select_pic(g_sel + 1);
            break;
        case INPUT_KEY_HOME:
            select_pic(0);
            break;
        case INPUT_KEY_END:
            select_pic(g_count - 1);
            break;
        case 'z':
            if (g_count > 0 && g_cur_ok) {
                g_zoom = (g_zoom + 1) % 3;
                if (build_display() == 0) update_status();
                g_need = 1;
            }
            break;
        default:
            break;
        }
        break;

    case GUI_EVT_RESIZE:
        if (g_count > 0 && g_cur_sel == g_sel) {
            if (build_display() != 0) set_error("Image is too large for the preview");
        }
        g_need = 1;
        break;

    case GUI_EVT_CLOSE:
        exit(0);

    default:
        break;
    }
}

/* The file manager launches the viewer as "/bin/image.lgy <path>". Split that
 * path into the album directory and the file to select, so a double click
 * opens the right picture instead of always starting at the first one. */
static void use_launch_path(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(g_dir, sizeof(g_dir), "/");
        snprintf(g_want, sizeof(g_want), "%s", path);
        return;
    }

    int n = (int)(slash - path);
    if (n <= 0) {
        snprintf(g_dir, sizeof(g_dir), "/");
    } else {
        if (n >= (int)sizeof(g_dir)) n = (int)sizeof(g_dir) - 1;
        memcpy(g_dir, path, (size_t)n);
        g_dir[n] = 0;
    }
    snprintf(g_want, sizeof(g_want), "%s", slash + 1);
}

static void select_launch_target(void) {
    if (!g_want[0]) return;

    if (!img_name_supported(g_want)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Not a PNG, BMP or PPM file: %s", g_want);
        set_error(msg);
        return;
    }

    for (int i = 0; i < g_count; i++) {
        if (name_cmp(g_pics[i].name, g_want) == 0) {
            g_sel = i;
            return;
        }
    }

    char msg[140];
    snprintf(msg, sizeof(msg), "%s was not found in %s", g_want, g_dir);
    set_error(msg);
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc >= 2 && argv[1] && argv[1][0]) use_launch_path(argv[1]);

    if (gui_init("Pictures", WIN_W, WIN_H) != 0) {
        printf("USER_IMAGE_FAIL: gui_init\n");
        return 1;
    }

    g_raw = malloc(PIC_MAX_BYTES);
    g_aux = malloc(PIC_MAX_BYTES);
    g_rows = malloc((size_t)IMGDEC_MAX_PX * 4 + 4096);
    g_nat = malloc((size_t)IMGDEC_MAX_PX * 4);
    g_disp = malloc((size_t)GUI_IMG_MAX_PX * 4);
    if (!g_raw || !g_aux || !g_rows || !g_nat || !g_disp) {
        printf("USER_IMAGE_FAIL: out of memory\n");
        return 1;
    }

    scan_pictures();
    select_launch_target();
    /* Only fall back to the first picture when no path was requested; otherwise
     * the "not found" message set above must stay on screen. */
    if (g_sel < 0 && g_count > 0 && !g_want[0]) g_sel = 0;
    if (g_sel >= 0) load_current();
    else if (!g_err[0]) update_status();
    draw();

    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 500);
        if (rc < 0) return 0;
        if (rc > 0) handle_event(&ev);
        if (g_need) draw();
    }
}
