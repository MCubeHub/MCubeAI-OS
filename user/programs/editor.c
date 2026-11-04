/* Notepad: the MCubeAIOS text editor, modelled on Windows Notepad.
 *
 * Layout: a menu bar (File / Edit / Format / View / Help) with pull-down menus,
 * a word-wrapped text area that accepts the caret being placed with the mouse,
 * an optional scrollbar, and a status bar that reports "Ln X, Col Y".
 * Everything is also available from the keyboard for people who prefer it.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>

#define EDIT_MAX 16384
#define ROW_H    16
#define MENU_H   22
#define ITEM_H   22
#define STATUS_H 20
#define PAD_X    8
#define SB_W     10
#define MAX_VIS  2048

#define C_BG      0x1E1E1E
#define C_MENU    0x2B2B2B
#define C_MENU_HOT 0x3A3A3A
#define C_TEXT    0xE6E6E6
#define C_DIM     0x9AA0A6
#define C_ACCENT  0x4CC2FF
#define C_SEL     0x264F78
#define C_DROP    0x252526
#define C_BORDER  0x3F3F46
#define C_STATUS  0x1B1B1B

static char g_name[128] = "(untitled)";
static char g_buf[EDIT_MAX + 1];
static int  g_len;
static int  g_off;
static int  g_sel_a = -1;      /* anchor of the selection, -1 = none */
static int  g_wrap = 1;
static int  g_top;
static int  g_rows = 1;
static int  g_dirty;
static int  g_need = 1;
static int  g_shift;
static int  g_menu = -1;       /* open pull-down menu, -1 = none */
static int  g_show_status = 1;
static char g_msg[96];

/* Visual (wrapped) lines, rebuilt for the current width before every draw. */
static int g_vpos[MAX_VIS];
static int g_vlen[MAX_VIS];
static int g_vcount;
static int g_vcols = 80;

/* Path prompt used by Open / Save As. mode: 0 none, 1 open, 2 save as. */
static int  g_prompt;
static char g_prompt_buf[180];
static int  g_prompt_len;

/* ---- menus ---------------------------------------------------------- */

static const char *const g_menu_names[] = { "File", "Edit", "Format", "View", "Help" };
#define MENU_COUNT ((int)(sizeof(g_menu_names) / sizeof(g_menu_names[0])))

static const char *const g_file_items[]   = { "New", "Open...", "Save", "Save As...", "Exit" };
static const char *const g_edit_items[]   = { "Select All", "Time / Date" };
static const char *const g_format_items[] = { "Word Wrap", "Font..." };
static const char *const g_view_items[]   = { "Status Bar" };
static const char *const g_help_items[]   = { "About Notepad" };

static const char *const *const g_menu_items[] = {
    g_file_items, g_edit_items, g_format_items, g_view_items, g_help_items
};
static const int g_menu_counts[] = { 5, 2, 2, 1, 1 };

/* ---- helpers -------------------------------------------------------- */

static int text_w(const char *s) { return (int)strlen(s) * 8; }

static void set_msg(const char *s) {
    snprintf(g_msg, sizeof(g_msg), "%s", s ? s : "");
    g_need = 1;
}

static int min_i(int a, int b) { return a < b ? a : b; }
static int max_i(int a, int b) { return a > b ? a : b; }

static void sel_range(int *lo, int *hi) {
    if (g_sel_a < 0) {
        *lo = *hi = g_off;
        return;
    }
    *lo = min_i(g_sel_a, g_off);
    *hi = max_i(g_sel_a, g_off);
}

static void caret_lc(int *line, int *col) {
    int v = 0;
    for (int i = 0; i < g_vcount; i++) {
        if (g_vpos[i] <= g_off) v = i;
        else break;
    }
    *line = v;
    *col = g_off - g_vpos[v];
    if (*col < 0) *col = 0;
}

/* ---- layout --------------------------------------------------------- */

