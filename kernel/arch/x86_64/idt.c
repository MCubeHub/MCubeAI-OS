/* IDT + legacy 8259 PIC bootstrap.
 *
 * M2 replaces the PIC with the local/IO APIC, but keeping the remapped PIC
 * alive early gives us timer and keyboard IRQs before the APIC is probed. */

#include <mcube/kernel.h>
#include <mcube/io.h>

#define IDT_ENTRIES 256
#define IRQ_BASE 0x20

struct PACKED idt_entry {
    u16 off_low;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 off_mid;
    u32 off_high;
    u32 zero;
};

struct PACKED idt_ptr {
    u16 limit;
    u64 base;
};

typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 vector, error;
    u64 rip, cs, rflags, rsp, ss;
} isr_frame_t;

static struct idt_entry g_idt[IDT_ENTRIES];
static struct idt_ptr g_idt_ptr;

extern void idt_load(const struct idt_ptr *ptr);
extern void isr_default(void);

#define DECL_ISR(n) extern void isr##n(void);
DECL_ISR(0)  DECL_ISR(1)  DECL_ISR(2)  DECL_ISR(3)  DECL_ISR(4)  DECL_ISR(5)
DECL_ISR(6)  DECL_ISR(7)  DECL_ISR(8)  DECL_ISR(9)  DECL_ISR(10) DECL_ISR(11)
DECL_ISR(12) DECL_ISR(13) DECL_ISR(14) DECL_ISR(15) DECL_ISR(16) DECL_ISR(17)
DECL_ISR(18) DECL_ISR(19) DECL_ISR(20) DECL_ISR(21) DECL_ISR(22) DECL_ISR(23)
DECL_ISR(24) DECL_ISR(25) DECL_ISR(26) DECL_ISR(27) DECL_ISR(28) DECL_ISR(29)
DECL_ISR(30) DECL_ISR(31)

void (*g_isr_table[32])(void) = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
    isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

static const char *const g_exception_names[32] = {
    "#DE divide error", "#DB debug", "NMI", "#BP breakpoint",
    "#OF overflow", "#BR bound range", "#UD invalid opcode", "#NM device not available",
    "#DF double fault", "coprocessor segment overrun", "#TS invalid TSS", "#NP segment not present",
    "#SS stack fault", "#GP general protection", "#PF page fault", "reserved (15)",
    "#MF x87 float error", "#AC alignment check", "#MC machine check", "#XM simd float error",
    "#VE virtualization", "#CP control protection", "reserved (22)", "reserved (23)",
    "reserved (24)", "reserved (25)", "reserved (26)", "reserved (27)",
    "#HV hypervisor injection", "#VC vmm communication", "#SX security", "reserved (31)",
};

static void set_gate(u8 vector, void (*handler)(void), u8 dpl, u8 ist) {
    u64 addr = (u64)(uptr)handler;
    g_idt[vector].off_low = (u16)(addr & 0xFFFF);
    g_idt[vector].selector = 0x08;
    g_idt[vector].ist = ist;
    g_idt[vector].type_attr = (u8)(0x8E | (dpl << 5));
    g_idt[vector].off_mid = (u16)((addr >> 16) & 0xFFFF);
    g_idt[vector].off_high = (u32)(addr >> 32);
    g_idt[vector].zero = 0;
}

static void pic_remap(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, IRQ_BASE); io_wait();
    outb(0xA1, IRQ_BASE + 8); io_wait();
    outb(0x21, 0x04); io_wait(); /* slave on IRQ2 */
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    outb(0x21, 0xFB); /* keep cascade only */
}

struct irq_entry {
    void (*fn)(void *frame, void *ctx);
    void *ctx;
};

static struct irq_entry g_irq_handlers[16];

void irq_unmask(u8 irq) {
    u16 port = irq < 8 ? 0x21 : 0xA1;
    u8 bit = (u8)(irq & 7);
    outb(port, (u8)(inb(port) & ~(1u << bit)));
}

void irq_mask(u8 irq) {
    u16 port = irq < 8 ? 0x21 : 0xA1;
    u8 bit = (u8)(irq & 7);
    outb(port, (u8)(inb(port) | (1u << bit)));
}

void irq_register(u8 vector, void (*handler)(void *frame, void *ctx), void *ctx) {
    if (vector < 32 || vector >= 48) return;
    g_irq_handlers[vector - 32].fn = handler;
    g_irq_handlers[vector - 32].ctx = ctx;
}

void idt_init(void) {
    memset(g_idt, 0, sizeof(g_idt));
    for (int i = 0; i < 32; i++) set_gate((u8)i, g_isr_table[i], 0, 0);
    for (int i = 32; i < IDT_ENTRIES; i++) set_gate((u8)i, isr_default, 0, 0);

    g_idt_ptr.limit = sizeof(g_idt) - 1;
    g_idt_ptr.base = (u64)(uptr)&g_idt;
    idt_load(&g_idt_ptr);

    pic_remap();
}

/* Called from isr_common (isr.asm). Never returns for CPU exceptions. */
void isr_handler(isr_frame_t *f) {
    if (f->vector < 32) {
        const char *name = g_exception_names[f->vector];
        kprintf("\n!!! CPU exception %u: %s\n", (u32)f->vector, name);
        kprintf("    rip=%p cs=%x rflags=%p error=%p\n",
                (void *)f->rip, (u32)f->cs, (void *)f->rflags, (void *)f->error);
        kprintf("    rsp=%p rax=%p rbx=%p rcx=%p rdx=%p\n",
                (void *)f->rsp, (void *)f->rax, (void *)f->rbx,
                (void *)f->rcx, (void *)f->rdx);
        if (f->vector == 14) {
            u64 cr2 = read_cr2();
            kprintf("    page fault @ %p: %s%s%s%s%s\n", (void *)cr2,
                    (f->error & 1) ? "present " : "not-present ",
                    (f->error & 2) ? "write " : "read ",
                    (f->error & 4) ? "user " : "kernel ",
                    (f->error & 8) ? "reserved-bit " : "",
                    (f->error & 16) ? "instruction-fetch" : "");
        }
        panic("unhandled CPU exception");
    }

    if (f->vector == 0xFF) {
        kprintf("[irq] spurious interrupt\n");
        return;
    }

    if (f->vector >= IRQ_BASE && f->vector < IRQ_BASE + 16) {
        u32 irq = (u32)f->vector - IRQ_BASE;
        if (g_irq_handlers[irq].fn) {
            g_irq_handlers[irq].fn(f, g_irq_handlers[irq].ctx);
        }
        if (irq >= 8) outb(0xA0, 0x20);
        outb(0x20, 0x20);
        return;
    }

    kprintf("[irq] unexpected vector %u\n", (u32)f->vector);
}
