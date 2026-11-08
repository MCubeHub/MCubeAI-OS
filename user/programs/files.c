/* Files: an Explorer-style directory browser.
 *
 * The window follows the shape of Windows Explorer: a toolbar with back,
 * forward and refresh buttons plus an address bar, a navigation pane on the
 * left, and a status bar at the bottom. The main pane has two view modes:
 * a details list (name, size, type) and a thumbnail grid for pictures.
 *
 * Everything is reachable with the mouse. A single click selects a row; a
 * second click on the row that is already selected opens it, which is also how
 * a double click behaves on Windows. The keyboard still works as a fallback
 * (arrows, Enter, Backspace, F5, F3 to switch views).
 *
 * Thumbnails are decoded with the shared codec in user/lib/imgdec.c. The
 * pixels of every thumbnail live in one desktop cache slot used as an atlas
 * and are written a rectangle at a time, so a grid repaint sends only small
 * draw commands. Decoding is lazy: at most two images per repaint, which keeps
 * scrolling responsive while a folder full of photos is still being processed.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>
#include <mcube/imgdec.h>

#define MAX_ENTRIES  200
#define ROW_H        26
#define NAV_W        168
#define TOOLBAR_H    36
#define HEADER_H     22
#define STATUS_H     24
#define SB_W         12
#define NAV_ITEM_H   30
#define ADDR_MAX     180
#define DOUBLE_MS    500u

/* Thumbnail grid. Cells are 96 px wide; the picture inside is 64x64 and the
 * file name is clipped to the cell. */
#define THUMB_W       64
#define THUMB_H       64
#define THUMB_CELL_W  96
#define THUMB_CELL_H  106
#define THUMB_COLS    8            /* atlas columns */
#define THUMB_MAX     32           /* atlas slots (8 x 4) */
#define THUMB_MAX_BYTES (1024 * 1024)
#define ATLAS_W       (THUMB_COLS * THUMB_W)
#define ATLAS_H       ((THUMB_MAX / THUMB_COLS) * THUMB_H)

#define C_BG        0x1B1F27
#define C_TOOLBAR   0x22262F
#define C_NAV       0x171B22
#define C_NAV_HOT   0x2A303B
#define C_LIST      0x20242D
#define C_LIST_ALT  0x232830
#define C_HEADER    0x1A1E26
#define C_LINE      0x30353F
#define C_TEXT      0xE6EDF7
#define C_DIM       0x98A3B3
#define C_SEL       0x0F5CBD
#define C_SEL_SOFT  0x2A4E7E
#define C_ACCENT    0x4CC2FF
#define C_FOLDER    0xE8B44A
#define C_FOLDER_D  0xB5852F
#define C_BTN       0x2C323D
#define C_BTN_HOT   0x39414F
#define C_BORDER    0x3A414D
#define C_STATUS    0x191D25

typedef struct {
    char name[64];
    u32 type;
    u64 size;
} entry_t;

static char g_cwd[256] = "/";
static entry_t g_entries[MAX_ENTRIES];
static int g_count;
static int g_sel = -1;
static int g_top;
static int g_rows = 1;
static int g_need = 1;
static char g_status[180] = "";

/* View mode: 0 = details list, 1 = thumbnail grid. */
static int g_view;
static int g_thumb_cols;
static int g_thumb_rows;

/* Thumbnail atlas cache. One desktop slot holds THUMB_COLS x 4 thumbnails of
 * THUMB_W x THUMB_H; each cache slot remembers which entry it was built from.
 * States: 0 empty, 1 ready, 2 failed (do not retry until the folder changes). */
static int g_tslot_idx[THUMB_MAX];
static int g_tslot_state[THUMB_MAX];

/* Decode buffers, allocated on the first thumbnail and reused for all of them.
 * The heap is a bump allocator, so they are never freed. */
static u8 *g_traw;
static u8 *g_taux;
static u8 *g_trows;
static u32 *g_tnat;
static u32 *g_tsmall;
static int g_tbuf_state;      /* 0 not tried, 1 ready, 2 allocation failed */
static u32 *g_tatlas;         /* fixed-size atlas sent once before first PUT */
static int g_tatlas_ready;

/* Address bar editing. Clicking the bar starts editing; Enter navigates. */
static int  g_addr_edit;
static char g_addr[ADDR_MAX];
static int  g_addr_len;

/* Second click on an already selected row opens it, like a double click. */
static int g_last_idx = -1;
static u64 g_last_ms;

/* ---- file clipboard (Ctrl+C / Ctrl+X / Ctrl+V) ---------------------- */

/* One path is remembered, matching the single-selection model of the list.
 * A cut only removes the source after the copy has succeeded, so a failed
 * paste never destroys the original. */
#define FC_PATH 320
static char g_fc_path[FC_PATH];
static int  g_fc_have;
static int  g_fc_cut;

/* Small modal used by Delete: 0 = none, 1 = waiting for confirmation. */
static int  g_prompt;
static char g_prompt_name[64];

/* Defined further down; the clipboard helpers reload the folder. */
static void load_dir(const char *path);

static const char *const g_nav_label[] = { "Home", "Bin", "Docs", "Models", "Pictures" };
static const char *const g_nav_path[]  = { "/", "/bin", "/docs", "/models", "/pictures" };
#define NAV_COUNT ((int)(sizeof(g_nav_path) / sizeof(g_nav_path[0])))

/* ---- helpers -------------------------------------------------------- */

static int text_w(const char *s) { return (int)strlen(s) * 8; }

static void status(const char *s) {
    snprintf(g_status, sizeof(g_status), "%s", s ? s : "");
    g_need = 1;
}

static void join_path(char *out, size_t cap, const char *dir, const char *name) {
    if (strcmp(dir, "/") == 0) snprintf(out, cap, "/%s", name);
    else snprintf(out, cap, "%s/%s", dir, name);
}

