/* Files: a small directory browser.
 *
 * Enter descends into directories or asks the desktop to open the selected
 * file in the editor. Backspace goes to the parent directory.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>

#define MAX_ENTRIES 128
#define ROW_H 18

#define C_BG     0x11161F
#define C_PANEL  0x1A2230
#define C_TEXT   0xDCE4EE
#define C_DIM    0x94A3B8
#define C_SEL    0x2D6CDF
#define C_ACCENT 0x60A5FA

typedef struct {
    char name[64];
    u32 type;
    u64 size;
} entry_t;

static char g_cwd[256] = "/";
static entry_t g_entries[MAX_ENTRIES];
static int g_count;
static int g_sel;
static int g_top;
static int g_need_redraw = 1;

static void join_path(char *out, size_t cap, const char *dir, const char *name) {
    if (strcmp(dir, "/") == 0) snprintf(out, cap, "/%s", name);
    else snprintf(out, cap, "%s/%s", dir, name);
}

static void load_dir(const char *path) {
    g_count = 0;
    g_sel = 0;
    g_top = 0;

    int fd = open(path, VFS_O_RDONLY);
    if (fd < 0) {
        snprintf(g_cwd, sizeof(g_cwd), "%s", path);
        g_need_redraw = 1;
        return;
    }

    mcube_dirent_t ent;
    while (g_count < MAX_ENTRIES && getdents64(fd, &ent, sizeof(ent)) > 0) {
        u32 n = ent.namelen;
        if (n >= sizeof(g_entries[0].name)) n = sizeof(g_entries[0].name) - 1;
        memcpy(g_entries[g_count].name, ent.name, n);
        g_entries[g_count].name[n] = 0;
        g_entries[g_count].type = ent.type;
        g_entries[g_count].size = 0;

        char full[384];
        join_path(full, sizeof(full), path, g_entries[g_count].name);
        mcube_stat_t st;
        if (stat(full, &st) == 0) {
            g_entries[g_count].type = st.type;
            g_entries[g_count].size = st.size;
        }
        g_count++;
    }
    close(fd);

    snprintf(g_cwd, sizeof(g_cwd), "%s", path);
    g_need_redraw = 1;
}

static void parent_dir(void) {
    if (strcmp(g_cwd, "/") == 0) return;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", g_cwd);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) {
        load_dir("/");
    } else {
        *slash = 0;
        load_dir(tmp);
    }
}

static void open_selected(void) {
    if (g_sel < 0 || g_sel >= g_count) return;
    entry_t *e = &g_entries[g_sel];
    char full[384];
    join_path(full, sizeof(full), g_cwd, e->name);

    if (e->type == VFS_TYPE_DIR) {
        load_dir(full);
        return;
    }

    char spec[GUI_TEXT_MAX + 1];
    snprintf(spec, sizeof(spec), "/bin/editor.lgy %s", full);
    gui_launch(spec);
}

static void draw(void) {
    int w = gui_width(), h = gui_height();
    int rows = (h - 56) / ROW_H;
    if (rows < 1) rows = 1;

    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + rows) g_top = g_sel - rows + 1;
    if (g_top < 0) g_top = 0;

    gui_clear(C_BG);
    gui_fill(0, 0, w, 26, C_PANEL);
    gui_text(8, 5, C_ACCENT, "Files");
    gui_text(72, 5, C_DIM, g_cwd);

    for (int r = 0; r < rows; r++) {
        int idx = g_top + r;
        if (idx >= g_count) break;
        int y = 30 + r * ROW_H;
        entry_t *e = &g_entries[idx];

        if (idx == g_sel) gui_fill(4, y - 1, w - 8, ROW_H, C_SEL);

        char label[96];
        if (e->type == VFS_TYPE_DIR) {
            snprintf(label, sizeof(label), "[DIR] %s", e->name);
            gui_text(10, y, idx == g_sel ? 0xFFFFFF : C_ACCENT, label);
        } else {
            snprintf(label, sizeof(label), "%s", e->name);
            gui_text(10, y, idx == g_sel ? 0xFFFFFF : C_TEXT, label);
            char size[32];
            snprintf(size, sizeof(size), "%u", (u32)e->size);
            gui_text(w - 70, y, C_DIM, size);
        }
    }

    gui_fill(0, h - 22, w, 22, C_PANEL);
    gui_text(8, h - 19, C_DIM, "Enter open  Bksp up  F5 refresh  (F1 menu)");
    gui_flush();
    g_need_redraw = 0;
}

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_KEY:
        if (ev->code == INPUT_KEY_UP && g_sel > 0) {
            g_sel--;
            g_need_redraw = 1;
        } else if (ev->code == INPUT_KEY_DOWN && g_sel + 1 < g_count) {
            g_sel++;
            g_need_redraw = 1;
        } else if (ev->code == INPUT_KEY_HOME) {
            g_sel = 0;
            g_need_redraw = 1;
        } else if (ev->code == INPUT_KEY_END) {
            g_sel = g_count > 0 ? g_count - 1 : 0;
            g_need_redraw = 1;
        } else if (ev->code == '\n') {
            open_selected();
        } else if (ev->code == '\b') {
            parent_dir();
        } else if (ev->code == INPUT_KEY_F5) {
            load_dir(g_cwd);
        } else if (ev->code == INPUT_KEY_F2) {
            open_selected();
        }
        break;

    case GUI_EVT_MOUSE: {
        if (!(ev->buttons & 1)) break;
        int row = (ev->y - 30) / ROW_H;
        int idx = g_top + row;
        if (ev->y >= 30 && idx >= 0 && idx < g_count) {
            g_sel = idx;
            g_need_redraw = 1;
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
    (void)argc; (void)argv; (void)envp;

    if (gui_init("Files", 520, 420) != 0) {
        printf("USER_FILES_FAIL: gui_init\n");
        return 1;
    }

    load_dir("/");
    draw();

    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 100);
        if (rc < 0) return 0;
        if (rc > 0) handle_event(&ev);
        if (g_need_redraw) draw();
    }
}
