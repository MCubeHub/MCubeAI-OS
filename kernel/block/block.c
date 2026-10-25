/* Block-device registry, sector I/O wrappers and partition scanning. */

#include <mcube/block.h>
#include <mcube/kernel.h>

static block_device_t *g_devices;
static block_device_t *g_root;

void block_register(block_device_t *dev) {
    if (!dev) return;
    if (!dev->sector_size) dev->sector_size = BLOCK_SECTOR_SIZE;
    dev->next = g_devices;
    g_devices = dev;
    if (!g_root) g_root = dev;
}

block_device_t *block_root(void) { return g_root; }

block_device_t *block_find(const char *name) {
    for (block_device_t *d = g_devices; d; d = d->next) {
        if (strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

int block_read(block_device_t *dev, u64 lba, u32 count, void *buf) {
    if (!dev || !dev->read || !buf) return -1;
    if (count == 0) return 0;
    if (lba + count < lba || lba + count > dev->sectors) return -1;
    return dev->read(dev, lba, count, buf);
}

int block_write(block_device_t *dev, u64 lba, u32 count, const void *buf) {
    if (!dev || !dev->write || !buf) return -1;
    if (count == 0) return 0;
    if (lba + count < lba || lba + count > dev->sectors) return -1;
    return dev->write(dev, lba, count, buf);
}

/* ---- partitions ----------------------------------------------------- */

/* A partition is a thin LBA-offset wrapper, so FAT32 and friends work on it
 * exactly like on a whole-disk volume. */
#define MAX_PARTS 8

typedef struct {
    block_device_t dev;
    block_device_t *disk;
    u64 start;
} part_dev_t;

static part_dev_t g_parts[MAX_PARTS];
static int g_part_count;

static int part_read(block_device_t *d, u64 lba, u32 count, void *buf) {
    part_dev_t *p = (part_dev_t *)d->priv;
    return block_read(p->disk, p->start + lba, count, buf);
}

static int part_write(block_device_t *d, u64 lba, u32 count, const void *buf) {
    part_dev_t *p = (part_dev_t *)d->priv;
    return block_write(p->disk, p->start + lba, count, buf);
}

static void part_add(block_device_t *disk, u64 start, u64 sectors, int num) {
    if (g_part_count >= MAX_PARTS || start == 0 || sectors == 0) return;
    if (start + sectors < start || start + sectors > disk->sectors) return;

    part_dev_t *p = &g_parts[g_part_count++];
    memset(p, 0, sizeof(*p));

    usize n = strlen(disk->name);
    if (n > sizeof(p->dev.name) - 3) n = sizeof(p->dev.name) - 3;
    memcpy(p->dev.name, disk->name, n);
    if (num >= 10) p->dev.name[n++] = (char)('0' + num / 10);
    p->dev.name[n++] = (char)('0' + num % 10);
    p->dev.name[n] = 0;

    p->dev.sectors = sectors;
    p->dev.sector_size = disk->sector_size;
    p->dev.read = part_read;
    p->dev.write = part_write;
    p->dev.priv = p;
    p->disk = disk;
    p->start = start;
    block_register(&p->dev);
}

static int looks_like_fat(const u8 *s) {
    if (s[0] != 0xEB && s[0] != 0xE9) return 0;
    u16 bps;
    memcpy(&bps, s + 11, 2);
    if (bps != BLOCK_SECTOR_SIZE) return 0;
    if (s[13] == 0 || (s[13] & (s[13] - 1)) != 0) return 0;
    u16 reserved;
    memcpy(&reserved, s + 16, 2);
    if (reserved == 0) return 0;
    return memcmp(s + 82, "FAT32   ", 8) == 0;
}

static void scan_gpt(block_device_t *disk) {
    static u8 hdr[BLOCK_SECTOR_SIZE];
    static u8 ent[256];
    static const u8 esp_type[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
    };

    if (block_read(disk, 1, 1, hdr) != 0) return;
    if (memcmp(hdr, "EFI PART", 8) != 0) return;

    u64 ent_lba;
    u32 count, entry_size;
    memcpy(&ent_lba, hdr + 72, 8);
    memcpy(&count, hdr + 80, 4);
    memcpy(&entry_size, hdr + 84, 4);
    if (entry_size < 128 || entry_size > sizeof(ent) || entry_size % 8 ||
        count == 0 || count > 128) {
        return;
    }

    u32 per_sector = BLOCK_SECTOR_SIZE / entry_size;
    if (per_sector == 0) return;

    for (u32 i = 0; i < count; i++) {
        u64 sect = ent_lba + i / per_sector;
        u32 slot = i % per_sector;
        if (block_read(disk, sect, 1, ent) != 0) break;
        const u8 *e = ent + slot * entry_size;
        if (memcmp(e, esp_type, 16) != 0) continue;

        u64 first, last;
        memcpy(&first, e + 32, 8);
        memcpy(&last, e + 40, 8);
        if (last < first) continue;
        part_add(disk, first, last - first + 1, (int)i + 1);
    }
}

static void scan_mbr(block_device_t *disk, const u8 *s) {
    for (int i = 0; i < 4; i++) {
        const u8 *e = s + 446 + i * 16;
        if (e[0] != 0x00 && e[0] != 0x80) continue;
        u8 type = e[4];
        if (type == 0) continue;
        if (type == 0xEE) {
            scan_gpt(disk);
            return;
        }
        u32 start, count;
        memcpy(&start, e + 8, 4);
        memcpy(&count, e + 12, 4);
        /* FAT12/16/32 and EFI system partitions. */
        if (type == 0x01 || type == 0x04 || type == 0x06 ||
            type == 0x0B || type == 0x0C || type == 0xEF) {
            part_add(disk, start, count, i + 1);
        }
    }
}

void block_scan_partitions(void) {
    static block_device_t *disks[MAX_PARTS];
    static u8 sec[BLOCK_SECTOR_SIZE];
    int ndisks = 0;

    /* Snapshot the whole disks before registering any partition wrappers. */
    for (block_device_t *d = g_devices; d && ndisks < MAX_PARTS; d = d->next) {
        disks[ndisks++] = d;
    }

    for (int i = 0; i < ndisks; i++) {
        block_device_t *d = disks[i];
        if (d->sectors < 8) continue;
        if (block_read(d, 0, 1, sec) != 0) continue;
        if (sec[510] != 0x55 || sec[511] != 0xAA) continue;
        if (looks_like_fat(sec)) continue;
        scan_mbr(d, sec);
    }

    /* The boot disk's ESP is the root when it carries one. */
    if (g_part_count > 0 && g_parts[0].disk == g_root) {
        g_root = &g_parts[0].dev;
        kprintf("[block] root volume is partition %s\n", g_root->name);
    }
}

void block_dump(void) {
    for (block_device_t *d = g_devices; d; d = d->next) {
        kprintf("[block] %s: %u sectors (%u MiB), %u-byte sectors\n",
                d->name, (u32)d->sectors,
                (u32)(d->sectors * d->sector_size / MiB), d->sector_size);
    }
}
