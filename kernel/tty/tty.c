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
static int g_alt;
static int g_ext;

static void push_mod(u32 code, int down) {
    input_push_key(down ? code : (code | INPUT_KEY_RELEASE));
}

static void kbd_irq(void *frame, void *ctx) {
    UNUSED(frame);
    UNUSED(ctx);

    u8 status = inb(0x64);
    u8 sc = inb(0x60);

    /* IRQ1 can carry auxiliary (mouse) bytes if the controller was left in a
     * non-standard configuration. Feed them to the shared packet parser. */
    if (status & 0x20) {
        input_mouse_byte(sc);
        return;
    }

    if (sc == 0xE0) {          /* extended scancode prefix */
        g_ext = 1;
        return;
    }
    int ext = g_ext;
    g_ext = 0;

    if (sc & 0x80) {           /* key release */
        u8 code = (u8)(sc & 0x7F);
        if (!ext && (code == 0x2A || code == 0x36)) {
            g_shift = 0;
            push_mod(code == 0x2A ? INPUT_KEY_LSHIFT : INPUT_KEY_RSHIFT, 0);
        } else if (!ext && code == 0x1D) {
            push_mod(INPUT_KEY_LCTRL, 0);
        } else if (!ext && code == 0x38) {
            g_alt = 0;
            push_mod(INPUT_KEY_LALT, 0);
        } else if (ext && code == 0x1D) {
            push_mod(INPUT_KEY_RCTRL, 0);
        } else if (ext && code == 0x38) {
            g_alt = 0;
            push_mod(INPUT_KEY_RALT, 0);
        } else if (ext && code == 0x5B) {
            push_mod(INPUT_KEY_LWIN, 0);
        } else if (ext && code == 0x5C) {
            push_mod(INPUT_KEY_RWIN, 0);
        }
        return;
    }
    if (!ext && (sc == 0x2A || sc == 0x36)) {
        g_shift = 1;
        push_mod(sc == 0x2A ? INPUT_KEY_LSHIFT : INPUT_KEY_RSHIFT, 1);
        return;
    }
    if (!ext && sc == 0x3A) {
        g_caps = !g_caps;
        return;
    }
    if (!ext && sc == 0x1D) {
        push_mod(INPUT_KEY_LCTRL, 1);
        return;
    }
    if (!ext && sc == 0x38) {
        g_alt = 1;
        push_mod(INPUT_KEY_LALT, 1);
        return;
    }
    if (ext && sc == 0x1D) {
        push_mod(INPUT_KEY_RCTRL, 1);
        return;
    }
    if (ext && sc == 0x38) {
        g_alt = 1;
        push_mod(INPUT_KEY_RALT, 1);
        return;
    }
    if (ext && (sc == 0x5B || sc == 0x5C)) {
        push_mod(sc == 0x5B ? INPUT_KEY_LWIN : INPUT_KEY_RWIN, 1);
        return;
    }

    u32 code = 0;
    if (ext) {
        switch (sc) {
        case 0x48: code = INPUT_KEY_UP; break;
        case 0x50: code = INPUT_KEY_DOWN; break;
        case 0x4B: code = INPUT_KEY_LEFT; break;
        case 0x4D: code = INPUT_KEY_RIGHT; break;
        case 0x53: code = INPUT_KEY_DELETE; break;
        case 0x47: code = INPUT_KEY_HOME; break;
        case 0x4F: code = INPUT_KEY_END; break;
        case 0x49: code = INPUT_KEY_PGUP; break;
        case 0x51: code = INPUT_KEY_PGDN; break;
        case 0x1C: code = '\n'; break;      /* keypad Enter */
        default: break;
        }
    } else if (sc >= 0x3B && sc <= 0x44) {
        code = INPUT_KEY_F1 + (u32)(sc - 0x3B);
    } else {
        char c = g_shift ? g_shiftmap[sc] : g_keymap[sc];
        if (g_caps && !g_shift && c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c) code = (u8)c;
    }
    if (!code) return;

    if (code < 0x100) {
        char c = (char)code;
        /* Alt is a GUI modifier: do not leak Alt+Tab as a TAB on the console. */
        if (!g_alt &&
            (c == '\n' || c == '\b' || c == '\t' || c == 27 || c >= 32)) {
            tty_input(c);
        }
    }
    input_push_key(code);
}

void keyboard_init(void) {
    g_shift = 0;
    g_caps = 0;
    g_alt = 0;
    g_ext = 0;
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