static void layout(int cols) {
    g_vcols = cols < 1 ? 1 : cols;
    g_vcount = 0;
    if (g_len == 0) {
        g_vpos[0] = 0;
        g_vlen[0] = 0;
        g_vcount = 1;
        return;
    }

    int i = 0;
    while (g_vcount < MAX_VIS) {
        int start = i;
        int stop;
        if (g_wrap) {
            int limit = start + g_vcols;
            stop = start;
            while (stop < g_len && g_buf[stop] != '\n' && stop < limit) stop++;
            /* Prefer to break on a space rather than mid-word. */
            if (stop < g_len && g_buf[stop] != '\n' && stop > start) {
                int brk = stop;
                while (brk > start && g_buf[brk - 1] != ' ') brk--;
                if (brk > start) stop = brk;
            }
        } else {
            stop = start;
            while (stop < g_len && g_buf[stop] != '\n') stop++;
        }

        g_vpos[g_vcount] = start;
        g_vlen[g_vcount] = stop - start;
        g_vcount++;

        i = stop;
        if (i < g_len && g_buf[i] == '\n') {
            i++;
            /* A trailing newline still needs an empty line for the caret. */
            if (i >= g_len && g_vcount < MAX_VIS) {
                g_vpos[g_vcount] = g_len;
                g_vlen[g_vcount] = 0;
                g_vcount++;
                break;
            }
        } else if (i >= g_len) {
            break;
        }
    }
}

/* ---- file I/O ------------------------------------------------------- */

static void load_file(const char *path) {
    if (!path || !path[0]) return;
    int fd = open(path, VFS_O_RDONLY);
    if (fd < 0) {
        char m[96];
        snprintf(m, sizeof(m), "%s not found", path);
        set_msg(m);
        return;
    }

    ssize_t n = read(fd, g_buf, EDIT_MAX);
    close(fd);
    if (n < 0) n = 0;
    g_len = (int)n;
    g_buf[g_len] = 0;

    /* Notepad renders tabs on 4-column stops; expanding them on load keeps
     * the layout code simple and the saved text still reads correctly. */
    for (int i = 0; i < g_len; i++) {
        if (g_buf[i] == '\t' && g_len + 3 <= EDIT_MAX) {
            memmove(g_buf + i + 4, g_buf + i + 1, (size_t)(g_len - i - 1));
            g_buf[i] = ' ';
            g_buf[i + 1] = ' ';
            g_buf[i + 2] = ' ';
            g_buf[i + 3] = ' ';
            g_len += 3;
        }
    }

    snprintf(g_name, sizeof(g_name), "%s", path);
    gui_set_title(path);
    g_off = 0;
    g_sel_a = -1;
    g_top = 0;
    g_dirty = 0;
    set_msg("Opened");
}

static void save_file(void) {
    if (strcmp(g_name, "(untitled)") == 0 || g_prompt == 2) {
        g_prompt = 2;
        g_prompt_len = snprintf(g_prompt_buf, sizeof(g_prompt_buf), "%s", "/");
        g_need = 1;
        return;
    }
    int fd = open(g_name, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        set_msg("Save failed");
        return;
    }
    int done = 0;
    while (done < g_len) {
        ssize_t n = write(fd, g_buf + done, (size_t)(g_len - done));
        if (n <= 0) break;
        done += (int)n;
    }
    close(fd);
    if (done == g_len) {
        g_dirty = 0;
        set_msg("Saved");
    } else {
        set_msg("Short write");
    }
}

/* ---- editing -------------------------------------------------------- */

static void delete_selection(void) {
    int lo, hi;
    sel_range(&lo, &hi);
    if (lo == hi) return;
    memmove(g_buf + lo, g_buf + hi, (size_t)(g_len - hi));
    g_len -= hi - lo;
    g_off = lo;
    g_sel_a = -1;
    g_dirty = 1;
    g_need = 1;
}

static void insert_char(char c) {
    delete_selection();
    if (g_len >= EDIT_MAX) {
        set_msg("Buffer full");
        return;
    }
    memmove(g_buf + g_off + 1, g_buf + g_off, (size_t)(g_len - g_off));
    g_buf[g_off++] = c;
    g_len++;
    g_buf[g_len] = 0;
    g_dirty = 1;
    g_need = 1;
}

static void backspace(void) {
    if (g_sel_a >= 0 && g_sel_a != g_off) {
        delete_selection();
        return;
    }
    if (g_off <= 0) return;
    memmove(g_buf + g_off - 1, g_buf + g_off, (size_t)(g_len - g_off));
    g_off--;
    g_len--;
    g_dirty = 1;
    g_need = 1;
}

