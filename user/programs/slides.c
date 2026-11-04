/* Slides: a small presentation editor, the MCubeAIOS take on PowerPoint.
 *
 * Window layout: a toolbar with slide actions, a thumbnail strip on the left,
 * a 16:9 canvas in the middle and a status bar. Clicking a thumbnail selects
 * the slide; clicking the title or body on the canvas puts the caret there so
 * the keyboard types into that placeholder. F5 (or the Play button) hides the
 * chrome and runs the deck, where a click on the right half advances and a
 * click on the left half goes back.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>

#define MAX_SLIDES 32
#define TITLE_MAX  64
#define BODY_MAX   600
#define TOOLBAR_H  36
#define STRIP_W    176
#define STATUS_H   22
#define THUMB_W    148
#define THUMB_H    83
#define THUMB_GAP  10
#define FILE_PATH  "/SLIDES.TXT"

#define C_CHROME   0x22262F
#define C_CHROME_L 0x2C323D
#define C_CHROME_D 0x1A1E26
#define C_LINE     0x3A414D
#define C_TEXT     0xE6EDF7
#define C_DIM      0x98A3B3
#define C_ACCENT   0x4CC2FF
#define C_CANVAS   0x14171D

typedef struct {
    char title[TITLE_MAX];
    char body[BODY_MAX];
} slide_t;

typedef struct {
    u32 bg;
    u32 accent;
    u32 text;
    u32 sub;
} theme_t;

static const theme_t g_themes[] = {
    { 0x0B1B33, 0x4CC2FF, 0xEAF3FF, 0x8FB2D6 },
    { 0xF6F7FB, 0x1F6FEB, 0x101828, 0x5A6478 },
    { 0x06251C, 0x4ADE80, 0xE9FFF5, 0x7FBFA0 },
    { 0x1A1A1D, 0xFF6B4A, 0xF5F5F7, 0x9A9AA2 },
};
#define THEME_COUNT ((int)(sizeof(g_themes) / sizeof(g_themes[0])))

static slide_t g_slides[MAX_SLIDES];
static int g_count;
static int g_cur;
static int g_theme;
static int g_focus;          /* 0 none, 1 title, 2 body */
static int g_caret;          /* caret offset while editing a field  */
static int g_playing;
static int g_need = 1;
static char g_msg[96];

/* ---- helpers -------------------------------------------------------- */

static int text_w(const char *s) { return (int)strlen(s) * 8; }
static int min_i(int a, int b) { return a < b ? a : b; }

static void say(const char *s) {
    snprintf(g_msg, sizeof(g_msg), "%s", s ? s : "");
    g_need = 1;
}

static const theme_t *theme(void) { return &g_themes[g_theme % THEME_COUNT]; }

/* ---- persistence ---------------------------------------------------- */

static void set_default_deck(void) {
    g_count = 0;
    snprintf(g_slides[0].title, TITLE_MAX, "MCubeAIOS");
    snprintf(g_slides[0].body, BODY_MAX,
             "An AI-native operating system\n"
             "built from scratch for x86_64\n"
             "\n"
             "click the text to edit it");
    snprintf(g_slides[1].title, TITLE_MAX, "What is inside");
    snprintf(g_slides[1].body, BODY_MAX,
             "- self-written boot chain: BIOS and UEFI\n"
             "- compositor, windows and taskbar\n"
             "- network stack, USB HID, local AI daemon");
    snprintf(g_slides[2].title, TITLE_MAX, "Thank you");
    snprintf(g_slides[2].body, BODY_MAX, "github.com/MCubeHub/MCubeAI-OS");
    g_count = 3;
    g_cur = 0;
    g_theme = 0;
}