static void human_size(u64 n, char *out, size_t cap) {
    if (n < 1024) snprintf(out, cap, "%llu B", (unsigned long long)n);
    else if (n < 1024u * 1024u) snprintf(out, cap, "%llu KB", (unsigned long long)((n + 512) / 1024));
    else snprintf(out, cap, "%llu MB", (unsigned long long)((n + 512u * 1024u) / (1024u * 1024u)));
}

/* ---- file operations ------------------------------------------------ */

static int entry_exists(const char *path) {
    mcube_stat_t st;
    return stat(path, &st) == 0;
}

static int dot_entry(const char *name) {
    return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

/* The FAT driver now hides "." and "..", but the checks stay here as well:
 * a future backend must not be able to turn a copy into a parent loop. */
static int copy_file_bytes(const char *src, const char *dst) {
    int in = open(src, VFS_O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (out < 0) { close(in); return -1; }

    static char buf[4096];   /* static: user stacks are small */
    int ok = 1;
    for (;;) {
        long n = read(in, buf, sizeof(buf));
        if (n < 0) { ok = 0; break; }
        if (n == 0) break;
        long off = 0;
        while (off < n) {
            long w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) { ok = 0; break; }
            off += w;
        }
        if (!ok) break;
    }

    close(out);
    close(in);
    return ok ? 0 : -1;
}

static int copy_any(const char *src, const char *dst, int depth) {
    if (depth > 8) return -1;

    mcube_stat_t st;
    if (stat(src, &st) != 0) return -1;
    if (st.type != VFS_TYPE_DIR) return copy_file_bytes(src, dst);

    if (!entry_exists(dst) && mkdir(dst, 0) != 0) return -1;

    for (int i = 0; ; i++) {
        mcube_dirent_t de;
        if (mc_readdir(src, i, &de) != 0) break;
        if (dot_entry(de.name)) continue;

        char cs[FC_PATH], cd[FC_PATH];
        join_path(cs, sizeof(cs), src, de.name);
        join_path(cd, sizeof(cd), dst, de.name);
        if (copy_any(cs, cd, depth + 1) != 0) return -1;
    }
    return 0;
}

static int remove_any(const char *path, int depth) {
    if (depth > 8) return -1;

    mcube_stat_t st;
    if (stat(path, &st) != 0) return -1;
    if (st.type != VFS_TYPE_DIR) return unlink(path) == 0 ? 0 : -1;

    /* Re-read index 0 after every delete: the entries shift down, so a fixed
     * index would skip every second child. */
    for (;;) {
        mcube_dirent_t de;
        int found = 0;
        for (int i = 0; ; i++) {
            if (mc_readdir(path, i, &de) != 0) break;
            if (dot_entry(de.name)) continue;
            found = 1;
            break;
        }
        if (!found) break;

        char child[FC_PATH];
        join_path(child, sizeof(child), path, de.name);
        if (remove_any(child, depth + 1) != 0) return -1;
    }
    return rmdir(path) == 0 ? 0 : -1;
}

/* Windows-style conflict handling, but kept 8.3: the FAT32 driver can only
 * create short names (no LFN writer yet), so "SUNSET.PNG" becomes
 * "SUNSET2.PNG" instead of "SUNSET (2).PNG". */
static void unique_dest(const char *dir, const char *name, char *out, size_t cap) {
    join_path(out, cap, dir, name);
    if (!entry_exists(out)) return;

    char base[16], ext[8];
    snprintf(base, sizeof(base), "%s", name);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) {
        snprintf(ext, sizeof(ext), "%s", dot);
        dot[0] = 0;
    } else {
        ext[0] = 0;
    }

    for (int n = 2; n < 10; n++) {
        int keep = (int)strlen(base);
        if (keep > 7) keep = 7;       /* reserve one character for the digit */
        /* The in-tree printf has no "%.*s" precision, so truncate by hand. */
        char saved = base[keep];
        base[keep] = 0;
        char stem[10];
        snprintf(stem, sizeof(stem), "%s%d", base, n);
        base[keep] = saved;
        char cand[32];
        snprintf(cand, sizeof(cand), "%s%s", stem, ext);
        join_path(out, cap, dir, cand);
        if (!entry_exists(out)) return;
    }
}

static void clip_copy(int cut) {
    if (g_sel < 0 || g_sel >= g_count) {
        status("Select a file or folder first");
        return;
    }
    join_path(g_fc_path, sizeof(g_fc_path), g_cwd, g_entries[g_sel].name);
    g_fc_have = 1;
    g_fc_cut = cut;

    char msg[120];
    snprintf(msg, sizeof(msg), "%s: %s", cut ? "Cut" : "Copied",
             g_entries[g_sel].name);
    status(msg);
}

static void clip_paste(void) {
    if (!g_fc_have) {
        status("Clipboard is empty");
        return;
    }

    const char *name = strrchr(g_fc_path, '/');
    name = name ? name + 1 : g_fc_path;

    char dst[FC_PATH];
    unique_dest(g_cwd, name, dst, sizeof(dst));

    int was_cut = g_fc_cut;
    int copied = copy_any(g_fc_path, dst, 0) == 0;
    int removed = 0;
    if (copied && was_cut) {
        removed = remove_any(g_fc_path, 0) == 0;
        if (removed) { g_fc_have = 0; g_fc_cut = 0; }
    }

    load_dir(g_cwd);
    if (!copied) status("Paste failed");
    else if (was_cut && !removed) status("Moved, but the source could not be removed");
    else status(was_cut ? "Moved" : "Pasted a copy");
}

static void delete_request(void) {
    if (g_sel < 0 || g_sel >= g_count) {
        status("Select a file or folder first");
        return;
    }
    snprintf(g_prompt_name, sizeof(g_prompt_name), "%s", g_entries[g_sel].name);
    g_prompt = 1;
    g_need = 1;
}

static void delete_commit(void) {
    char full[FC_PATH];
    join_path(full, sizeof(full), g_cwd, g_prompt_name);

    int ok = remove_any(full, 0) == 0;
    if (g_fc_have && strcmp(g_fc_path, full) == 0) {
        g_fc_have = 0;
        g_fc_cut = 0;
    }

    g_prompt = 0;
    load_dir(g_cwd);
    status(ok ? "Deleted" : "Delete failed");
}

