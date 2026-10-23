/* FAT32 read/write driver.
 *
 * Supports a FAT32 volume that starts either at LBA 0 (superfloppy, what
 * mformat -F produces) or in the first MBR partition. Only 512-byte sectors
 * are accepted for now, which covers every image the build system creates.
 *
 * The driver is intentionally synchronous and polled: the ATA PIO layer below
 * it is polled too. A single spinlock protects the shared FAT cache and
 * cluster scratch buffer so a preempted syscall cannot corrupt the volume.
 */

#include <mcube/fat32.h>
#include <mcube/kernel.h>
#include <mcube/mm.h>
#include <mcube/syscall.h>

#define ATTR_RO     0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYS    0x04
#define ATTR_VOL    0x08
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20
#define ATTR_LFN    0x0F

#define DIRENT_SIZE 32
#define NO_SLOT     0xFFFFFFFFu

typedef struct PACKED fat32_bpb {
    u8  jmp[3];
    u8  oem[8];
    u16 bps;
    u8  spc;
    u16 reserved;
    u8  nfat;
    u16 root_entries;
    u16 total16;
    u8  media;
    u16 fatsz16;
    u16 spt;
    u16 heads;
    u32 hidden;
    u32 total32;
    u32 fatsz32;
    u16 ext_flags;
    u16 fs_version;
    u32 root_cluster;
    u16 fsinfo;
    u16 backup_boot;
    u8  reserved2[12];
    u8  drive;
    u8  reserved3;
    u8  boot_sig;
    u32 volume_id;
    u8  label[11];
    u8  fs_type[8];
} fat32_bpb_t;

typedef struct PACKED fat32_dirent {
    u8  name[11];
    u8  attr;
    u8  ntres;
    u8  crt_tenth;
    u16 crt_time;
    u16 crt_date;
    u16 acc_date;
    u16 clus_hi;
    u16 wrt_time;
    u16 wrt_date;
    u16 clus_lo;
    u32 size;
} fat32_dirent_t;

/* ---- small helpers ------------------------------------------------- */

static u32 cluster_bytes(const fat32_fs_t *fs) {
    return fs->bps * fs->spc;
}

static u64 cluster_lba(const fat32_fs_t *fs, u32 cluster) {
    return fs->data_start + (u64)(cluster - 2) * fs->spc;
}

static u32 de_cluster(const fat32_dirent_t *de) {
    return ((u32)de->clus_hi << 16) | (u32)de->clus_lo;
}

static int ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static void dirent_name(const fat32_dirent_t *de, char *out, usize n) {
    usize k = 0;
    int b = 8;
    int e = 3;
    while (b > 0 && de->name[b - 1] == ' ') b--;
    while (e > 0 && de->name[8 + e - 1] == ' ') e--;
    for (int i = 0; i < b && k + 1 < n; i++) out[k++] = (char)de->name[i];
    if (e > 0 && k + 1 < n) {
        out[k++] = '.';
        for (int i = 0; i < e && k + 1 < n; i++) out[k++] = (char)de->name[8 + i];
    }
    out[k] = 0;
}

static int make_83(const char *name, u8 out[11]) {
    memset(out, ' ', 11);
    int base = 0, ext = 0;
    int in_ext = 0;

    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c == '.') {
            if (in_ext) return -EINVAL;
            in_ext = 1;
            continue;
        }
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c < 0x20 || c == ' ' || c == '"' || c == '*' || c == '+' ||
            c == ',' || c == '/' || c == ':' || c == ';' || c == '<' ||
            c == '=' || c == '>' || c == '?' || c == '[' || c == '\\' ||
            c == ']' || c == '|') {
            return -EINVAL;
        }
        if (in_ext) {
            if (ext >= 3) return -ENAMETOOLONG;
            out[8 + ext++] = (u8)c;
        } else {
            if (base >= 8) return -ENAMETOOLONG;
            out[base++] = (u8)c;
        }
    }
    if (base == 0) return -EINVAL;
    return 0;
}