static void delete_forward(void) {
    if (g_sel_a >= 0 && g_sel_a != g_off) {
        delete_selection();
        return;
    }
    if (g_off >= g_len) return;
    memmove(g_buf + g_off, g_buf + g_off + 1, (size_t)(g_len - g_off - 1));
    g_len--;
    g_dirty = 1;
    g_need = 1;
}

static void move_caret(int to) {
    if (to < 0) to = 0;
    if (to > g_len) to = g_len;
    if (g_shift) {
        if (g_sel_a < 0) g_sel_a = g_off;
    } else {
        g_sel_a = -1;
    }
    g_off = to;
    g_need = 1;
}

static void move_vertical(int dir) {
    int line, col;
    caret_lc(&line, &col);
    int target = line + dir;
    if (target < 0 || target >= g_vcount) return;
    if (col > g_vlen[target]) col = g_vlen[target];
    move_caret(g_vpos[target] + col);
}

static void move_home(void) {
    int line, col;
    caret_lc(&line, &col);
    (void)col;
    move_caret(g_vpos[line]);
}

static void move_end(void) {
    int line, col;
    caret_lc(&line, &col);
    (void)col;
    int end = g_vpos[line] + g_vlen[line];
    /* Skip the newline that follows a wrapped line, so End lands on the
     * last visible character of the logical line only when unwrapped. */
    if (end < g_len && g_buf[end] == '\n' && !g_wrap) move_caret(end);
    else move_caret(end);
}

static void insert_time_date(void) {
    mcube_time_t ts;
    if (mc_time(&ts) != 0) {
        insert_char('?');
        return;
    }
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    u64 secs = ts.epoch;
    u64 days = secs / 86400u;
    u64 rem = secs % 86400u;
    int hour = (int)(rem / 3600u);
    int minute = (int)((rem / 60u) % 60u);
    int second = (int)(rem % 60u);

    /* Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm). */
    i64 z = (i64)days + 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    u64 doe = (u64)(z - era * 146097);
    u64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    u64 y = (u64)(yoe + era * 400);
    u64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    u64 mp = (5 * doy + 2) / 153;
    u64 day = doy - (153 * mp + 2) / 5 + 1;
    i64 m = (i64)mp + (mp < 10 ? 3 : -9);
    if (m <= 2) y++;

    char stamp[64];
    snprintf(stamp, sizeof(stamp), "%s %d %02d:%02d:%02d %04d",
             months[m - 1], (int)day, hour, minute, second, (int)y);
    for (const char *p = stamp; *p; p++) insert_char(*p);
    (void)secs;
}

/* ---- drawing -------------------------------------------------------- */

static int menu_x(int idx) {
    int x = 8;
    for (int i = 0; i < idx; i++) x += text_w(g_menu_names[i]) + 20;
    return x;
}

static void draw_menu_bar(int w) {
    gui_fill(0, 0, w, MENU_H, C_MENU);
    gui_fill(0, MENU_H - 1, w, 1, C_BORDER);
    int x = 8;
    for (int i = 0; i < MENU_COUNT; i++) {
        int bw = text_w(g_menu_names[i]) + 20;
        if (g_menu == i) gui_fill(x, 0, bw, MENU_H, C_MENU_HOT);
        gui_text(x + 10, 3, i == g_menu ? C_TEXT : C_DIM, g_menu_names[i]);
        x += bw;
    }
}

static void draw_dropdown(int w) {
    if (g_menu < 0) return;
    int x = menu_x(g_menu);
    int count = g_menu_counts[g_menu];
    int dw = 176;
    if (x + dw > w) x = w - dw - 4;
    if (x < 0) x = 0;

    gui_fill(x, MENU_H, dw, count * ITEM_H + 6, C_DROP);
    gui_rect(x, MENU_H, dw, count * ITEM_H + 6, C_BORDER);
    for (int i = 0; i < count; i++) {
        int y = MENU_H + 3 + i * ITEM_H;
        gui_text(x + 12, y + 3, C_TEXT, g_menu_items[g_menu][i]);
        if (g_menu == 2 && i == 0 && g_wrap) {
            gui_text(x + dw - 18, y + 3, C_ACCENT, "*");
        }
        if (g_menu == 3 && i == 0 && g_show_status) {
            gui_text(x + dw - 18, y + 3, C_ACCENT, "*");
        }
    }
}

