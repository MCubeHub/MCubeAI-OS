/* ACPI fixed-table support.
 *
 * The kernel previously powered the machine off by poking the PM1a control
 * port of three emulators (0x604 / 0xB004 / 0x4004). On real firmware those
 * ports do not exist, the write is silently dropped and the CPU ends in
 * halt_forever() -- the screen freezes and pressing "Shut down" looks like a
 * no-op. This module parses the tables the loader already handed over
 * (RSDP -> RSDT/XSDT -> FADT, plus the DSDT for the \_S5_ sleep types) so the
 * SLP_TYP value and the control block match whatever the firmware actually
 * provides.
 */

#include <mcube/acpi.h>
#include <mcube/kernel.h>
#include <mcube/io.h>
#include <mcube/mm.h>

/* FADT field offsets (ACPI 1.0 .. 6.4, after the 36-byte common header). */
#define FADT_FACS            0x24
#define FADT_DSDT            0x28
#define FADT_SMI_CMD         0x30
#define FADT_ACPI_ENABLE     0x34
#define FADT_ACPI_DISABLE    0x35
#define FADT_PM1A_EVT        0x38
#define FADT_PM1B_EVT        0x3C
#define FADT_PM1A_CNT        0x40
#define FADT_PM1B_CNT        0x44
#define FADT_PM1_CNT_LEN     0x59
#define FADT_CENTURY         0x6C
#define FADT_FLAGS           0x70
#define FADT_RESET_REG       0x74
#define FADT_RESET_VALUE     0x80
#define FADT_X_DSDT          0x8C
#define FADT_X_PM1A_CNT      0xAC
#define FADT_X_PM1B_CNT      0xB8
#define FADT_MIN_LEN         0xB4

#define FADT_FLAG_RESET_REG  (1u << 10)

/* PM1 control register bits. */
#define PM1_SCI_EN           (1u << 0)
#define PM1_SLP_TYP_SHIFT    10
#define PM1_SLP_EN           (1u << 13)

typedef struct {
    int  present;
    u64  rsdp;
    u32  fadt_len;
    u16  pm1a_cnt;
    u16  pm1b_cnt;
    u8   pm1_cnt_len;
    u8   slp_typa;      /* \_S5_ */
    u8   slp_typb;
    u32  smi_cmd;
    u8   acpi_enable;
    u8   reset_space;
    u8   reset_size;
    u8   reset_value;
    u64  reset_addr;
    int  have_reset;
    int  sci_en;
    int  has_battery;   /* DSDT describes a battery pack */
    char oem_id[7];
} acpi_state_t;

static acpi_state_t g_acpi;

static u16 rd16(const u8 *p, u32 off) {
    return (u16)(p[off] | ((u16)p[off + 1] << 8));
}

static u32 rd32(const u8 *p, u32 off) {
    return (u32)p[off] | ((u32)p[off + 1] << 8) | ((u32)p[off + 2] << 16) |
           ((u32)p[off + 3] << 24);
}

static u64 rd64(const u8 *p, u32 off) {
    return (u64)rd32(p, off) | ((u64)rd32(p, off + 4) << 32);
}

/* Decode a Generic Address Structure that names an I/O port. Returns 0 when
 * the structure points somewhere else (memory space, MMIO, ...). */
static int gas_port(const u8 *t, u32 off, u16 *out) {
    u8 space = t[off];
    u64 addr = rd64(t, off + 4);
    if (space != 1 || addr == 0 || addr > 0xFFFF) return 0;
    *out = (u16)addr;
    return 1;
}

static int table_ok(const u8 *t, const char *sig) {
    if (memcmp(t, sig, 4) != 0) return 0;
    u32 len = rd32(t, 4);
    if (len < 36 || len > (4u << 20)) return 0;
    u8 sum = 0;
    for (u32 i = 0; i < len; i++) sum = (u8)(sum + t[i]);
    return sum == 0;
}