/* Split "/a/b/c.txt" into "/a/b" and "c.txt". */
static int split_path(const char *path, char *dir, usize dirsz,
                      char *name, usize namesz) {
    if (!path || path[0] != '/') return -EINVAL;

    usize len = strlen(path);
    if (len == 0 || len >= 256) return -ENAMETOOLONG;

    const char *slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') slash = p;
    }
    if (!slash) return -EINVAL;

    const char *base = slash + 1;
    if (*base == 0) return -EINVAL; /* trailing slash: not a file name */
    if (strlen(base) >= namesz) return -ENAMETOOLONG;

    usize dlen = (usize)(slash - path);
    if (dlen == 0) dlen = 1; /* root */
    if (dlen >= dirsz) return -ENAMETOOLONG;
    memcpy(dir, path, dlen);
    dir[dlen] = 0;
    strcpy(name, base);
    return 0;
}

/* ---- FAT access ---------------------------------------------------- */

static u32 fat_get(fat32_fs_t *fs, u32 cluster) {
    u64 off = (u64)cluster * 4;
    u32 sec = fs->reserved + (u32)(off / fs->bps);
    u32 idx = (u32)(off % fs->bps);
    u32 v;

    if (fs->fat_cache_sector != sec) {
        if (block_read(fs->dev, fs->base_lba + sec, 1, fs->fat_cache) != 0) {
            return FAT32_EOC;
        }
        fs->fat_cache_sector = sec;
    }
    memcpy(&v, fs->fat_cache + idx, sizeof(v));
    return v & 0x0FFFFFFFu;
}

static int fat_set(fat32_fs_t *fs, u32 cluster, u32 value) {
    u64 off = (u64)cluster * 4;
    u32 sec = fs->reserved + (u32)(off / fs->bps);
    u32 idx = (u32)(off % fs->bps);
    u8 sector[512];

    for (u32 copy = 0; copy < fs->nfat; copy++) {
        u64 lba = fs->base_lba + sec + (u64)copy * fs->fatsz;
        if (block_read(fs->dev, lba, 1, sector) != 0) return -EIO;
        u32 old;
        memcpy(&old, sector + idx, sizeof(old));
        u32 merged = (old & 0xF0000000u) | (value & 0x0FFFFFFFu);
        memcpy(sector + idx, &merged, sizeof(merged));
        if (block_write(fs->dev, lba, 1, sector) != 0) return -EIO;
    }
    fs->fat_cache_sector = NO_SLOT;
    return 0;
}

static int cluster_read(fat32_fs_t *fs, u32 cluster) {
    if (cluster < 2 || cluster >= fs->total_clusters + 2) return -EIO;
    return block_read(fs->dev, cluster_lba(fs, cluster), fs->spc,
                      fs->cluster_buf);
}

static int cluster_write(fat32_fs_t *fs, u32 cluster) {
    if (cluster < 2 || cluster >= fs->total_clusters + 2) return -EIO;
    return block_write(fs->dev, cluster_lba(fs, cluster), fs->spc,
                       fs->cluster_buf);
}

static u32 fat_alloc(fat32_fs_t *fs) {
    u32 first = fs->alloc_hint;
    if (first < 2 || first >= fs->total_clusters + 2) first = 2;

    for (u32 pass = 0; pass < 2; pass++) {
        u32 begin = pass ? 2 : first;
        u32 end = pass ? first : fs->total_clusters + 2;
        for (u32 c = begin; c < end; c++) {
            if (fat_get(fs, c) == 0) {
                if (fat_set(fs, c, 0x0FFFFFFFu) != 0) return 0;
                fs->alloc_hint = c + 1;
                return c;
            }
        }
    }
    return 0;
}

static int fat_free_chain(fat32_fs_t *fs, u32 cluster) {
    u32 guard = 0;
    while (cluster >= 2 && cluster < FAT32_EOC &&
           cluster < fs->total_clusters + 2) {
        u32 next = fat_get(fs, cluster);
        if (fat_set(fs, cluster, 0) != 0) return -EIO;
        if (next == cluster) break;
        cluster = next;
        if (++guard > fs->total_clusters) return -EIO;
    }
    return 0;
}

/* Return the cluster at chain index `index` (0-based), or 0. */
static u32 chain_at(fat32_fs_t *fs, u32 first, u32 index) {
    u32 c = first;
    u32 guard = 0;
    while (c >= 2 && c < FAT32_EOC && guard <= index) {
        if (guard == index) return c;
        c = fat_get(fs, c);
        guard++;
    }
    return 0;
}

/* ---- directory access ---------------------------------------------- */

