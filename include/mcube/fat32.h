#pragma once
/* FAT32 read/write driver (superfloppy or first MBR partition). */

#include <mcube/types.h>
#include <mcube/block.h>
#include <mcube/atomic.h>

#define FAT32_NAME_MAX 128
#define FAT32_EOC      0x0FFFFFF8u

typedef struct fat32_fs {
    block_device_t *dev;
    u64 base_lba;
    u32 bps;             /* bytes per sector (must be 512)         */
    u32 spc;             /* sectors per cluster                    */
    u32 reserved;        /* reserved sectors before the FATs       */
    u32 nfat;            /* number of FAT copies                   */
    u32 fatsz;           /* sectors per FAT                        */
    u32 total_sectors;
    u32 root_cluster;
    u32 data_start;      /* absolute sector of cluster 2           */
    u32 total_clusters;
    u32 fat_cache_sector;
    u32 alloc_hint;
    u8  fat_cache[512];
    u8 *cluster_buf;     /* bps * spc scratch cluster              */
    spinlock_t lock;
    int mounted;
} fat32_fs_t;

typedef struct fat32_node {
    u32 cluster;         /* first cluster, 0 for an empty file      */
    u32 size;
    u32 attr;
    u32 dir_cluster;     /* parent directory cluster (0 = root)     */
    u32 dir_index;       /* raw 32-byte slot index in the parent    */
    char name[FAT32_NAME_MAX];
} fat32_node_t;

int  fat32_mount(fat32_fs_t *fs, block_device_t *dev);
int  fat32_lookup(fat32_fs_t *fs, const char *path, fat32_node_t *out);
int  fat32_read(fat32_fs_t *fs, const fat32_node_t *node, u64 off, void *buf, u64 len);
int  fat32_write(fat32_fs_t *fs, fat32_node_t *node, u64 off, const void *buf, u64 len);
int  fat32_truncate(fat32_fs_t *fs, fat32_node_t *node);
int  fat32_create(fat32_fs_t *fs, const char *path, u32 attr, fat32_node_t *out);
int  fat32_readdir(fat32_fs_t *fs, const fat32_node_t *dir, u32 index, fat32_node_t *out);
int  fat32_unlink(fat32_fs_t *fs, const char *path);
void fat32_dump(fat32_fs_t *fs);