/* Walk RSDT/XSDT for a table with the given 4-character signature. */
static const u8 *find_table(u64 rsdp_phys, const char *sig) {
    const u8 *rsdp = (const u8 *)phys_to_virt(rsdp_phys);
    if (!rsdp) return NULL;

    u8 revision = rsdp[15];
    u64 root_phys = 0;
    u32 entry_size = 4;
    if (revision >= 2) {
        root_phys = rd64(rsdp, 24);
    }
    if (!root_phys) {
        root_phys = rd32(rsdp, 16);
        entry_size = 4;
    } else if (!(revision >= 2)) {
        entry_size = 4;
    }
    if (revision >= 2 && rd64(rsdp, 24)) entry_size = 8;
    if (!root_phys) return NULL;

    const u8 *root = (const u8 *)phys_to_virt(root_phys);
    u32 len = rd32(root, 4);
    if (len < 36 || len > (4u << 20)) return NULL;
    u32 count = (len - 36) / entry_size;
    for (u32 i = 0; i < count; i++) {
        u64 phys = (entry_size == 8) ? rd64(root, 36 + i * 8)
                                     : (u64)rd32(root, 36 + i * 4);
        if (!phys) continue;
        const u8 *t = (const u8 *)phys_to_virt(phys);
        if (!t || !table_ok(t, sig)) continue;
        return t;
    }
    return NULL;
}

/* Read one AML integer element (ZeroOp / OneOp / BytePrefix / WordPrefix /
 * DwordPrefix). Returns 0 when the stream no longer looks like an integer. */
static int aml_read_int(const u8 *p, u32 len, u32 *pos, u32 *out) {
    if (*pos >= len) return 0;
    u8 op = p[*pos];
    switch (op) {
    case 0x00: *out = 0; *pos += 1; return 1;
    case 0x01: *out = 1; *pos += 1; return 1;
    case 0x0A:
        if (*pos + 1 >= len) return 0;
        *out = p[*pos + 1];
        *pos += 2;
        return 1;
    case 0x0B:
        if (*pos + 2 >= len) return 0;
        *out = (u32)(p[*pos + 1] | ((u32)p[*pos + 2] << 8));
        *pos += 3;
        return 1;
    case 0x0C:
        if (*pos + 4 >= len) return 0;
        *out = rd32(p, *pos + 1);
        *pos += 5;
        return 1;
    default:
        return 0;
    }
}

/* Locate `_S5_` in the DSDT and decode its Package(typa, typb). */
static int read_sleep_type(const u8 *dsdt, u32 len, u8 *typa, u8 *typb) {
    if (!dsdt || len < 8) return 0;
    for (u32 i = 0; i + 8 < len; i++) {
        if (dsdt[i] != '_' || dsdt[i + 1] != 'S' || dsdt[i + 2] != '5' ||
            dsdt[i + 3] != '_')
            continue;

        u32 pos = i + 4;
        /* Optional package operator, then PkgLength (1..4 bytes) and the
         * element count. */
        if (dsdt[pos] != 0x12) continue;
        pos++;
        u8 lead = dsdt[pos];
        if ((lead >> 6) == 0) pos += 1;
        else if ((lead >> 6) == 1) pos += 2;
        else continue;

        u32 count = 0;
        if (!aml_read_int(dsdt, len, &pos, &count)) continue;
        if (count == 0 || count > 8) continue;

        u32 a = 0;
        if (!aml_read_int(dsdt, len, &pos, &a)) continue;
        u32 b = a;
        if (count > 1) {
            u32 tmp = 0;
            if (aml_read_int(dsdt, len, &pos, &tmp)) b = tmp;
        }
        *typa = (u8)(a & 0x07);
        *typb = (u8)(b & 0x07);
        return 1;
    }
    return 0;
}

/* Put the platform into ACPI mode. Legacy (BIOS) boots start with ACPI
 * disabled, and while SCI_EN is clear SLP_EN is ignored by the chipset. */
static void acpi_enable_mode(void) {
    if (!g_acpi.pm1a_cnt) return;
    if (inw(g_acpi.pm1a_cnt) & PM1_SCI_EN) {
        g_acpi.sci_en = 1;
        return;
    }
    if (!g_acpi.smi_cmd || !g_acpi.acpi_enable) return;

    outb((u16)g_acpi.smi_cmd, g_acpi.acpi_enable);
    for (int i = 0; i < 3000; i++) {
        if (inw(g_acpi.pm1a_cnt) & PM1_SCI_EN) {
            g_acpi.sci_en = 1;
            return;
        }
        for (volatile int j = 0; j < 1000; j++) { }
    }
}

/* Battery devices are described by AML methods (_BST status, _BIF/_BIX
 * information). We have no AML interpreter, so the probe is a plain byte scan
 * of the DSDT blob we already parsed for \_S5_: if those names are there, the
 * firmware does describe a battery pack. Cheap, cannot fault, and enough for
 * the shell to tell a laptop apart from a desktop. */
