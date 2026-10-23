/* ATA PIO block driver for the primary IDE channel.
 *
 * QEMU's q35 machine exposes the ESP image on the legacy PIIX IDE ports when
 * started with `-drive if=ide`. Polled PIO is slower than DMA but needs no
 * interrupt plumbing and works before the APIC exists. The block layer hides
 * the difference, so an AHCI/NVMe driver can replace this later.
 */

#include <mcube/block.h>
#include <mcube/io.h>
#include <mcube/kernel.h>

#define ATA_IO     0x1F0
#define ATA_CTRL   0x3F6

#define ATA_REG_DATA  0
#define ATA_REG_ERR   1
#define ATA_REG_SEC   2
#define ATA_REG_LBA0  3
#define ATA_REG_LBA1  4
#define ATA_REG_LBA2  5
#define ATA_REG_DRV   6
#define ATA_REG_CMD   7

#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF  0x20
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

#define ATA_CMD_READ  0x20
#define ATA_CMD_WRITE 0x30
#define ATA_CMD_FLUSH 0xE7
#define ATA_CMD_IDENT 0xEC

static block_device_t g_ata;

static int ata_wait_not_busy(void) {
    for (int i = 0; i < 1000000; i++) {
        u8 st = inb(ATA_IO + ATA_REG_CMD);
        if (st == 0xFF) return -1;      /* floating bus: no controller */
        if (!(st & ATA_SR_BSY)) return 0;
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (int i = 0; i < 1000000; i++) {
        u8 st = inb(ATA_IO + ATA_REG_CMD);
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

static int ata_identify(u16 *id) {
    outb(ATA_CTRL, 0x02); /* nIEN: poll, do not raise IRQ14 */
    outb(ATA_IO + ATA_REG_DRV, 0xA0);
    outb(ATA_IO + ATA_REG_SEC, 0);
    outb(ATA_IO + ATA_REG_LBA0, 0);
    outb(ATA_IO + ATA_REG_LBA1, 0);
    outb(ATA_IO + ATA_REG_LBA2, 0);
    outb(ATA_IO + ATA_REG_CMD, ATA_CMD_IDENT);

    u8 st = inb(ATA_IO + ATA_REG_CMD);
    if (st == 0 || st == 0xFF) return -1;
    if (ata_wait_not_busy() != 0) return -1;
    if (inb(ATA_IO + ATA_REG_LBA1) != 0 || inb(ATA_IO + ATA_REG_LBA2) != 0) {
        return -1; /* not an ATA device */
    }
    if (ata_wait_drq() != 0) return -1;

    for (int i = 0; i < 256; i++) id[i] = inw(ATA_IO + ATA_REG_DATA);
    return 0;
}

static int ata_read(block_device_t *dev, u64 lba, u32 count, void *buf) {
    UNUSED(dev);
    u8 *p = (u8 *)buf;
    for (u32 s = 0; s < count; s++, lba++, p += BLOCK_SECTOR_SIZE) {
        if (ata_wait_not_busy() != 0) return -1;
        outb(ATA_IO + ATA_REG_DRV, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
        outb(ATA_IO + ATA_REG_SEC, 1);
        outb(ATA_IO + ATA_REG_LBA0, (u8)(lba & 0xFF));
        outb(ATA_IO + ATA_REG_LBA1, (u8)((lba >> 8) & 0xFF));
        outb(ATA_IO + ATA_REG_LBA2, (u8)((lba >> 16) & 0xFF));
        outb(ATA_IO + ATA_REG_CMD, ATA_CMD_READ);
        if (ata_wait_drq() != 0) return -1;
        for (int i = 0; i < 256; i++) {
            u16 v = inw(ATA_IO + ATA_REG_DATA);
            p[i * 2] = (u8)(v & 0xFF);
            p[i * 2 + 1] = (u8)(v >> 8);
        }
    }
    return 0;
}

static int ata_write(block_device_t *dev, u64 lba, u32 count, const void *buf) {
    UNUSED(dev);
    const u8 *p = (const u8 *)buf;
    for (u32 s = 0; s < count; s++, lba++, p += BLOCK_SECTOR_SIZE) {
        if (ata_wait_not_busy() != 0) return -1;
        outb(ATA_IO + ATA_REG_DRV, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
        outb(ATA_IO + ATA_REG_SEC, 1);
        outb(ATA_IO + ATA_REG_LBA0, (u8)(lba & 0xFF));
        outb(ATA_IO + ATA_REG_LBA1, (u8)((lba >> 8) & 0xFF));
        outb(ATA_IO + ATA_REG_LBA2, (u8)((lba >> 16) & 0xFF));
        outb(ATA_IO + ATA_REG_CMD, ATA_CMD_WRITE);
        if (ata_wait_drq() != 0) return -1;
        for (int i = 0; i < 256; i++) {
            u16 v = (u16)p[i * 2] | ((u16)p[i * 2 + 1] << 8);
            outw(ATA_IO + ATA_REG_DATA, v);
        }
    }
    if (ata_wait_not_busy() != 0) return -1;
    outb(ATA_IO + ATA_REG_CMD, ATA_CMD_FLUSH);
    return ata_wait_not_busy();
}

void block_init(void) {
    u16 id[256];
    if (ata_identify(id) != 0) {
        kprintf("[block] ATA: no drive on primary master\n");
        return;
    }

    u64 sectors = (u64)id[60] | ((u64)id[61] << 16);
    if (sectors == 0) {
        kprintf("[block] ATA: drive reports zero sectors\n");
        return;
    }

    strncpy(g_ata.name, "sda", sizeof(g_ata.name) - 1);
    g_ata.sectors = sectors;
    g_ata.sector_size = BLOCK_SECTOR_SIZE;
    g_ata.read = ata_read;
    g_ata.write = ata_write;
    g_ata.priv = NULL;
    block_register(&g_ata);

    kprintf("[block] ATA PIO drive ready: %u sectors (%u MiB)\n",
            (u32)sectors, (u32)(sectors * BLOCK_SECTOR_SIZE / MiB));
}