static void load_deck(void) {
    int fd = open(FILE_PATH, VFS_O_RDONLY);
    if (fd < 0) {
        set_default_deck();
        say("New presentation");
        return;
    }
    static char buf[8192];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        set_default_deck();
        return;
    }
    buf[n] = 0;

    g_count = 0;
    g_cur = 0;
    g_theme = 0;

    int in_body = 0;
    slide_t *s = 0;
    char *p = buf;
    while (*p && g_count < MAX_SLIDES) {
        char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        char line[256];
        if (len > 255) len = 255;
        memcpy(line, p, (size_t)len);
        line[len] = 0;
        p += nl ? len + 1 : len;

        if (strncmp(line, "theme ", 6) == 0) {
            g_theme = atoi(line + 6) % THEME_COUNT;
        } else if (strncmp(line, "slide ", 6) == 0) {
            s = &g_slides[g_count++];
            snprintf(s->title, TITLE_MAX, "%s", line + 6);
            s->body[0] = 0;
            in_body = 0;
        } else if (strcmp(line, "body") == 0) {
            in_body = 1;
        } else if (strcmp(line, "end") == 0) {
            in_body = 0;
        } else if (in_body && s) {
            int bl = (int)strlen(s->body);
            if (bl + len + 2 < BODY_MAX) {
                if (bl && s->body[bl - 1] != '\n') s->body[bl++] = '\n';
                memcpy(s->body + bl, line, (size_t)len);
                s->body[bl + len] = 0;
            }
        }
        if (!nl) break;
    }
    if (g_count == 0) set_default_deck();
    say("Opened /SLIDES.TXT");
}

static void save_deck(void) {
    int fd = open(FILE_PATH, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        say("Save failed");
        return;
    }
    static char buf[8192];
    int n = snprintf(buf, sizeof(buf), "MCSLIDES 1\ntheme %d\n", g_theme);
    for (int i = 0; i < g_count && n < (int)sizeof(buf) - 300; i++) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "slide %s\nbody\n%s\nend\n",
                      g_slides[i].title, g_slides[i].body);
    }
    ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    say(w == n ? "Saved to /SLIDES.TXT" : "Short write");
}

/* ---- slide actions -------------------------------------------------- */

static void add_slide(void) {
    if (g_count >= MAX_SLIDES) {
        say("Slides are full");
        return;
    }
    memmove(&g_slides[g_cur + 2], &g_slides[g_cur + 1],
            sizeof(slide_t) * (size_t)(g_count - g_cur - 1));
    g_count++;
    g_cur++;
    memset(&g_slides[g_cur], 0, sizeof(slide_t));
    snprintf(g_slides[g_cur].title, TITLE_MAX, "New slide");
    g_focus = 1;
    g_caret = (int)strlen(g_slides[g_cur].title);
    g_need = 1;
}

static void delete_slide(void) {
    if (g_count <= 1) {
        memset(&g_slides[0], 0, sizeof(slide_t));
        g_focus = 0;
        say("Cleared the last slide");
        g_need = 1;
        return;
    }
    memmove(&g_slides[g_cur], &g_slides[g_cur + 1],
            sizeof(slide_t) * (size_t)(g_count - g_cur - 1));
    g_count--;
    if (g_cur >= g_count) g_cur = g_count - 1;
    g_focus = 0;
    say("Slide deleted");
}

/* ---- layout --------------------------------------------------------- */

static void canvas_rect(int w, int h, int *cx, int *cy, int *cw, int *ch) {
    int x = STRIP_W + 12;
    int y = TOOLBAR_H + 12;
    int avail_w = w - x - 12;
    int avail_h = h - y - STATUS_H - 12;
    if (avail_w < 40) avail_w = 40;
    if (avail_h < 24) avail_h = 24;
    int cw_ = avail_w;
    int ch_ = cw_ * 9 / 16;
    if (ch_ > avail_h) {
        ch_ = avail_h;
        cw_ = ch_ * 16 / 9;
    }
    *cx = x;
    *cy = y + (avail_h - ch_) / 2;
    *cw = cw_;
    *ch = ch_;
}