static int dsdt_has_battery(const u8 *dsdt, u32 len) {
    if (!dsdt || len < 8) return 0;
    for (u32 i = 0; i + 4 <= len; i++) {
        if (dsdt[i] != '_' || dsdt[i + 1] != 'B') continue;
        u8 c = dsdt[i + 2];
        u8 d = dsdt[i + 3];
        if (c == 'S' && d == 'T') return 1;   /* _BST: battery status */
        if (c == 'I' && (d == 'F' || d == 'X')) return 1;  /* _BIF/_BIX */
    }
    return 0;
}

void acpi_init(const boot_info_t *bi) {
    memset(&g_acpi, 0, sizeof(g_acpi));
    if (!bi || !bi->rsdp) {
        kprintf("[acpi] no RSDP handed over by loader\n");
        return;
    }
    if (memcmp((const void *)phys_to_virt(bi->rsdp), "RSD PTR ", 8) != 0) {
        kprintf("[acpi] bad RSDP signature at %p\n", phys_to_virt(bi->rsdp));
        return;
    }

    const u8 *fadt = find_table(bi->rsdp, "FACP");
    if (!fadt) {
        kprintf("[acpi] FADT (FACP) not found\n");
        return;
    }

    g_acpi.present = 1;
    g_acpi.rsdp = bi->rsdp;
    g_acpi.fadt_len = rd32(fadt, 4);
    for (int i = 0; i < 6; i++) g_acpi.oem_id[i] = (char)fadt[10 + i];
    g_acpi.oem_id[6] = 0;

    g_acpi.pm1a_cnt = rd16(fadt, FADT_PM1A_CNT);
    g_acpi.pm1b_cnt = rd16(fadt, FADT_PM1B_CNT);
    if (g_acpi.fadt_len > FADT_MIN_LEN) {
        g_acpi.pm1_cnt_len = fadt[FADT_PM1_CNT_LEN];
        /* ACPI 2.0+ machines may only populate the 64-bit GAS variant. */
        if (!g_acpi.pm1a_cnt) (void)gas_port(fadt, FADT_X_PM1A_CNT, &g_acpi.pm1a_cnt);
        if (!g_acpi.pm1b_cnt) (void)gas_port(fadt, FADT_X_PM1B_CNT, &g_acpi.pm1b_cnt);

        u32 flags = rd32(fadt, FADT_FLAGS);
        if (flags & FADT_FLAG_RESET_REG) {
            g_acpi.reset_space = fadt[FADT_RESET_REG + 0];
            g_acpi.reset_size  = fadt[FADT_RESET_REG + 3];
            g_acpi.reset_addr  = rd64(fadt, FADT_RESET_REG + 4);
            g_acpi.reset_value = fadt[FADT_RESET_VALUE];
            g_acpi.have_reset = (g_acpi.reset_space == 1 &&
                                 g_acpi.reset_size == 1 &&
                                 g_acpi.reset_addr != 0);
        }
    }
    if (!g_acpi.pm1_cnt_len) g_acpi.pm1_cnt_len = 2;

    g_acpi.smi_cmd = rd32(fadt, FADT_SMI_CMD);
    g_acpi.acpi_enable = fadt[FADT_ACPI_ENABLE];

    /* Sleep types live in the DSDT. */
    u64 dsdt_phys = (g_acpi.fadt_len > FADT_MIN_LEN) ? rd64(fadt, FADT_X_DSDT) : 0;
    if (!dsdt_phys) dsdt_phys = rd32(fadt, FADT_DSDT);
    g_acpi.slp_typa = 5;
    g_acpi.slp_typb = 5;
    if (dsdt_phys) {
        const u8 *dsdt = (const u8 *)phys_to_virt(dsdt_phys);
        if (dsdt && table_ok(dsdt, "DSDT")) {
            u32 dlen = rd32(dsdt, 4);
            u8 a, b;
            if (read_sleep_type(dsdt, dlen, &a, &b)) {
                g_acpi.slp_typa = a;
                g_acpi.slp_typb = b;
            }
            g_acpi.has_battery = dsdt_has_battery(dsdt, dlen);
            (void)aml_init(dsdt, dlen);
        }
    }

    /* ACPI mode is deliberately NOT entered here. The SMI handshake switches
     * the chipset's interrupt routing from the legacy 8259 to the SCI/APIC
     * path; on several modern firmwares that disconnects IRQ0 from the CPU
     * mid-boot and the PIT timer goes silent. It is only needed before a
     * shutdown (SLP_EN is ignored while SCI_EN is clear), so acpi_shutdown()
     * performs it right before writing SLP_TYP, where breaking the PIC is
     * harmless.
     */
}

