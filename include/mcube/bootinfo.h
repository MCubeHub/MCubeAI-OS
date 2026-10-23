#pragma once
/* Boot Info Block: the ABI between the UEFI loader and the kernel.
 * Everything the kernel needs before it has drivers lives here.
 * Layout is packed and versioned; never reorder existing fields. */

#include <mcube/types.h>

#define BOOTINFO_MAGIC   0x4D4355424F4F5431ULL /* "MCUBOOT1" */
#define BOOTINFO_VERSION 1u
#define BOOTINFO_VERSION_STRING "1"
#define BOOTINFO_CMDLINE_MAX 256

/* Virtual layout shared by loader and kernel. The loader maps:
 *   [0xFFFF_8000_0000_0000, +64 GiB) -> physical 0 (direct map)
 *   [0xFFFF_FFFF_8000_0000, +2 GiB)  -> physical 0 (kernel image window)
 *   low identity map for the trampoline and early page tables.
 */
#define MCUBE_DIRECT_MAP_BASE 0xFFFF800000000000ULL
#define MCUBE_KERNEL_VBASE    0xFFFFFFFF80000000ULL

/* Simplified physical memory map entry types. */
enum boot_mem_type {
    BOOT_MEM_USABLE = 1,   /* free RAM the allocator may use              */
    BOOT_MEM_RESERVED,     /* firmware/loader reserved, never allocate    */
    BOOT_MEM_ACPI_RECLAIM, /* ACPI tables, reusable after parsing         */
    BOOT_MEM_ACPI_NVS,     /* must be preserved                           */
    BOOT_MEM_MMIO,         /* device MMIO, must not be cached             */
    BOOT_MEM_BAD,          /* unusable/poisoned memory                    */
    BOOT_MEM_KERNEL,       /* kernel image + bootinfo + page tables       */
};

/* Framebuffer pixel layouts (values mirror UEFI GOP PixelFormat). */
enum boot_pixel_format {
    BOOT_PIXEL_RGBA8888 = 0,
    BOOT_PIXEL_BGRA8888 = 1,
    BOOT_PIXEL_BITMASK  = 2,
    BOOT_PIXEL_BLT_ONLY = 3,
};

typedef struct PACKED boot_mem_entry {
    u64 base;          /* physical start, page aligned */
    u64 length;        /* bytes */
    u32 type;          /* enum boot_mem_type */
    u32 reserved;
} boot_mem_entry_t;

typedef struct PACKED boot_info {
    u64 magic;
    u32 version;
    u32 size;                  /* sizeof(boot_info_t) */

    /* Physical memory map, copied out of UEFI before ExitBootServices. */
    u64 memmap_addr;           /* virtual == physical (identity mapped) */
    u32 memmap_count;
    u32 memmap_desc_size;      /* sizeof(boot_mem_entry_t) */

    /* Linear framebuffer from UEFI GOP. */
    u64 fb_base;
    u64 fb_size;
    u32 fb_width;
    u32 fb_height;
    u32 fb_pitch;              /* bytes per scanline */
    u32 fb_format;             /* enum boot_pixel_format */
    u32 fb_red_mask;
    u32 fb_green_mask;
    u32 fb_blue_mask;

    /* Firmware tables. */
    u64 rsdp;                  /* ACPI RSDP physical address, 0 if absent */
    u64 smbios;

    /* Loaded images. */
    u64 kernel_base;
    u64 kernel_size;
    u64 kernel_entry;
    u64 initrd_base;
    u64 initrd_size;

    /* Paging created by the loader: identity map of low physical memory. */
    u64 pml4_phys;
    u64 loader_stack_top;

    u32 boot_cpu;              /* CPU that reached kernel_main */
    u32 cpu_count;             /* 0 = unknown until SMP bring-up */
    char cmdline[BOOTINFO_CMDLINE_MAX];
} boot_info_t;

_Static_assert(sizeof(boot_info_t) <= 4096, "boot_info must fit a page");

static inline int bootinfo_valid(const boot_info_t *bi) {
    return bi != NULL && bi->magic == BOOTINFO_MAGIC &&
           bi->version == BOOTINFO_VERSION && bi->size == sizeof(boot_info_t);
}
