/* sysinfo: CPU, firmware, memory and PCI inventory.
 *
 * The data comes from SYS_HWINFO, which the kernel fills once at boot from
 * CPUID, SMBIOS and PCI config space. With a desktop attached the same data
 * is drawn as a window; otherwise it is printed for the shell.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>

#define C_BG     0x080C11
#define C_PANEL  0x111923
#define C_ACCENT 0x22D3EE
#define C_TEXT   0xD7E2F2
#define C_DIM    0x7E8FA6
#define C_OK     0x22C55E

typedef struct {
    u8 word;   /* 0=cpuid.1 ecx, 1=cpuid.1 edx, 2=cpuid.7 ebx, 3=cpuid.7 ecx */
    u8 bit;
    const char *name;
} feature_t;

static const feature_t g_features[] = {
    { 0, 25, "SSE" },     { 0, 26, "SSE2" },    { 1, 0,  "SSE3" },
    { 1, 9,  "SSSE3" },   { 1, 19, "SSE4.1" },  { 1, 20, "SSE4.2" },
    { 1, 25, "AES" },     { 1, 28, "AVX" },     { 1, 12, "FMA" },
    { 1, 23, "POPCNT" },  { 1, 5,  "VMX" },     { 1, 26, "XSAVE" },
    { 2, 5,  "AVX2" },    { 2, 3,  "BMI1" },    { 2, 8,  "BMI2" },
    { 2, 9,  "ERMS" },    { 2, 7,  "SMEP" },    { 2, 18, "RDSEED" },
    { 2, 29, "SHA" },     { 2, 16, "AVX512F" }, { 2, 30, "AVX512BW" },
    { 2, 31, "AVX512VL" },{ 3, 1,  "AVX512VBMI" }, { 3, 9, "VAES" },
    { 3, 3,  "PKU" },     { 3, 7,  "CET-SS" },  { 3, 8,  "GFNI" },
};

static u32 feature_word(const mcube_cpu_info_t *c, u8 word) {
    switch (word) {
    case 0: return c->feat_ecx;
    case 1: return c->feat_edx;
    case 2: return c->feat_ebx7;
    case 3: return c->feat_ecx7;
    default: return 0;
    }
}

static const char *next_feature(const mcube_cpu_info_t *c, int *idx) {
    while (*idx < (int)(sizeof(g_features) / sizeof(g_features[0]))) {
        const feature_t *f = &g_features[*idx];
        (*idx)++;
        if (feature_word(c, f->word) & (1u << f->bit)) return f->name;
    }
    return NULL;
}

static const char *mem_type_name(u8 t) {
    switch (t) {
    case 0x01: return "Other";
    case 0x02: return "Unknown";
    case 0x03: return "DRAM";
    case 0x0F: return "SDRAM";
    case 0x11: return "DDR";
    case 0x12: return "DDR2";
    case 0x13: return "DDR2 FB-DIMM";
    case 0x18: return "DDR3";
    case 0x1A: return "DDR4";
    case 0x1B: return "LPDDR";
    case 0x1C: return "LPDDR2";
    case 0x1D: return "LPDDR3";
    case 0x1E: return "LPDDR4";
    case 0x20: return "HBM";
    case 0x21: return "HBM2";
    case 0x22: return "DDR5";
    case 0x23: return "LPDDR5";
    default: return "RAM";
    }
}

static const char *form_factor_name(u8 f) {
    switch (f) {
    case 0x01: return "Other";
    case 0x02: return "Unknown";
    case 0x03: return "SIMM";
    case 0x04: return "SIP";
    case 0x05: return "Chip";
    case 0x06: return "DIP";
    case 0x07: return "ZIP";
    case 0x08: return "Proprietary";
    case 0x09: return "DIMM";
    case 0x0A: return "TSOP";
    case 0x0B: return "Row of chips";
    case 0x0C: return "RIMM";
    case 0x0D: return "SODIMM";
    case 0x0E: return "SRIMM";
    case 0x0F: return "FB-DIMM";
    default: return "DIMM";
    }
}