static void fill_frame(int x, int y, int w, int h, u32 fill, u32 border) {
    gui_fill(x, y, w, h, fill);
    gui_rect(x, y, w, h, border);
}

/* Small folder / page glyphs, built out of fills: the GUI has no images. */
static void draw_folder(int x, int y, int on_sel) {
    gui_fill(x, y + 3, 11, 3, on_sel ? 0xCFE0F5 : C_FOLDER_D);
    gui_fill(x + 2, y + 1, 6, 3, on_sel ? 0xCFE0F5 : C_FOLDER);
    gui_fill(x, y + 5, 15, 9, on_sel ? 0xDCE9F8 : C_FOLDER);
    gui_fill(x, y + 5, 15, 1, on_sel ? 0xF2F8FF : 0xF6D79A);
}

static void draw_page(int x, int y, int on_sel) {
    u32 body = on_sel ? 0xE2ECFA : 0xC9D2DE;
    u32 fold = on_sel ? 0xAFC4DE : 0x8C97A6;
    gui_fill(x + 1, y + 1, 12, 14, body);
    gui_fill(x + 3, y + 4, 8, 1, fold);
    gui_fill(x + 3, y + 7, 8, 1, fold);
    gui_fill(x + 3, y + 10, 6, 1, fold);
}

static void draw_arrow(int x, int y, int dir, u32 color) {
    /* A chevron pointing left (dir < 0) or right, drawn as stepping columns. */
    for (int i = 0; i < 5; i++) {
        int col = dir < 0 ? x + 4 - i : x + i;
        gui_fill(col, y + i, 2, 2, color);
        gui_fill(col, y + 8 - i, 2, 2, color);
    }
}

static void draw_refresh(int x, int y, u32 color) {
    gui_fill(x, y, 12, 2, color);
    gui_fill(x, y, 2, 10, color);
    gui_fill(x + 10, y + 2, 2, 10, color);
    gui_fill(x, y + 10, 12, 2, color);
    gui_fill(x + 8, y - 2, 5, 5, color);
    gui_fill(x + 8, y + 11, 5, 5, color);
}

/* Larger glyphs for the thumbnail cells: folders and non-picture files still
 * need something recognisable next to the preview. */
static void draw_folder_big(int x, int y, int on_sel) {
    u32 tab = on_sel ? 0xDCE9F8 : C_FOLDER_D;
    u32 body = on_sel ? 0xEAF3FF : C_FOLDER;
    gui_fill(x + 6, y + 2, 18, 9, tab);
    gui_fill(x, y + 7, 40, 22, body);
    gui_fill(x, y + 7, 40, 2, on_sel ? 0xF4FAFF : 0xF6D79A);
}

static void draw_page_big(int x, int y, int on_sel) {
    u32 body = on_sel ? 0xE2ECFA : 0xC9D2DE;
    u32 fold = on_sel ? 0xAFC4DE : 0x8C97A6;
    gui_fill(x + 8, y, 26, 32, body);
    gui_fill(x + 12, y + 7, 18, 2, fold);
    gui_fill(x + 12, y + 13, 18, 2, fold);
    gui_fill(x + 12, y + 19, 13, 2, fold);
}

/* Icon for the view-mode button: shows the mode a click switches to. */
static void draw_view_icon(int x, int y, u32 color, int grid_target) {
    if (grid_target) {
        gui_fill(x, y, 5, 5, color);
        gui_fill(x + 7, y, 5, 5, color);
        gui_fill(x, y + 7, 5, 5, color);
        gui_fill(x + 7, y + 7, 5, 5, color);
    } else {
        for (int i = 0; i < 3; i++) gui_fill(x, y + i * 5, 12, 2, color);
    }
}

/* ---- view layout and thumbnails ------------------------------------- */

static void grid_geom(int *cols, int *rows, int *gx, int *gy, int *gw, int *gh) {
    int w = gui_width(), h = gui_height();
    *gx = NAV_W + 1;
    *gy = TOOLBAR_H + 8;
    *gw = w - *gx - SB_W;
    *gh = h - *gy - STATUS_H - 8;
    if (*gw < 1) *gw = 1;
    if (*gh < 1) *gh = 1;

    int c = *gw / THUMB_CELL_W;
    if (c < 1) c = 1;
    int r = *gh / THUMB_CELL_H;
    if (r < 1) r = 1;
    *cols = c;
    *rows = r;
}

/* Visible item capacity and scroll clamp. Called once per repaint, before the
 * scrollbar and the list/grid paint, so both always agree on g_rows. */
static void layout_view(void) {
    if (g_view == 0) {
        int list_h = gui_height() - TOOLBAR_H - HEADER_H - STATUS_H;
        g_rows = list_h / ROW_H;
        if (g_rows < 1) g_rows = 1;
        g_thumb_cols = 0;
        g_thumb_rows = 0;
    } else {
        int cols, rows, gx, gy, gw, gh;
        grid_geom(&cols, &rows, &gx, &gy, &gw, &gh);
        g_thumb_cols = cols;
        g_thumb_rows = rows;
        g_rows = cols * rows;
    }

    if (g_sel >= 0) {
        if (g_sel < g_top) g_top = g_sel;
        if (g_sel >= g_top + g_rows) g_top = g_sel - g_rows + 1;
    }
    int max_top = g_count - g_rows;
    if (max_top < 0) max_top = 0;
    if (g_top > max_top) g_top = max_top;
    if (g_top < 0) g_top = 0;
}

static int thumb_slot(int idx) { return idx % THUMB_MAX; }

static int thumb_ready(int idx) {
    int s = thumb_slot(idx);
    return g_tslot_state[s] == 1 && g_tslot_idx[s] == idx;
}

static int thumb_failed(int idx) {
    int s = thumb_slot(idx);
    return g_tslot_state[s] == 2 && g_tslot_idx[s] == idx;
}

