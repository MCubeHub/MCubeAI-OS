/* AHCI (SATA) block driver.
 *
 * Polled DMA, no interrupts. Each active port gets a 1 KiB command list, a
 * 1 KiB FIS receive area, one command table and a 64 KiB physically contiguous
 * bounce buffer, all taken from the physical allocator and reached through the
 * direct map. Only command slot 0 is used, which keeps the bookkeeping small
 * while still allowing full-speed 128-sector transfers.
 */

#include <mcube/block.h>
#include <mcube/io.h>
#include <mcube/kernel.h>
#include <mcube/mm.h>
#include <mcube/pci.h>

#define AHCI_MAX_PORTS  8
#define AHCI_MMIO_VA    0xFFFFD00000000000ULL
#define AHCI_MMIO_SIZE  0x2000

#define HBA_CAP         0x00
#define HBA_GHC         0x04
#define HBA_IS          0x08
#define HBA_PI          0x0C
#define HBA_VS          0x10

#define GHC_HR          0x00000001u
#define GHC_AE          0x80000000u

#define PX_CLB          0x00
#define PX_CLBU         0x04
#define PX_FB           0x08
#define PX_FBU          0x0C
#define PX_IS           0x10
#define PX_IE           0x14
#define PX_CMD          0x18
#define PX_TFD          0x20
#define PX_SIG          0x24
#define PX_SSTS         0x28
#define PX_SCTL         0x2C
#define PX_SERR         0x30
#define PX_CI           0x38

#define PX_CMD_ST       0x0001u
#define PX_CMD_FRE      0x0010u
#define PX_CMD_FR       0x4000u   /* FIS receive running (RO)  */
#define PX_CMD_CR       0x8000u   /* command list running (RO)  */

#define PX_IS_ERR       0x7D000000u /* TFES|HBFS|HBDS|IFS|INFS|OFS */
#define PX_TFD_ERR      0x00000021u /* ERR | DF */
#define PX_TFD_BSY      0x00000080u

#define ATA_IDENTIFY    0xEC
#define ATA_READ_DMA_EXT  0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH_EXT   0xEA

#define AHCI_MAX_XFER   128u      /* sectors per command (64 KiB) */
#define AHCI_BOUNCE_PAGES 16u

typedef struct ahci_port {
    volatile u8 *reg;
    u8 *clb;                 /* 1 KiB command list                 */
    u8 *fb;                  /* 256-byte FIS receive area          */
    u8 *ct;                  /* 256-byte command table + PRDT      */
    u8 *bounce;              /* 64 KiB physically contiguous DMA   */
    u64 clb_phys;
    u64 fb_phys;
    u64 ct_phys;
    u64 bounce_phys;
} ahci_port_t;

static volatile u8 *g_hba;
static ahci_port_t g_ports[AHCI_MAX_PORTS];
static block_device_t g_disks[AHCI_MAX_PORTS];

static u32 hba_rd(u32 off) {
    return *(volatile u32 *)(g_hba + off);
}

static void hba_wr(u32 off, u32 val) {
    *(volatile u32 *)(g_hba + off) = val;
}

static u32 px_rd(ahci_port_t *p, u32 off) {
    return *(volatile u32 *)(p->reg + off);
}

static void px_wr(ahci_port_t *p, u32 off, u32 val) {
    *(volatile u32 *)(p->reg + off) = val;
}

static void ahci_pause(u32 count) {
    while (count--) __asm__ __volatile__("pause");
}

static int hba_reset(void) {
    u32 ghc = hba_rd(HBA_GHC);
    hba_wr(HBA_GHC, ghc | GHC_AE);
    hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_HR);

    int ok = 0;
    for (u32 i = 0; i < 20000000u; i++) {
        if (!(hba_rd(HBA_GHC) & GHC_HR)) {
            ok = 1;
            break;
        }
        __asm__ __volatile__("pause");
    }
    if (!ok) return -1;

    hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_AE);
    return 0;
}

