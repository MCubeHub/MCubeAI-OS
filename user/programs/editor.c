/* Editor: a small text editor with F2 save.
 *
 * The buffer is fixed at 16 KiB, which is plenty for the demo files and keeps
 * the implementation focused on cursor movement, scrolling and persistence.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>

#define EDIT_MAX 16384
#define ROW_H    16

#define C_BG     0x0F141C
#define C_PANEL  0x1A2230
#define C_TEXT   0xE2E8F0
#define C_DIM    0x93A1B5
#define C_ACCENT 0x60A5FA
#define C_CUR    0x2D6CDF

static char g_name[128] = "(scratch)";
static char g_buf[EDIT_MAX + 1];
static int g_len;
static int g_off;
static int g_top;
static int g_dirty;
static int g_need_redraw = 1;
static char g_status[96];

static void set_status(const char *s) {
    snprintf(g_status, sizeof(g_status), "%s", s ? s : "");
    g_need_redraw = 1;
}

static void load_file(const char *path) {
    snprintf(g_name, sizeof(g_name), "%s", path);
    g_len = 0;
    g_off = 0;
    g_top = 0;

    int fd = open(path, VFS_O_RDONLY);
    if (fd < 0) {
        const char *welcome =
            "MCubeAIOS editor\n"
            "\n"
            "No file was loaded. Type here and press F2 to save.\n"
            "Arrow keys move, Home/End jump, Delete removes.\n";
        g_len = (int)strlen(welcome);
        if (g_len > EDIT_MAX) g_len = EDIT_MAX;
        memcpy(g_buf, welcome, (size_t)g_len);
        set_status("new buffer");
        return;
    }

    ssize_t n = read(fd, g_buf, EDIT_MAX);
    close(fd);
    if (n < 0) n = 0;
    g_len = (int)n;
    set_status("loaded");
}

static void save_file(void) {
    if (strcmp(g_name, "(scratch)") == 0) {
        set_status("no filename; launch with a path");
        return;
    }
    int fd = open(g_name, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        set_status("save failed");
        return;
    }
    int done = 0;
    while (done < g_len) {
        ssize_t n = write(fd, g_buf + done, (size_t)(g_len - done));
        if (n <= 0) break;
        done += (int)n;
    }
    close(fd);
    g_dirty = 0;
    set_status(done == g_len ? "saved" : "short write");
}

static int line_start(int off) {
    while (off > 0 && g_buf[off - 1] != '\n') off--;
    return off;
}

static int line_end(int off) {
    while (off < g_len && g_buf[off] != '\n') off++;
    return off;
}

static int line_number(int off) {
    int line = 0;
    for (int i = 0; i < off && i < g_len; i++) if (g_buf[i] == '\n') line++;
    return line;
}

static void cursor_lc(int *line, int *col) {
    int ls = line_start(g_off);
    *line = line_number(g_off);
    *col = g_off - ls;
}

static void move_vertical(int dir) {
    int ls = line_start(g_off);
    int col = g_off - ls;
    if (dir < 0) {
        if (ls == 0) return;
        int prev_end = ls - 1;
        int prev_start = line_start(prev_end);
        int prev_len = prev_end - prev_start;
        g_off = prev_start + (col < prev_len ? col : prev_len);
    } else {
        int le = line_end(g_off);
        if (le >= g_len) return;
        int next_start = le + 1;
        int next_end = line_end(next_start);
        int next_len = next_end - next_start;
        g_off = next_start + (col < next_len ? col : next_len);
    }
    g_need_redraw = 1;
}

static void insert_char(char c) {
    if (g_len >= EDIT_MAX) {
        set_status("buffer full");
        return;
    }
    memmove(g_buf + g_off + 1, g_buf + g_off, (size_t)(g_len - g_off));
    g_buf[g_off++] = c;
    g_len++;
    g_dirty = 1;
    g_need_redraw = 1;
}

static void delete_before(void) {
    if (g_off <= 0) return;
    memmove(g_buf + g_off - 1, g_buf + g_off, (size_t)(g_len - g_off));
    g_off--;
    g_len--;
    g_dirty = 1;
    g_need_redraw = 1;
}

static void delete_at(void) {
    if (g_off >= g_len) return;
    memmove(g_buf + g_off, g_buf + g_off + 1, (size_t)(g_len - g_off - 1));
    g_len--;
    g_dirty = 1;
    g_need_redraw = 1;
}

static void draw(void) {
    int w = gui_width(), h = gui_height();
    int rows = (h - 56) / ROW_H;
    int cols = (w - 12) / 8;
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;

    int cline, ccol;
    cursor_lc(&cline, &ccol);
    if (cline < g_top) g_top = cline;
    if (cline >= g_top + rows) g_top = cline - rows + 1;
    if (g_top < 0) g_top = 0;

    gui_clear(C_BG);
    gui_fill(0, 0, w, 26, C_PANEL);
    gui_text(8, 5, C_ACCENT, "Editor");
    gui_text(76, 5, C_TEXT, g_name);
    if (g_dirty) gui_text(w - 84, 5, 0xF59E0B, "[modified]");

    int pos = 0;
    int line = 0;
    while (pos < g_len && line < g_top) {
        if (g_buf[pos] == '\n') line++;
        pos++;
    }

    for (int r = 0; r < rows; r++) {
        if (pos > g_len) break;
        int end = pos;
        while (end < g_len && g_buf[end] != '\n') end++;
        int len = end - pos;
        if (len > cols) len = cols;
        int y = 30 + r * ROW_H;
        gui_textn(6, y, C_TEXT, g_buf + pos, len);

        if (line == cline) {
            int cx = 6 + ccol * 8;
            if (ccol <= cols) gui_fill(cx, y + 14, 8, 2, C_CUR);
        }

        pos = end;
        if (pos < g_len && g_buf[pos] == '\n') pos++;
        else if (pos >= g_len) break;
        line++;
    }

    gui_fill(0, h - 22, w, 22, C_PANEL);
    char info[128];
    snprintf(info, sizeof(info), "F2 save  Arrows move  Home/End  %d bytes  %s",
             g_len, g_status);
    gui_text(8, h - 19, C_DIM, info);
    gui_flush();
    g_need_redraw = 0;
}

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_KEY: {
        u32 c = ev->code;
        if (c == INPUT_KEY_LEFT) {
            if (g_off > 0) g_off--;
            g_need_redraw = 1;
        } else if (c == INPUT_KEY_RIGHT) {
            if (g_off < g_len) g_off++;
            g_need_redraw = 1;
        } else if (c == INPUT_KEY_UP) {
            move_vertical(-1);
        } else if (c == INPUT_KEY_DOWN) {
            move_vertical(1);
        } else if (c == INPUT_KEY_HOME) {
            g_off = line_start(g_off);
            g_need_redraw = 1;
        } else if (c == INPUT_KEY_END) {
            g_off = line_end(g_off);
            g_need_redraw = 1;
        } else if (c == INPUT_KEY_PGUP) {
            for (int i = 0; i < 12; i++) move_vertical(-1);
        } else if (c == INPUT_KEY_PGDN) {
            for (int i = 0; i < 12; i++) move_vertical(1);
        } else if (c == INPUT_KEY_DELETE) {
            delete_at();
        } else if (c == '\b') {
            delete_before();
        } else if (c == '\n') {
            insert_char('\n');
        } else if (c == '\t') {
            for (int i = 0; i < 4; i++) insert_char(' ');
        } else if (c == INPUT_KEY_F2) {
            save_file();
        } else if (c >= 32 && c < 0x100) {
            insert_char((char)c);
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

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (gui_init("Editor", 600, 440) != 0) {
        printf("USER_EDITOR_FAIL: gui_init\n");
        return 1;
    }

    if (argc >= 2 && argv[1] && argv[1][0]) load_file(argv[1]);
    else load_file("/TEST.TXT");

    draw();

    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 100);
        if (rc < 0) return 0;
        if (rc > 0) handle_event(&ev);
        if (g_need_redraw) draw();
    }
}