static int dir_slot_lba(fat32_fs_t *fs, u32 dir_cluster, u32 slot,
                        u64 *lba, u32 *off) {
    u32 cb = cluster_bytes(fs);
    u32 per_cluster = cb / DIRENT_SIZE;
    if (per_cluster == 0) return -EIO;

    u32 ci = slot / per_cluster;
    u32 si = slot % per_cluster;
    u32 c = chain_at(fs, dir_cluster, ci);
    if (!c) return -EIO;

    u64 byte_off = (u64)si * DIRENT_SIZE;
    *lba = cluster_lba(fs, c) + byte_off / fs->bps;
    *off = (u32)(byte_off % fs->bps);
    return 0;
}

static int dir_find(fat32_fs_t *fs, u32 dir_cluster, const char *want,
                    fat32_node_t *out, u32 *slot_out) {
    u32 cb = cluster_bytes(fs);
    u32 per_cluster = cb / DIRENT_SIZE;
    u32 slot = 0;
    u32 c = dir_cluster;

    while (c >= 2 && c < FAT32_EOC) {
        if (cluster_read(fs, c) != 0) return -EIO;
        for (u32 i = 0; i < per_cluster; i++, slot++) {
            const fat32_dirent_t *de =
                (const fat32_dirent_t *)(fs->cluster_buf + (u64)i * DIRENT_SIZE);
            if (de->name[0] == 0x00) return -ENOENT;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == ATTR_LFN) continue;
            if (de->attr & ATTR_VOL) continue;

            char name[FAT32_NAME_MAX];
            dirent_name(de, name, sizeof(name));
            if (ci_equal(name, want)) {
                if (out) {
                    memset(out, 0, sizeof(*out));
                    out->cluster = de_cluster(de);
                    out->size = de->size;
                    out->attr = de->attr;
                    out->dir_cluster = dir_cluster;
                    out->dir_index = slot;
                    strncpy(out->name, name, sizeof(out->name) - 1);
                }
                if (slot_out) *slot_out = slot;
                return 0;
            }
        }
        c = fat_get(fs, c);
    }
    return -ENOENT;
}

/* A directory is empty when it contains nothing but "." and "..". */
static int dir_is_empty(fat32_fs_t *fs, u32 dir_cluster) {
    u32 cb = cluster_bytes(fs);
    u32 per_cluster = cb / DIRENT_SIZE;
    u32 c = dir_cluster;

    while (c >= 2 && c < FAT32_EOC) {
        if (cluster_read(fs, c) != 0) return -EIO;
        for (u32 i = 0; i < per_cluster; i++) {
            const fat32_dirent_t *de =
                (const fat32_dirent_t *)(fs->cluster_buf + (u64)i * DIRENT_SIZE);
            if (de->name[0] == 0x00) return 1;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == ATTR_LFN) continue;
            if (de->attr & ATTR_VOL) continue;

            char name[FAT32_NAME_MAX];
            dirent_name(de, name, sizeof(name));
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
            return 0;
        }
        c = fat_get(fs, c);
    }
    return 1;
}

/* Find a free directory slot, extending the directory if necessary. */
static int dir_alloc_slot(fat32_fs_t *fs, u32 dir_cluster, u32 *slot_out) {
    u32 cb = cluster_bytes(fs);
    u32 per_cluster = cb / DIRENT_SIZE;
    u32 slot = 0;
    u32 c = dir_cluster;
    u32 last = dir_cluster;
    u32 free_slot = NO_SLOT;

    while (c >= 2 && c < FAT32_EOC) {
        if (cluster_read(fs, c) != 0) return -EIO;
        for (u32 i = 0; i < per_cluster; i++, slot++) {
            u8 first = fs->cluster_buf[(u64)i * DIRENT_SIZE];
            if (first == 0x00) {
                *slot_out = (free_slot != NO_SLOT) ? free_slot : slot;
                return 0;
            }
            if (first == 0xE5 && free_slot == NO_SLOT) free_slot = slot;
        }
        last = c;
        c = fat_get(fs, c);
    }

    u32 nc = fat_alloc(fs);
    if (!nc) return -ENOSPC;
    if (fat_set(fs, last, nc) != 0) return -EIO;
    if (fat_set(fs, nc, 0x0FFFFFFFu) != 0) return -EIO;
    memset(fs->cluster_buf, 0, cb);
    if (cluster_write(fs, nc) != 0) return -EIO;

    *slot_out = (free_slot != NO_SLOT) ? free_slot : slot;
    return 0;
}