static int thumb_buffers(void) {
    if (g_tbuf_state) return g_tbuf_state == 1;
    g_traw = malloc(THUMB_MAX_BYTES);
    g_taux = malloc(THUMB_MAX_BYTES);
    g_trows = malloc((size_t)IMGDEC_MAX_PX * 4 + 4096);
    g_tnat = malloc((size_t)IMGDEC_MAX_PX * 4);
    g_tsmall = malloc(THUMB_W * THUMB_H * 4);
    if (!g_traw || !g_taux || !g_trows || !g_tnat || !g_tsmall) {
        g_tbuf_state = 2;
        return 0;
    }
    g_tbuf_state = 1;
    return 1;
}

/* Decode entry `idx` and place it in its atlas slot. Returns 1 when the
 * thumbnail is ready, 0 otherwise; failures are latched so a broken file is
 * not decoded again on every repaint. */
static int atlas_prepare(void);

static int thumb_build(int idx) {
    int slot = thumb_slot(idx);
    if (thumb_ready(idx)) return 1;
    if (thumb_failed(idx)) return 0;

    g_tslot_idx[slot] = idx;
    g_tslot_state[slot] = 2;          /* assume failure until proven otherwise */
    if (!thumb_buffers()) return 0;
    if (!atlas_prepare()) return 0;

    char full[384];
    join_path(full, sizeof(full), g_cwd, g_entries[idx].name);
    int fd = open(full, VFS_O_RDONLY);
    if (fd < 0) return 0;

    mcube_stat_t st;
    if (fstat(fd, &st) != 0 || st.size == 0 || st.size > THUMB_MAX_BYTES) {
        close(fd);
        return 0;
    }

    int got = 0;
    while (got < (int)st.size) {
        ssize_t n = read(fd, g_traw + got, (size_t)((int)st.size - got));
        if (n <= 0) break;
        got += (int)n;
    }
    close(fd);
    if (got <= 0) return 0;

    int nw = 0, nh = 0;
    if (img_decode(g_traw, got, g_tnat, &nw, &nh, g_taux, THUMB_MAX_BYTES, g_trows,
                   IMGDEC_MAX_PX * 4 + 4096) != 0) {
        return 0;
    }

    /* Fit inside the 64x64 cell without upscaling, then centre it on a dark
     * tile so cells line up no matter what aspect ratio the picture has. */
    int dw = nw, dh = nh;
    if (dw > THUMB_W || dh > THUMB_H) {
        if ((long long)THUMB_W * dh <= (long long)THUMB_H * dw) {
            dw = THUMB_W;
            dh = (int)((long long)nh * THUMB_W / nw);
        } else {
            dh = THUMB_H;
            dw = (int)((long long)nw * THUMB_H / nh);
        }
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    for (int i = 0; i < THUMB_W * THUMB_H; i++) g_tsmall[i] = 0x0E131A;
    int ox = (THUMB_W - dw) / 2;
    int oy = (THUMB_H - dh) / 2;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long long)y * nh / dh);
        if (sy >= nh) sy = nh - 1;
        const u32 *srow = g_tnat + (size_t)sy * (size_t)nw;
        u32 *drow = g_tsmall + (size_t)(oy + y) * THUMB_W + ox;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long long)x * nw / dw);
            if (sx >= nw) sx = nw - 1;
            drow[x] = srow[sx];
        }
    }

    int ax = (slot % THUMB_COLS) * THUMB_W;
    int ay = (slot / THUMB_COLS) * THUMB_H;
    if (gui_image_put(0, ax, ay, THUMB_W, THUMB_H, g_tsmall) != 0) return 0;

    g_tslot_state[slot] = 1;
    return 1;
}

/* Decode a bounded number of visible thumbnails per repaint. Anything left
 * over schedules another frame, so the grid fills in progressively instead of
 * blocking the event loop while a whole folder is decoded. Returns 1 when more
 * work is pending. */
static int thumb_pump(void) {
    if (g_view != 1 || g_count == 0) return 0;

    int budget = 2;
    int pending = 0;
    int end = g_top + g_rows;
    if (end > g_count) end = g_count;

    for (int i = g_top; i < end; i++) {
        entry_t *e = &g_entries[i];
        if (e->type == VFS_TYPE_DIR) continue;
        if (!img_name_supported(e->name)) continue;
        if (thumb_ready(i) || thumb_failed(i)) continue;
        if (budget <= 0) {
            pending = 1;
            break;
        }
        thumb_build(i);
        budget--;
    }
    return pending;
}

/* Publish the full atlas once before any rectangle is written. Sub-rectangle
 * uploads can grow a desktop slot, and a growing slot has to shift its rows to
 * a wider stride; doing that while thumbnails are already cached makes the
 * earlier tiles read back at the wrong stride. A fixed-size atlas keeps the
 * stride constant, so every gui_image_put/gui_image_blit uses the same width. */
static int atlas_prepare(void) {
    if (g_tatlas_ready == 1) return 1;
    if (g_tatlas_ready == 2) return 0;

    if (!g_tatlas) g_tatlas = malloc((size_t)ATLAS_W * ATLAS_H * 4u);
    if (!g_tatlas) {
        g_tatlas_ready = 2;
        return 0;
    }
    for (int i = 0; i < ATLAS_W * ATLAS_H; i++) g_tatlas[i] = 0x10161E;
    if (gui_image_load(0, ATLAS_W, ATLAS_H, g_tatlas) != 0) {
        g_tatlas_ready = 2;
        return 0;
    }
    g_tatlas_ready = 1;
    return 1;
}

static void toggle_view(void) {
    g_view = g_view ? 0 : 1;
    if (g_view) atlas_prepare();
    g_top = 0;
    if (g_sel >= 0) g_top = g_sel;
    g_need = 1;
}

/* ---- directory loading ---------------------------------------------- */