static int body_first_y(int cy, int ch) { return cy + ch / 4; }
static int body_line_h(int ch) { return ch >= 400 ? 22 : 18; }

/* Draw one wrapped paragraph block; returns the y after the last line. */
static int draw_block(int x, int y, int maxw, u32 color, const char *text, int lh) {
    int cols = maxw / 8;
    if (cols < 4) cols = 4;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        while (len > 0) {
            int take = len > cols ? cols : len;
            if (take < len) {
                int brk = take;
                while (brk > 0 && p[brk - 1] != ' ') brk--;
                if (brk > cols / 3) take = brk;
            }
            gui_textn(x, y, color, p, take);
            y += lh;
            p += take;
            len -= take;
            if (len == 1 && *p == '\n') break;
        }
        if (!nl) break;
        p = nl + 1;
        if (*p == 0) break;
    }
    return y;
}

/* ---- drawing -------------------------------------------------------- */

static void draw_toolbar(int w) {
    gui_fill(0, 0, w, TOOLBAR_H, C_CHROME);
    gui_fill(0, TOOLBAR_H - 1, w, 1, C_LINE);

    int x = 8;
    struct { const char *label; int w; } btn[] = {
        { "New", 50 }, { "Delete", 60 }, { "Play", 50 }, { "Save", 50 }, { "Theme", 56 }
    };
    for (int i = 0; i < 5; i++) {
        gui_fill(x, 5, btn[i].w, 26, C_CHROME_L);
        gui_rect(x, 5, btn[i].w, 26, C_LINE);
        gui_text(x + (btn[i].w - text_w(btn[i].label)) / 2, 10, C_TEXT, btn[i].label);
        x += btn[i].w + 6;
    }

    char info[96];
    snprintf(info, sizeof(info), "Theme %d  ·  F5 to present", g_theme + 1);
    int ix = w - 8 - text_w(info);
    if (ix > x + 12) gui_text(ix, 10, C_DIM, info);
}

static void draw_strip(int h) {
    int y0 = TOOLBAR_H;
    int strip_h = h - y0 - STATUS_H;
    gui_fill(0, y0, STRIP_W, strip_h, C_CHROME_D);
    gui_fill(STRIP_W, y0, 1, strip_h, C_LINE);
    gui_text(10, y0 + 8, C_DIM, "Slides");

    int per_page = (strip_h - 34) / (THUMB_H + THUMB_GAP);
    if (per_page < 1) per_page = 1;
    int first = 0;
    if (g_cur >= per_page) first = g_cur - per_page + 1;

    for (int i = first; i < g_count && i < first + per_page; i++) {
        int y = y0 + 30 + (i - first) * (THUMB_H + THUMB_GAP);
        int sel = i == g_cur;
        gui_fill(8, y, THUMB_W, THUMB_H, g_themes[g_theme % THEME_COUNT].bg);
        if (sel) {
            gui_rect(6, y - 2, THUMB_W + 4, THUMB_H + 4, C_ACCENT);
            gui_rect(7, y - 1, THUMB_W + 2, THUMB_H + 2, C_ACCENT);
        } else {
            gui_rect(8, y, THUMB_W, THUMB_H, C_LINE);
        }
        int num = i + 1;
        gui_fill(8, y, 22, 16, sel ? C_ACCENT : C_CHROME_L);
        char ns[8];
        snprintf(ns, sizeof(ns), "%d", num);
        gui_text(13, y + 1, sel ? 0x081018 : C_TEXT, ns);

        char t[TITLE_MAX];
        snprintf(t, sizeof(t), "%s", g_slides[i].title);
        int cap = (THUMB_W - 18) / 8;
        if (cap > 0 && (int)strlen(t) > cap) t[cap] = 0;
        gui_text(14, y + 20, g_themes[g_theme % THEME_COUNT].text, t);

        int lines = 0;
        for (const char *p = g_slides[i].body; *p && lines < 3; p++) {
            if (*p == '\n') lines++;
        }
        gui_fill(14, y + 40, (THUMB_W - 28) * min_i(min_i(lines + 1, 4), 4) / 4,
                 THUMB_H - 48, g_themes[g_theme % THEME_COUNT].sub);
    }

    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d / %d", g_cur + 1, g_count);
    gui_text(10, h - STATUS_H - 20, C_DIM, cnt);
}

