/* Global Descriptor Table with kernel/user segments and a 64-bit TSS. */

#include <mcube/kernel.h>
#include <mcube/io.h>

#define GDT_ENTRIES 7

struct PACKED gdt_ptr {
    u16 limit;
    u64 base;
};

struct PACKED tss64 {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};

static u64 g_gdt[GDT_ENTRIES];
static struct tss64 g_tss;
static struct gdt_ptr g_gdt_ptr;

extern void gdt_flush(const struct gdt_ptr *ptr);
extern void tss_flush(void);

static u64 make_descriptor(u32 base, u32 limit, u8 access, u8 flags) {
    u64 d = 0;
    d |= (u64)(limit & 0xFFFF);
    d |= (u64)(base & 0xFFFFFF) << 16;
    d |= (u64)access << 40;
    d |= (u64)((limit >> 16) & 0xF) << 48;
    d |= (u64)(flags & 0xF) << 52;
    d |= (u64)((base >> 24) & 0xFF) << 56;
    return d;
}

static u64 make_tss_descriptor(u64 base, u32 limit) {
    u64 low = 0;
    low |= (u64)(limit & 0xFFFF);
    low |= (u64)(base & 0xFFFFFF) << 16;
    low |= (u64)0x89 << 40;                     /* present, type = available TSS */
    low |= (u64)((limit >> 16) & 0xF) << 48;
    low |= (u64)((base >> 24) & 0xFF) << 56;
    return low; /* caller stores base>>32 as the second half */
}

void tss_set_rsp0(u64 rsp0) {
    g_tss.rsp0 = rsp0;
}

void gdt_init(void) {
    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.iomap_base = sizeof(g_tss);

    g_gdt[0] = 0;
    /* 0x08 kernel code: present, DPL0, S=1, exec/read, L=1 */
    g_gdt[1] = make_descriptor(0, 0xFFFFF, 0x9A, 0xA);
    /* 0x10 kernel data */
    g_gdt[2] = make_descriptor(0, 0xFFFFF, 0x92, 0xC);
    /* 0x18 user code: DPL3 */
    g_gdt[3] = make_descriptor(0, 0xFFFFF, 0xFA, 0xA);
    /* 0x20 user data: DPL3 */
    g_gdt[4] = make_descriptor(0, 0xFFFFF, 0xF2, 0xC);
    /* 0x28 TSS occupies entries 5 and 6 */
    g_gdt[5] = make_tss_descriptor((u64)(uptr)&g_tss, sizeof(g_tss) - 1);
    g_gdt[6] = (u64)(uptr)&g_tss >> 32;

    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base = (u64)(uptr)&g_gdt;

    gdt_flush(&g_gdt_ptr);
    tss_flush();
}