int acpi_available(void) {
    return g_acpi.present && g_acpi.pm1a_cnt != 0;
}

int acpi_battery_present(void) {
    return g_acpi.has_battery;
}

/* A battery reading walks the EC through AML, and the shell asks for it once a
 * second per consumer. Cache for two seconds so the desktop clock refresh and
 * the Settings page never queue duplicate EC transactions. */
#define BATTERY_CACHE_MS 2000u

static acpi_battery_state_t g_bat_cache;
static u64 g_bat_stamp;
static int g_bat_valid;

int acpi_battery_read(acpi_battery_state_t *out) {
    if (!out) return -1;

    u64 now = timer_uptime_ms();
    if (g_bat_valid && now - g_bat_stamp < BATTERY_CACHE_MS) {
        *out = g_bat_cache;
        return 0;
    }

    acpi_battery_state_t st;
    int rc = aml_battery_query(&st);
    if (rc != 0) {
        /* No _BST/_BIF in the DSDT, or the methods could not be evaluated. The
         * byte-scan presence probe still tells the shell a pack exists, so the
         * glyph is drawn without inventing a percentage. */
        if (!g_acpi.has_battery) return -1;
        memset(&st, 0, sizeof(st));
        st.present = 1;
        st.ac_online = 1;
    }

    g_bat_cache = st;
    g_bat_stamp = now;
    g_bat_valid = 1;
    *out = st;
    return 0;
}

void acpi_print_summary(void) {
    if (!g_acpi.present) {
        kprintf("[acpi] no usable FADT, power management falls back to legacy ports\n");
        return;
    }
    kprintf("[acpi] oem %s  PM1a_CNT %p PM1b_CNT %p  SLP_TYP %u/%u  SCI_EN %u\n",
            g_acpi.oem_id, (void *)(uptr)g_acpi.pm1a_cnt,
            (void *)(uptr)g_acpi.pm1b_cnt, (u32)g_acpi.slp_typa,
            (u32)g_acpi.slp_typb, (u32)g_acpi.sci_en);
    kprintf("[acpi] reset register %s\n",
            g_acpi.have_reset ? "available" : "not advertised");
}

int acpi_shutdown(void) {
    if (!acpi_available()) return 0;

    /* SLP_EN is ignored unless the platform is in ACPI mode; do the SMI
     * handshake now that breaking the legacy interrupt routing no longer
     * matters. */
    acpi_enable_mode();

    u16 a = (u16)(((u16)g_acpi.slp_typa << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN);
    kprintf("[acpi] S5 request: PM1a_CNT[%p] <- %p\n",
            (void *)(uptr)g_acpi.pm1a_cnt, (void *)(uptr)a);
    outw(g_acpi.pm1a_cnt, a);
    io_wait();
    if (g_acpi.pm1b_cnt) {
        u16 b = (u16)(((u16)g_acpi.slp_typb << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN);
        outw(g_acpi.pm1b_cnt, b);
        io_wait();
    }

    /* The chipset cuts power asynchronously; if we are still executing after a
     * short wait the write did not take effect. */
    for (int i = 0; i < 2000; i++) {
        (void)inw(g_acpi.pm1a_cnt);
        for (volatile int j = 0; j < 1000; j++) { }
    }
    return 0;
}

int acpi_reboot(void) {
    /* Preferred: the FADT reset register (the only portable reset). */
    if (g_acpi.have_reset) {
        kprintf("[acpi] reset via FADT register %p\n",
                (void *)(uptr)g_acpi.reset_addr);
        outb((u16)g_acpi.reset_addr, g_acpi.reset_value);
        for (volatile int i = 0; i < 1000000; i++) { }
    }
    /* Chipset reset control (PIIX4 / ICH). */
    outb(0xCF9, 0x02);
    for (volatile int i = 0; i < 10000; i++) { }
    outb(0xCF9, 0x06);
    for (volatile int i = 0; i < 10000; i++) { }
    /* Legacy 8042 pulse. */
    while (inb(0x64) & 0x02) { }
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 1000000; i++) { }
    return 0;
}