static void draw_canvas(int w, int h) {
    int cx, cy, cw, ch;
    canvas_rect(w, h, &cx, &cy, &cw, &ch);
    const theme_t *t = theme();

    gui_fill(cx - 2, cy - 2, cw + 4, ch + 4, 0x30363F);
    gui_fill(cx, cy, cw, ch, t->bg);
    gui_fill(cx, cy, cw, 5, t->accent);

    slide_t *s = &g_slides[g_cur];
    int tx = cx + 24;
    int tw = cw - 48;

    if (s->title[0]) {
        gui_textn(tx, cy + 24, t->text, s->title, min_i((int)strlen(s->title), tw / 8));
        /* A one-pixel drop shadow stands in for bold: there is one weight. */
        gui_textn(tx + 1, cy + 25, t->accent, s->title, min_i((int)strlen(s->title), tw / 8));
    } else if (g_focus != 1) {
        gui_text(tx, cy + 24, t->sub, "Click to add title");
    }

    if (s->body[0]) {
        draw_block(tx, body_first_y(cy, ch), tw, t->text, s->body, body_line_h(ch));
    } else if (g_focus != 2) {
        gui_text(tx, body_first_y(cy, ch), t->sub, "Click to add text");
    }

    /* Caret in the field being edited. */
    if (g_focus == 1) {
        int cols_ok = tw / 8;
        int len = g_caret > (int)strlen(s->title) ? (int)strlen(s->title) : g_caret;
        if (len > cols_ok) len = cols_ok;
        gui_fill(tx + len * 8, cy + 24, 2, 16, t->accent);
    } else if (g_focus == 2) {
        int last_y = body_first_y(cy, ch);
        int lh = body_line_h(ch);
        int cols = tw / 8;
        int len = g_caret > (int)strlen(s->body) ? (int)strlen(s->body) : g_caret;
        /* Walk the wrapped body to place the caret after the last character. */
        const char *p = s->body;
        while (*p && len > 0) {
            int take = len > cols ? cols : len;
            if (take < len) {
                int brk = take;
                while (brk > 0 && p[brk - 1] != ' ') brk--;
                if (brk > cols / 3) take = brk;
            }
            if (*p == '\n') {
                p++;
                len--;
                last_y += lh;
                continue;
            }
            last_y += lh;
            p += take;
            len -= take;
            if (last_y > cy + ch - lh) break;
        }
        gui_fill(tx, last_y, 2, 16, t->accent);
    }

    char pos[48];
    snprintf(pos, sizeof(pos), "Slide %d of %d", g_cur + 1, g_count);
    gui_text(cx, cy + ch - 18, t->sub, pos);
}

static void draw_status(int w, int h) {
    int y = h - STATUS_H;
    gui_fill(0, y, w, STATUS_H, C_CHROME);
    gui_fill(0, y, w, 1, C_LINE);
    const char *hint = "Click text to type  ·  Tab switches  ·  F5 present  ·  Ctrl+S save";
    gui_text(8, y + 3, C_DIM, hint);
    if (g_msg[0]) {
        int mx = w - 8 - text_w(g_msg);
        if (mx > 8 + text_w(hint) + 12) gui_text(mx, y + 3, C_ACCENT, g_msg);
    }
}

