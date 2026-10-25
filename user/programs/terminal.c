/* Terminal: a GUI window running /bin/sh over two pipes.
 *
 * The window is a simple character grid. Shell output is parsed into the grid
 * and keyboard events are forwarded to the shell's stdin. Scrollback is kept
 * in memory; the visible area follows the bottom of the buffer.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>

#define TERM_ROWS 64
#define TERM_COLS 128

#define C_BG   0x0B0F14
#define C_FG   0xD7E0EA
#define C_CUR  0x3B82F6

static char g_text[TERM_ROWS][TERM_COLS];
static int g_row, g_col;
static int g_cols = 80;           /* wrap width, tracks the window size */
static int g_scroll;              /* first visible row */
static int g_need_redraw = 1;

static int g_in_fd = -1;          /* write to shell stdin */
static int g_out_fd = -1;         /* read shell stdout */
static pid_t g_shell;
static int g_shell_dead;
static int g_esc;

static void term_clear(void) {
    memset(g_text, ' ', sizeof(g_text));
    g_row = g_col = 0;
    g_scroll = 0;
}

static void term_scroll(void) {
    memmove(g_text[0], g_text[1], (TERM_ROWS - 1) * TERM_COLS);
    memset(g_text[TERM_ROWS - 1], ' ', TERM_COLS);
    if (g_row > 0) g_row--;
    if (g_scroll > 0) g_scroll--;
}

static void term_putc(char c) {
    if (g_esc) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) g_esc = 0;
        return;
    }
    if (c == 27) { g_esc = 1; return; }

    switch (c) {
    case '\r':
        g_col = 0;
        break;
    case '\n':
        g_col = 0;
        g_row++;
        break;
    case '\b':
        if (g_col > 0) g_col--;
        break;
    case '\t': {
        int next = (g_col + 8) & ~7;
        while (g_col < next && g_col < g_cols) g_text[g_row][g_col++] = ' ';
        break;
    }
    default:
        if ((u8)c < 32) break;
        if (g_col >= g_cols) {
            g_col = 0;
            g_row++;
        }
        g_text[g_row][g_col++] = c;
        break;
    }

    while (g_row >= TERM_ROWS) term_scroll();
    g_need_redraw = 1;
}

/* Re-wrap the grid when the window width changes. Each grid row becomes one
 * logical line, which is the best we can do after the fact but keeps long
 * shell lines readable in narrow tiles. */
static void reflow(int new_cols) {
    static char flat[TERM_ROWS * TERM_COLS + TERM_ROWS + 1];
    int n = 0;

    /* Stop at the cursor row or the last non-blank row, whichever is lower:
     * emitting the empty tail would scroll the prompt off the top. */
    int last_row = g_row;
    for (int r = TERM_ROWS - 1; r > last_row; r--) {
        int c = TERM_COLS - 1;
        while (c >= 0 && g_text[r][c] == ' ') c--;
        if (c >= 0) { last_row = r; break; }
    }
    if (last_row >= TERM_ROWS) last_row = TERM_ROWS - 1;

    for (int r = 0; r <= last_row; r++) {
        int last = TERM_COLS - 1;
        while (last >= 0 && g_text[r][last] == ' ') last--;
        if (r == g_row && g_col - 1 > last) last = g_col - 1;
        for (int c = 0; c <= last && c < TERM_COLS; c++) {
            char ch = g_text[r][c];
            flat[n++] = ch ? ch : ' ';
        }
        if (r < last_row) flat[n++] = '\n';
    }

    memset(g_text, ' ', sizeof(g_text));
    g_row = g_col = 0;
    g_scroll = 0;
    g_esc = 0;
    g_cols = new_cols;
    for (int i = 0; i < n; i++) term_putc(flat[i]);
    g_need_redraw = 1;
}