static u32 mib(u64 bytes) { return (u32)(bytes / (1024ULL * 1024ULL)); }

/* ---- CLI ------------------------------------------------------------ */

static void cli_cpu(const mcube_hw_info_t *hw) {
    const mcube_cpu_info_t *c = &hw->cpu;
    printf("CPU\n");
    printf("  vendor     %s\n", c->vendor[0] ? c->vendor : "unknown");
    printf("  model      %s\n", c->brand[0] ? c->brand : "unknown");
    printf("  topology   %u cores / %u threads, family %u model %u stepping %u\n",
           c->cores, c->logical_cpus, c->family, c->model, c->stepping);
    if (c->tsc_mhz) printf("  clock      %u MHz base\n", c->tsc_mhz);
    if (c->hypervisor[0]) printf("  hypervisor %s\n", c->hypervisor);

    char line[100];
    int len = 0, idx = 0;
    const char *name;
    while ((name = next_feature(c, &idx)) != NULL) {
        int n = (int)strlen(name);
        if (len && len + 1 + n > 76) {
            line[len] = 0;
            printf("  features   %s\n", line);
            len = 0;
        }
        if (len) line[len++] = ' ';
        memcpy(line + len, name, (size_t)n);
        len += n;
    }
    if (len) {
        line[len] = 0;
        printf("  features   %s\n", line);
    }
}

static void cli_firmware(const mcube_hw_info_t *hw) {
    printf("Firmware / system\n");
    if (hw->smbios_present) {
        printf("  SMBIOS     %u.%u\n", hw->smbios_major, hw->smbios_minor);
        printf("  BIOS       %s %s %s\n",
               hw->bios_vendor[0] ? hw->bios_vendor : "unknown",
               hw->bios_version, hw->bios_date);
        printf("  system     %s %s %s\n",
               hw->system_vendor[0] ? hw->system_vendor : "unknown",
               hw->system_product, hw->system_version);
        if (hw->board_vendor[0] || hw->board_product[0]) {
            printf("  board      %s %s %s\n", hw->board_vendor,
                   hw->board_product, hw->board_version);
        }
        if (hw->system_serial[0]) {
            printf("  serial     %s\n", hw->system_serial);
        }
    } else {
        printf("  SMBIOS     not provided by firmware\n");
    }
    printf("  boot       %u memory-map entries\n", hw->memmap_entries);
}

static void cli_memory(const mcube_hw_info_t *hw) {
    printf("Memory\n");
    printf("  installed  %u MiB, usable %u MiB\n",
           mib(hw->total_ram_bytes), mib(hw->usable_ram_bytes));
    for (u32 i = 0; i < hw->mem_module_count; i++) {
        const mcube_mem_module_t *m = &hw->mem[i];
        u32 speed = m->configured_speed_mts ? m->configured_speed_mts
                                            : m->speed_mts;
        printf("  %-10s %u MiB %s %s", m->locator[0] ? m->locator : "DIMM",
               mib(m->size_bytes), mem_type_name(m->mem_type),
               form_factor_name(m->form_factor));
        if (speed) printf(" %u MT/s", speed);
        if (m->manufacturer[0]) printf(" %s", m->manufacturer);
        if (m->part[0]) printf(" [%s]", m->part);
        printf("\n");
    }
    if (!hw->mem_module_count) {
        printf("  modules    not described by SMBIOS (total from boot map)\n");
    }
}