static void sort_entries(void) {
    for (int i = 1; i < g_count; i++) {
        entry_t tmp = g_entries[i];
        int j = i - 1;
        while (j >= 0) {
            entry_t *a = &g_entries[j];
            int a_dir = a->type == VFS_TYPE_DIR;
            int t_dir = tmp.type == VFS_TYPE_DIR;
            int swap;
            if (a_dir != t_dir) swap = a_dir ? 0 : 1;
            else swap = strcmp(a->name, tmp.name) > 0;
            if (!swap) break;
            g_entries[j + 1] = *a;
            j--;
        }
        g_entries[j + 1] = tmp;
    }
}

static void load_dir(const char *path) {
    char want[256];
    snprintf(want, sizeof(want), "%s", path && path[0] ? path : "/");

    g_count = 0;
    g_sel = -1;
    g_top = 0;
    g_last_idx = -1;
    g_addr_edit = 0;

    /* A different folder means different thumbnails; drop the atlas mapping
     * (the atlas itself is overwritten slot by slot as new previews arrive). */
    for (int i = 0; i < THUMB_MAX; i++) {
        g_tslot_idx[i] = -1;
        g_tslot_state[i] = 0;
    }

    int fd = open(want, VFS_O_RDONLY);
    if (fd < 0) {
        status("This folder cannot be opened.");
        snprintf(g_cwd, sizeof(g_cwd), "%s", want);
        g_need = 1;
        return;
    }

    mcube_dirent_t ent;
    while (g_count < MAX_ENTRIES && getdents64(fd, &ent, sizeof(ent)) > 0) {
        u32 n = ent.namelen;
        if (n >= sizeof(g_entries[0].name)) n = sizeof(g_entries[0].name) - 1;
        if (n == 0) continue;
        memcpy(g_entries[g_count].name, ent.name, n);
        g_entries[g_count].name[n] = 0;
        g_entries[g_count].type = ent.type;
        g_entries[g_count].size = 0;

        char full[384];
        join_path(full, sizeof(full), want, g_entries[g_count].name);
        mcube_stat_t st;
        if (stat(full, &st) == 0) {
            g_entries[g_count].type = st.type;
            g_entries[g_count].size = st.size;
        }
        g_count++;
    }
    close(fd);

    sort_entries();
    snprintf(g_cwd, sizeof(g_cwd), "%s", want);
    if (g_count == 0) status("This folder is empty.");
    else {
        char msg[96];
        snprintf(msg, sizeof(msg), "%d item%s", g_count, g_count == 1 ? "" : "s");
        status(msg);
    }
    g_need = 1;
}

static void parent_dir(void) {
    if (strcmp(g_cwd, "/") == 0) return;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", g_cwd);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) load_dir("/");
    else {
        *slash = 0;
        load_dir(tmp);
    }
}

static void activate(int idx) {
    if (idx < 0 || idx >= g_count) return;
    entry_t *e = &g_entries[idx];
    char full[384];
    join_path(full, sizeof(full), g_cwd, e->name);

    if (e->type == VFS_TYPE_DIR) {
        load_dir(full);
        return;
    }

    /* Pictures open in the album viewer, which re-reads the file through VFS;
     * everything else still goes to the text editor. */
    char spec[GUI_TEXT_MAX + 1];
    if (img_name_supported(e->name)) {
        snprintf(spec, sizeof(spec), "/bin/image.lgy %s", full);
        gui_launch(spec);
        status("Opening in Pictures...");
        return;
    }
    snprintf(spec, sizeof(spec), "/bin/editor.lgy %s", full);
    gui_launch(spec);
    status("Opening in Notepad...");
}

/* ---- drawing -------------------------------------------------------- */

static void draw_toolbar(int w) {
    gui_fill(0, 0, w, TOOLBAR_H, C_TOOLBAR);
    gui_fill(0, TOOLBAR_H - 1, w, 1, C_LINE);

    int by = 5;
    int bx = 6;

    /* Back (parent folder) and refresh. */
    int back_hot = 0;
    fill_frame(bx, by, 26, 26, back_hot ? C_BTN_HOT : C_BTN, C_BORDER);
    draw_arrow(bx + 5, by + 6, -1, strcmp(g_cwd, "/") == 0 ? 0x5A6272 : C_TEXT);
    bx += 30;
    fill_frame(bx, by, 26, 26, C_BTN, C_BORDER);
    draw_refresh(bx + 7, by + 8, C_TEXT);
    bx += 34;

    /* View switch: grid icon while in details mode, list icon while in
     * thumbnail mode, so the button always shows what a click will do. */
    fill_frame(bx, by, 26, 26, C_BTN, C_BORDER);
    draw_view_icon(bx + 7, by + 7, C_TEXT, g_view == 0);
    bx += 34;

    /* Address bar: editable while clicked, otherwise the current path. */
    int aw = w - bx - 8;
    if (aw < 40) aw = 40;
    fill_frame(bx, by + 2, aw, 22, g_addr_edit ? 0x10141B : 0x2A2F39,
               g_addr_edit ? C_ACCENT : C_BORDER);
    const char *shown = g_addr_edit ? g_addr : g_cwd;
    char clipped[64];
    int cap = (aw - 16) / 8;
    if (cap < 1) cap = 1;
    if (cap > 63) cap = 63;
    int len = (int)strlen(shown);
    if (len > cap) {
        snprintf(clipped, sizeof(clipped), "%s", shown + (len - cap));
    } else {
        snprintf(clipped, sizeof(clipped), "%s", shown);
    }
    gui_text(bx + 8, by + 5, g_addr_edit ? C_TEXT : C_DIM, clipped);
    if (g_addr_edit && (g_addr_len < cap)) {
        gui_fill(bx + 8 + g_addr_len * 8, by + 5, 1, 16, C_ACCENT);
    }
}

