#pragma once
/* Hardware inventory ABI.
 *
 * The kernel probes CPUID, SMBIOS and PCI once during boot and keeps the
 * result in a fixed-layout block. User space reads it with SYS_HWINFO, so
 * `sysinfo` and the desktop never talk to firmware tables or PCI config
 * space directly. Strings are always NUL terminated and truncated, never
 * pointer-valued, so the block can be copied to another address space as-is.
 */

#include <mcube/types.h>

#define MCUBE_HWINFO_VERSION 1u

#define MCUBE_CPU_VENDOR_LEN 16
#define MCUBE_CPU_BRAND_LEN  52
#define MCUBE_HW_STR_LEN     48
#define MCUBE_SMBIOS_STR_LEN 40

#define MCUBE_HW_MEM_MODULES 8
#define MCUBE_HW_PCI_DEVICES 16

typedef struct PACKED mcube_cpu_info {
    char vendor[MCUBE_CPU_VENDOR_LEN];   /* CPUID.0: "GenuineIntel" ... */
    char brand[MCUBE_CPU_BRAND_LEN];     /* CPUID.80000002h: model name */
    char hypervisor[16];                 /* CPUID.40000000h, empty if bare metal */
    u32 signature;                       /* CPUID.1 EAX */
    u32 family;
    u32 model;
    u32 stepping;
    u32 logical_cpus;                    /* CPUID.1 EBX[23:16] */
    u32 cores;                           /* SMBIOS type 4 if present */
    u32 max_leaf;
    u32 max_ext_leaf;
    u32 feat_ecx;                        /* CPUID.1 ECX */
    u32 feat_edx;                        /* CPUID.1 EDX */
    u32 feat_ebx7;                       /* CPUID.7.0 EBX */
    u32 feat_ecx7;                       /* CPUID.7.0 ECX */
    u32 tsc_mhz;                         /* 0 when the CPU does not report it */
} mcube_cpu_info_t;

/* SMBIOS type 17 memory device. size_bytes == 0 means an empty slot and is
 * not copied into the inventory; mem_type/form_factor use SMBIOS byte codes
 * so the front end owns the human-readable names. */
typedef struct PACKED mcube_mem_module {
    u64 size_bytes;
    u32 speed_mts;                       /* rated speed, 0 = unknown */
    u32 configured_speed_mts;
    u8  mem_type;                        /* 0x1A DDR4, 0x22 DDR5, ... */
    u8  form_factor;
    char locator[24];                    /* "DIMM 0", "ChannelA-DIMM0" */
    char manufacturer[24];
    char part[32];
} mcube_mem_module_t;

typedef struct PACKED mcube_pci_device_info {
    u16 vendor;
    u16 device;
    u8  bus;
    u8  slot;
    u8  func;
    u8  class_code;
    u8  subclass;
    u8  prog_if;
    u8  irq;
    char vendor_name[16];                /* "", "Intel", "AMD", ... */
    char class_name[24];                 /* "Display", "Network", ... */
} mcube_pci_device_info_t;

typedef struct PACKED mcube_hw_info {
    u32 version;
    u32 size;                            /* sizeof(mcube_hw_info_t) */
    u64 total_ram_bytes;                 /* installed, from SMBIOS if possible */
    u64 usable_ram_bytes;                /* UEFI memory map, BOOT_MEM_USABLE */
    u32 memmap_entries;
    u32 smbios_major;
    u32 smbios_minor;
    u32 smbios_present;

    char bios_vendor[MCUBE_SMBIOS_STR_LEN];
    char bios_version[MCUBE_SMBIOS_STR_LEN];
    char bios_date[16];
    char system_vendor[MCUBE_SMBIOS_STR_LEN];
    char system_product[MCUBE_HW_STR_LEN];
    char system_version[MCUBE_SMBIOS_STR_LEN];
    char system_serial[24];
    char board_vendor[MCUBE_SMBIOS_STR_LEN];
    char board_product[MCUBE_SMBIOS_STR_LEN];
    char board_version[16];

    mcube_cpu_info_t cpu;

    u32 mem_module_count;
    mcube_mem_module_t mem[MCUBE_HW_MEM_MODULES];

    u32 pci_count;
    mcube_pci_device_info_t pci[MCUBE_HW_PCI_DEVICES];
} mcube_hw_info_t;

/* Kernel side. */
struct boot_info;
void hw_info_init(const struct boot_info *bi);
const mcube_hw_info_t *hw_info_get(void);
void hw_info_print_summary(void);
