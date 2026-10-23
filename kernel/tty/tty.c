/* TTY input queue and console output helpers.
 *
 * The PS/2 keyboard IRQ and the COM1 receive IRQ both push bytes here. Reads
 * are non-blocking for now: user space gets EAGAIN when the queue is empty,
 * which keeps a syscall from sleeping with interrupts masked.
 */

#include <mcube/tty.h>
#include <mcube/kernel.h>
#include <mcube/io.h>
#include <mcube/atomic.h>
#include <mcube/syscall.h>

#define TTY_BUF_SIZE 256

static char g_buf[TTY_BUF_SIZE];
static volatile u32 g_head;
static volatile u32 g_tail;
static spinlock_t g_lock = SPINLOCK_INIT;

void tty_input(char c) {
    u64 flags = spin_lock_irqsave(&g_lock);
    u32 next = (g_head + 1) % TTY_BUF_SIZE;
    if (next != g_tail) {
        g_buf[g_head] = c;
        g_head = next;
    }
    spin_unlock_irqrestore(&g_lock, flags);
}

int tty_pending(void) {
    return g_head != g_tail;
}

long tty_read(char *buf, u64 len) {
    if (!buf || len == 0) return -EINVAL;

    u64 flags = spin_lock_irqsave(&g_lock);
    u64 done = 0;
    while (done < len && g_tail != g_head) {
        buf[done++] = g_buf[g_tail];
        g_tail = (g_tail + 1) % TTY_BUF_SIZE;
    }
    spin_unlock_irqrestore(&g_lock, flags);

    if (done == 0) return -EAGAIN;
    return (long)done;
}

void tty_write(const char *buf, u64 len) {
    for (u64 i = 0; i < len; i++) kputc(buf[i]);
}

/* ---- PS/2 keyboard (scancode set 1) -------------------------------- */

#define KBD_DATA 0x60

static const char g_keymap[128] = {
    [0x01] = 27,  [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9',
    [0x0B] = '0', [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n', [0x1E] = 'a', [0x1F] = 's',
    [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h', [0x24] = 'j',
    [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.',
    [0x35] = '/', [0x39] = ' ',
};

static const char g_shiftmap[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x1A] = '{', [0x1B] = '}', [0x27] = ':',
    [0x28] = '"', [0x29] = '~', [0x2B] = '|', [0x33] = '<', [0x34] = '>',
    [0x35] = '?',
};

static int g_shift;
static int g_caps;

static void kbd_irq(void *frame, void *ctx) {
    UNUSED(frame);
    UNUSED(ctx);

    u8 sc = inb(KBD_DATA);
    if (sc & 0x80) {
        u8 code = (u8)(sc & 0x7F);
        if (code == 0x2A || code == 0x36) g_shift = 0;
        return;
    }
    if (sc == 0x2A || sc == 0x36) {
        g_shift = 1;
        return;
    }
    if (sc == 0x3A) {
        g_caps = !g_caps;
        return;
    }

    char c = g_shift ? g_shiftmap[sc] : g_keymap[sc];
    if (!c) return;
    if (g_caps && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    tty_input(c);
}

void keyboard_init(void) {
    irq_register(33, kbd_irq, NULL); /* IRQ1 -> vector 0x21 */
    irq_unmask(1);
}

/* ---- COM1 receive -------------------------------------------------- */

#define COM1 0x3F8

static void serial_rx_irq(void *frame, void *ctx) {
    UNUSED(frame);
    UNUSED(ctx);

    while (inb(COM1 + 5) & 0x01) {
        char c = (char)inb(COM1);
        if (c == '\r') c = '\n';
        tty_input(c);
    }
}

static void serial_rx_init(void) {
    outb(COM1 + 1, 0x01); /* enable "received data available" */
    irq_register(36, serial_rx_irq, NULL); /* IRQ4 -> vector 0x24 */
    irq_unmask(4);
}

void tty_init(void) {
    g_head = 0;
    g_tail = 0;
    keyboard_init();
    serial_rx_init();
    kprintf("[tty] console input ready (PS/2 IRQ1, COM1 IRQ4)\n");
}
