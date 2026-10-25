#pragma once
/* Generic block-device layer. Sector size is fixed at 512 bytes for now;
 * the interface is shared by ATA PIO, AHCI and NVMe drivers. */

#include <mcube/types.h>

#define BLOCK_SECTOR_SIZE 512

typedef struct block_device block_device_t;

struct block_device {
    char name[16];
    u64 sectors;
    u32 sector_size;
    int (*read)(block_device_t *dev, u64 lba, u32 count, void *buf);
    int (*write)(block_device_t *dev, u64 lba, u32 count, const void *buf);
    void *priv;
    block_device_t *next;
};

void block_init(void);
void ahci_init(void);
void block_scan_partitions(void);
void block_register(block_device_t *dev);
block_device_t *block_root(void);
block_device_t *block_find(const char *name);
int block_read(block_device_t *dev, u64 lba, u32 count, void *buf);
int block_write(block_device_t *dev, u64 lba, u32 count, const void *buf);
void block_dump(void);