static int port_stop(ahci_port_t *p) {
    u32 cmd = px_rd(p, PX_CMD);
    px_wr(p, PX_CMD, cmd & ~(PX_CMD_ST | PX_CMD_FRE));

    for (u32 i = 0; i < 5000000u; i++) {
        if (!(px_rd(p, PX_CMD) & (PX_CMD_FR | PX_CMD_CR))) return 0;
        __asm__ __volatile__("pause");
    }
    return -1;
}

static int port_link_up(ahci_port_t *p) {
    /* A controller reset leaves PxSIG reading all ones on some HBAs until the
     * link has been reset once, so only trust it when it is already valid. */
    if ((px_rd(p, PX_SSTS) & 0xF) == 3 && px_rd(p, PX_SIG) == 0x00000101u) {
        return 0;
    }

    u32 sctl = px_rd(p, PX_SCTL);
    px_wr(p, PX_SCTL, (sctl & ~0xFu) | 1u);   /* COMRESET */
    ahci_pause(20000);
    px_wr(p, PX_SCTL, sctl & ~0xFu);

    for (u32 i = 0; i < 20000000u; i++) {
        if ((px_rd(p, PX_SSTS) & 0xF) == 3) return 0;
        __asm__ __volatile__("pause");
    }
    return -1;
}

static int port_start(ahci_port_t *p) {
    px_wr(p, PX_CLB, (u32)p->clb_phys);
    px_wr(p, PX_CLBU, (u32)(p->clb_phys >> 32));
    px_wr(p, PX_FB, (u32)p->fb_phys);
    px_wr(p, PX_FBU, (u32)(p->fb_phys >> 32));
    px_wr(p, PX_SERR, 0xFFFFFFFFu);
    px_wr(p, PX_IS, 0xFFFFFFFFu);

    px_wr(p, PX_CMD, px_rd(p, PX_CMD) | PX_CMD_FRE);
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) | PX_CMD_ST);
    ahci_pause(100000);
    return 0;
}

/* Build one H2D register FIS in slot 0 and poll until the HBA retires it. */
static int ahci_exec(ahci_port_t *p, u8 op, u64 lba, u16 count, u32 bytes,
                     int write) {
    int idle = 0;
    for (u32 i = 0; i < 2000000u; i++) {
        if (!(px_rd(p, PX_CI) & 1u) && !(px_rd(p, PX_TFD) & PX_TFD_BSY)) {
            idle = 1;
            break;
        }
        __asm__ __volatile__("pause");
    }
    if (!idle) return -1;

    px_wr(p, PX_IS, 0xFFFFFFFFu);
    memset(p->ct, 0, 0x100);

    u8 *f = p->ct;
    f[0] = 0x27;                 /* host-to-device register FIS */
    f[1] = 0x80;                 /* command flag               */
    f[2] = op;
    f[4] = (u8)(lba & 0xFF);
    f[5] = (u8)((lba >> 8) & 0xFF);
    f[6] = (u8)((lba >> 16) & 0xFF);
    f[7] = (op == ATA_IDENTIFY) ? 0 : 0x40;
    f[8] = (u8)((lba >> 24) & 0xFF);
    f[9] = (u8)((lba >> 32) & 0xFF);
    f[10] = (u8)((lba >> 40) & 0xFF);
    f[12] = (u8)(count & 0xFF);
    f[13] = (u8)(count >> 8);

    u32 prdtl = 0;
    if (bytes) {
        u8 *prd = p->ct + 0x80;
        *(u32 *)(prd + 0) = (u32)p->bounce_phys;
        *(u32 *)(prd + 4) = (u32)(p->bounce_phys >> 32);
        *(u32 *)(prd + 8) = 0;
        *(u32 *)(prd + 12) = (bytes - 1) & 0x3FFFFFu;
        prdtl = 1;
    }

    u8 *hdr = p->clb;
    memset(hdr, 0, 32);
    *(u32 *)(hdr + 0) = 5u | (write ? (1u << 6) : 0u) | (prdtl << 16);
    *(u32 *)(hdr + 8) = (u32)p->ct_phys;
    *(u32 *)(hdr + 12) = (u32)(p->ct_phys >> 32);

    __asm__ __volatile__("mfence" ::: "memory");
    px_wr(p, PX_CI, 1u);

    for (u32 i = 0; i < 20000000u; i++) {
        u32 is = px_rd(p, PX_IS);
        if (is & PX_IS_ERR) {
            px_wr(p, PX_IS, 0xFFFFFFFFu);
            px_wr(p, PX_SERR, 0xFFFFFFFFu);
            return -1;
        }
        if (!(px_rd(p, PX_CI) & 1u)) {
            px_wr(p, PX_IS, 0xFFFFFFFFu);
            if (px_rd(p, PX_TFD) & PX_TFD_ERR) return -1;
            return 0;
        }
        __asm__ __volatile__("pause");
    }

    px_wr(p, PX_IS, 0xFFFFFFFFu);
    return -1;
}

