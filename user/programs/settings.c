/* Settings: system information and a few desktop actions. */

#include <mcube/libc.h>
#include <mcube/gui.h>
#include <mcube/fb.h>

#define C_PANEL 0x1A2230
#define C_TEXT  0xE6ECF5
#define C_DIM   0x93A1B5
#define C_ACCENT 0x3B82F6

static u32 g_bg = 0x10151E;
static u32 g_panel = 0x1A2230;
static int g_theme;
static int g_need_redraw = 1;

static const u32 g_themes[3] = { 0x10151E, 0x1A1020, 0x0C1A14 };

static void draw_button(int x, int y, int w, int h, const char *label) {
    gui_fill(x, y, w, h, C_ACCENT);
    gui_rect(x, y, w, h, 0x60A5FA);
    gui_text(x + 8, y + (h - 16) / 2, 0xFFFFFF, label);
}

static void draw(void) {
    int w = gui_width(), h = gui_height();

    gui_clear(g_bg);
    gui_fill(0, 0, w, 26, g_panel);
    gui_text(8, 5, C_ACCENT, "Settings");

    mcube_fb_info_t fb;
    mcube_time_t ts;
    mcube_task_info_t tasks[16];

    int have_fb = fb_info(&fb) == 0;
    int have_time = mc_time(&ts) == 0;
    int ntasks = taskinfo(tasks, 16);

    char line[128];
    int y = 40;

    gui_text(14, y, C_DIM, "Display"); y += 18;
    if (have_fb) {
        snprintf(line, sizeof(line), "  framebuffer %ux%u, pitch %u, %s",
                 fb.width, fb.height, fb.pitch,
                 fb.format == 1 ? "BGRA8888" : "RGBA8888");
        gui_text(14, y, C_TEXT, line);
    }
    y += 26;

    gui_text(14, y, C_DIM, "System"); y += 18;
    if (have_time) {
        u32 secs = (u32)(ts.epoch % 86400);
        snprintf(line, sizeof(line), "  uptime %u s   clock %02u:%02u:%02u UTC",
                 (u32)(ts.uptime_ms / 1000), secs / 3600,
                 (secs / 60) % 60, secs % 60);
        gui_text(14, y, C_TEXT, line); y += 18;
    }
    snprintf(line, sizeof(line), "  processes %d   PIT ticks %u",
             ntasks, have_time ? (u32)ts.ticks : 0);
    gui_text(14, y, C_TEXT, line); y += 26;

    gui_text(14, y, C_DIM, "Theme"); y += 18;
    for (int i = 0; i < 3; i++) {
        int bx = 14 + i * 44;
        gui_fill(bx, y, 34, 22, g_themes[i]);
        gui_rect(bx, y, 34, 22, i == g_theme ? 0xFFFFFF : 0x3A475C);
        gui_text(bx + 8, y + 3, C_TEXT, i == g_theme ? "*" : " ");
    }
    y += 38;

    draw_button(14, y, 88, 26, "Refresh");
    draw_button(108, y, 88, 26, "Taskman");
    draw_button(202, y, 88, 26, "Terminal");

    gui_text(14, h - 24, C_DIM, "Updates every second.");

    gui_flush();
    g_need_redraw = 0;
}

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_KEY:
        if (ev->code == '1' || ev->code == '2' || ev->code == '3') {
            g_theme = (int)(ev->code - '1');
            g_bg = g_themes[g_theme];
            g_need_redraw = 1;
        } else if (ev->code == INPUT_KEY_F5) {
            g_need_redraw = 1;
        }
        break;

    case GUI_EVT_MOUSE: {
        if (!(ev->buttons & 1)) break;
        int y = 40 + 18 + 26 + 18 + 18 + 26 + 18;
        for (int i = 0; i < 3; i++) {
            int bx = 14 + i * 44;
            if (ev->x >= bx && ev->x < bx + 34 && ev->y >= y && ev->y < y + 22) {
                g_theme = i;
                g_bg = g_themes[i];
                g_need_redraw = 1;
                return;
            }
        }
        int by = y + 38;
        if (ev->y >= by && ev->y < by + 26) {
            if (ev->x >= 14 && ev->x < 102) g_need_redraw = 1;
            else if (ev->x >= 108 && ev->x < 196) gui_launch("/bin/taskman.lgy");
            else if (ev->x >= 202 && ev->x < 290) gui_launch("/bin/terminal.lgy");
        }
        break;
    }

    case GUI_EVT_TICK:
        g_need_redraw = 1;
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

    if (gui_init("Settings", 420, 360) != 0) {
        printf("USER_SETTINGS_FAIL: gui_init\n");
        return 1;
    }

    draw();
    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 250);
        if (rc < 0) return 0;
        if (rc > 0) handle_event(&ev);
        if (g_need_redraw) draw();
    }
}
