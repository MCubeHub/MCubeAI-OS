/* Client side of the desktop GUI protocol (see <mcube/gui.h>). */

#include <mcube/libc.h>
#include <mcube/gui.h>

static int g_ok;
static int g_w = 640;
static int g_h = 400;

static int write_all(int fd, const void *buf, size_t len) {
    const u8 *p = (const u8 *)buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len) {
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

static int send_cmd(gui_cmd_t *cmd) {
    if (!g_ok) return -1;
    if (write_all(GUI_CMD_FD, cmd, sizeof(*cmd)) != 0) {
        g_ok = 0;
        return -1;
    }
    return 0;
}

static void init_cmd(gui_cmd_t *cmd, u32 type) {
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = type;
}

int gui_init(const char *title, int want_w, int want_h) {
    gui_cmd_t cmd;
    init_cmd(&cmd, GUI_CMD_INIT);
    cmd.x = want_w;
    cmd.y = want_h;
    if (title) {
        cmd.len = (u32)strlen(title);
        if (cmd.len >= GUI_TEXT_MAX) cmd.len = GUI_TEXT_MAX - 1;
        memcpy(cmd.text, title, cmd.len);
    }

    g_ok = 1;
    g_w = want_w;
    g_h = want_h;
    if (send_cmd(&cmd) != 0) return -1;

    /* The desktop replies with the size it assigned to our window. */
    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 3000);
        if (rc < 0) return -1;
        if (rc == 0) break;          /* tolerate a slow/absent resize */
        if (ev.type == GUI_EVT_RESIZE) {
            if (ev.w > 0) g_w = (int)ev.w;
            if (ev.h > 0) g_h = (int)ev.h;
            break;
        }
    }
    return 0;
}

int gui_connected(void) { return g_ok; }
int gui_width(void) { return g_w; }
int gui_height(void) { return g_h; }

void gui_clear(u32 rgb) {
    gui_cmd_t cmd;
    init_cmd(&cmd, GUI_CMD_CLEAR);
    cmd.color = rgb;
    send_cmd(&cmd);
}

void gui_fill(int x, int y, int w, int h, u32 rgb) {
    gui_cmd_t cmd;
    init_cmd(&cmd, GUI_CMD_FILL);
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h; cmd.color = rgb;
    send_cmd(&cmd);
}

void gui_rect(int x, int y, int w, int h, u32 rgb) {
    gui_cmd_t cmd;
    init_cmd(&cmd, GUI_CMD_RECT);
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h; cmd.color = rgb;
    send_cmd(&cmd);
}

void gui_textn(int x, int y, u32 rgb, const char *s, int n) {
    gui_cmd_t cmd;
    init_cmd(&cmd, GUI_CMD_TEXT);
    cmd.x = x; cmd.y = y; cmd.color = rgb;
    if (!s) n = 0;
    if (n < 0) n = (int)strlen(s);
    if (n > GUI_TEXT_MAX - 1) n = GUI_TEXT_MAX - 1;
    cmd.len = (u32)n;
    if (n) memcpy(cmd.text, s, (size_t)n);
    send_cmd(&cmd);
}

void gui_text(int x, int y, u32 rgb, const char *s) {
    gui_textn(x, y, rgb, s, -1);
}

void gui_flush(void) {
    gui_cmd_t cmd;
    init_cmd(&cmd, GUI_CMD_FLUSH);
    send_cmd(&cmd);
}

void gui_set_title(const char *title) {
    gui_cmd_t cmd;
    init_cmd(&cmd, GUI_CMD_TITLE);
    if (title) {
        cmd.len = (u32)strlen(title);
        if (cmd.len >= GUI_TEXT_MAX) cmd.len = GUI_TEXT_MAX - 1;
        memcpy(cmd.text, title, cmd.len);
    }
    send_cmd(&cmd);
}

void gui_launch(const char *path) {
    gui_cmd_t cmd;
    init_cmd(&cmd, GUI_CMD_LAUNCH);
    if (path) {
        cmd.len = (u32)strlen(path);
        if (cmd.len >= GUI_TEXT_MAX) cmd.len = GUI_TEXT_MAX - 1;
        memcpy(cmd.text, path, cmd.len);
    }
    send_cmd(&cmd);
}

void gui_close(void) {
    gui_cmd_t cmd;
    init_cmd(&cmd, GUI_CMD_CLOSE);
    send_cmd(&cmd);
    g_ok = 0;
}

void gui_kill(int pid) {
    gui_cmd_t cmd;
    init_cmd(&cmd, GUI_CMD_KILL);
    cmd.x = pid;
    send_cmd(&cmd);
}

int gui_wait(gui_event_t *ev, int timeout_ms) {
    if (!g_ok) return -1;

    mcube_pollfd_t pfd;
    pfd.fd = GUI_EVT_FD;
    pfd.events = MCUBE_POLLIN;
    pfd.revents = 0;

    int rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0) return -1;
    if (rc == 0) return 0;

    if (read_all(GUI_EVT_FD, ev, sizeof(*ev)) != 0) {
        g_ok = 0;
        return -1;
    }
    /* Keep the cached geometry in sync with every resize, not just the one
     * consumed during gui_init(): the user can resize or maximize a window at
     * any time. */
    if (ev->type == GUI_EVT_RESIZE) {
        if ((i32)ev->w > 0) g_w = (int)ev->w;
        if ((i32)ev->h > 0) g_h = (int)ev->h;
    }
    return 1;
}

int gui_poll(gui_event_t *ev) {
    return gui_wait(ev, 0);
}
