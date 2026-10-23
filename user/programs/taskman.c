/* Task Manager: live process list built from SYS_TASKINFO.
 *
 * The list refreshes on every desktop tick (once a second), so it doubles as a
 * smoke test for the scheduler snapshot ABI. Selecting a row and pressing
 * Delete (or clicking "End Task") asks the desktop to terminate that process.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>

#define MAX_TASKS 32
#define ROW_H     18
#define LIST_Y    68

#define C_BG     0x0F141C
#define C_PANEL  0x1A2230
#define C_TEXT   0xE2E8F0
#define C_DIM    0x93A1B5
#define C_ACCENT 0x60A5FA
#define C_SEL    0x2D6CDF
#define C_BTN    0x232D3E
#define C_WARN   0xEF4444

static mcube_task_info_t g_tasks[MAX_TASKS];
static int g_count;
static int g_sel;
static int g_top;
static int g_need_redraw = 1;
static u64 g_uptime_s;
static u64 g_total_ticks;
static char g_status[64] = "ready";

static void refresh(void) {
    g_count = taskinfo(g_tasks, MAX_TASKS);
    if (g_count < 0) g_count = 0;

    g_total_ticks = 0;
    for (int i = 0; i < g_count; i++) g_total_ticks += g_tasks[i].ticks;

    mcube_time_t ts;
    if (mc_time(&ts) == 0) g_uptime_s = ts.uptime_ms / 1000;

    if (g_sel >= g_count) g_sel = g_count > 0 ? g_count - 1 : 0;
    if (g_sel < 0) g_sel = 0;
    g_need_redraw = 1;
}

static const char *state_name(u32 state) {
    switch (state) {
    case 0: return "ready";
    case 1: return "run";
    case 2: return "block";
    case 3: return "dead";
    default: return "?";
    }
}

static void kill_selected(void) {
    if (g_sel < 0 || g_sel >= g_count) return;
    u32 pid = g_tasks[g_sel].pid;
    pid_t self = getpid();
    if (pid <= 1 || (int)pid == (int)self) {
        snprintf(g_status, sizeof(g_status), "refusing to end pid %u", pid);
        g_need_redraw = 1;
        return;
    }
    gui_kill((int)pid);
    snprintf(g_status, sizeof(g_status), "sent SIGTERM to pid %u", pid);
    g_need_redraw = 1;
}

static void button_rect(int w, int h, int *bx, int *by, int *bw, int *bh) {
    *bw = 104;
    *bh = 24;
    *bx = 10;
    *by = h - *bh - 8;
    (void)w;
}

static void draw(void) {
    int w = gui_width(), h = gui_height();
    int list_y = LIST_Y;
    int rows = (h - list_y - 46) / ROW_H;
    if (rows < 1) rows = 1;

    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + rows) g_top = g_sel - rows + 1;
    if (g_top < 0) g_top = 0;

    gui_clear(C_BG);
    gui_fill(0, 0, w, 26, C_PANEL);
    gui_text(8, 5, C_ACCENT, "Task Manager");

    char line[128];
    snprintf(line, sizeof(line), "%d processes   uptime %u s   %u ticks",
             g_count, (u32)g_uptime_s, (u32)g_total_ticks);
    gui_text(10, 30, C_DIM, line);

    gui_text(10, list_y - 17, C_DIM, "PID PPID STATE  CPU% NAME");

    for (int r = 0; r < rows; r++) {
        int idx = g_top + r;
        if (idx >= g_count) break;
        int y = list_y + r * ROW_H;
        mcube_task_info_t *t = &g_tasks[idx];

        if (idx == g_sel) gui_fill(4, y - 1, w - 8, ROW_H, C_SEL);

        u32 pct = 0;
        if (g_total_ticks) pct = (u32)(t->ticks * 100 / g_total_ticks);

        snprintf(line, sizeof(line), "%3u %4u %-5s %3u%% %s",
                 t->pid, t->ppid, state_name(t->state), pct, t->name);
        gui_text(10, y, idx == g_sel ? 0xFFFFFF : C_TEXT, line);
    }

    if (g_count == 0) gui_text(10, list_y, C_DIM, "(no tasks)");

    gui_fill(0, h - 40, w, 40, C_PANEL);
    int bx, by, bw, bh;
    button_rect(w, h, &bx, &by, &bw, &bh);
    gui_fill(bx, by, bw, bh, C_BTN);
    gui_rect(bx, by, bw, bh, C_WARN);
    gui_text(bx + 10, by + 4, C_TEXT, "End Task");

    gui_fill(bx + bw + 8, by, bw, bh, C_BTN);
    gui_rect(bx + bw + 8, by, bw, bh, 0x3A475C);
    gui_text(bx + bw + 18, by + 4, C_TEXT, "Refresh");

    gui_text(bx + 2 * bw + 24, by + 4, C_DIM, g_status);
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
        } else if (ev->code == INPUT_KEY_DELETE) {
            kill_selected();
        } else if (ev->code == INPUT_KEY_F5) {
            refresh();
        }
        break;

    case GUI_EVT_MOUSE: {
        if (!(ev->buttons & 1)) break;

        int w = gui_width(), h = gui_height();
        int bx, by, bw, bh;
        button_rect(w, h, &bx, &by, &bw, &bh);
        if (ev->y >= by && ev->y < by + bh) {
            if (ev->x >= bx && ev->x < bx + bw) kill_selected();
            else if (ev->x >= bx + bw + 8 && ev->x < bx + 2 * bw + 8) refresh();
            break;
        }

        int row = (ev->y - LIST_Y) / ROW_H;
        int idx = g_top + row;
        if (ev->y >= LIST_Y && row >= 0 && idx >= 0 && idx < g_count) {
            g_sel = idx;
            g_need_redraw = 1;
        }
        break;
    }

    case GUI_EVT_TICK:
        refresh();
        break;

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

    if (gui_init("Task Manager", 480, 380) != 0) {
        printf("USER_TASKMAN_FAIL: gui_init\n");
        return 1;
    }

    refresh();
    draw();

    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 250);
        if (rc < 0) return 0;
        if (rc > 0) handle_event(&ev);
        if (g_need_redraw) draw();
    }
}