static void draw_present(int w, int h) {
    const theme_t *t = theme();
    slide_t *s = &g_slides[g_cur];
    gui_fill(0, 0, w, h, t->bg);

    int tx = w / 10;
    int tw = w - tx * 2;
    if (s->title[0]) {
        gui_textn(tx, h / 6, t->text, s->title, min_i((int)strlen(s->title), tw / 8));
        gui_textn(tx + 1, h / 6 + 1, t->accent, s->title, min_i((int)strlen(s->title), tw / 8));
        gui_fill(tx, h / 6 + 22, 64, 2, t->accent);
    }
    if (s->body[0]) draw_block(tx, h / 2 - 60, tw, t->text, s->body, 20);

    char pos[32];
    snprintf(pos, sizeof(pos), "%d / %d", g_cur + 1, g_count);
    gui_text(w - 8 - text_w(pos), h - 24, t->sub, pos);
    gui_text(12, h - 24, t->sub, "Esc exits");
}

static void draw(void) {
    int w = gui_width(), h = gui_height();
    gui_clear(C_CANVAS);
    if (g_playing) {
        draw_present(w, h);
    } else {
        draw_toolbar(w);
        draw_strip(h);
        draw_canvas(w, h);
        draw_status(w, h);
    }
    gui_flush();
    g_need = 0;
}

/* ---- editing -------------------------------------------------------- */

static char *field_buf(void) {
    slide_t *s = &g_slides[g_cur];
    if (g_focus == 1) return s->title;
    if (g_focus == 2) return s->body;
    return 0;
}

static int field_max(void) { return g_focus == 1 ? TITLE_MAX : BODY_MAX; }

static void insert_char(char c) {
    char *buf = field_buf();
    if (!buf) return;
    int len = (int)strlen(buf);
    if (len + 1 >= field_max()) {
        say("Field is full");
        return;
    }
    if (c == '\n' && g_focus == 1) return;
    if (g_caret > len) g_caret = len;
    memmove(buf + g_caret + 1, buf + g_caret, (size_t)(len - g_caret));
    buf[g_caret++] = c;
    buf[len + 1] = 0;
    g_need = 1;
}

static void backspace(void) {
    char *buf = field_buf();
    if (!buf || g_caret <= 0) return;
    int len = (int)strlen(buf);
    if (g_caret > len) g_caret = len;
    memmove(buf + g_caret - 1, buf + g_caret, (size_t)(len - g_caret));
    g_caret--;
    buf[len - 1] = 0;
    g_need = 1;
}

static void delete_forward(void) {
    char *buf = field_buf();
    if (!buf) return;
    int len = (int)strlen(buf);
    if (g_caret >= len) return;
    memmove(buf + g_caret, buf + g_caret + 1, (size_t)(len - g_caret - 1));
    buf[len - 1] = 0;
    g_need = 1;
}

static void goto_slide(int idx) {
    if (idx < 0 || idx >= g_count) return;
    g_cur = idx;
    g_focus = 0;
    g_need = 1;
}

/* ---- input ---------------------------------------------------------- */

static void canvas_click(int x, int y) {
    int w = gui_width(), h = gui_height();
    int cx, cy, cw, ch;
    canvas_rect(w, h, &cx, &cy, &cw, &ch);
    if (x < cx || x >= cx + cw || y < cy || y >= cy + ch) {
        g_focus = 0;
        g_need = 1;
        return;
    }
    if (y < body_first_y(cy, ch)) {
        g_focus = 1;
        g_caret = (int)strlen(g_slides[g_cur].title);
    } else {
        g_focus = 2;
        g_caret = (int)strlen(g_slides[g_cur].body);
    }
    g_need = 1;
}

static void strip_click(int y) {
    int h = gui_height();
    int strip_h = h - TOOLBAR_H - STATUS_H;
    int per_page = (strip_h - 34) / (THUMB_H + THUMB_GAP);
    if (per_page < 1) per_page = 1;
    int first = 0;
    if (g_cur >= per_page) first = g_cur - per_page + 1;
    int rel = (y - TOOLBAR_H - 30) / (THUMB_H + THUMB_GAP);
    if (y < TOOLBAR_H + 30) return;
    int idx = first + rel;
    goto_slide(idx);
}