static void draw_text_area(int w, int h) {
    int y0 = MENU_H;
    int area_h = h - y0 - (g_show_status ? STATUS_H : 0);
    if (area_h < ROW_H) area_h = ROW_H;

    int cols = (w - PAD_X * 2 - SB_W) / 8;
    if (cols < 8) cols = 8;
    layout(cols);

    int line, col;
    caret_lc(&line, &col);
    g_rows = area_h / ROW_H;
    if (g_rows < 1) g_rows = 1;
    if (line < g_top) g_top = line;
    if (line >= g_top + g_rows) g_top = line - g_rows + 1;
    if (g_top > g_vcount - g_rows) g_top = g_vcount - g_rows;
    if (g_top < 0) g_top = 0;

    gui_fill(0, y0, w, area_h, C_BG);

    int lo, hi;
    sel_range(&lo, &hi);

    for (int r = 0; r < g_rows; r++) {
        int v = g_top + r;
        if (v >= g_vcount) break;
        int y = y0 + r * ROW_H;
        int pos = g_vpos[v];
        int len = g_vlen[v];
        if (len > 0) gui_textn(PAD_X, y + 1, C_TEXT, g_buf + pos, len);

        /* Highlight the part of this visual line that is selected. */
        if (hi > lo) {
            int a = max_i(lo, pos);
            int b = min_i(hi, pos + len);
            if (b > a) {
                gui_fill(PAD_X + (a - pos) * 8, y, (b - a) * 8, ROW_H, C_SEL);
                gui_textn(PAD_X + (a - pos) * 8, y + 1, C_TEXT, g_buf + a, b - a);
            }
            /* A selection that runs past this line also covers its break. */
            if (hi > pos + len && lo <= pos + len) {
                gui_fill(PAD_X + len * 8, y, 6, ROW_H, C_SEL);
            }
        }
    }

    /* Caret, drawn last so the selection never hides it. */
    int cy = y0 + (line - g_top) * ROW_H;
    int cx = PAD_X + col * 8;
    if (col <= cols) gui_fill(cx, cy + 1, 2, ROW_H - 2, C_ACCENT);

    /* Scrollbar. */
    if (g_vcount > g_rows) {
        int x = w - SB_W;
        gui_fill(x, y0, SB_W, area_h, 0x252526);
        int thumb_h = area_h * g_rows / g_vcount;
        if (thumb_h < 24) thumb_h = 24;
        int span = area_h - thumb_h;
        int max_top = g_vcount - g_rows;
        int off = max_top > 0 ? span * g_top / max_top : 0;
        gui_fill(x + 2, y0 + off, SB_W - 4, thumb_h, 0x4A4A4A);
    }
}

static void draw_status_bar(int w, int h) {
    if (!g_show_status) return;
    int y = h - STATUS_H;
    gui_fill(0, y, w, STATUS_H, C_STATUS);
    gui_fill(0, y, w, 1, C_BORDER);

    int line, col;
    caret_lc(&line, &col);
    char left[64];
    snprintf(left, sizeof(left), "Ln %d, Col %d", line + 1, col + 1);
    gui_text(8, y + 2, C_DIM, left);

    char right[128];
    snprintf(right, sizeof(right), "%s%s  %d bytes",
             g_dirty ? "* " : "", g_name, g_len);
    int rx = w - 8 - text_w(right);
    if (rx > 8 + text_w(left) + 16) gui_text(rx, y + 2, C_DIM, right);
}

static void draw_prompt(int w) {
    if (!g_prompt) return;
    int y = MENU_H;
    gui_fill(0, y, w, 28, C_DROP);
    gui_fill(0, y + 27, w, 1, C_BORDER);
    gui_text(8, y + 6, C_ACCENT, g_prompt == 1 ? "Open:" : "Save as:");
    gui_text(80, y + 6, C_TEXT, g_prompt_buf);
    int caret = 80 + text_w(g_prompt_buf);
    if (caret < w - 8) gui_fill(caret, y + 6, 1, 16, C_ACCENT);
}