static void drain_shell(void) {
    char buf[256];
    for (;;) {
        /* pipe_read() blocks while the pipe is empty, so only read when a
         * zero-timeout poll says there is data. */
        if (g_out_fd < 0) return;
        mcube_pollfd_t pfd;
        pfd.fd = g_out_fd;
        pfd.events = MCUBE_POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 0) <= 0) return;

        ssize_t n = read(g_out_fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) term_putc(buf[i]);
            continue;
        }
        if (n == 0) {
            if (!g_shell_dead) {
                g_shell_dead = 1;
                const char *msg = "\n[shell exited]\n";
                while (*msg) term_putc(*msg++);
            }
        }
        return;
    }
}

static void draw(void) {
    int w = gui_width(), h = gui_height();
    int cols = (w - 8) / 8;
    int rows = (h - 8) / 16;
    if (cols > TERM_COLS) cols = TERM_COLS;
    if (rows > TERM_ROWS) rows = TERM_ROWS;
    if (cols < 1 || rows < 1) return;
    if (cols != g_cols) reflow(cols);

    /* Follow the cursor: keep the last line visible. */
    if (g_row >= g_scroll + rows) g_scroll = g_row - rows + 1;
    if (g_row < g_scroll) g_scroll = g_row;
    if (g_scroll < 0) g_scroll = 0;

    gui_clear(C_BG);
    for (int r = 0; r < rows; r++) {
        int src = g_scroll + r;
        if (src >= TERM_ROWS) break;
        gui_textn(4, 4 + r * 16, C_FG, g_text[src], cols);
    }

    int cy = 4 + (g_row - g_scroll) * 16;
    int cx = 4 + g_col * 8;
    if (g_row >= g_scroll && g_row < g_scroll + rows && cx + 8 <= w && cy + 16 <= h) {
        gui_fill(cx, cy + 14, 8, 2, C_CUR);
    }
    gui_flush();
    g_need_redraw = 0;
}

static void forward_key(u32 code) {
    if (g_in_fd < 0 || g_shell_dead) return;
    char c;
    if (code == '\n') c = '\n';
    else if (code == '\b') c = 8;
    else if (code == '\t') c = '\t';
    else if (code >= 32 && code < 0x100) c = (char)code;
    else return;
    write(g_in_fd, &c, 1);
}

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_KEY:
        forward_key(ev->code);
        break;
    case GUI_EVT_RESIZE:
        g_need_redraw = 1;
        break;
    case GUI_EVT_CLOSE:
        if (g_shell > 0) kill(g_shell, 15);
        exit(0);
    default:
        break;
    }
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    if (gui_init("Terminal", 640, 400) != 0) {
        printf("USER_TERMINAL_FAIL: gui_init\n");
        return 1;
    }

    term_clear();
    g_cols = (gui_width() - 8) / 8;
    if (g_cols < 1) g_cols = 1;
    if (g_cols > TERM_COLS) g_cols = TERM_COLS;
    const char *banner = "MCubeAIOS terminal -- /bin/sh over a pipe\n";
    while (*banner) term_putc(*banner++);

    int in[2], out[2];
    if (pipe(in) == 0 && pipe(out) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            dup2(in[0], 0);
            dup2(out[1], 1);
            dup2(out[1], 2);
            close(in[0]); close(in[1]);
            close(out[0]); close(out[1]);
            char *sh_argv[] = { "sh", NULL };
            execv("/bin/sh.lgy", sh_argv);
            _exit(127);
        }
        if (pid > 0) {
            g_shell = pid;
            close(in[0]);
            close(out[1]);
            g_in_fd = in[1];
            g_out_fd = out[0];
        } else {
            close(in[0]); close(in[1]);
            close(out[0]); close(out[1]);
        }
    }

    draw();

    for (;;) {
        mcube_pollfd_t pfd;
        pfd.fd = g_out_fd;
        pfd.events = MCUBE_POLLIN;
        pfd.revents = 0;

        if (g_out_fd >= 0) {
            int rc = poll(&pfd, 1, 20);
            if (rc > 0 && (pfd.revents & MCUBE_POLLIN)) drain_shell();
        } else {
            sleep_ms(20);
        }

        gui_event_t ev;
        int rc;
        while ((rc = gui_poll(&ev)) == 1) handle_event(&ev);
        if (rc < 0) {
            if (g_shell > 0) kill(g_shell, 15);
            return 0;
        }

        if (g_need_redraw) draw();
    }
}
