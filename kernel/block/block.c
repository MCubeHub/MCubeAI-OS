/* Block-device registry and sector I/O wrappers. */

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

void block_dump(void) {
    for (block_device_t *d = g_devices; d; d = d->next) {
        kprintf("[block] %s: %u sectors (%u MiB), %u-byte sectors\n",
                d->name, (u32)d->sectors,
                (u32)(d->sectors * d->sector_size / MiB), d->sector_size);
    }
}