static void cli_pci(const mcube_hw_info_t *hw) {
    printf("PCI (%u devices)\n", hw->pci_count);
    u32 shown = hw->pci_count < MCUBE_HW_PCI_DEVICES ? hw->pci_count
                                                     : MCUBE_HW_PCI_DEVICES;
    for (u32 i = 0; i < shown; i++) {
        const mcube_pci_device_info_t *d = &hw->pci[i];
        printf("  %02x:%02x.%u  %04x:%04x  %-10s %s\n",
               d->bus, d->slot, d->func, d->vendor, d->device,
               d->vendor_name[0] ? d->vendor_name : "unknown",
               d->class_name);
    }
    if (hw->pci_count > shown) {
        printf("  ... %u more not recorded\n", hw->pci_count - shown);
    }
}

static int cli_main(const mcube_hw_info_t *hw, int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "all";

    printf("\nMCubeAIOS hardware inventory (ABI v%u)\n", hw->version);
    printf("==================================================\n");
    if (!strcmp(what, "all")) {
        cli_cpu(hw);
        printf("\n");
        cli_firmware(hw);
        printf("\n");
        cli_memory(hw);
        printf("\n");
        cli_pci(hw);
    } else if (!strcmp(what, "cpu")) {
        cli_cpu(hw);
    } else if (!strcmp(what, "mem") || !strcmp(what, "memory")) {
        cli_memory(hw);
    } else if (!strcmp(what, "pci")) {
        cli_pci(hw);
    } else if (!strcmp(what, "fw") || !strcmp(what, "firmware")) {
        cli_firmware(hw);
    } else {
        printf("usage: sysinfo [cpu|mem|pci|firmware]\n");
        return 2;
    }
    return 0;
}

/* ---- GUI ------------------------------------------------------------ */

static int draw_section(int y, const char *title) {
    gui_fill(12, y + 6, 3, 12, C_ACCENT);
    gui_text(20, y, C_ACCENT, title);
    return y + 20;
}

static int draw_features(int x, int y, const mcube_cpu_info_t *c, int width) {
    char line[128];
    int len = 0, idx = 0;
    const char *name;
    while ((name = next_feature(c, &idx)) != NULL) {
        int n = (int)strlen(name);
        int max = width / 8;
        if (len && len + 1 + n > max) {
            line[len] = 0;
            gui_text(x, y, C_TEXT, line);
            y += 16;
            len = 0;
        }
        if (len) line[len++] = ' ';
        memcpy(line + len, name, (size_t)n);
        len += n;
    }
    if (len) {
        line[len] = 0;
        gui_text(x, y, C_TEXT, line);
        y += 16;
    }
    return y;
}