static int dir_write_entry(fat32_fs_t *fs, u32 dir_cluster, u32 slot,
                           const u8 name[11], u32 attr, u32 cluster,
                           u32 size) {
    u64 lba;
    u32 off;
    if (dir_slot_lba(fs, dir_cluster, slot, &lba, &off) != 0) return -EIO;

    u8 sector[512];
    if (block_read(fs->dev, lba, 1, sector) != 0) return -EIO;
    fat32_dirent_t *de = (fat32_dirent_t *)(sector + off);
    memset(de, 0, sizeof(*de));
    memcpy(de->name, name, 11);
    de->attr = (u8)attr;
    de->clus_hi = (u16)(cluster >> 16);
    de->clus_lo = (u16)(cluster & 0xFFFF);
    de->size = size;
    if (block_write(fs->dev, lba, 1, sector) != 0) return -EIO;
    return 0;
}

static int dir_mark_deleted(fat32_fs_t *fs, u32 dir_cluster, u32 slot) {
    u64 lba;
    u32 off;
    if (dir_slot_lba(fs, dir_cluster, slot, &lba, &off) != 0) return -EIO;

    u8 sector[512];
    if (block_read(fs->dev, lba, 1, sector) != 0) return -EIO;
    sector[off] = 0xE5;
    if (block_write(fs->dev, lba, 1, sector) != 0) return -EIO;
    return 0;
}

static int dir_update_entry(fat32_fs_t *fs, const fat32_node_t *node) {
    if (node->dir_cluster == 0) return 0; /* root directory itself */

    u64 lba;
    u32 off;
    if (dir_slot_lba(fs, node->dir_cluster, node->dir_index, &lba, &off) != 0) {
        return -EIO;
    }

    u8 sector[512];
    if (block_read(fs->dev, lba, 1, sector) != 0) return -EIO;
    fat32_dirent_t *de = (fat32_dirent_t *)(sector + off);
    de->clus_hi = (u16)(node->cluster >> 16);
    de->clus_lo = (u16)(node->cluster & 0xFFFF);
    de->size = node->size;
    if (block_write(fs->dev, lba, 1, sector) != 0) return -EIO;
    return 0;
}

/* ---- public API ---------------------------------------------------- */

static int lookup_locked(fat32_fs_t *fs, const char *path, fat32_node_t *out) {
    fat32_node_t cur;
    memset(&cur, 0, sizeof(cur));
    cur.cluster = fs->root_cluster;
    cur.attr = ATTR_DIR;
    cur.dir_cluster = 0;
    cur.dir_index = 0;
    strcpy(cur.name, "/");

    const char *p = path;
    while (*p == '/') p++;
    if (*p == 0) {
        *out = cur;
        return 0;
    }

    while (*p) {
        char comp[FAT32_NAME_MAX];
        usize n = 0;
        while (*p && *p != '/') {
            if (n + 1 >= sizeof(comp)) return -ENAMETOOLONG;
            comp[n++] = *p++;
        }
        comp[n] = 0;
        while (*p == '/') p++;

        if (!(cur.attr & ATTR_DIR)) return -ENOTDIR;

        fat32_node_t next;
        int rc = dir_find(fs, cur.cluster, comp, &next, NULL);
        if (rc != 0) return rc;
        cur = next;
    }

    *out = cur;
    return 0;
}

int fat32_lookup(fat32_fs_t *fs, const char *path, fat32_node_t *out) {
    if (!fs || !fs->mounted || !path || !out) return -EINVAL;
    u64 flags = spin_lock_irqsave(&fs->lock);
    int rc = lookup_locked(fs, path, out);
    spin_unlock_irqrestore(&fs->lock, flags);
    return rc;
}

int fat32_read(fat32_fs_t *fs, const fat32_node_t *node, u64 off,
               void *buf, u64 len) {
    if (!fs || !fs->mounted || !node || !buf) return -EINVAL;
    if (node->attr & ATTR_DIR) return -EISDIR;
    if (off >= node->size) return 0;
    if (len > (u64)node->size - off) len = (u64)node->size - off;
    if (len == 0) return 0;

    u64 flags = spin_lock_irqsave(&fs->lock);
    u32 cb = cluster_bytes(fs);
    u32 idx = (u32)(off / cb);
    u32 in_cluster = (u32)(off % cb);
    u32 c = chain_at(fs, node->cluster, idx);
    u64 done = 0;

    if (!c) {
        spin_unlock_irqrestore(&fs->lock, flags);
        return -EIO;
    }

    while (done < len) {
        if (cluster_read(fs, c) != 0) {
            spin_unlock_irqrestore(&fs->lock, flags);
            return done ? (int)done : -EIO;
        }
        u64 chunk = MIN(len - done, (u64)(cb - in_cluster));
        memcpy((u8 *)buf + done, fs->cluster_buf + in_cluster, chunk);
        done += chunk;
        in_cluster = 0;
        if (done < len) {
            c = fat_get(fs, c);
            if (!c || c >= FAT32_EOC) break;
        }
    }

    spin_unlock_irqrestore(&fs->lock, flags);
    return (int)done;
}

