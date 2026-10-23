#pragma once
/* Kernel-wide core interfaces. */

#include <mcube/types.h>
#include <mcube/bootinfo.h>

/* lib/string.c */
void *memcpy(void *dst, const void *src, usize n);
void *memmove(void *dst, const void *src, usize n);
void *memset(void *dst, int c, usize n);
int memcmp(const void *a, const void *b, usize n);
usize strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, usize n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, usize n);

/* lib/kprintf.c */
int kvsnprintf(char *buf, usize size, const char *fmt, __builtin_va_list ap);
int ksnprintf(char *buf, usize size, const char *fmt, ...);
int kprintf(const char *fmt, ...);
void kputs(const char *s);
void kputc(char c);

/* drivers/serial.c */
void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

/* drivers/fb.c */
struct console;
void fb_init(const boot_info_t *bi);
void fb_clear(void);
void fb_putc(char c);
void fb_write(const char *s);

/* console multiplexer (serial + framebuffer) */
void console_init(const boot_info_t *bi);
void console_clear(void);

/* drivers/keyboard.c (implemented in kernel/tty/tty.c) */
void keyboard_init(void);

/* panic.c */
NORETURN void panic(const char *fmt, ...);
NORETURN void panic_at(const char *file, int line, const char *fmt, ...);
void stack_trace(void);
u64 kmalloc_used_bytes(void);

#define BUG_ON(cond, msg) do { if (cond) panic_at(__FILE__, __LINE__, "%s", msg); } while (0)
#define PANIC(fmt, ...) panic_at(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* arch/x86_64/gdt.c */
void gdt_init(void);
void tss_set_rsp0(u64 rsp0);

/* arch/x86_64/idt.c */
void idt_init(void);
void irq_register(u8 vector, void (*handler)(void *frame, void *ctx), void *ctx);
void irq_unmask(u8 irq);
void irq_mask(u8 irq);

/* lib/panic? boot helpers */
NORETURN void halt_forever(void);
void reboot_now(void);

/* Time / early timing */
u64 timer_uptime_ms(void);

/* arch/x86_64/cpu.asm */
u64 read_rflags(void);

/* AI-native subsystem hooks (implemented in kernel/ai/) */
void ai_subsys_init(void);