static void gui_draw(const mcube_hw_info_t *hw) {
    int w = gui_width();
    int y = 34;

    gui_clear(C_BG);
    gui_fill(0, 0, w, 26, C_PANEL);
    gui_text(10, 5, C_ACCENT, "System Information");
    char badge[80];
    snprintf(badge, sizeof(badge), "%s %s", hw->system_vendor, hw->system_product);
    int bl = (int)strlen(badge) * 8;
    if (bl < w - 120) gui_text(w - bl - 12, 5, C_DIM, badge);

    /* Firmware */
    y = draw_section(y, "FIRMWARE");
    char line[160];
    if (hw->smbios_present) {
        snprintf(line, sizeof(line), "  BIOS    %s %s  (%s)",
                 hw->bios_vendor, hw->bios_version, hw->bios_date);
        gui_text(12, y, C_TEXT, line); y += 16;
        snprintf(line, sizeof(line), "  SMBIOS  %u.%u    board %s %s",
                 hw->smbios_major, hw->smbios_minor,
                 hw->board_vendor, hw->board_product);
        gui_text(12, y, C_TEXT, line); y += 16;
    } else {
        gui_text(12, y, C_DIM, "  SMBIOS not provided by firmware"); y += 16;
    }
    y += 8;

    /* CPU */
    y = draw_section(y, "PROCESSOR");
    const mcube_cpu_info_t *c = &hw->cpu;
    snprintf(line, sizeof(line), "  %s  [%s]",
             c->brand[0] ? c->brand : "unknown",
             c->vendor[0] ? c->vendor : "unknown");
    gui_text(12, y, C_TEXT, line); y += 16;
    snprintf(line, sizeof(line),
             "  %u cores / %u threads  family %u model %u stepping %u",
             c->cores, c->logical_cpus, c->family, c->model, c->stepping);
    gui_text(12, y, C_TEXT, line); y += 16;
    if (c->tsc_mhz) {
        snprintf(line, sizeof(line), "  %u MHz base clock%s%s",
                 c->tsc_mhz,
                 c->hypervisor[0] ? "   hypervisor " : "",
                 c->hypervisor);
        gui_text(12, y, C_TEXT, line); y += 16;
    }
    y = draw_features(12, y, c, w - 30) + 8;

    /* Memory */
    y = draw_section(y, "MEMORY");
    snprintf(line, sizeof(line), "  %u MiB installed, %u MiB usable by the kernel",
             mib(hw->total_ram_bytes), mib(hw->usable_ram_bytes));
    gui_text(12, y, C_TEXT, line); y += 16;
    for (u32 i = 0; i < hw->mem_module_count && i < 4; i++) {
        const mcube_mem_module_t *m = &hw->mem[i];
        u32 speed = m->configured_speed_mts ? m->configured_speed_mts
                                            : m->speed_mts;
        snprintf(line, sizeof(line), "  %-8s %u MiB %s %s%s%u MT/s",
                 m->locator[0] ? m->locator : "DIMM", mib(m->size_bytes),
                 mem_type_name(m->mem_type), form_factor_name(m->form_factor),
                 speed ? " " : "", speed);
        gui_text(12, y, C_DIM, line); y += 16;
    }
    y += 8;

    /* PCI, two columns when the window is wide enough */
    y = draw_section(y, "PCI DEVICES");
    u32 shown = hw->pci_count < MCUBE_HW_PCI_DEVICES ? hw->pci_count
                                                     : MCUBE_HW_PCI_DEVICES;
    int cols = w >= 700 ? 2 : 1;
    int per_col = cols == 2 ? 8 : (int)shown;
    if (per_col < 1) per_col = 1;
    for (u32 i = 0; i < shown; i++) {
        int col = (int)i / per_col;
        int row = (int)i % per_col;
        if (col >= cols) break;
        const mcube_pci_device_info_t *d = &hw->pci[i];
        snprintf(line, sizeof(line), "  %02x:%02x.%u %04x:%04x %s %s",
                 d->bus, d->slot, d->func, d->vendor, d->device,
                 d->vendor_name[0] ? d->vendor_name : "",
                 d->class_name);
        gui_text(12 + col * ((w - 24) / cols), y + row * 16, C_TEXT, line);
    }
    if (hw->pci_count > shown) {
        snprintf(line, sizeof(line), "  ... %u more devices not recorded",
                 hw->pci_count - shown);
        gui_text(12, y + per_col * 16, C_DIM, line);
    }

    gui_text(12, gui_height() - 22, C_DIM, "F5 refresh    Esc close");
    gui_flush();
}

static int gui_main(void) {
    mcube_hw_info_t hw;
    if (hwinfo(&hw) != 0) return 1;

    if (gui_init("System Information", 780, 560) != 0) return -1;
    gui_draw(&hw);

    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 500);
        if (rc < 0) break;
        if (rc == 0) continue;
        if (ev.type == GUI_EVT_CLOSE) break;
        if (ev.type == GUI_EVT_RESIZE) {
            gui_draw(&hw);
            continue;
        }
        if (ev.type == GUI_EVT_KEY) {
            if (ev.code == 27) break;
            if (ev.code == INPUT_KEY_F5) {
                if (hwinfo(&hw) == 0) gui_draw(&hw);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    mcube_hw_info_t hw;
    if (hwinfo(&hw) != 0) {
        printf("sysinfo: cannot read hardware inventory\n");
        return 1;
    }
    int rc = gui_main();
    if (rc >= 0) return rc;
    return cli_main(&hw, argc, argv);
}
