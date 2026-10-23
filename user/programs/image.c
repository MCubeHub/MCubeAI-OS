/* Image Viewer: a procedurally generated landscape.
 *
 * The point is to exercise the drawing protocol with a few hundred primitives
 * without shipping a bitmap codec. Clicking (or pressing 1/2) switches palette.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>

#define C_PANEL 0x141A24
#define C_TEXT  0xE8EEF7

static int g_palette;
static int g_need_redraw = 1;

static u32 sky_color(int t) {
    /* t: 0..255 top..bottom */
    if (g_palette == 0) {
        int r = 20 + t * 210 / 255;
        int g = 30 + t * 110 / 255;
        int b = 90 - t * 50 / 255;
        return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
    }
    int r = 8 + t * 80 / 255;
    int g = 18 + t * 150 / 255;
    int b = 40 + t * 160 / 255;
    return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

static void draw(void) {
    int w = gui_width(), h = gui_height();
    if (w < 32 || h < 32) return;

    int horizon = h * 55 / 100;

    for (int y = 0; y < horizon; y += 4) {
        u32 c = sky_color(y * 255 / (horizon ? horizon : 1));
        gui_fill(0, y, w, 4, c);
    }

    /* Sun */
    int sun_x = w * 72 / 100;
    int sun_y = horizon * 38 / 100;
    int sun_r = (w < h ? w : h) / 12;
    for (int dy = -sun_r; dy <= sun_r; dy++) {
        int span = 0;
        int rr = sun_r * sun_r - dy * dy;
        while ((span + 1) * (span + 1) <= rr) span++;
        gui_fill(sun_x - span, sun_y + dy, span * 2 + 1, 1,
                 g_palette == 0 ? 0xFFD166 : 0x9BE7FF);
    }

    /* Distant mountains */
    for (int x = 0; x < w; x += 4) {
        int m1 = horizon - 10 - (x * 37 % 90);
        int m2 = horizon - 6 - ((w - x) * 23 % 60);
        int top = m1 < m2 ? m1 : m2;
        if (top < 0) top = 0;
        gui_fill(x, top, 4, horizon - top,
                 g_palette == 0 ? 0x2B3A55 : 0x18324A);
    }

    /* Near hills */
    for (int x = 0; x < w; x += 3) {
        int top = horizon - 4 - ((x * 13) % 34);
        gui_fill(x, top, 3, h - top,
                 g_palette == 0 ? 0x16202E : 0x0C1A26);
    }

    /* Horizon glow */
    gui_fill(0, horizon - 2, w, 3,
             g_palette == 0 ? 0xF4A261 : 0x4CC9F0);

    /* Title card */
    gui_fill(10, 34, w - 20, 46, C_PANEL);
    gui_rect(10, 34, w - 20, 46, 0x2D6CDF);
    gui_text(20, 40, C_TEXT, "MCubeAIOS  --  M8 graphics");
    gui_text(20, 58, 0x93A1B5, g_palette == 0 ? "sunset palette (click to change)"
                                               : "night palette (click to change)");

    gui_flush();
    g_need_redraw = 0;
}

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_MOUSE:
        if (ev->buttons & 1) {
            g_palette ^= 1;
            g_need_redraw = 1;
        }
        break;
    case GUI_EVT_KEY:
        if (ev->code == '1') { g_palette = 0; g_need_redraw = 1; }
        else if (ev->code == '2') { g_palette = 1; g_need_redraw = 1; }
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

    if (gui_init("Image Viewer", 520, 400) != 0) {
        printf("USER_IMAGE_FAIL: gui_init\n");
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
