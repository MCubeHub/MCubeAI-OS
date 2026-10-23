/* Calculator: integer expression evaluator with a clickable keypad. */

#include <mcube/libc.h>
#include <mcube/gui.h>

#define GAP 6

#define C_BG     0x10151E
#define C_PANEL  0x1A2230
#define C_TEXT   0xE5EBF3
#define C_DIM    0x93A1B5
#define C_ACCENT 0x3B82F6
#define C_BTN    0x232D3E
#define C_BTN_HI 0x2D6CDF
#define C_RESULT 0x60A5FA

static char g_expr[128];
static char g_result[48] = "0";
static int g_need_redraw = 1;

typedef struct {
    const char *p;
    int ok;
} parser_t;

static void skip_ws(parser_t *ps) {
    while (*ps->p == ' ') ps->p++;
}

static long parse_expr(parser_t *ps);

static long parse_factor(parser_t *ps) {
    skip_ws(ps);
    if (!ps->ok) return 0;

    if (*ps->p == '-') {
        ps->p++;
        return -parse_factor(ps);
    }
    if (*ps->p == '+') {
        ps->p++;
        return parse_factor(ps);
    }
    if (*ps->p == '(') {
        ps->p++;
        long v = parse_expr(ps);
        skip_ws(ps);
        if (*ps->p != ')') { ps->ok = 0; return 0; }
        ps->p++;
        return v;
    }
    if (*ps->p < '0' || *ps->p > '9') {
        ps->ok = 0;
        return 0;
    }

    long v = 0;
    while (*ps->p >= '0' && *ps->p <= '9') {
        v = v * 10 + (*ps->p - '0');
        ps->p++;
    }
    return v;
}

static long parse_term(parser_t *ps) {
    long v = parse_factor(ps);
    for (;;) {
        skip_ws(ps);
        if (*ps->p != '*' && *ps->p != '/') break;
        char op = *ps->p++;
        long rhs = parse_factor(ps);
        if (!ps->ok) return 0;
        if (op == '*') {
            v *= rhs;
        } else {
            if (rhs == 0) { ps->ok = 0; return 0; }
            v /= rhs;
        }
    }
    return v;
}

static long parse_expr(parser_t *ps) {
    long v = parse_term(ps);
    for (;;) {
        skip_ws(ps);
        if (*ps->p != '+' && *ps->p != '-') break;
        char op = *ps->p++;
        long rhs = parse_term(ps);
        if (!ps->ok) return 0;
        if (op == '+') v += rhs;
        else v -= rhs;
    }
    return v;
}

static void evaluate(void) {
    parser_t ps;
    ps.p = g_expr;
    ps.ok = 1;
    long v = parse_expr(&ps);
    skip_ws(&ps);
    if (!ps.ok || *ps.p != 0) {
        snprintf(g_result, sizeof(g_result), "error");
        return;
    }
    snprintf(g_result, sizeof(g_result), "%d", (int)v);
    snprintf(g_expr, sizeof(g_expr), "%d", (int)v);
    g_need_redraw = 1;
}

static void append(const char *s) {
    usize len = strlen(g_expr);
    usize add = strlen(s);
    if (len + add >= sizeof(g_expr)) return;
    memcpy(g_expr + len, s, add + 1);
    g_need_redraw = 1;
}

static void backspace(void) {
    usize len = strlen(g_expr);
    if (len) g_expr[len - 1] = 0;
    g_need_redraw = 1;
}

static void clear_all(void) {
    g_expr[0] = 0;
    snprintf(g_result, sizeof(g_result), "0");
    g_need_redraw = 1;
}

/* ---- keypad --------------------------------------------------------- */

static const char *const g_labels[5][4] = {
    { "C", "(", ")", "/" },
    { "7", "8", "9", "*" },
    { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },
    { "0", ".", "<", "=" },
};

static void button_rect(int r, int c, int w, int h, int *bx, int *by,
                        int *bw, int *bh) {
    int grid_w = w - 20;
    int grid_h = h - 120;
    *bw = (grid_w - 3 * GAP) / 4;
    *bh = (grid_h - 4 * GAP) / 5;
    if (*bw < 20) *bw = 20;
    if (*bh < 16) *bh = 16;
    *bx = 10 + c * (*bw + GAP);
    *by = 100 + r * (*bh + GAP);
}

static void press(const char *label) {
    if (strcmp(label, "C") == 0) {
        clear_all();
    } else if (strcmp(label, "<") == 0) {
        backspace();
    } else if (strcmp(label, "=") == 0) {
        evaluate();
    } else if (strcmp(label, ".") == 0) {
        /* Integers only; keep the key for visual completeness. */
    } else {
        append(label);
    }
}

static void draw(void) {
    int w = gui_width(), h = gui_height();

    gui_clear(C_BG);
    gui_fill(0, 0, w, 26, C_PANEL);
    gui_text(8, 5, C_ACCENT, "Calculator");

    gui_fill(10, 34, w - 20, 56, C_PANEL);
    gui_text(18, 42, C_TEXT, g_expr[0] ? g_expr : "0");
    gui_text(18, 64, C_RESULT, g_result);

    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 4; c++) {
            int bx, by, bw, bh;
            button_rect(r, c, w, h, &bx, &by, &bw, &bh);
            const char *label = g_labels[r][c];
            u32 bg = C_BTN;
            if (strcmp(label, "=") == 0) bg = C_ACCENT;
            else if (strcmp(label, "C") == 0) bg = 0x7F1D1D;
            gui_fill(bx, by, bw, bh, bg);
            gui_rect(bx, by, bw, bh, 0x3A475C);
            int tx = bx + (bw - 8) / 2;
            int ty = by + (bh - 16) / 2;
            gui_text(tx, ty, C_TEXT, label);
        }
    }
    gui_flush();
    g_need_redraw = 0;
}

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_KEY: {
        u32 c = ev->code;
        if (c >= '0' && c <= '9') {
            char s[2] = { (char)c, 0 };
            append(s);
        } else if (c == '+' || c == '-' || c == '*' || c == '/' ||
                   c == '(' || c == ')') {
            char s[2] = { (char)c, 0 };
            append(s);
        } else if (c == '\n') {
            evaluate();
        } else if (c == '\b') {
            backspace();
        } else if (c == 27) {
            clear_all();
        }
        break;
    }

    case GUI_EVT_MOUSE: {
        int w = gui_width(), h = gui_height();
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 4; c++) {
                int bx, by, bw, bh;
                button_rect(r, c, w, h, &bx, &by, &bw, &bh);
                if (ev->x >= bx && ev->x < bx + bw &&
                    ev->y >= by && ev->y < by + bh) {
                    press(g_labels[r][c]);
                    return;
                }
            }
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

    if (gui_init("Calculator", 320, 400) != 0) {
        printf("USER_CALC_FAIL: gui_init\n");
        return 1;
    }

    draw();
    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 200);
        if (rc < 0) return 0;
        if (rc > 0) handle_event(&ev);
        if (g_need_redraw) draw();
    }
}
