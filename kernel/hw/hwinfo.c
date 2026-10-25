/* Hardware inventory: CPUID, SMBIOS and PCI probe.
 *
 * Linux keeps this split between arch code (CPUID) and bus code (DMI/PCI); we
 * do the same but produce one immutable block at boot. Nothing here runs after
 * hw_info_init(), so the syscall path is a plain copy_to_user of static data.
 */

#include <mcube/hwinfo.h>
#include <mcube/bootinfo.h>
#include <mcube/kernel.h>
#include <mcube/mm.h>
#include <mcube/pci.h>

static mcube_hw_info_t g_hw;

/* ---- CPUID ---------------------------------------------------------- */

static inline void cpuid(u32 leaf, u32 subleaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ __volatile__("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(subleaf));
}

static void copy_trim(char *dst, usize cap, const char *src) {
    if (cap == 0) return;
    if (!src) src = "";
    while (*src == ' ') src++;
    usize n = strlen(src);
    while (n > 0 && src[n - 1] == ' ') n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void probe_cpu(void) {
    mcube_cpu_info_t *c = &g_hw.cpu;
    u32 a, b, cc, d;

    cpuid(0, 0, &a, &b, &cc, &d);
    c->max_leaf = a;
    char vendor[13];
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &cc, 4);
    vendor[12] = 0;
    copy_trim(c->vendor, sizeof(c->vendor), vendor);

    if (c->max_leaf >= 1) {
        cpuid(1, 0, &a, &b, &cc, &d);
        c->signature = a;
        c->feat_ecx = cc;
        c->feat_edx = d;
        c->logical_cpus = (b >> 16) & 0xFF;
        if (c->logical_cpus == 0) c->logical_cpus = 1;

        u32 base_family = (a >> 8) & 0xF;
        u32 ext_family = (a >> 20) & 0xFF;
        u32 base_model = (a >> 4) & 0xF;
        u32 ext_model = (a >> 16) & 0xF;
        c->family = (base_family == 0xF) ? base_family + ext_family : base_family;
        c->model = base_model;
        if (base_family == 0x6 || base_family == 0xF) {
            c->model |= ext_model << 4;
        }
        c->stepping = a & 0xF;

        if (cc & (1u << 31)) {
            u32 ha, hb, hc, hd;
            cpuid(0x40000000, 0, &ha, &hb, &hc, &hd);
            char hv[13];
            memcpy(hv, &hb, 4);
            memcpy(hv + 4, &hc, 4);
            memcpy(hv + 8, &hd, 4);
            hv[12] = 0;
            copy_trim(c->hypervisor, sizeof(c->hypervisor), hv);
        }
    }

    if (c->max_leaf >= 7) {
        cpuid(7, 0, &a, &b, &cc, &d);
        c->feat_ebx7 = b;
        c->feat_ecx7 = cc;
    }

    u32 ext_max = 0;
    cpuid(0x80000000, 0, &ext_max, &b, &cc, &d);
    c->max_ext_leaf = ext_max;
    if (ext_max >= 0x80000004) {
        char brand[49];
        for (u32 i = 0; i < 3; i++) {
            cpuid(0x80000002u + i, 0, &a, &b, &cc, &d);
            memcpy(brand + i * 16, &a, 4);
            memcpy(brand + i * 16 + 4, &b, 4);
            memcpy(brand + i * 16 + 8, &cc, 4);
            memcpy(brand + i * 16 + 12, &d, 4);
        }
        brand[48] = 0;
        copy_trim(c->brand, sizeof(c->brand), brand);
    }

    u64 tsc_hz = 0;
    if (c->max_leaf >= 0x16) {
        cpuid(0x16, 0, &a, &b, &cc, &d);
        if (a) tsc_hz = (u64)a * 1000000ULL;
    }
    if (!tsc_hz && c->max_leaf >= 0x15) {
        cpuid(0x15, 0, &a, &b, &cc, &d);
        if (a && b && cc) tsc_hz = (u64)cc * b / a;
    }
    c->tsc_mhz = (u32)(tsc_hz / 1000000ULL);

    c->cores = c->logical_cpus;
    g_hw.total_ram_bytes = 0;
}

/* ---- SMBIOS --------------------------------------------------------- */

static u16 rd16(const u8 *p) {
    u16 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static u32 rd32(const u8 *p) {
    u32 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static u64 rd64(const u8 *p) {
    u64 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static const u8 *g_dmi_end;

static const char *dmi_string(const u8 *st, u8 index) {
    if (index == 0) return "";
    const u8 *q = st + st[1];
    if (q >= g_dmi_end) return "";
    for (u32 n = 1; *q && q < g_dmi_end; n++) {
        if (n == index) {
            const u8 *e = q;
            while (e < g_dmi_end && *e) e++;
            return e < g_dmi_end ? (const char *)q : "";
        }
        while (*q && q < g_dmi_end) q++;
        if (q < g_dmi_end) q++;
    }
    return "";
}

static const u8 *dmi_next(const u8 *p, const u8 *end) {
    if (p + p[1] > end) return NULL;
    const u8 *q = p + p[1];
    while (q + 1 < end) {
        if (q[0] == 0 && q[1] == 0) return q + 2;
        q++;
    }
    return NULL;
}

static int smbios_entry_ok(const u8 *ep, u32 len) {
    u8 sum = 0;
    for (u32 i = 0; i < len; i++) sum = (u8)(sum + ep[i]);
    return sum == 0;
}

static int smbios_locate(u64 phys) {
    if (!phys) return -1;
    const u8 *ep = (const u8 *)phys_to_virt(phys);

    const u8 *table = NULL;
    u32 length = 0;
    u8 major = 0, minor = 0;

    if (memcmp(ep, "_SM3_", 5) == 0) {
        u8 len = ep[0x06];
        if (len < 0x18 || !smbios_entry_ok(ep, len)) return -1;
        u64 addr = rd64(ep + 0x10);
        u32 max = rd32(ep + 0x0C);
        if (!addr) return -1;
        if (max == 0 || max > 16u * 1024u * 1024u) max = 16u * 1024u * 1024u;
        table = (const u8 *)phys_to_virt(addr);
        length = max;
        major = ep[0x07];
        minor = ep[0x08];
    } else if (memcmp(ep, "_SM_", 4) == 0) {
        u8 len = ep[0x05];
        if (len < 0x1F || !smbios_entry_ok(ep, len)) return -1;
        u32 addr = rd32(ep + 0x18);
        u32 tlen = rd16(ep + 0x16);
        if (!addr || tlen < 8) return -1;
        if (tlen > 16u * 1024u * 1024u) tlen = 16u * 1024u * 1024u;
        table = (const u8 *)phys_to_virt(addr);
        length = tlen;
        major = ep[0x06];
        minor = ep[0x07];
    } else {
        return -1;
    }

    g_hw.smbios_present = 1;
    g_hw.smbios_major = major;
    g_hw.smbios_minor = minor;

    u64 module_total = 0;
    u64 array_capacity = 0;
    const u8 *p = table;
    const u8 *end = table + length;
    g_dmi_end = end;

    for (u32 guard = 0; guard < 512 && p + 4 <= end; guard++) {
        u8 type = p[0];
        u8 len = p[1];
        if (type == 127) break;
        if (len < 4 || p + len > end) break;

        switch (type) {
        case 0: /* BIOS information */
            copy_trim(g_hw.bios_vendor, sizeof(g_hw.bios_vendor),
                      dmi_string(p, p[0x04]));
            copy_trim(g_hw.bios_version, sizeof(g_hw.bios_version),
                      dmi_string(p, p[0x05]));
            copy_trim(g_hw.bios_date, sizeof(g_hw.bios_date),
                      dmi_string(p, p[0x08]));
            break;

        case 1: /* System information */
            copy_trim(g_hw.system_vendor, sizeof(g_hw.system_vendor),
                      dmi_string(p, p[0x04]));
            copy_trim(g_hw.system_product, sizeof(g_hw.system_product),
                      dmi_string(p, p[0x05]));
            copy_trim(g_hw.system_version, sizeof(g_hw.system_version),
                      dmi_string(p, p[0x06]));
            copy_trim(g_hw.system_serial, sizeof(g_hw.system_serial),
                      dmi_string(p, p[0x07]));
            break;

        case 2: /* Baseboard */
            copy_trim(g_hw.board_vendor, sizeof(g_hw.board_vendor),
                      dmi_string(p, p[0x04]));
            copy_trim(g_hw.board_product, sizeof(g_hw.board_product),
                      dmi_string(p, p[0x05]));
            copy_trim(g_hw.board_version, sizeof(g_hw.board_version),
                      dmi_string(p, p[0x06]));
            break;

        case 4: /* Processor: core counts */
            if (len >= 0x26) {
                u32 cores = p[0x23];
                u32 threads = p[0x25];
                if (len >= 0x30 && p[0x2A]) cores = rd16(p + 0x2A);
                if (cores > 0 && cores <= 4096) g_hw.cpu.cores = cores;
                UNUSED(threads);
            }
            break;

        case 16: /* Physical memory array */
            if (len >= 0x0B) {
                u32 kb = rd32(p + 0x07);
                if (kb != 0x80000000u) array_capacity = (u64)kb * KiB;
            }
            break;

        case 17: { /* Memory device */
            if (len < 0x15) break;
            u16 raw_size = rd16(p + 0x0C);
            u64 bytes = 0;
            if (raw_size == 0) break;             /* empty slot */
            if (raw_size == 0x7FFF && len >= 0x20) {
                u32 ext = rd32(p + 0x1C) & 0x7FFFFFFFu;
                bytes = (u64)ext * MiB;
            } else if (raw_size & 0x8000) {
                bytes = (u64)(raw_size & 0x7FFF) * KiB;
            } else {
                bytes = (u64)raw_size * MiB;
            }
            if (!bytes) break;
            module_total += bytes;

            if (g_hw.mem_module_count >= MCUBE_HW_MEM_MODULES) break;
            mcube_mem_module_t *m = &g_hw.mem[g_hw.mem_module_count];
            memset(m, 0, sizeof(*m));
            m->size_bytes = bytes;
            m->mem_type = p[0x12];
            m->form_factor = p[0x0E];
            m->speed_mts = rd16(p + 0x15);
            if (m->speed_mts == 0xFFFF) m->speed_mts = 0;
            if (len >= 0x22) {
                u16 cfg = rd16(p + 0x20);
                if (cfg != 0 && cfg != 0xFFFF) m->configured_speed_mts = cfg;
            }
            copy_trim(m->locator, sizeof(m->locator), dmi_string(p, p[0x10]));
            copy_trim(m->manufacturer, sizeof(m->manufacturer),
                      dmi_string(p, p[0x17]));
            copy_trim(m->part, sizeof(m->part), dmi_string(p, p[0x1A]));
            g_hw.mem_module_count++;
            break;
        }

        default:
            break;
        }

        const u8 *next = dmi_next(p, end);
        if (!next || next <= p) break;
        p = next;
    }

    if (module_total) {
        g_hw.total_ram_bytes = module_total;
    } else if (array_capacity) {
        g_hw.total_ram_bytes = array_capacity;
    }
    return 0;
}

/* ---- memory map ----------------------------------------------------- */

static void probe_memmap(const boot_info_t *bi) {
    const boot_mem_entry_t *map =
        (const boot_mem_entry_t *)phys_to_virt(bi->memmap_addr);
    u64 usable = 0;
    g_hw.memmap_entries = bi->memmap_count;
    for (u32 i = 0; i < bi->memmap_count; i++) {
        if (map[i].type == BOOT_MEM_USABLE) usable += map[i].length;
    }
    g_hw.usable_ram_bytes = usable;
    if (!g_hw.total_ram_bytes) g_hw.total_ram_bytes = usable;
}

/* ---- PCI ------------------------------------------------------------ */

static const char *pci_vendor_name(u16 vendor) {
    switch (vendor) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA";
    case 0x1234: return "QEMU";
    case 0x1AF4: return "Red Hat";
    case 0x1B36: return "Red Hat";
    case 0x10EC: return "Realtek";
    case 0x14E4: return "Broadcom";
    case 0x15B3: return "Mellanox";
    case 0x1D0F: return "Amazon";
    case 0x1414: return "Microsoft";
    case 0x5143: return "Qualcomm";
    case 0x17CB: return "Qualcomm";
    case 0x14C3: return "MediaTek";
    case 0x1D94: return "Hygon";
    case 0x1D17: return "Zhaoxin";
    case 0x1DB7: return "Phytium";
    case 0x19E5: return "Huawei";
    case 0x0014: return "Loongson";
    default: return "";
    }
}

static const char *pci_class_name(u8 class_code, u8 subclass) {
    switch (class_code) {
    case 0x01:
        if (subclass == 0x01) return "IDE";
        if (subclass == 0x06) return "SATA/AHCI";
        if (subclass == 0x08) return "NVMe";
        return "Storage";
    case 0x02: return "Network";
    case 0x03: return "Display";
    case 0x04: return "Multimedia";
    case 0x05: return "Memory";
    case 0x06:
        if (subclass == 0x00) return "Host bridge";
        if (subclass == 0x01) return "ISA bridge";
        if (subclass == 0x04) return "PCI bridge";
        return "Bridge";
    case 0x07: return "Serial";
    case 0x08: return "System";
    case 0x0C:
        if (subclass == 0x03) return "USB";
        return "Serial bus";
    case 0x0D: return "Wireless";
    case 0x10: return "Encryption";
    case 0x11: return "Signal";
    default: return "Other";
    }
}

static void probe_pci(void) {
    for (u8 bus = 0; bus < 8; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                u32 id = pci_config_read32(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                if (g_hw.pci_count < MCUBE_HW_PCI_DEVICES) {
                    mcube_pci_device_info_t *d =
                        &g_hw.pci[g_hw.pci_count];
                    u32 classreg =
                        pci_config_read32(bus, slot, func, 0x08);
                    u32 irq = pci_config_read32(bus, slot, func, 0x3C);
                    memset(d, 0, sizeof(*d));
                    d->vendor = (u16)(id & 0xFFFF);
                    d->device = (u16)(id >> 16);
                    d->bus = bus;
                    d->slot = slot;
                    d->func = func;
                    d->class_code = (u8)(classreg >> 24);
                    d->subclass = (u8)(classreg >> 16);
                    d->prog_if = (u8)(classreg >> 8);
                    d->irq = (u8)(irq & 0xFF);
                    copy_trim(d->vendor_name, sizeof(d->vendor_name),
                              pci_vendor_name(d->vendor));
                    copy_trim(d->class_name, sizeof(d->class_name),
                              pci_class_name(d->class_code, d->subclass));
                }
                g_hw.pci_count++;
            }
        }
    }
}

/* ---- public --------------------------------------------------------- */

void hw_info_init(const boot_info_t *bi) {
    memset(&g_hw, 0, sizeof(g_hw));
    g_hw.version = MCUBE_HWINFO_VERSION;
    g_hw.size = (u32)sizeof(g_hw);

    probe_cpu();
    probe_memmap(bi);
    if (smbios_locate(bi->smbios) != 0) {
        g_hw.smbios_present = 0;
    }
    if (g_hw.total_ram_bytes < g_hw.usable_ram_bytes) {
        g_hw.total_ram_bytes = g_hw.usable_ram_bytes;
    }
    probe_pci();
}

const mcube_hw_info_t *hw_info_get(void) { return &g_hw; }

void hw_info_print_summary(void) {
    const mcube_cpu_info_t *c = &g_hw.cpu;
    kprintf("[hw] cpu: %s %s (%u cores / %u threads, family %u model %u step %u)\n",
            c->vendor[0] ? c->vendor : "unknown",
            c->brand[0] ? c->brand : "unknown",
            c->cores, c->logical_cpus, c->family, c->model, c->stepping);
    if (c->hypervisor[0]) {
        kprintf("[hw] cpu: running under %s\n", c->hypervisor);
    }
    if (g_hw.smbios_present) {
        kprintf("[hw] firmware: %s %s, SMBIOS %u.%u\n",
                g_hw.bios_vendor[0] ? g_hw.bios_vendor : "unknown",
                g_hw.bios_version[0] ? g_hw.bios_version : "",
                g_hw.smbios_major, g_hw.smbios_minor);
        kprintf("[hw] system: %s %s\n",
                g_hw.system_vendor[0] ? g_hw.system_vendor : "unknown",
                g_hw.system_product);
    }
    kprintf("[hw] ram: %u MiB installed, %u MiB usable, %u module(s)\n",
            (u32)(g_hw.total_ram_bytes / MiB),
            (u32)(g_hw.usable_ram_bytes / MiB), g_hw.mem_module_count);
    for (u32 i = 0; i < g_hw.mem_module_count; i++) {
        const mcube_mem_module_t *m = &g_hw.mem[i];
        kprintf("     [%u] %s %u MiB type 0x%02x %u MT/s\n", i,
                m->locator[0] ? m->locator : "DIMM",
                (u32)(m->size_bytes / MiB), m->mem_type,
                m->configured_speed_mts ? m->configured_speed_mts : m->speed_mts);
    }

    u32 shown = 0;
    for (u32 i = 0; i < g_hw.pci_count && shown < 4; i++) {
        const mcube_pci_device_info_t *d = &g_hw.pci[i];
        if (d->class_code != 0x03 && d->class_code != 0x02 &&
            d->class_code != 0x01 && d->class_code != 0x0C) {
            continue;
        }
        kprintf("[hw] pci: %02x:%02x.%u %04x:%04x %s %s\n", d->bus, d->slot,
                d->func, d->vendor, d->device, d->vendor_name, d->class_name);
        shown++;
    }
    kprintf("[hw] pci: %u device(s) (%u recorded)\n", g_hw.pci_count,
            g_hw.pci_count < MCUBE_HW_PCI_DEVICES ? g_hw.pci_count
                                                   : MCUBE_HW_PCI_DEVICES);
}
