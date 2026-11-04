/* MCubeAIOS kernel entry.
 *
 * At this point the loader has:
 *   - identity mapped low physical memory (64 GiB, 2 MiB pages)
 *   - built the direct map at MCUBE_DIRECT_MAP_BASE (physical 0, 64 GiB)
 *   - mapped the higher-half kernel window at MCUBE_KERNEL_VBASE
 *   - exited boot services
 *   - loaded the kernel ELF at physical 2 MiB, executing in the top 2 GiB
 * and RDI points at a validated Boot Info Block (physical address).
 * Entry translates that pointer through the direct map immediately so the
 * rest of the kernel never has to care.
 */

#include <mcube/kernel.h>
#include <mcube/io.h>
#include <mcube/mm.h>
#include <mcube/timer.h>
#include <mcube/sched.h>
#include <mcube/percpu.h>
#include <mcube/syscall.h>
#include <mcube/user.h>
#include <mcube/block.h>
#include <mcube/vfs.h>
#include <mcube/tty.h>
#include <mcube/net.h>
#include <mcube/hwinfo.h>
#include <mcube/usb.h>
#include <mcube/acpi.h>

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
    kprintf("  kernel [%p, %p)  entry %p\n",
            (void *)(uptr)(MCUBE_KERNEL_VBASE + bi->kernel_base),
            (void *)(uptr)(MCUBE_KERNEL_VBASE + bi->kernel_base + bi->kernel_size),
            (void *)bi->kernel_entry);
    kprintf("  rsdp %p   pml4 %p\n",
            phys_to_virt(bi->rsdp), (void *)bi->pml4_phys);
}

static void dump_memory_map(const boot_info_t *bi) {
    boot_mem_entry_t *map = (boot_mem_entry_t *)phys_to_virt(bi->memmap_addr);
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

static u64 usable_bytes(const boot_info_t *bi) {
    boot_mem_entry_t *map = (boot_mem_entry_t *)phys_to_virt(bi->memmap_addr);
    u64 usable = 0;
    for (u32 i = 0; i < bi->memmap_count; i++) {
        if (map[i].type == BOOT_MEM_USABLE) usable += map[i].length;
    }
    return usable;
}

static volatile int g_soft_timer_fired;
static void soft_timer_cb(void *arg) {
    *(volatile int *)arg = 1;
}

static void sched_demo(void *arg) {
    const char *name = (const char *)arg;
    for (u32 i = 0; i < 4; i++) {
        kprintf("[sched] %s: iteration %u\n", name, i);
        task_sleep_ms(8);
    }
    kprintf("[sched] %s: exiting\n", name);
}

static void sched_monitor(void *arg) {
    UNUSED(arg);
    task_sleep_ms(180);
    kprintf("[sched] demo threads finished, task count %u\n", task_count());
    sched_dump();
    kprintf("SCHED_TEST_OK\n");
    for (;;) task_sleep_ms(1000);
}

void kernel_main(boot_info_t *bi) {
    /* The loader hands us a physical pointer. Move it into the direct map so
     * every later dereference is a normal kernel-virtual access. */
    bi = (boot_info_t *)phys_to_virt((u64)(uptr)bi);

    serial_init();
    console_init(bi);

    if (!bootinfo_valid(bi)) {
        panic("invalid Boot Info Block at %p", (void *)bi);
    }

    banner(bi);
    acpi_init(bi);
    acpi_print_summary();
    dump_memory_map(bi);
    hw_info_init(bi);
    hw_info_print_summary();

    kprintf("[cpu] loading GDT + TSS...\n");
    gdt_init();
    percpu_init();
    kprintf("[cpu] loading IDT (256 vectors) and remapping PIC...\n");
    idt_init();
    timer_init();

    kprintf("[mm] initialising physical allocator...\n");
    pmm_init(bi);
    vmm_init(bi);
    kmalloc_init();
    pmm_dump_stats();
    if (mm_selftest() != 0) {
        panic("memory self test failed");
    }
    kmalloc_dump_stats();

    syscall_init();
    block_init();
    block_dump();
    tty_init();
    input_init();
    if (vfs_init(bi) == 0) {
        vfs_selftest();
    } else {
        kprintf("[vfs] filesystem self test skipped (root not mounted)\n");
    }
    /* Timer acceptance, before anything that sleeps.
     *
     * The window is measured with the polled PIT channel-2 counter rather than
     * by halting and hoping IRQ0 wakes us: on firmware that leaves the 8259
     * disconnected that halt never returns and the boot wedges right here.
     * timer_selftest() also brings up the local APIC timer when no PIT tick
     * shows up, so every later wait in the boot has a live time base.
     * Failures are reported, not fatal. */
    g_soft_timer_fired = 0;
    timer_add(20, 0, soft_timer_cb, (void *)&g_soft_timer_fired);
    cpu_sti();
    u32 elapsed = timer_selftest(50);
    kprintf("[time] uptime %u ticks in the 50 ms window, wall clock epoch %u\n",
            elapsed, (u32)timer_boot_time_unix());
    if (!g_soft_timer_fired) {
        kprintf("[time] warning: soft timer did not fire\n");
    } else {
        kprintf("[time] soft timer fired, tick source OK\n");
    }

    /* The network self test waits for ARP/DNS replies with timer_sleep_ms(),
     * so it runs only once the tick source above has been accepted. */
    if (net_init() == 0) {
        net_selftest();
    } else {
        kprintf("[net] network self test skipped (no device)\n");
    }

    /* USB: probe the host controllers here (a handful of register reads) and
     * leave the port-by-port enumeration to the usb-enum thread below, so a
     * wedged port can never stall the boot. */
    kprintf("[boot] usb: probing host controllers\n");
    usb_init();
    kprintf("[boot] usb: controllers probed\n");

    ai_subsys_init();

    kprintf("[init] kernel %s (%s) boot complete, %u MiB usable\n",
            MCUBE_VERSION, MCUBE_CODENAME, (u32)(usable_bytes(bi) / MiB));
    kprintf("\nMCUBE_BOOT_OK\n");

    /* Show the hacker boot sequence while the serial console keeps logging.
     * The kernel console stops painting here; user space owns the screen from
     * the moment the desktop maps it. */
    kprintf("[boot] splash: painting\n");
    fb_boot_splash((u32)(usable_bytes(bi) / MiB));
    kprintf("[boot] splash: done, starting the scheduler\n");

    /* Hand the CPU to the round-robin scheduler. kernel_main's stack becomes
     * the abandoned boot context; all further work runs in kernel threads. */
    sched_init();
    task_create("alpha", sched_demo, (void *)"alpha");
    task_create("beta", sched_demo, (void *)"beta");
    task_create("gamma", sched_demo, (void *)"gamma");
    task_create("monitor", sched_monitor, NULL);
    /* Enumeration sleeps on the PIT and can be slow on a 20-port xHCI: it runs
     * as a normal kernel thread so boot never waits for it. */
    task_create("usb-enum", usb_enum_task, NULL);
    kprintf("[sched] tasks created, spawning user init\n");
    if (user_spawn_init() != 0) {
        panic("cannot start user init");
    }
    kprintf("[sched] starting kernel threads (RR, %u ms quantum)\n",
            SCHED_QUANTUM_TICKS);
    sched_start();
}