static void draw_nav(int h) {
    int y0 = TOOLBAR_H;
    gui_fill(0, y0, NAV_W, h - y0 - STATUS_H, C_NAV);
    gui_fill(NAV_W, y0, 1, h - y0 - STATUS_H, C_LINE);

    gui_text(10, y0 + 8, C_DIM, "Quick access");
    int y = y0 + 28;
    for (int i = 0; i < NAV_COUNT; i++) {
        int hot = strcmp(g_cwd, g_nav_path[i]) == 0;
        if (hot) {
            gui_fill(0, y, NAV_W, NAV_ITEM_H, C_NAV_HOT);
            gui_fill(0, y, 3, NAV_ITEM_H, C_ACCENT);
        }
        draw_folder(10, y + 9, hot);
        gui_text(30, y + 7, hot ? C_TEXT : C_DIM, g_nav_label[i]);
        y += NAV_ITEM_H;
    }

    gui_fill(12, y + 6, NAV_W - 24, 1, C_LINE);
    gui_text(10, y + 14, C_DIM, "This PC");
    draw_folder(10, y + 34, 0);
    gui_text(30, y + 32, C_DIM, "MCubeAIOS");
}

static void draw_list(int w, int h) {
    int x0 = NAV_W + 1;
    int y0 = TOOLBAR_H;
    int list_w = w - x0 - SB_W;
    int list_h = h - y0 - STATUS_H;
    int body_y = y0 + HEADER_H;

    gui_fill(x0, y0, list_w, list_h, C_LIST);

    /* Column headers, like Explorer's "Name / Size / Type". */
    gui_fill(x0, y0, list_w, HEADER_H, C_HEADER);
    gui_fill(x0, y0 + HEADER_H - 1, list_w, 1, C_LINE);
    gui_text(x0 + 30, y0 + 3, C_DIM, "Name");
    int size_x = x0 + list_w - 120;
    gui_text(size_x, y0 + 3, C_DIM, "Size");
    gui_text(x0 + list_w - 190, y0 + 3, C_DIM, "Type");

    for (int r = 0; r < g_rows; r++) {
        int idx = g_top + r;
        if (idx >= g_count) break;
        int y = body_y + r * ROW_H;
        entry_t *e = &g_entries[idx];
        int sel = idx == g_sel;

        if (sel) gui_fill(x0, y, list_w, ROW_H, C_SEL);
        else if (r & 1) gui_fill(x0, y, list_w, ROW_H, C_LIST_ALT);
        if (sel) gui_fill(x0, y, 2, ROW_H, C_ACCENT);

        int is_dir = e->type == VFS_TYPE_DIR;
        if (is_dir) draw_folder(x0 + 10, y + 7, sel);
        else draw_page(x0 + 10, y + 6, sel);

        char label[64];
        snprintf(label, sizeof(label), "%s", e->name);
        int cap = (list_w - 210) / 8;
        if (cap < 4) cap = 4;
        if ((int)strlen(label) > cap) label[cap] = 0;
        gui_text(x0 + 30, y + 5, sel ? 0xFFFFFF : C_TEXT, label);

        if (is_dir) {
            gui_text(x0 + list_w - 190, y + 5, sel ? 0xE8F1FF : C_DIM, "Folder");
        } else {
            char sz[32];
            human_size(e->size, sz, sizeof(sz));
            gui_text(size_x, y + 5, sel ? 0xE8F1FF : C_DIM, sz);
            gui_text(x0 + list_w - 190, y + 5, sel ? 0xE8F1FF : C_DIM, "File");
        }
        gui_fill(x0, y + ROW_H - 1, list_w, 1, sel ? C_SEL_SOFT : C_LINE);
    }

    if (g_count == 0) {
        gui_text(x0 + 30, body_y + 20, C_DIM, "This folder is empty.");
    }
}

static void draw_grid(int w, int h) {
    int cols, rows, gx, gy, gw, gh;
    grid_geom(&cols, &rows, &gx, &gy, &gw, &gh);

    gui_fill(gx, TOOLBAR_H, w - gx - SB_W, h - TOOLBAR_H - STATUS_H, C_LIST);

    /* Centre the fixed-width cells in the pane so the grid does not hug one
     * edge on wide windows. */
    int grid_w = cols * THUMB_CELL_W;
    int ox = gx + (gw - grid_w) / 2;
    if (ox < gx) ox = gx;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = g_top + r * cols + c;
            if (idx >= g_count) break;

            entry_t *e = &g_entries[idx];
            int sel = idx == g_sel;
            int cx = ox + c * THUMB_CELL_W;
            int cy = gy + r * THUMB_CELL_H;
            int tx = cx + (THUMB_CELL_W - THUMB_W) / 2;
            int ty = cy + 8;

            if (sel) {
                gui_fill(cx + 2, cy + 2, THUMB_CELL_W - 4, THUMB_CELL_H - 4,
                         C_SEL_SOFT);
                gui_rect(cx + 2, cy + 2, THUMB_CELL_W - 4, THUMB_CELL_H - 4,
                         C_ACCENT);
            }

            int is_dir = e->type == VFS_TYPE_DIR;
            if (is_dir) {
                draw_folder_big(tx + 12, ty + 17, sel);
            } else if (img_name_supported(e->name)) {
                if (thumb_ready(idx)) {
                    int ax = (thumb_slot(idx) % THUMB_COLS) * THUMB_W;
                    int ay = (thumb_slot(idx) / THUMB_COLS) * THUMB_H;
                    gui_image_blit(0, tx, ty, ax, ay, THUMB_W, THUMB_H);
                } else {
                    /* Empty tile until the lazy decoder reaches this cell. */
                    gui_fill(tx, ty, THUMB_W, THUMB_H, 0x10161E);
                    gui_rect(tx, ty, THUMB_W, THUMB_H, C_LINE);
                    gui_text(tx + 16, ty + 24, C_DIM, "IMG");
                }
            } else {
                draw_page_big(tx + 19, ty + 16, sel);
            }

            /* File name under the picture, centred and clipped to the cell. */
            char label[24];
            snprintf(label, sizeof(label), "%s", e->name);
            int cap = (THUMB_CELL_W - 8) / 8;
            if ((int)strlen(label) > cap) label[cap] = 0;
            int lx = cx + (THUMB_CELL_W - (int)strlen(label) * 8) / 2;
            gui_text(lx, ty + THUMB_H + 6, sel ? 0xFFFFFF : C_TEXT, label);
        }
    }

    if (g_count == 0) {
        gui_text(gx + 24, gy + 20, C_DIM, "This folder is empty.");
    }
}

