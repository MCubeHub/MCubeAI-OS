/* Panic / oops path: dump state, walk the frame pointer chain, halt. */

#include <mcube/kernel.h>
#include <mcube/io.h>
#include <stdarg.h>

static volatile int g_panicking;

static int in_kernel_text(u64 addr) {
    extern char __kernel_end[];
    /* The kernel executes from the top 2 GiB; the loader maps the image's
     * physical pages there (KERNEL_VMA + 2 MiB .. __kernel_end). */
    return addr >= MCUBE_KERNEL_VBASE + 0x200000ULL &&
           addr < (u64)(uptr)__kernel_end;
}

void stack_trace(void) {
    u64 *rbp;
    __asm__ __volatile__("mov %%rbp, %0" : "=r"(rbp));
    kprintf("stack trace (frame pointers):\n");
    for (int depth = 0; depth < 16; depth++) {
        if (!rbp || ((u64)(uptr)rbp & 7) || !in_kernel_text((u64)(uptr)rbp)) break;
        u64 ret = rbp[1];
        if (!ret) break;
        kprintf("  #%d %p\n", depth, (void *)ret);
        u64 *next = (u64 *)rbp[0];
        if (next <= rbp) break;
        rbp = next;
    }
}

NORETURN void halt_forever(void) {
    cpu_cli();
    for (;;) cpu_hlt();
}

void reboot_now(void) {
    kprintf("[panic] rebooting via ACPI reset register...\n");
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    for (volatile int i = 0; i < 1000000; i++) { }
    /* Fallback: 8042 keyboard controller pulse reset line. */
    while (inb(0x64) & 0x02) { }
    outb(0x64, 0xFE);
    halt_forever();
}

static void vpanic(const char *file, int line, const char *fmt, va_list ap) {
    cpu_cli();
    g_panicking++;
    if (g_panicking > 2) halt_forever();

    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  MCubeAIOS KERNEL PANIC\n");
    if (file) kprintf("  at %s:%d\n", file, line);
    kprintf("  ");
    char msg[512];
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    kprintf("%s\n", msg);
    kprintf("  cr2=%p cr3=%p rflags=%p\n",
            (void *)read_cr2(), (void *)read_cr3(), (void *)(uptr)read_rflags);
    kprintf("------------------------------------------------------------\n");
    stack_trace();
    kprintf("============================================================\n");
    kprintf("system halted. press Ctrl+Alt+Del or reset the machine.\n");
    halt_forever();
}

NORETURN void panic(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vpanic(NULL, 0, fmt, ap);
    va_end(ap);
    halt_forever();
}

NORETURN void panic_at(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vpanic(file, line, fmt, ap);
    va_end(ap);
    halt_forever();
}
