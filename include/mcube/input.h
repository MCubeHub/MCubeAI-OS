#pragma once
/* Keyboard/mouse event queue shared by the GUI and the console.
 *
 * The PS/2 keyboard IRQ still feeds the TTY ring for the serial console, but
 * it also produces a raw event here so a user-space display server can
 * implement its own focus and shortcuts. The mouse driver (IRQ12) only feeds
 * this queue.
 */

#include <mcube/types.h>

#define INPUT_KEY        1u
#define INPUT_MOUSE      2u

/* Non-ASCII key codes. Printable keys use their ASCII value. */
#define INPUT_KEY_UP     0x100u
#define INPUT_KEY_DOWN   0x101u
#define INPUT_KEY_LEFT   0x102u
#define INPUT_KEY_RIGHT  0x103u
#define INPUT_KEY_DELETE 0x104u
#define INPUT_KEY_HOME   0x105u
#define INPUT_KEY_END    0x106u
#define INPUT_KEY_PGUP   0x107u
#define INPUT_KEY_PGDN   0x108u
#define INPUT_KEY_F1     0x110u
#define INPUT_KEY_F2     0x111u
#define INPUT_KEY_F3     0x112u
#define INPUT_KEY_F4     0x113u
#define INPUT_KEY_F5     0x114u
#define INPUT_KEY_F6     0x115u
#define INPUT_KEY_F7     0x116u
#define INPUT_KEY_F8     0x117u
#define INPUT_KEY_F9     0x118u
#define INPUT_KEY_F10    0x119u
#define INPUT_KEY_F11    0x11Au
#define INPUT_KEY_F12    0x11Bu

/* Modifier keys report their own press/release events so a display server can
 * implement Alt+Tab, Alt+F4 and the Windows key. Printable keys keep emitting
 * plain ASCII for terminal consumers. */
#define INPUT_KEY_LSHIFT 0x120u
#define INPUT_KEY_RSHIFT 0x121u
#define INPUT_KEY_LCTRL  0x122u
#define INPUT_KEY_RCTRL  0x123u
#define INPUT_KEY_LALT   0x124u
#define INPUT_KEY_RALT   0x125u
#define INPUT_KEY_LWIN   0x126u
#define INPUT_KEY_RWIN   0x127u
#define INPUT_KEY_RELEASE 0x80000000u

#define INPUT_QUEUE_SIZE 256

typedef struct PACKED input_event {
    u32 type;
    u32 code;      /* key code, or mouse button mask */
    i32 x, y;      /* absolute mouse position (pixels) */
    i32 dx, dy;    /* mouse delta */
    u32 buttons;
} input_event_t;

void input_init(void);
void input_push_key(u32 code);
void input_push_mouse(i32 dx, i32 dy, u32 buttons);
void input_mouse_byte(u8 byte);       /* raw PS/2 packet byte from IRQ1/IRQ12 */
int  input_pop(input_event_t *out);   /* 1 = event, 0 = empty */
void input_mouse_bounds(u32 width, u32 height);
