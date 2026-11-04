/* Panic / oops path: dump state, walk the frame pointer chain, halt. */

#include <mcube/kernel.h>
#include <mcube/io.h>
#include <mcube/acpi.h>
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
    kprintf("[power] reboot requested\n");
    /* Platform reset through the FADT reset register first; the chipset and
     * 8042 fallbacks live inside acpi_reboot(). */
    acpi_reboot();
    halt_forever();
}

/* ACPI S5 (soft off). The control block, SLP_TYP and the ACPI enable handshake
 * all come from the firmware tables; the legacy emulator ports are only tried
 * as a last resort for machines that expose no FADT. */
void power_off(void) {
    kprintf("[power] shutdown requested\n");
    acpi_shutdown();

    /* No FADT, or the request was ignored: try the ports QEMU and Bochs use. */
    outw(0x604, 0x2000);    /* QEMU PIIX4:  SLP_EN | SLP_TYP(5) */
    outw(0xB004, 0x2000);   /* QEMU ICH9 (q35) */
    outw(0x4004, 0x3400);   /* Bochs / VirtualBox */
    for (volatile int i = 0; i < 1000000; i++) { }
    kprintf("[power] S5 request sent, halting as a fallback\n");
    halt_forever();
}

static void vpanic(const char *file, int line, const char *fmt, va_list ap) {
    cpu_cli();
    g_panicking++;
    if (g_panicking > 2) halt_forever();

    /* A crash after the boot splash (or after user mode mapped the
     * framebuffer) would otherwise freeze the last painted frame on screen,
     * which is indistinguishable from "boot stuck at 100%". Bring the text
     * console back so the reason is visible on the VM display too. */
    fb_console_enable();

    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  MCubeAIOS KERNEL PANIC\n");
    if (file) kprintf("  at %s:%d\n", file, line);
    kprintf("  ");
    char msg[512];
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    kprintf("%s\n", msg);
    kprintf("  cr2=%p cr3=%p rflags=%p\n",
            (void *)read_cr2(), (void *)read_cr3(), (void *)(uptr)read_rflags());
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