int fat32_write(fat32_fs_t *fs, fat32_node_t *node, u64 off,
                const void *buf, u64 len) {
    if (!fs || !fs->mounted || !node || !buf) return -EINVAL;
    if (node->attr & ATTR_DIR) return -EISDIR;
    if (len == 0) return 0;
    if (off + len < off || off + len > 0xFFFFFFFFULL) return -EINVAL;

    u64 flags = spin_lock_irqsave(&fs->lock);
    u32 cb = cluster_bytes(fs);
    u64 end = off + len;
    u32 need = (u32)((end + cb - 1) / cb);

    if (!node->cluster) {
        u32 first = fat_alloc(fs);
        if (!first) {
            spin_unlock_irqrestore(&fs->lock, flags);
            return -ENOSPC;
        }
        node->cluster = first;
    }

    /* Count and, if required, extend the cluster chain. */
    u32 count = 0;
    u32 last = node->cluster;
    u32 c = node->cluster;
    while (c >= 2 && c < FAT32_EOC) {
        last = c;
        count++;
        if (count >= need) break;
        c = fat_get(fs, c);
    }
    while (count < need) {
        u32 nc = fat_alloc(fs);
        if (!nc) {
            spin_unlock_irqrestore(&fs->lock, flags);
            return -ENOSPC;
        }
        if (fat_set(fs, last, nc) != 0) {
            spin_unlock_irqrestore(&fs->lock, flags);
            return -EIO;
        }
        if (fat_set(fs, nc, 0x0FFFFFFFu) != 0) {
            spin_unlock_irqrestore(&fs->lock, flags);
            return -EIO;
        }
        last = nc;
        count++;
    }

    u32 start_idx = (u32)(off / cb);
    u32 in_cluster = (u32)(off % cb);
    c = chain_at(fs, node->cluster, start_idx);
    u64 done = 0;

    if (!c) {
        spin_unlock_irqrestore(&fs->lock, flags);
        return -EIO;
    }

    while (done < len) {
        if (cluster_read(fs, c) != 0) {
            spin_unlock_irqrestore(&fs->lock, flags);
            return done ? (int)done : -EIO;
        }
        u64 chunk = MIN(len - done, (u64)(cb - in_cluster));
        memcpy(fs->cluster_buf + in_cluster, (const u8 *)buf + done, chunk);
        if (cluster_write(fs, c) != 0) {
            spin_unlock_irqrestore(&fs->lock, flags);
            return done ? (int)done : -EIO;
        }
        done += chunk;
        in_cluster = 0;
        if (done < len) {
            c = fat_get(fs, c);
            if (!c || c >= FAT32_EOC) break;
        }
    }

    if (end > node->size) node->size = (u32)end;
    dir_update_entry(fs, node);

    spin_unlock_irqrestore(&fs->lock, flags);
    return (int)done;
}

int fat32_truncate(fat32_fs_t *fs, fat32_node_t *node) {
    if (!fs || !fs->mounted || !node) return -EINVAL;
    if (node->attr & ATTR_DIR) return -EISDIR;

    u64 flags = spin_lock_irqsave(&fs->lock);
    int rc = 0;
    if (node->cluster) {
        rc = fat_free_chain(fs, node->cluster);
        node->cluster = 0;
    }
    node->size = 0;
    if (dir_update_entry(fs, node) != 0) rc = -EIO;
    spin_unlock_irqrestore(&fs->lock, flags);
    return rc;
}