static void draw(void) {
    int w = gui_width(), h = gui_height();
    gui_clear(C_BG);
    draw_menu_bar(w);
    draw_text_area(w, h);
    draw_status_bar(w, h);
    if (g_prompt) draw_prompt(w);
    draw_dropdown(w);
    gui_flush();
    g_need = 0;
}

/* ---- menu actions --------------------------------------------------- */

static void new_file(void) {
    g_len = 0;
    g_buf[0] = 0;
    g_off = 0;
    g_sel_a = -1;
    g_top = 0;
    g_dirty = 0;
    snprintf(g_name, sizeof(g_name), "(untitled)");
    gui_set_title("Notepad");
    set_msg("New file");
}

static void start_prompt(int mode) {
    g_prompt = mode;
    g_prompt_len = 0;
    g_prompt_buf[0] = 0;
    g_need = 1;
}

static void commit_prompt(void) {
    int mode = g_prompt;
    g_prompt = 0;
    if (g_prompt_len == 0) {
        g_need = 1;
        return;
    }
    if (mode == 1) {
        load_file(g_prompt_buf);
    } else if (mode == 2) {
        snprintf(g_name, sizeof(g_name), "%s", g_prompt_buf);
        gui_set_title(g_name);
        {
            int fd = open(g_name, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
            if (fd < 0) {
                set_msg("Save failed");
                g_need = 1;
                return;
            }
            int done = 0;
            while (done < g_len) {
                ssize_t n = write(fd, g_buf + done, (size_t)(g_len - done));
                if (n <= 0) break;
                done += (int)n;
            }
            close(fd);
            g_dirty = done != g_len;
            set_msg(done == g_len ? "Saved" : "Short write");
        }
    }
    g_need = 1;
}

static void run_menu_item(int menu, int item) {
    g_menu = -1;
    g_need = 1;
    if (menu == 0) {
        switch (item) {
        case 0: new_file(); break;
        case 1: start_prompt(1); break;
        case 2: save_file(); break;
        case 3: start_prompt(2); break;
        case 4: exit(0); break;
        default: break;
        }
    } else if (menu == 1) {
        if (item == 0) {
            g_sel_a = 0;
            g_off = g_len;
            g_need = 1;
        } else {
            insert_time_date();
        }
    } else if (menu == 2) {
        if (item == 0) {
            g_wrap = !g_wrap;
            set_msg(g_wrap ? "Word wrap on" : "Word wrap off");
        } else {
            set_msg("Only one font is installed");
        }
    } else if (menu == 3) {
        g_show_status = !g_show_status;
        g_need = 1;
    } else {
        set_msg("MCubeAIOS Notepad 0.1.0");
    }
}

/* ---- input ---------------------------------------------------------- */

static void menu_click(int x, int y) {
    if (g_menu >= 0) {
        int mx = menu_x(g_menu);
        int dw = 176;
        if (mx + dw > gui_width()) mx = gui_width() - dw - 4;
        if (mx < 0) mx = 0;
        int count = g_menu_counts[g_menu];
        if (x >= mx && x < mx + dw && y >= MENU_H && y < MENU_H + 3 + count * ITEM_H) {
            int item = (y - MENU_H - 3) / ITEM_H;
            if (item >= 0 && item < count) {
                run_menu_item(g_menu, item);
                return;
            }
        }
        g_menu = -1;
        g_need = 1;
    }

    if (y < MENU_H) {
        for (int i = 0; i < MENU_COUNT; i++) {
            int bx = menu_x(i);
            int bw = text_w(g_menu_names[i]) + 20;
            if (x >= bx && x < bx + bw) {
                g_menu = (g_menu == i) ? -1 : i;
                g_need = 1;
                return;
            }
        }
        return;
    }
    if (g_menu >= 0) {
        g_menu = -1;
        g_need = 1;
        return;
    }
}

static void text_click(int x, int y) {
    int w = gui_width(), h = gui_height();
    int area_h = h - MENU_H - (g_show_status ? STATUS_H : 0);
    if (x >= w - SB_W) {
        /* Clicking the scrollbar pages through the document. */
        int list_h = area_h;
        int thumb_h = list_h * g_rows / g_vcount;
        if (thumb_h < 24) thumb_h = 24;
        int span = list_h - thumb_h;
        int max_top = g_vcount - g_rows;
        int off = max_top > 0 ? span * g_top / max_top : 0;
        if (y - MENU_H < off) g_top -= g_rows;
        else if (y - MENU_H >= off + thumb_h) g_top += g_rows;
        if (g_top < 0) g_top = 0;
        if (g_top > max_top) g_top = max_top;
        g_need = 1;
        return;
    }

    int v = g_top + (y - MENU_H) / ROW_H;
    if (v < 0) v = 0;
    if (v >= g_vcount) v = g_vcount - 1;
    int c = (x - PAD_X) / 8;
    if (c < 0) c = 0;
    if (c > g_vlen[v]) c = g_vlen[v];
    g_sel_a = -1;
    g_off = g_vpos[v] + c;
    if (g_off > g_len) g_off = g_len;
    g_need = 1;
}

static void key_event(u32 c) {
    if (g_prompt) {
        if (c == '\n') {
            commit_prompt();
        } else if (c == '\b') {
            if (g_prompt_len > 0) g_prompt_buf[--g_prompt_len] = 0;
            g_need = 1;
        } else if (c == 27) {
            g_prompt = 0;
            g_need = 1;
        } else if (c >= 32 && c < 0x100 && g_prompt_len < (int)sizeof(g_prompt_buf) - 1) {
            g_prompt_buf[g_prompt_len++] = (char)c;
            g_prompt_buf[g_prompt_len] = 0;
            g_need = 1;
        }
        return;
    }

    if (g_menu >= 0 && c != 27) {
        g_menu = -1;
        g_need = 1;
    }
    if (c == 27) {
        g_menu = -1;
        g_sel_a = -1;
        g_need = 1;
        return;
    }

    if (c == INPUT_KEY_LEFT) move_caret(g_off - 1);
    else if (c == INPUT_KEY_RIGHT) move_caret(g_off + 1);
    else if (c == INPUT_KEY_UP) move_vertical(-1);
    else if (c == INPUT_KEY_DOWN) move_vertical(1);
    else if (c == INPUT_KEY_HOME) move_home();
    else if (c == INPUT_KEY_END) move_end();
    else if (c == INPUT_KEY_PGUP) { for (int i = 0; i < g_rows; i++) move_vertical(-1); }
    else if (c == INPUT_KEY_PGDN) { for (int i = 0; i < g_rows; i++) move_vertical(1); }
    else if (c == INPUT_KEY_DELETE) delete_forward();
    else if (c == '\b') backspace();
    else if (c == '\n') insert_char('\n');
    else if (c == '\t') { for (int i = 0; i < 4; i++) insert_char(' '); }
    else if (c == INPUT_KEY_F5) insert_time_date();
    else if (c >= 32 && c < 0x100) insert_char((char)c);
}

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_KEY: {
        u32 code = ev->code;
        u32 base = code & ~INPUT_KEY_RELEASE;
        int released = (code & INPUT_KEY_RELEASE) != 0;
        if (base == INPUT_KEY_LSHIFT || base == INPUT_KEY_RSHIFT) {
            g_shift = released ? 0 : 1;
            break;
        }
        if (base == INPUT_KEY_LCTRL || base == INPUT_KEY_RCTRL ||
            base == INPUT_KEY_LALT || base == INPUT_KEY_RALT ||
            base == INPUT_KEY_LWIN || base == INPUT_KEY_RWIN) {
            break;
        }
        if (!released) key_event(code);
        break;
    }

    case GUI_EVT_MOUSE:
        if (!(ev->buttons & 1)) break;
        if (g_prompt) {
            /* Clicks anywhere dismiss the prompt, like a modal dialog. */
            commit_prompt();
            break;
        }
        if (ev->y < MENU_H || g_menu >= 0) {
            menu_click(ev->x, ev->y);
            break;
        }
        if (g_show_status && ev->y >= gui_height() - STATUS_H) break;
        text_click(ev->x, ev->y);
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

    if (gui_init("Notepad", 760, 560) != 0) {
        printf("USER_EDITOR_FAIL: gui_init\n");
        return 1;
    }

    if (argc >= 2 && argv[1] && argv[1][0]) load_file(argv[1]);

    draw();

    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 250);
        if (rc < 0) return 0;
        if (rc > 0) handle_event(&ev);
        if (g_need) draw();
    }
}
