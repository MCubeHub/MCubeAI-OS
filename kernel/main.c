/* MCubeAIOS kernel entry (M1).
 *
 * At this point the loader has:
 *   - identity mapped low physical memory (64 GiB, 2 MiB pages)
 *   - exited boot services
 *   - loaded the kernel ELF at 2 MiB
 * and RDI points at a validated Boot Info Block.
 */

#include <mcube/kernel.h>
#include <mcube/io.h>
#include <mcube/mm.h>

#define MCUBE_VERSION "0.1.0"
#define MCUBE_CODENAME "Genesis"

extern char __bss_start[], __bss_end[], __kernel_end[];

static const char *mem_type_name(u32 t) {
    switch (t) {
    case BOOT_MEM_USABLE: return "usable";
    case BOOT_MEM_RESERVED: return "reserved";
    case BOOT_MEM_ACPI_RECLAIM: return "acpi-reclaim";
    case BOOT_MEM_ACPI_NVS: return "acpi-nvs";
    case BOOT_MEM_MMIO: return "mmio";
    case BOOT_MEM_BAD: return "bad";
    case BOOT_MEM_KERNEL: return "kernel";
    default: return "?";
    }
}

static void banner(const boot_info_t *bi) {
    kprintf("\n");
    kprintf("  __  __  ____        _         _    ___ ____  \n");
    kprintf(" |  \\/  |/ ___| _   _| |__   ___| |  |_ _/ ___| \n");
    kprintf(" | |\\/| | |   | | | | '_ \\ / _ \\ |   | |\\___ \\ \n");
    kprintf(" | |  | | |___| |_| | |_) |  __/ |___| | ___) |\n");
    kprintf(" |_|  |_|\\____|\\__,_|_.__/ \\___|_____|___|____/ \n");
    kprintf("\n");
    kprintf("  MCubeAIOS %s (%s)  --  an AI-native operating system\n", MCUBE_VERSION, MCUBE_CODENAME);
    kprintf("  ------------------------------------------------------------\n");
    kprintf("  boot cpu %u   fb %ux%u   cmdline: %s\n",
            bi->boot_cpu, bi->fb_width, bi->fb_height, bi->cmdline);
    kprintf("  kernel [%p, %p)  entry %p\n", (void *)bi->kernel_base,
            (void *)(bi->kernel_base + bi->kernel_size), (void *)bi->kernel_entry);
    kprintf("  rsdp %p   pml4 %p\n", (void *)bi->rsdp, (void *)bi->pml4_phys);
}

static void dump_memory_map(const boot_info_t *bi) {
    boot_mem_entry_t *map = (boot_mem_entry_t *)(uptr)bi->memmap_addr;
    u64 usable = 0, reserved = 0, total = 0;

    for (u32 i = 0; i < bi->memmap_count; i++) {
        total += map[i].length;
        switch (map[i].type) {
        case BOOT_MEM_USABLE: usable += map[i].length; break;
        case BOOT_MEM_KERNEL:
        case BOOT_MEM_RESERVED:
        case BOOT_MEM_ACPI_NVS:
        case BOOT_MEM_MMIO:
        case BOOT_MEM_BAD: reserved += map[i].length; break;
        default: break;
        }
    }

    kprintf("[mm] physical map: %u entries, total %u MiB, usable %u MiB, reserved %u MiB\n",
            bi->memmap_count, (u32)(total / MiB), (u32)(usable / MiB), (u32)(reserved / MiB));
    u32 show = bi->memmap_count < 12 ? bi->memmap_count : 12;
    for (u32 i = 0; i < show; i++) {
        kprintf("     [%02u] %016p-%016p %-12s %u KiB\n", i,
                (void *)map[i].base, (void *)(map[i].base + map[i].length),
                mem_type_name(map[i].type), (u32)(map[i].length / KiB));
    }
    if (bi->memmap_count > show) {
        kprintf("     ... %u more entries\n", bi->memmap_count - show);
    }
}

static void check_rsdp(const boot_info_t *bi) {
    if (!bi->rsdp) {
        kprintf("[acpi] no RSDP handed over by loader\n");
        return;
    }
    const char *sig = (const char *)(uptr)bi->rsdp;
    if (memcmp(sig, "RSD PTR ", 8) == 0) {
        kprintf("[acpi] RSDP signature valid, revision %u\n", (u32)((const u8 *)(uptr)bi->rsdp)[15]);
    } else {
        kprintf("[acpi] bad RSDP signature at %p\n", (void *)bi->rsdp);
    }
}

static u64 usable_bytes(const boot_info_t *bi) {
    boot_mem_entry_t *map = (boot_mem_entry_t *)(uptr)bi->memmap_addr;
    u64 usable = 0;
    for (u32 i = 0; i < bi->memmap_count; i++) {
        if (map[i].type == BOOT_MEM_USABLE) usable += map[i].length;
    }
    return usable;
}

void kernel_main(boot_info_t *bi) {
    serial_init();
    console_init(bi);

    if (!bootinfo_valid(bi)) {
        panic("invalid Boot Info Block at %p", (void *)bi);
    }

    banner(bi);
    check_rsdp(bi);
    dump_memory_map(bi);

    kprintf("[cpu] loading GDT + TSS...\n");
    gdt_init();
    kprintf("[cpu] loading IDT (256 vectors) and remapping PIC...\n");
    idt_init();

    kprintf("[mm] initialising physical allocator...\n");
    pmm_init(bi);
    vmm_init(bi);
    kmalloc_init();
    pmm_dump_stats();
    if (mm_selftest() != 0) {
        panic("memory self test failed");
    }
    kmalloc_dump_stats();

    ai_subsys_init();

    kprintf("[init] kernel %s (%s) boot complete, %u MiB usable\n",
            MCUBE_VERSION, MCUBE_CODENAME, (u32)(usable_bytes(bi) / MiB));
    kprintf("[init] halting: milestone M2 (virtual memory + scheduler) is next\n");
    kprintf("\nMCUBE_BOOT_OK\n");

    for (;;) {
        cpu_hlt();
    }
}