static void draw_scrollbar(int w, int h) {
    if (g_count <= g_rows) return;
    int x = w - SB_W;
    int top_extra = g_view ? 0 : HEADER_H;
    int y0 = TOOLBAR_H + top_extra;
    int list_h = h - TOOLBAR_H - top_extra - STATUS_H;
    gui_fill(x, TOOLBAR_H, SB_W, h - TOOLBAR_H - STATUS_H, 0x1A1E26);
    int thumb_h = list_h * g_rows / g_count;
    if (thumb_h < 24) thumb_h = 24;
    if (thumb_h > list_h) thumb_h = list_h;
    int span = list_h - thumb_h;
    int max_top = g_count - g_rows;
    int off = max_top > 0 ? span * g_top / max_top : 0;
    if (off < 0) off = 0;
    gui_fill(x + 2, y0 + off, SB_W - 4, thumb_h, 0x4A5260);
    gui_fill(x + 2, y0 + off, SB_W - 4, 1, 0x606878);
}

static void draw_status(int w, int h) {
    int y = h - STATUS_H;
    gui_fill(0, y, w, STATUS_H, C_STATUS);
    gui_fill(0, y, w, 1, C_LINE);
    char left[128];
    if (g_sel >= 0 && g_sel < g_count) {
        char full[384];
        join_path(full, sizeof(full), g_cwd, g_entries[g_sel].name);
        snprintf(left, sizeof(left), "%s", full);
    } else {
        snprintf(left, sizeof(left), "%d item%s", g_count, g_count == 1 ? "" : "s");
    }
    gui_text(8, y + 4, C_DIM, left);
    const char *right = g_status;
    int rx = w - 8 - text_w(right);
    if (rx > 8 + text_w(left) + 12) gui_text(rx, y + 4, C_DIM, right);
}

/* Modal confirmation for Delete. The dialog is drawn last so it covers both
 * the list and the thumbnail grid; no mouse hit testing runs while it is open. */
static void draw_prompt(int w, int h) {
    if (!g_prompt) return;

    int bw = 420, bh = 118;
    int x = (w - bw) / 2, y = (h - bh) / 2;

    gui_fill(x + 4, y + 4, bw, bh, 0x0A0D12);
    fill_frame(x, y, bw, bh, 0x1E2430, C_BORDER);
    gui_text(x + 16, y + 14, C_TEXT, "Delete this item?");
    char line[96];
    snprintf(line, sizeof(line), "%s", g_prompt_name);
    gui_text(x + 16, y + 38, C_ACCENT, line);
    gui_text(x + 16, y + 76, C_DIM, "Enter  Delete");
    gui_text(x + 140, y + 76, C_DIM, "Esc   Cancel");
}

static void draw(void) {
    int w = gui_width(), h = gui_height();
    layout_view();
    int pending = thumb_pump();
    gui_clear(C_BG);
    draw_toolbar(w);
    draw_nav(h);
    if (g_view) draw_grid(w, h);
    else draw_list(w, h);
    draw_scrollbar(w, h);
    draw_status(w, h);
    draw_prompt(w, h);
    gui_flush();
    /* Keep the pending flag so the next loop iteration decodes the rest of the
     * visible thumbnails; clearing it here would strand the placeholders. */
    g_need = pending;
}

/* ---- hit testing ---------------------------------------------------- */

static int nav_hit(int x, int y) {
    if (x >= NAV_W) return -1;
    int top = TOOLBAR_H + 28;
    for (int i = 0; i < NAV_COUNT; i++) {
        int iy = top + i * NAV_ITEM_H;
        if (y >= iy && y < iy + NAV_ITEM_H) return i;
    }
    return -1;
}

static int row_hit(int x, int y) {
    int x0 = NAV_W + 1;
    if (x < x0 || x >= gui_width() - SB_W) return -1;

    if (g_view) {
        int cols, rows, gx, gy, gw, gh;
        grid_geom(&cols, &rows, &gx, &gy, &gw, &gh);
        int grid_w = cols * THUMB_CELL_W;
        int ox = gx + (gw - grid_w) / 2;
        if (ox < gx) ox = gx;
        if (y < gy || y >= gy + rows * THUMB_CELL_H) return -1;

        int col = (x - ox) / THUMB_CELL_W;
        if (col < 0 || col >= cols) return -1;
        int row = (y - gy) / THUMB_CELL_H;
        int idx = g_top + row * cols + col;
        if (idx < 0 || idx >= g_count) return -1;
        return idx;
    }

    int y0 = TOOLBAR_H + HEADER_H;
    int list_h = gui_height() - TOOLBAR_H - HEADER_H - STATUS_H;
    if (y < y0 || y >= y0 + list_h) return -1;
    int row = (y - y0) / ROW_H;
    int idx = g_top + row;
    if (idx < 0 || idx >= g_count) return -1;
    return idx;
}

static void click_scrollbar(int x, int y) {
    if (g_count <= g_rows) return;
    int h = gui_height();
    int top_extra = g_view ? 0 : HEADER_H;
    int list_h = h - TOOLBAR_H - top_extra - STATUS_H;
    int thumb_h = list_h * g_rows / g_count;
    if (thumb_h < 24) thumb_h = 24;
    if (thumb_h > list_h) thumb_h = list_h;
    int span = list_h - thumb_h;
    int max_top = g_count - g_rows;
    int off = max_top > 0 ? span * g_top / max_top : 0;
    int y0 = TOOLBAR_H + top_extra;
    if (y < y0 + off) g_top -= g_rows;
    else if (y >= y0 + off + thumb_h) g_top += g_rows;
    if (g_top < 0) g_top = 0;
    if (g_top > max_top) g_top = max_top;
    (void)x;
    g_need = 1;
}

/* ---- input ---------------------------------------------------------- */

