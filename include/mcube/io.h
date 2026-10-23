#pragma once
/* x86_64 port I/O, MSR and CPU control primitives. */

#include <mcube/types.h>

static inline void outb(u16 port, u8 val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(u16 port, u16 val) {
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outl(u16 port, u32 val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline u16 inw(u16 port) {
    u16 v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline u32 inl(u16 port) {
    u32 v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void cpu_cli(void) { __asm__ __volatile__("cli" ::: "memory"); }
static inline void cpu_sti(void) { __asm__ __volatile__("sti" ::: "memory"); }
static inline void cpu_hlt(void) { __asm__ __volatile__("hlt" ::: "memory"); }

static inline u64 read_cr0(void) { u64 v; __asm__ __volatile__("mov %%cr0, %0" : "=r"(v)); return v; }
static inline u64 read_cr2(void) { u64 v; __asm__ __volatile__("mov %%cr2, %0" : "=r"(v)); return v; }
static inline u64 read_cr3(void) { u64 v; __asm__ __volatile__("mov %%cr3, %0" : "=r"(v)); return v; }
static inline u64 read_cr4(void) { u64 v; __asm__ __volatile__("mov %%cr4, %0" : "=r"(v)); return v; }

static inline void write_cr3(u64 v) { __asm__ __volatile__("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline void write_cr4(u64 v) { __asm__ __volatile__("mov %0, %%cr4" : : "r"(v) : "memory"); }

static inline u64 read_msr(u32 msr) {
    u32 lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static inline void write_msr(u32 msr, u64 val) {
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"((u32)val), "d"((u32)(val >> 32)));
}

#define MSR_EFER 0xC0000080u
#define MSR_STAR 0xC0000081u
#define MSR_LSTAR 0xC0000082u
#define MSR_FMASK 0xC0000084u
#define MSR_FS_BASE 0xC0000100u
#define MSR_GS_BASE 0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u

static inline u64 irq_save(void) {
    u64 flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(u64 flags) {
    __asm__ __volatile__("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}
