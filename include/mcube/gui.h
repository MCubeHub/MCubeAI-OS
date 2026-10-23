#pragma once
/* MCubeAIOS GUI protocol (M8).
 *
 * Applications are ordinary processes. The desktop creates two pipes per
 * window and passes them as fd 3 (commands, app -> desktop) and fd 4 (events,
 * desktop -> app). Messages are fixed-size structures, so both sides can read
 * one message at a time without a framing layer.
 *
 * All drawing is executed by the desktop into a per-window surface; apps never
 * touch the framebuffer. That keeps clipping, stacking, the mouse cursor and
 * focus in one place and makes a crashing app harmless to the rest of the UI.
 */

#include <mcube/types.h>

#define GUI_CMD_FD 3
#define GUI_EVT_FD 4

#define GUI_TEXT_MAX 120

/* app -> desktop */
#define GUI_CMD_INIT    1
#define GUI_CMD_CLEAR   2
#define GUI_CMD_FILL    3
#define GUI_CMD_RECT    4
#define GUI_CMD_TEXT    5
#define GUI_CMD_FLUSH   6
#define GUI_CMD_TITLE   7
#define GUI_CMD_CLOSE   8
#define GUI_CMD_LAUNCH  9
#define GUI_CMD_KILL    10

/* desktop -> app */
#define GUI_EVT_RESIZE  1
#define GUI_EVT_KEY     2
#define GUI_EVT_MOUSE   3
#define GUI_EVT_FOCUS   4
#define GUI_EVT_CLOSE   5
#define GUI_EVT_TICK    6

typedef struct PACKED gui_cmd {
    u32 type;
    i32 x, y, w, h;
    u32 color;      /* 0x00RRGGBB */
    u32 len;        /* text length, 0 = NUL terminated */
    u32 flags;
    char text[GUI_TEXT_MAX];
} gui_cmd_t;

typedef struct PACKED gui_event {
    u32 type;
    u32 code;       /* key code, or mouse button mask */
    i32 x, y;       /* mouse position in window-content coordinates */
    i32 dx, dy;
    u32 buttons;
    u32 w, h;       /* new content size for RESIZE */
} gui_event_t;

/* Client side (user/lib/gui.c). Returns 0 on success. */
int  gui_init(const char *title, int want_w, int want_h);
int  gui_connected(void);
int  gui_width(void);
int  gui_height(void);

void gui_clear(u32 rgb);
void gui_fill(int x, int y, int w, int h, u32 rgb);
void gui_rect(int x, int y, int w, int h, u32 rgb);
void gui_text(int x, int y, u32 rgb, const char *s);
void gui_textn(int x, int y, u32 rgb, const char *s, int n);
void gui_flush(void);
void gui_set_title(const char *title);
void gui_launch(const char *path);
void gui_close(void);
void gui_kill(int pid);

int  gui_poll(gui_event_t *ev);              /* 1 event, 0 none, -1 gone */
int  gui_wait(gui_event_t *ev, int timeout_ms);

/* 8x16 bitmap font shared with the kernel console. */
extern const u8 mc_font8x16[256][16];
