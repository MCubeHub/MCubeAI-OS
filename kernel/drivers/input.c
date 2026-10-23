/* Raw keyboard/mouse event queue and the PS/2 mouse driver.
 *
 * The keyboard IRQ (IRQ1) still feeds the TTY ring so the serial console keeps
 * working, but it also pushes an input_event_t here. The mouse uses IRQ12 and
 * only feeds this queue. A user-space display server drains the queue with
 * SYS_INPUT_POLL; the kernel never interprets the events.
 */

#include <mcube/input.h>
#include <mcube/kernel.h>
#include <mcube/io.h>
#include <mcube/atomic.h>

#define PS2_STATUS 0x64
#define PS2_DATA   0x60

static input_event_t g_queue[INPUT_QUEUE_SIZE];
static volatile u32 g_head;
static volatile u32 g_tail;
static spinlock_t g_lock = SPINLOCK_INIT;

static i32 g_mouse_x;
static i32 g_mouse_y;
static i32 g_mouse_w = 1024;
static i32 g_mouse_h = 768;
static u32 g_buttons;

static u8  g_packet[3];
static u32 g_packet_len;

static void queue_push(const input_event_t *ev) {
    u64 flags = spin_lock_irqsave(&g_lock);
    u32 next = (g_head + 1) % INPUT_QUEUE_SIZE;
    if (next != g_tail) {
        g_queue[g_head] = *ev;
        g_head = next;
    }
    spin_unlock_irqrestore(&g_lock, flags);
}

void input_push_key(u32 code) {
    input_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_KEY;
    ev.code = code;
    ev.x = g_mouse_x;
    ev.y = g_mouse_y;
    ev.buttons = g_buttons;
    queue_push(&ev);
}

static i32 clamp_i32(i32 v, i32 lo, i32 hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void input_push_mouse(i32 dx, i32 dy, u32 buttons) {
    i32 maxx = g_mouse_w > 1 ? g_mouse_w - 1 : 0;
    i32 maxy = g_mouse_h > 1 ? g_mouse_h - 1 : 0;
    g_mouse_x = clamp_i32(g_mouse_x + dx, 0, maxx);
    g_mouse_y = clamp_i32(g_mouse_y + dy, 0, maxy);
    g_buttons = buttons;

    input_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_MOUSE;
    ev.code = buttons;
    ev.x = g_mouse_x;
    ev.y = g_mouse_y;
    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = buttons;
    queue_push(&ev);
}

int input_pop(input_event_t *out) {
    if (!out) return 0;
    u64 flags = spin_lock_irqsave(&g_lock);
    if (g_head == g_tail) {
        spin_unlock_irqrestore(&g_lock, flags);
        return 0;
    }
    *out = g_queue[g_tail];
    g_tail = (g_tail + 1) % INPUT_QUEUE_SIZE;
    spin_unlock_irqrestore(&g_lock, flags);
    return 1;
}

void input_mouse_bounds(u32 width, u32 height) {
    u64 flags = irq_save();
    if (width) g_mouse_w = (i32)width;
    if (height) g_mouse_h = (i32)height;
    if (g_mouse_x >= g_mouse_w) g_mouse_x = g_mouse_w - 1;
    if (g_mouse_y >= g_mouse_h) g_mouse_y = g_mouse_h - 1;
    irq_restore(flags);
}

/* ---- PS/2 controller helpers --------------------------------------- */

static void ps2_wait_write(void) {
    for (u32 i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & 0x02)) return;
        io_wait();
    }
}

static void ps2_wait_read(void) {
    for (u32 i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & 0x01) return;
        io_wait();
    }
}

static void ps2_flush(void) {
    for (u32 i = 0; i < 64 && (inb(PS2_STATUS) & 0x01); i++) (void)inb(PS2_DATA);
}

static void mouse_command(u8 cmd) {
    ps2_wait_write();
    outb(PS2_STATUS, 0xD4);  /* next byte goes to the auxiliary device */
    ps2_wait_write();
    outb(PS2_DATA, cmd);
}

/* Packet assembly is shared by IRQ1 and IRQ12: on some firmware the mouse
 * byte is delivered on IRQ1 if the controller config was not programmed. */
void input_mouse_byte(u8 byte) {
    if (g_packet_len == 0) {
        if (!(byte & 0x08)) return;   /* out of sync: wait for bit 3 */
        g_packet[0] = byte;
        g_packet_len = 1;
        return;
    }

    g_packet[g_packet_len++] = byte;
    if (g_packet_len < 3) return;
    g_packet_len = 0;

    i32 dx = (i32)(i8)g_packet[1];
    i32 dy = -(i32)(i8)g_packet[2];   /* PS/2 Y grows upwards */
    u32 buttons = (u32)(g_packet[0] & 0x07);
    input_push_mouse(dx, dy, buttons);
}

static void mouse_irq(void *frame, void *ctx) {
    UNUSED(frame);
    UNUSED(ctx);

    u8 status = inb(PS2_STATUS);
    if (!(status & 0x20)) return;    /* not auxiliary data */
    input_mouse_byte(inb(PS2_DATA));
}

static int mouse_init(void) {
    ps2_flush();

    ps2_wait_write();
    outb(PS2_STATUS, 0xA8);          /* enable auxiliary (mouse) port */

    ps2_wait_write();
    outb(PS2_STATUS, 0x20);          /* read controller configuration */
    ps2_wait_read();
    u8 cfg = inb(PS2_DATA);
    cfg |= 0x02;                     /* enable IRQ12 */
    cfg &= (u8)~0x20;                /* enable the mouse clock */
    ps2_wait_write();
    outb(PS2_STATUS, 0x60);
    ps2_wait_write();
    outb(PS2_DATA, cfg);

    mouse_command(0xF6);             /* set defaults */
    mouse_command(0xF4);             /* enable data reporting */
    ps2_flush();

    irq_register(44, mouse_irq, NULL); /* IRQ12 -> vector 0x2C */
    irq_unmask(12);
    return 0;
}

void input_init(void) {
    g_head = 0;
    g_tail = 0;
    g_packet_len = 0;
    g_buttons = 0;

    mcube_fb_info_t fb;
    if (fb_get_info(&fb) == 0 && fb.width && fb.height) {
        g_mouse_w = (i32)fb.width;
        g_mouse_h = (i32)fb.height;
    }
    g_mouse_x = g_mouse_w / 2;
    g_mouse_y = g_mouse_h / 2;

    mouse_init();
    kprintf("[input] event queue %u entries, mouse at %ux%u IRQ12\n",
            (u32)INPUT_QUEUE_SIZE, (u32)g_mouse_w, (u32)g_mouse_h);
}