static void begin_address_edit(void) {
    g_addr_edit = 1;
    g_addr_len = 0;
    g_addr[0] = 0;
    g_need = 1;
}

static void commit_address(void) {
    g_addr_edit = 0;
    if (g_addr_len > 0) load_dir(g_addr);
    else g_need = 1;
}

static void key_event(u32 c, u32 mods) {
    /* Release events only matter for modifier bookkeeping, which the desktop
     * already folds into mods, so nothing else here consumes them. */
    if (c & INPUT_KEY_RELEASE) return;

    if (g_prompt) {
        if (c == '\n') delete_commit();
        else if (c == 27) { g_prompt = 0; status("Delete cancelled"); }
        return;
    }

    if (g_addr_edit) {
        if (c == '\n') {
            commit_address();
        } else if (c == '\b') {
            if (g_addr_len > 0) g_addr[--g_addr_len] = 0;
            g_need = 1;
        } else if (c == 27) {
            g_addr_edit = 0;
            g_need = 1;
        } else if (c >= 32 && c < 0x100 && g_addr_len < ADDR_MAX - 1) {
            g_addr[g_addr_len++] = (char)c;
            g_addr[g_addr_len] = 0;
            g_need = 1;
        }
        return;
    }

    if (mods & GUI_MOD_CTRL) {
        if (c == 'c' || c == 'C') { clip_copy(0); return; }
        if (c == 'x' || c == 'X') { clip_copy(1); return; }
        if (c == 'v' || c == 'V') { clip_paste(); return; }
    }

    if (c == INPUT_KEY_UP) {
        int step = g_view ? g_thumb_cols : 1;
        if (g_sel >= 0 && g_sel - step >= 0) { g_sel -= step; g_need = 1; }
        else if (g_sel < 0 && g_count > 0) { g_sel = 0; g_need = 1; }
    } else if (c == INPUT_KEY_DOWN) {
        int step = g_view ? g_thumb_cols : 1;
        if (g_sel < 0) { g_sel = 0; g_need = 1; }
        else if (g_sel + step < g_count) { g_sel += step; g_need = 1; }
    } else if (g_view && c == INPUT_KEY_LEFT) {
        if (g_sel > 0) { g_sel--; g_need = 1; }
    } else if (g_view && c == INPUT_KEY_RIGHT) {
        if (g_sel < g_count - 1) { g_sel++; g_need = 1; }
    } else if (c == INPUT_KEY_HOME) {
        g_sel = 0; g_need = 1;
    } else if (c == INPUT_KEY_END) {
        g_sel = g_count > 0 ? g_count - 1 : -1; g_need = 1;
    } else if (c == '\n') {
        activate(g_sel);
    } else if (c == '\b') {
        parent_dir();
    } else if (c == INPUT_KEY_F5) {
        load_dir(g_cwd);
    } else if (c == INPUT_KEY_F3) {
        toggle_view();
    } else if (c == INPUT_KEY_DELETE) {
        delete_request();
    } else if (c == INPUT_KEY_F2) {
        begin_address_edit();
    } else if (c >= 32 && c < 0x100) {
        /* Type-ahead: jump to the first entry starting with this letter. */
        char ch = (char)c;
        int start = g_sel < 0 ? 0 : g_sel + 1;
        for (int i = 0; i < g_count; i++) {
            int idx = (start + i) % g_count;
            char a = g_entries[idx].name[0];
            if (a >= 'a' && a <= 'z') a -= 32;
            char b = ch;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a == b) { g_sel = idx; g_need = 1; break; }
        }
    }
}

static void mouse_press(int x, int y, u32 buttons) {
    if (!(buttons & 1)) return;
    if (g_prompt) return;      /* the confirmation dialog is keyboard only */

    int w = gui_width(), h = gui_height();

    /* Toolbar. */
    if (y < TOOLBAR_H) {
        if (g_addr_edit && x >= 90) {
            /* Clicking the address bar again re-selects its whole text. */
            g_addr_len = 0;
            g_addr[0] = 0;
            g_need = 1;
            return;
        }
        if (x < 32) { parent_dir(); return; }
        if (x < 66) { load_dir(g_cwd); return; }
        if (x < 100) { toggle_view(); return; }
        begin_address_edit();
        return;
    }

    /* Status bar: nothing to hit. */
    if (y >= h - STATUS_H) return;

    /* Navigation pane. */
    int nav = nav_hit(x, y);
    if (nav >= 0) {
        load_dir(g_nav_path[nav]);
        return;
    }

    /* Scrollbar track. */
    if (x >= w - SB_W) {
        click_scrollbar(x, y);
        return;
    }

    /* List rows: select, repaint, and open on a second click. */
    int idx = row_hit(x, y);
    if (idx < 0) {
        g_sel = -1;
        g_need = 1;
        return;
    }

    u64 now = 0;
    mcube_time_t ts;
    if (mc_time(&ts) == 0) now = ts.uptime_ms;

    int reopen = idx == g_sel ||
                 (idx == g_last_idx && now >= g_last_ms && now - g_last_ms < DOUBLE_MS);
    g_sel = idx;
    g_last_idx = idx;
    g_last_ms = now;
    g_need = 1;
    if (reopen) activate(idx);
}

static void mouse_release(u32 buttons) {
    (void)buttons;
}

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_KEY:
        key_event(ev->code, ev->buttons);
        break;

    case GUI_EVT_MOUSE:
        if (ev->buttons & 1) mouse_press(ev->x, ev->y, ev->buttons);
        else mouse_release(ev->buttons);
        break;

    case GUI_EVT_RESIZE:
        g_need = 1;
        break;

    case GUI_EVT_CLOSE:
        exit(0);

    default:
        break;
    }
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (gui_init("File Explorer", 820, 560) != 0) {
        printf("USER_FILES_FAIL: gui_init\n");
        return 1;
    }

    load_dir(argc >= 2 && argv[1] && argv[1][0] ? argv[1] : "/");
    draw();

    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 200);
        if (rc < 0) return 0;
        if (rc > 0) handle_event(&ev);
        if (g_need) draw();
    }
}