static void ata_string(char *dst, const u16 *src, int words) {
    int n = 0;
    for (int i = 0; i < words; i++) {
        dst[n++] = (char)(src[i] >> 8);
        dst[n++] = (char)(src[i] & 0xFF);
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0)) n--;
    dst[n] = 0;
}

static int ahci_read(block_device_t *dev, u64 lba, u32 count, void *buf) {
    ahci_port_t *p = dev->priv;
    u8 *dst = buf;

    while (count) {
        u32 n = count > AHCI_MAX_XFER ? AHCI_MAX_XFER : count;
        if (lba + n < lba) return -1;
        if (ahci_exec(p, ATA_READ_DMA_EXT, lba, (u16)n, n * BLOCK_SECTOR_SIZE,
                      0) != 0) {
            return -1;
        }
        memcpy(dst, p->bounce, n * BLOCK_SECTOR_SIZE);
        dst += n * BLOCK_SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

static int ahci_write(block_device_t *dev, u64 lba, u32 count,
                      const void *buf) {
    ahci_port_t *p = dev->priv;
    const u8 *src = buf;

    while (count) {
        u32 n = count > AHCI_MAX_XFER ? AHCI_MAX_XFER : count;
        if (lba + n < lba) return -1;
        memcpy(p->bounce, src, n * BLOCK_SECTOR_SIZE);
        __asm__ __volatile__("mfence" ::: "memory");
        if (ahci_exec(p, ATA_WRITE_DMA_EXT, lba, (u16)n,
                      n * BLOCK_SECTOR_SIZE, 1) != 0) {
            return -1;
        }
        src += n * BLOCK_SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return ahci_exec(p, ATA_FLUSH_EXT, 0, 0, 0, 0);
}

static int port_probe(int index, int letter) {
    ahci_port_t *p = &g_ports[index];
    p->reg = g_hba + 0x100 + 0x80 * index;

    if (port_stop(p) != 0) {
        kprintf("[ahci] port %d: did not stop\n", index);
        return -1;
    }
    if (port_link_up(p) != 0) {
        kprintf("[ahci] port %d: no device\n", index);
        return -1;
    }

    u32 sig = px_rd(p, PX_SIG);
    if (sig != 0x00000101u && sig != 0xFFFFFFFFu) {
        kprintf("[ahci] port %d: signature %x (not a SATA disk)\n", index, sig);
        return -1;
    }
    if (sig != 0x00000101u) {
        kprintf("[ahci] port %d: signature unknown, probing anyway\n", index);
    }

    u64 page = pmm_alloc_page();
    u64 bounce = pmm_alloc_pages(AHCI_BOUNCE_PAGES);
    if (!page || !bounce) {
        kprintf("[ahci] port %d: out of DMA memory\n", index);
        if (page) pmm_free_page(page);
        if (bounce) pmm_free_pages(bounce, AHCI_BOUNCE_PAGES);
        return -1;
    }

    p->clb = phys_to_virt(page);
    p->fb = p->clb + 0x400;
    p->ct = p->clb + 0x800;
    p->clb_phys = page;
    p->fb_phys = page + 0x400;
    p->ct_phys = page + 0x800;
    p->bounce = phys_to_virt(bounce);
    p->bounce_phys = bounce;
    memset(p->clb, 0, 0x1000);
    memset(p->bounce, 0, AHCI_BOUNCE_PAGES * 0x1000);

    port_start(p);

    u16 id[256];
    if (ahci_exec(p, ATA_IDENTIFY, 0, 0, 512, 0) != 0) {
        kprintf("[ahci] port %d: IDENTIFY failed\n", index);
        return -1;
    }
    memcpy(id, p->bounce, sizeof(id));

    if (id[106] & 0x1000) {
        kprintf("[ahci] port %d: 4K logical sectors unsupported\n", index);
        return -1;
    }

    u64 sectors = (u64)id[100] | ((u64)id[101] << 16) |
                  ((u64)id[102] << 32) | ((u64)id[103] << 48);
    if (!(id[83] & (1u << 10)) || sectors == 0) {
        kprintf("[ahci] port %d: drive does not report LBA48 capacity\n",
                index);
        return -1;
    }

    char model[41];
    ata_string(model, &id[27], 20);

    block_device_t *dev = &g_disks[index];
    dev->name[0] = 's';
    dev->name[1] = 'd';
    dev->name[2] = (char)('a' + letter);
    dev->name[3] = 0;
    dev->sectors = sectors;
    dev->sector_size = BLOCK_SECTOR_SIZE;
    dev->read = ahci_read;
    dev->write = ahci_write;
    dev->priv = p;
    block_register(dev);

    kprintf("[ahci] port %d: %s %u MiB (%s)\n", index, dev->name,
            (u32)(sectors * BLOCK_SECTOR_SIZE / MiB), model);

    u8 boot[BLOCK_SECTOR_SIZE];
    if (ahci_read(dev, 0, 1, boot) == 0 &&
        memcmp(boot, "MCUBE-AHCI-TEST", 15) == 0) {
        kprintf("AHCI_TEST_OK\n");
    }
    return 0;
}

void ahci_init(void) {
    pci_device_t dev;
    if (pci_find_class(0x01, 0x06, &dev) != 0 || dev.prog_if != 0x01) {
        kprintf("[ahci] no AHCI controller\n");
        return;
    }

    u64 abar = dev.bar[5] & ~0xFULL;
    if (!abar) {
        kprintf("[ahci] controller has no ABAR\n");
        return;
    }

    pci_enable_bus_master(&dev);
    for (u32 off = 0; off < AHCI_MMIO_SIZE; off += 0x1000) {
        vmm_map_page(AHCI_MMIO_VA + off, abar + off,
                     PTE_WRITE | PTE_PCD | PTE_NX);
    }
    g_hba = (volatile u8 *)(uptr)AHCI_MMIO_VA;

    kprintf("[ahci] HBA %04x:%04x at %02x:%02x.%u, ABAR %p\n",
            dev.vendor, dev.device, dev.bus, dev.slot, dev.func, (void *)abar);

    if (hba_reset() != 0) {
        kprintf("[ahci] HBA reset timeout\n");
        return;
    }

    u32 version = hba_rd(HBA_VS);
    u32 pi = hba_rd(HBA_PI);
    kprintf("[ahci] version %u.%u, port bitmap %x\n",
            (version >> 16) & 0xFF, (version >> 8) & 0xFF, pi);

    int letter = block_root() ? 1 : 0;
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(pi & (1u << i))) continue;
        if (port_probe(i, letter) == 0) letter++;
    }
}