int fat32_create(fat32_fs_t *fs, const char *path, u32 attr,
                 fat32_node_t *out) {
    if (!fs || !fs->mounted || !path || !out) return -EINVAL;

    char dir[256], name[FAT32_NAME_MAX];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc != 0) return rc;

    u8 n11[11];
    rc = make_83(name, n11);
    if (rc != 0) return rc;

    u64 flags = spin_lock_irqsave(&fs->lock);

    fat32_node_t parent;
    rc = lookup_locked(fs, dir, &parent);
    if (rc != 0) goto out;
    if (!(parent.attr & ATTR_DIR)) {
        rc = -ENOTDIR;
        goto out;
    }

    fat32_node_t existing;
    if (dir_find(fs, parent.cluster, name, &existing, NULL) == 0) {
        rc = -EEXIST;
        goto out;
    }

    u32 slot;
    rc = dir_alloc_slot(fs, parent.cluster, &slot);
    if (rc != 0) goto out;

    u32 cluster = 0;
    if (attr & ATTR_DIR) {
        cluster = fat_alloc(fs);
        if (!cluster) {
            rc = -ENOSPC;
            goto out;
        }

        u32 cb = cluster_bytes(fs);
        memset(fs->cluster_buf, 0, cb);
        fat32_dirent_t *dot = (fat32_dirent_t *)fs->cluster_buf;
        memcpy(dot->name, ".          ", 11);
        dot->attr = ATTR_DIR;
        dot->clus_hi = (u16)(cluster >> 16);
        dot->clus_lo = (u16)(cluster & 0xFFFF);
        fat32_dirent_t *dotdot = (fat32_dirent_t *)(fs->cluster_buf + DIRENT_SIZE);
        memcpy(dotdot->name, "..         ", 11);
        dotdot->attr = ATTR_DIR;
        dotdot->clus_hi = (u16)(parent.cluster >> 16);
        dotdot->clus_lo = (u16)(parent.cluster & 0xFFFF);
        if (cluster_write(fs, cluster) != 0) {
            fat_free_chain(fs, cluster);
            rc = -EIO;
            goto out;
        }
    }

    rc = dir_write_entry(fs, parent.cluster, slot, n11, attr, cluster, 0);
    if (rc != 0) {
        if (cluster) fat_free_chain(fs, cluster);
        goto out;
    }

    memset(out, 0, sizeof(*out));
    out->cluster = cluster;
    out->size = 0;
    out->attr = attr;
    out->dir_cluster = parent.cluster;
    out->dir_index = slot;
    strncpy(out->name, name, sizeof(out->name) - 1);
    rc = 0;

out:
    spin_unlock_irqrestore(&fs->lock, flags);
    return rc;
}

int fat32_readdir(fat32_fs_t *fs, const fat32_node_t *dir, u32 index,
                  fat32_node_t *out) {
    if (!fs || !fs->mounted || !dir || !out) return -EINVAL;
    if (!(dir->attr & ATTR_DIR)) return -ENOTDIR;

    u64 flags = spin_lock_irqsave(&fs->lock);
    u32 cb = cluster_bytes(fs);
    u32 per_cluster = cb / DIRENT_SIZE;
    u32 c = dir->cluster ? dir->cluster : fs->root_cluster;
    u32 ordinal = 0;
    u32 slot = 0;
    int rc = -ENOENT;

    while (c >= 2 && c < FAT32_EOC) {
        if (cluster_read(fs, c) != 0) {
            rc = -EIO;
            break;
        }
        for (u32 i = 0; i < per_cluster; i++, slot++) {
            const fat32_dirent_t *de =
                (const fat32_dirent_t *)(fs->cluster_buf + (u64)i * DIRENT_SIZE);
            if (de->name[0] == 0x00) goto out;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == ATTR_LFN) continue;
            if (de->attr & ATTR_VOL) continue;

            if (ordinal == index) {
                memset(out, 0, sizeof(*out));
                dirent_name(de, out->name, sizeof(out->name));
                out->cluster = de_cluster(de);
                out->size = de->size;
                out->attr = de->attr;
                out->dir_cluster = dir->cluster ? dir->cluster : fs->root_cluster;
                out->dir_index = slot;
                rc = 0;
                goto out;
            }
            ordinal++;
        }
        c = fat_get(fs, c);
    }

out:
    spin_unlock_irqrestore(&fs->lock, flags);
    return rc;
}

