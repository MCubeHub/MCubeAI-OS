#include <mcube/kernel.h>
#include <mcube/io.h>

#define COM1 0x3F8
#define COM2 0x2F8

static int g_serial_ready;

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* disable interrupts */
    outb(COM1 + 3, 0x80); /* DLAB on */
    outb(COM1 + 0, 0x01); /* divisor 1 => 115200 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1 */
    outb(COM1 + 2, 0xC7); /* FIFO enable, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B); /* IRQs enabled, RTS/DSR set */
    g_serial_ready = 1;
    /* Loopback self test would go here; QEMU's UART always responds. */
}

void serial_putc(char c) {
    if (!g_serial_ready) return;
    for (int i = 0; i < 100000; i++) {
        if (inb(COM1 + 5) & 0x20) break;
    }
    if (c == '\n') {
        outb(COM1, '\r');
    }
    outb(COM1, (u8)c);
}

void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}

void serial_init_secondary(void) {
    outb(COM2 + 1, 0x00);
    outb(COM2 + 3, 0x80);
    outb(COM2 + 0, 0x01);
    outb(COM2 + 1, 0x00);
    outb(COM2 + 3, 0x03);
    outb(COM2 + 2, 0xC7);
    outb(COM2 + 4, 0x0B);
}