static void toolbar_click(int x) {
    int bx = 8;
    if (x < bx || x > bx + 360) return;
    if (x < 58) { add_slide(); return; }
    if (x < 124) { delete_slide(); return; }
    if (x < 180) { g_playing = 1; g_focus = 0; g_need = 1; return; }
    if (x < 236) { save_deck(); return; }
    g_theme = (g_theme + 1) % THEME_COUNT;
    say("Theme changed");
}

static void key_event(u32 c) {
    if (g_playing) {
        if (c == 27) {
            g_playing = 0;
            g_need = 1;
        } else if (c == INPUT_KEY_RIGHT || c == INPUT_KEY_DOWN ||
                   c == INPUT_KEY_PGDN || c == ' ') {
            if (g_cur + 1 < g_count) goto_slide(g_cur + 1);
        } else if (c == INPUT_KEY_LEFT || c == INPUT_KEY_UP || c == INPUT_KEY_PGUP) {
            if (g_cur > 0) goto_slide(g_cur - 1);
        } else if (c == INPUT_KEY_F5) {
            g_playing = 0;
            g_need = 1;
        }
        return;
    }

    if (c == INPUT_KEY_F5) {
        g_playing = 1;
        g_focus = 0;
        g_need = 1;
        return;
    }
    if (c == 19) {  /* Ctrl+S */
        save_deck();
        return;
    }
    if (c == INPUT_KEY_PGDN) { goto_slide(g_cur + 1); return; }
    if (c == INPUT_KEY_PGUP) { goto_slide(g_cur - 1); return; }
    if (c == '\t') {
        g_focus = g_focus == 1 ? 2 : 1;
        char *buf = field_buf();
        g_caret = buf ? (int)strlen(buf) : 0;
        g_need = 1;
        return;
    }
    if (c == INPUT_KEY_F2) { save_deck(); return; }

    if (!g_focus) return;
    if (c == INPUT_KEY_LEFT) {
        if (g_caret > 0) g_caret--;
        g_need = 1;
    } else if (c == INPUT_KEY_RIGHT) {
        char *buf = field_buf();
        if (buf && g_caret < (int)strlen(buf)) g_caret++;
        g_need = 1;
    } else if (c == INPUT_KEY_HOME) {
        g_caret = 0;
        g_need = 1;
    } else if (c == INPUT_KEY_END) {
        char *buf = field_buf();
        g_caret = buf ? (int)strlen(buf) : 0;
        g_need = 1;
    } else if (c == INPUT_KEY_DELETE) {
        delete_forward();
    } else if (c == '\b') {
        backspace();
    } else if (c == '\n') {
        insert_char('\n');
    } else if (c == 27) {
        g_focus = 0;
        g_need = 1;
    } else if (c >= 32 && c < 0x100) {
        insert_char((char)c);
    }
}

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_KEY:
        if (!(ev->code & INPUT_KEY_RELEASE)) key_event(ev->code);
        break;

    case GUI_EVT_MOUSE:
        if (!(ev->buttons & 1)) break;
        if (g_playing) {
            if (ev->x > gui_width() / 2) {
                if (g_cur + 1 < g_count) goto_slide(g_cur + 1);
            } else if (g_cur > 0) {
                goto_slide(g_cur - 1);
            }
            break;
        }
        if (ev->y < TOOLBAR_H) {
            toolbar_click(ev->x);
        } else if (ev->x < STRIP_W) {
            strip_click(ev->y);
        } else if (ev->y < gui_height() - STATUS_H) {
            canvas_click(ev->x, ev->y);
        }
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
    (void)argc; (void)argv; (void)envp;

    if (gui_init("Slides", 980, 640) != 0) {
        printf("USER_SLIDES_FAIL: gui_init\n");
        return 1;
    }

    load_deck();
    draw();

    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 250);
        if (rc < 0) return 0;
        if (rc > 0) handle_event(&ev);
        if (g_need) draw();
    }
}