int fat32_unlink(fat32_fs_t *fs, const char *path) {
    if (!fs || !fs->mounted || !path) return -EINVAL;

    char dir[256], name[FAT32_NAME_MAX];
    int rc = split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (rc != 0) return rc;

    u64 flags = spin_lock_irqsave(&fs->lock);

    fat32_node_t parent;
    rc = lookup_locked(fs, dir, &parent);
    if (rc != 0) goto out;

    fat32_node_t node;
    rc = dir_find(fs, parent.cluster, name, &node, NULL);
    if (rc != 0) goto out;

    if (node.attr & ATTR_DIR) {
        int empty = dir_is_empty(fs, node.cluster);
        if (empty < 0) {
            rc = empty;
            goto out;
        }
        if (!empty) {
            rc = -ENOTEMPTY;
            goto out;
        }
    }

    if (node.cluster) {
        rc = fat_free_chain(fs, node.cluster);
        if (rc != 0) goto out;
    }
    rc = dir_mark_deleted(fs, parent.cluster, node.dir_index);

out:
    spin_unlock_irqrestore(&fs->lock, flags);
    return rc;
}

int fat32_mount(fat32_fs_t *fs, block_device_t *dev) {
    if (!fs || !dev) return -EINVAL;

    u8 sector[512];
    u64 base = 0;

    if (block_read(dev, 0, 1, sector) != 0) return -EIO;

    if (!(sector[510] == 0x55 && sector[511] == 0xAA)) return -EINVAL;

    /* Superfloppy? The FAT32 boot sector carries the type string at 0x52. */
    if (memcmp(sector + 82, "FAT32", 5) != 0) {
        int found = 0;
        for (int i = 0; i < 4; i++) {
            const u8 *part = sector + 446 + i * 16;
            u8 type = part[4];
            if (type == 0x0B || type == 0x0C) {
                memcpy(&base, part + 8, sizeof(u32));
                found = 1;
                break;
            }
        }
        if (!found) return -EINVAL;
        if (block_read(dev, base, 1, sector) != 0) return -EIO;
        if (memcmp(sector + 82, "FAT32", 5) != 0) return -EINVAL;
    }

    fat32_bpb_t bpb;
    memcpy(&bpb, sector, sizeof(bpb));

    if (bpb.bps != 512) return -EINVAL;
    if (bpb.spc == 0 || (bpb.spc & (bpb.spc - 1)) != 0) return -EINVAL;
    if (bpb.nfat == 0 || bpb.fatsz32 == 0) return -EINVAL;

    u32 total = bpb.total16 ? bpb.total16 : bpb.total32;
    if (total == 0) return -EINVAL;

    u64 meta = (u64)bpb.reserved + (u64)bpb.nfat * bpb.fatsz32;
    if (meta >= total) return -EINVAL;

    memset(fs, 0, sizeof(*fs));
    fs->dev = dev;
    fs->base_lba = base;
    fs->bps = bpb.bps;
    fs->spc = bpb.spc;
    fs->reserved = bpb.reserved;
    fs->nfat = bpb.nfat;
    fs->fatsz = bpb.fatsz32;
    fs->total_sectors = total;
    fs->root_cluster = bpb.root_cluster;
    fs->data_start = base + bpb.reserved + (u64)bpb.nfat * bpb.fatsz32;
    fs->total_clusters = (u32)((total - meta) / bpb.spc);
    fs->fat_cache_sector = NO_SLOT;
    fs->alloc_hint = 2;
    fs->lock.locked = 0;

    fs->cluster_buf = kmalloc((usize)fs->bps * fs->spc);
    if (!fs->cluster_buf) return -ENOMEM;

    if (fs->root_cluster < 2 || fs->root_cluster >= fs->total_clusters + 2) {
        kfree(fs->cluster_buf);
        fs->cluster_buf = NULL;
        return -EINVAL;
    }

    fs->mounted = 1;
    return 0;
}

void fat32_dump(fat32_fs_t *fs) {
    if (!fs || !fs->mounted) {
        kprintf("[fat32] not mounted\n");
        return;
    }
    kprintf("[fat32] %s: bps %u spc %u reserved %u fats %u fatsz %u\n",
            fs->dev->name, fs->bps, fs->spc, fs->reserved, fs->nfat, fs->fatsz);
    kprintf("[fat32] base %u data %u clusters %u root cluster %u (%u MiB)\n",
            (u32)fs->base_lba, fs->data_start, fs->total_clusters,
            fs->root_cluster,
            (u32)((u64)fs->total_clusters * fs->spc * fs->bps / MiB));
}
