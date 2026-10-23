/* Virtual file system core and the three filesystems we boot with.
 *
 *   /      FAT32 on the ATA disk (the ESP image)
 *   /tmp   tmpfs (kernel heap backed)
 *   /dev   device nodes (console, serial, null, zero)
 *
 * Mount lookup uses longest-prefix matching. Inodes are allocated per open
 * and freed on close; filesystems that own their node memory implement the
 * inode destroy hook, others just let the core free the inode wrapper.
 */

#include <mcube/vfs.h>
#include <mcube/fat32.h>
#include <mcube/block.h>
#include <mcube/kernel.h>
#include <mcube/mm.h>
#include <mcube/tty.h>
#include <mcube/sched.h>
#include <mcube/syscall.h>

#define MAX_MOUNTS 8
#define FAT_ATTR_DIR 0x10
#define FAT_ATTR_RO  0x01
#define FAT_ATTR_ARCH 0x20

typedef struct vfs_mount {
    char path[VFS_PATH_MAX];
    int type;
    void *fs;
    const vfs_mount_ops_t *ops;
} vfs_mount_t;

static vfs_mount_t g_mounts[MAX_MOUNTS];
static int g_mount_count;

/* ---- mount table --------------------------------------------------- */

int vfs_mount_add(const char *path, int type, void *fs,
                  const vfs_mount_ops_t *ops) {
    if (!path || !ops || g_mount_count >= MAX_MOUNTS) return -ENOMEM;
    if (strlen(path) >= VFS_PATH_MAX) return -ENAMETOOLONG;

    vfs_mount_t *m = &g_mounts[g_mount_count++];
    memset(m, 0, sizeof(*m));
    strcpy(m->path, path);
    m->type = type;
    m->fs = fs;
    m->ops = ops;
    return 0;
}

static vfs_mount_t *find_mount(const char *path, const char **rel) {
    vfs_mount_t *best = NULL;
    usize best_len = 0;

    for (int i = 0; i < g_mount_count; i++) {
        vfs_mount_t *m = &g_mounts[i];
        usize n = strlen(m->path);
        if (n == 1 && m->path[0] == '/') {
            if (n >= best_len) {
                best = m;
                best_len = n;
            }
            continue;
        }
        if (strncmp(path, m->path, n) == 0 &&
            (path[n] == 0 || path[n] == '/')) {
            if (n >= best_len) {
                best = m;
                best_len = n;
            }
        }
    }

    if (!best) return NULL;
    const char *p = path + best_len;
    while (*p == '/') p++;
    *rel = p;
    return best;
}

static void inode_put(vfs_inode_t *in) {
    if (!in) return;
    if (in->ops && in->ops->destroy) in->ops->destroy(in);
    kfree(in);
}

/* ---- FAT32 filesystem ---------------------------------------------- */

static fat32_fs_t g_fat;

static int fat_inode_from_node(const fat32_node_t *node, vfs_inode_t **out) {
    vfs_inode_t *in = kzalloc(sizeof(*in));
    fat32_node_t *priv = kmalloc(sizeof(*priv));
    if (!in || !priv) {
        kfree(in);
        kfree(priv);
        return -ENOMEM;
    }
    *priv = *node;

    in->type = (node->attr & FAT_ATTR_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    in->mode = (node->attr & FAT_ATTR_RO) ? 0444u : 0644u;
    in->size = node->size;
    in->priv = priv;
    in->ops = NULL; /* set below */
    *out = in;
    return 0;
}

static int fat_i_read(vfs_inode_t *in, u64 off, void *buf, u64 len) {
    return fat32_read(&g_fat, (fat32_node_t *)in->priv, off, buf, len);
}

static int fat_i_write(vfs_inode_t *in, u64 off, const void *buf, u64 len) {
    int rc = fat32_write(&g_fat, (fat32_node_t *)in->priv, off, buf, len);
    if (rc > 0) in->size = ((fat32_node_t *)in->priv)->size;
    return rc;
}

static int fat_i_truncate(vfs_inode_t *in) {
    int rc = fat32_truncate(&g_fat, (fat32_node_t *)in->priv);
    if (rc == 0) in->size = 0;
    return rc;
}

static int fat_i_readdir(vfs_inode_t *in, u32 index, char *name, u64 namesz,
                         u32 *type) {
    fat32_node_t child;
    int rc = fat32_readdir(&g_fat, (fat32_node_t *)in->priv, index, &child);
    if (rc != 0) return rc;
    if (namesz) {
        strncpy(name, child.name, namesz - 1);
        name[namesz - 1] = 0;
    }
    *type = (child.attr & FAT_ATTR_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    return 0;
}

static void fat_i_destroy(vfs_inode_t *in) {
    kfree(in->priv);
}

static const vfs_inode_ops_t g_fat_ops = {
    .read = fat_i_read,
    .write = fat_i_write,
    .truncate = fat_i_truncate,
    .readdir = fat_i_readdir,
    .destroy = fat_i_destroy,
};

static int fat_rel_path(const char *rel, char *path, usize size) {
    usize n = strlen(rel);
    if (n + 2 > size) return -ENAMETOOLONG;
    path[0] = '/';
    memcpy(path + 1, rel, n + 1);
    return 0;
}

static int fat_m_lookup(void *fsv, const char *rel, vfs_inode_t **out) {
    char path[VFS_PATH_MAX];
    int rc = fat_rel_path(rel, path, sizeof(path));
    if (rc != 0) return rc;

    fat32_node_t node;
    rc = fat32_lookup((fat32_fs_t *)fsv, path, &node);
    if (rc != 0) return rc;

    rc = fat_inode_from_node(&node, out);
    if (rc == 0) (*out)->ops = &g_fat_ops;
    return rc;
}

static int fat_m_create(void *fsv, const char *rel, u32 type,
                        vfs_inode_t **out) {
    char path[VFS_PATH_MAX];
    int rc = fat_rel_path(rel, path, sizeof(path));
    if (rc != 0) return rc;

    u32 attr = (type == VFS_TYPE_DIR) ? FAT_ATTR_DIR : FAT_ATTR_ARCH;
    fat32_node_t node;
    rc = fat32_create((fat32_fs_t *)fsv, path, attr, &node);
    if (rc != 0) return rc;

    rc = fat_inode_from_node(&node, out);
    if (rc == 0) (*out)->ops = &g_fat_ops;
    return rc;
}

static int fat_m_unlink(void *fsv, const char *rel) {
    char path[VFS_PATH_MAX];
    int rc = fat_rel_path(rel, path, sizeof(path));
    if (rc != 0) return rc;
    return fat32_unlink((fat32_fs_t *)fsv, path);
}

static const vfs_mount_ops_t g_fat_mount_ops = {
    .lookup = fat_m_lookup,
    .create = fat_m_create,
    .unlink = fat_m_unlink,
};

/* ---- tmpfs ---------------------------------------------------------- */

typedef struct tmpfs_node {
    char name[VFS_NAME_MAX];
    int type;
    u64 size;
    u64 cap;
    u8 *data;
    struct tmpfs_node *parent;
    struct tmpfs_node *children;
    struct tmpfs_node *next;
} tmpfs_node_t;

typedef struct tmpfs_fs {
    tmpfs_node_t *root;
} tmpfs_fs_t;

static tmpfs_fs_t g_tmpfs;

static tmpfs_node_t *tmpfs_child(tmpfs_node_t *dir, const char *name) {
    for (tmpfs_node_t *n = dir->children; n; n = n->next) {
        if (strcmp(n->name, name) == 0) return n;
    }
    return NULL;
}

static int tmpfs_walk(tmpfs_fs_t *fs, const char *rel, tmpfs_node_t **out) {
    tmpfs_node_t *n = fs->root;
    const char *p = rel;

    while (*p) {
        if (n->type != VFS_TYPE_DIR) return -ENOTDIR;
        char comp[VFS_NAME_MAX];
        usize k = 0;
        while (*p && *p != '/') {
            if (k + 1 >= sizeof(comp)) return -ENAMETOOLONG;
            comp[k++] = *p++;
        }
        comp[k] = 0;
        while (*p == '/') p++;

        tmpfs_node_t *c = tmpfs_child(n, comp);
        if (!c) return -ENOENT;
        n = c;
    }
    *out = n;
    return 0;
}

static int tmpfs_split(const char *rel, char *parent, usize psz,
                       char *name, usize nsz) {
    usize len = strlen(rel);
    if (len == 0) return -EINVAL;

    const char *slash = NULL;
    for (const char *p = rel; *p; p++) {
        if (*p == '/') slash = p;
    }

    const char *base = slash ? slash + 1 : rel;
    if (*base == 0) return -EINVAL;
    if (strlen(base) >= nsz) return -ENAMETOOLONG;

    if (slash) {
        usize d = (usize)(slash - rel);
        if (d >= psz) return -ENAMETOOLONG;
        memcpy(parent, rel, d);
        parent[d] = 0;
    } else {
        parent[0] = 0;
    }
    strcpy(name, base);
    return 0;
}

static int tmpfs_inode(tmpfs_node_t *n, vfs_inode_t **out) {
    vfs_inode_t *in = kzalloc(sizeof(*in));
    if (!in) return -ENOMEM;
    in->type = n->type;
    in->size = n->size;
    in->mode = 0644u;
    in->priv = n;
    in->ops = NULL;
    *out = in;
    return 0;
}

static int tmpfs_i_read(vfs_inode_t *in, u64 off, void *buf, u64 len) {
    tmpfs_node_t *n = (tmpfs_node_t *)in->priv;
    if (n->type == VFS_TYPE_DIR) return -EISDIR;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = n->size - off;
    if (n->data) memcpy(buf, n->data + off, len);
    else memset(buf, 0, len);
    return (int)len;
}

static int tmpfs_i_write(vfs_inode_t *in, u64 off, const void *buf, u64 len) {
    tmpfs_node_t *n = (tmpfs_node_t *)in->priv;
    if (n->type == VFS_TYPE_DIR) return -EISDIR;

    u64 end = off + len;
    if (end < off) return -EINVAL;
    if (end > n->cap) {
        u64 cap = n->cap ? n->cap : 64;
        while (cap < end) cap *= 2;
        u8 *data = krealloc(n->data, (usize)cap);
        if (!data) return -ENOMEM;
        if (cap > n->cap) memset(data + n->cap, 0, (usize)(cap - n->cap));
        n->data = data;
        n->cap = cap;
    }

    memcpy(n->data + off, buf, len);
    if (end > n->size) n->size = end;
    in->size = n->size;
    return (int)len;
}

static int tmpfs_i_truncate(vfs_inode_t *in) {
    tmpfs_node_t *n = (tmpfs_node_t *)in->priv;
    if (n->type == VFS_TYPE_DIR) return -EISDIR;
    n->size = 0;
    in->size = 0;
    return 0;
}

static int tmpfs_i_readdir(vfs_inode_t *in, u32 index, char *name, u64 namesz,
                           u32 *type) {
    tmpfs_node_t *n = (tmpfs_node_t *)in->priv;
    if (n->type != VFS_TYPE_DIR) return -ENOTDIR;

    u32 i = 0;
    for (tmpfs_node_t *c = n->children; c; c = c->next, i++) {
        if (i == index) {
            if (namesz) {
                strncpy(name, c->name, namesz - 1);
                name[namesz - 1] = 0;
            }
            *type = (u32)c->type;
            return 0;
        }
    }
    return -ENOENT;
}

static const vfs_inode_ops_t g_tmpfs_ops = {
    .read = tmpfs_i_read,
    .write = tmpfs_i_write,
    .truncate = tmpfs_i_truncate,
    .readdir = tmpfs_i_readdir,
    .destroy = NULL,
};

static int tmpfs_m_lookup(void *fsv, const char *rel, vfs_inode_t **out) {
    tmpfs_node_t *n;
    int rc = tmpfs_walk((tmpfs_fs_t *)fsv, rel, &n);
    if (rc != 0) return rc;
    rc = tmpfs_inode(n, out);
    if (rc == 0) (*out)->ops = &g_tmpfs_ops;
    return rc;
}

static int tmpfs_m_create(void *fsv, const char *rel, u32 type,
                          vfs_inode_t **out) {
    tmpfs_fs_t *fs = (tmpfs_fs_t *)fsv;
    char parent_path[VFS_PATH_MAX];
    char name[VFS_NAME_MAX];
    int rc = tmpfs_split(rel, parent_path, sizeof(parent_path),
                         name, sizeof(name));
    if (rc != 0) return rc;

    tmpfs_node_t *parent;
    rc = tmpfs_walk(fs, parent_path, &parent);
    if (rc != 0) return rc;
    if (parent->type != VFS_TYPE_DIR) return -ENOTDIR;
    if (tmpfs_child(parent, name)) return -EEXIST;

    tmpfs_node_t *n = kzalloc(sizeof(*n));
    if (!n) return -ENOMEM;
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->type = (int)type;
    n->parent = parent;
    n->next = parent->children;
    parent->children = n;

    rc = tmpfs_inode(n, out);
    if (rc != 0) {
        parent->children = n->next;
        kfree(n);
        return rc;
    }
    (*out)->ops = &g_tmpfs_ops;
    return 0;
}

static int tmpfs_m_unlink(void *fsv, const char *rel) {
    tmpfs_fs_t *fs = (tmpfs_fs_t *)fsv;
    char parent_path[VFS_PATH_MAX];
    char name[VFS_NAME_MAX];
    int rc = tmpfs_split(rel, parent_path, sizeof(parent_path),
                         name, sizeof(name));
    if (rc != 0) return rc;

    tmpfs_node_t *parent;
    rc = tmpfs_walk(fs, parent_path, &parent);
    if (rc != 0) return rc;

    tmpfs_node_t **link = &parent->children;
    while (*link && strcmp((*link)->name, name) != 0) link = &(*link)->next;
    if (!*link) return -ENOENT;

    tmpfs_node_t *n = *link;
    if (n->type == VFS_TYPE_DIR && n->children) return -ENOTEMPTY;

    *link = n->next;
    kfree(n->data);
    kfree(n);
    return 0;
}

static const vfs_mount_ops_t g_tmpfs_mount_ops = {
    .lookup = tmpfs_m_lookup,
    .create = tmpfs_m_create,
    .unlink = tmpfs_m_unlink,
};

/* ---- devfs ---------------------------------------------------------- */

typedef struct dev_node {
    const char *name;
    int (*read)(void *buf, u64 len);
    int (*write)(const void *buf, u64 len);
} dev_node_t;

static int dev_null_read(void *buf, u64 len) {
    UNUSED(buf);
    UNUSED(len);
    return 0;
}

static int dev_null_write(const void *buf, u64 len) {
    UNUSED(buf);
    return (int)len;
}

static int dev_zero_read(void *buf, u64 len) {
    memset(buf, 0, len);
    return (int)len;
}

static int dev_console_read(void *buf, u64 len) {
    return (int)tty_read((char *)buf, len);
}

static int dev_console_write(const void *buf, u64 len) {
    tty_write((const char *)buf, len);
    return (int)len;
}

static int dev_serial_write(const void *buf, u64 len) {
    const char *s = (const char *)buf;
    for (u64 i = 0; i < len; i++) serial_putc(s[i]);
    return (int)len;
}

static const dev_node_t g_devs[] = {
    { "console", dev_console_read, dev_console_write },
    { "tty0",    dev_console_read, dev_console_write },
    { "ttyS0",   dev_console_read, dev_serial_write  },
    { "null",    dev_null_read,    dev_null_write    },
    { "zero",    dev_zero_read,    dev_null_write    },
};

static int dev_i_read(vfs_inode_t *in, u64 off, void *buf, u64 len) {
    UNUSED(off);
    const dev_node_t *d = (const dev_node_t *)in->priv;
    return d->read(buf, len);
}

static int dev_i_write(vfs_inode_t *in, u64 off, const void *buf, u64 len) {
    UNUSED(off);
    const dev_node_t *d = (const dev_node_t *)in->priv;
    return d->write(buf, len);
}

static int dev_i_readdir(vfs_inode_t *in, u32 index, char *name, u64 namesz,
                         u32 *type) {
    UNUSED(in);
    if (index >= ARRAY_SIZE(g_devs)) return -ENOENT;
    if (namesz) {
        strncpy(name, g_devs[index].name, namesz - 1);
        name[namesz - 1] = 0;
    }
    *type = VFS_TYPE_CHR;
    return 0;
}

static const vfs_inode_ops_t g_dev_ops = {
    .read = dev_i_read,
    .write = dev_i_write,
    .truncate = NULL,
    .readdir = dev_i_readdir,
    .destroy = NULL,
};

static int devfs_m_lookup(void *fsv, const char *rel, vfs_inode_t **out) {
    UNUSED(fsv);
    for (usize i = 0; i < ARRAY_SIZE(g_devs); i++) {
        if (strcmp(rel, g_devs[i].name) != 0) continue;
        vfs_inode_t *in = kzalloc(sizeof(*in));
        if (!in) return -ENOMEM;
        in->type = VFS_TYPE_CHR;
        in->mode = 0666u;
        in->size = 0;
        in->priv = (void *)&g_devs[i];
        in->ops = &g_dev_ops;
        *out = in;
        return 0;
    }
    return -ENOENT;
}

static int devfs_m_create(void *fsv, const char *rel, u32 type,
                          vfs_inode_t **out) {
    UNUSED(fsv);
    UNUSED(rel);
    UNUSED(type);
    UNUSED(out);
    return -EPERM;
}

static int devfs_m_unlink(void *fsv, const char *rel) {
    UNUSED(fsv);
    UNUSED(rel);
    return -EPERM;
}

static const vfs_mount_ops_t g_devfs_mount_ops = {
    .lookup = devfs_m_lookup,
    .create = devfs_m_create,
    .unlink = devfs_m_unlink,
};

/* ---- generic file API ---------------------------------------------- */

int vfs_open(const char *path, int flags, vfs_file_t **out) {
    if (!path || path[0] != '/') return -EINVAL;
    if (strlen(path) >= VFS_PATH_MAX) return -ENAMETOOLONG;
    if (!out) return -EINVAL;

    const char *rel = NULL;
    vfs_mount_t *m = find_mount(path, &rel);
    if (!m) return -ENOENT;

    vfs_inode_t *in = NULL;
    int rc = m->ops->lookup(m->fs, rel, &in);
    if (rc != 0) {
        if (rc != -ENOENT || !(flags & VFS_O_CREAT)) return rc;
        rc = m->ops->create(m->fs, rel, VFS_TYPE_FILE, &in);
        if (rc != 0) return rc;
    }

    if (in->type == VFS_TYPE_DIR &&
        (flags & VFS_O_ACCMODE) != VFS_O_RDONLY) {
        inode_put(in);
        return -EISDIR;
    }

    if ((flags & VFS_O_TRUNC) && in->type == VFS_TYPE_FILE &&
        in->ops && in->ops->truncate) {
        rc = in->ops->truncate(in);
        if (rc != 0) {
            inode_put(in);
            return rc;
        }
    }

    vfs_file_t *f = kzalloc(sizeof(*f));
    if (!f) {
        inode_put(in);
        return -ENOMEM;
    }
    f->inode = in;
    f->flags = flags;
    f->refs = 1;
    if ((flags & VFS_O_APPEND) && in->type == VFS_TYPE_FILE) {
        f->offset = in->size;
    }
    *out = f;
    return 0;
}

long vfs_read(vfs_file_t *f, void *buf, u64 len) {
    if (!f || !f->inode) return -EBADF;
    if ((f->flags & VFS_O_ACCMODE) == VFS_O_WRONLY) return -EBADF;
    if (f->inode->type == VFS_TYPE_DIR) return -EISDIR;
    if (!f->inode->ops || !f->inode->ops->read) return -EINVAL;

    int rc = f->inode->ops->read(f->inode, f->offset, buf, len);
    if (rc > 0 && f->inode->type != VFS_TYPE_CHR) f->offset += (u64)rc;
    return rc;
}

long vfs_write(vfs_file_t *f, const void *buf, u64 len) {
    if (!f || !f->inode) return -EBADF;
    if ((f->flags & VFS_O_ACCMODE) == VFS_O_RDONLY) return -EBADF;
    if (f->inode->type == VFS_TYPE_DIR) return -EISDIR;
    if (!f->inode->ops || !f->inode->ops->write) return -EINVAL;

    int rc = f->inode->ops->write(f->inode, f->offset, buf, len);
    if (rc > 0 && f->inode->type != VFS_TYPE_CHR) f->offset += (u64)rc;
    return rc;
}

int vfs_close(vfs_file_t *f) {
    if (!f) return -EBADF;
    if (f->refs > 1) {
        f->refs--;
        return 0;
    }
    inode_put(f->inode);
    kfree(f);
    return 0;
}

int vfs_stat(const char *path, vfs_stat_t *st) {
    if (!path || path[0] != '/' || !st) return -EINVAL;

    const char *rel = NULL;
    vfs_mount_t *m = find_mount(path, &rel);
    if (!m) return -ENOENT;

    vfs_inode_t *in = NULL;
    int rc = m->ops->lookup(m->fs, rel, &in);
    if (rc != 0) return rc;

    st->size = in->size;
    st->type = (u32)in->type;
    st->mode = in->mode;
    inode_put(in);
    return 0;
}

int vfs_fstat(vfs_file_t *f, vfs_stat_t *st) {
    if (!f || !f->inode || !st) return -EBADF;
    st->size = f->inode->size;
    st->type = (u32)f->inode->type;
    st->mode = f->inode->mode;
    return 0;
}

int vfs_readdir(vfs_file_t *f, u32 index, vfs_dirent_t *out) {
    if (!f || !f->inode || !out) return -EBADF;
    if (f->inode->type != VFS_TYPE_DIR) return -ENOTDIR;
    if (!f->inode->ops || !f->inode->ops->readdir) return -EINVAL;

    char name[VFS_NAME_MAX];
    u32 type = 0;
    memset(name, 0, sizeof(name));
    int rc = f->inode->ops->readdir(f->inode, index, name, sizeof(name), &type);
    if (rc != 0) return rc;

    memset(out, 0, sizeof(*out));
    out->ino = index + 1;
    out->type = type;
    out->namelen = (u32)strlen(name);
    strncpy(out->name, name, sizeof(out->name) - 1);
    return 0;
}

int vfs_unlink(const char *path) {
    if (!path || path[0] != '/') return -EINVAL;
    const char *rel = NULL;
    vfs_mount_t *m = find_mount(path, &rel);
    if (!m) return -ENOENT;
    return m->ops->unlink(m->fs, rel);
}

int vfs_mkdir(const char *path) {
    if (!path || path[0] != '/') return -EINVAL;
    const char *rel = NULL;
    vfs_mount_t *m = find_mount(path, &rel);
    if (!m) return -ENOENT;

    vfs_inode_t *in = NULL;
    int rc = m->ops->create(m->fs, rel, VFS_TYPE_DIR, &in);
    if (rc == 0) inode_put(in);
    return rc;
}

void vfs_task_cleanup(struct task *t) {
    task_t *task = (task_t *)t;
    if (!task) return;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (task->files[i]) {
            vfs_close(task->files[i]);
            task->files[i] = NULL;
        }
    }
}

/* ---- boot-time self test ------------------------------------------- */

static int vfs_check_read(const char *path, const char *expect, u64 len) {
    vfs_file_t *f = NULL;
    int rc = vfs_open(path, VFS_O_RDONLY, &f);
    if (rc != 0) return rc;

    char buf[64];
    if (len > sizeof(buf)) len = sizeof(buf);
    long n = vfs_read(f, buf, len);
    vfs_close(f);
    if (n != (long)len) return -EIO;
    if (memcmp(buf, expect, len) != 0) return -EINVAL;
    return 0;
}

static int vfs_check_write(const char *path, const char *text) {
    u64 len = strlen(text);
    vfs_file_t *f = NULL;
    int rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &f);
    if (rc != 0) return rc;

    long n = vfs_write(f, text, len);
    vfs_close(f);
    if (n != (long)len) return -EIO;
    return vfs_check_read(path, text, len);
}

int vfs_selftest(void) {
    char path[VFS_PATH_MAX];

    /* Root directory listing. */
    vfs_file_t *dir = NULL;
    int rc = vfs_open("/", VFS_O_RDONLY, &dir);
    if (rc != 0) {
        kprintf("[vfs] cannot open root: %d\n", rc);
        return rc;
    }

    kprintf("[vfs] root directory:\n");
    for (u32 i = 0;; i++) {
        vfs_dirent_t ent;
        rc = vfs_readdir(dir, i, &ent);
        if (rc == -ENOENT) break;
        if (rc != 0) {
            kprintf("[vfs] readdir failed: %d\n", rc);
            vfs_close(dir);
            return rc;
        }
        kprintf("[vfs]   %-20s type %u\n", ent.name, ent.type);
        if (i > 64) break;
    }
    vfs_close(dir);

    /* The kernel ELF is the first file the loader also read. */
    rc = vfs_check_read("/kernel.elf", "\x7f" "ELF", 4);
    if (rc != 0) {
        kprintf("[vfs] cannot read /kernel.elf: %d\n", rc);
        return rc;
    }
    kprintf("[vfs] /kernel.elf has a valid ELF header\n");

    /* FAT32 create, write and read back. */
    rc = vfs_check_write("/TEST.TXT", "hello from FAT32\n");
    if (rc != 0) {
        kprintf("[vfs] FAT32 write test failed: %d\n", rc);
        return rc;
    }
    kprintf("[vfs] FAT32 write/read-back OK (/TEST.TXT)\n");

    /* tmpfs. */
    rc = vfs_check_write("/tmp/mcube.txt", "tmpfs says hello\n");
    if (rc != 0) {
        kprintf("[vfs] tmpfs write test failed: %d\n", rc);
        return rc;
    }
    kprintf("[vfs] tmpfs write/read-back OK (/tmp/mcube.txt)\n");

    /* Device nodes. */
    char zeros[16];
    vfs_file_t *zf = NULL;
    rc = vfs_open("/dev/zero", VFS_O_RDONLY, &zf);
    if (rc == 0) {
        long n = vfs_read(zf, zeros, sizeof(zeros));
        vfs_close(zf);
        if (n != (long)sizeof(zeros)) rc = -EIO;
        for (usize i = 0; i < sizeof(zeros); i++) {
            if (zeros[i] != 0) rc = -EIO;
        }
    }
    if (rc != 0) {
        kprintf("[vfs] /dev/zero test failed: %d\n", rc);
        return rc;
    }

    const char *msg = "vfs: /dev/ttyS0 write path\n";
    vfs_file_t *sf = NULL;
    rc = vfs_open("/dev/ttyS0", VFS_O_WRONLY, &sf);
    if (rc != 0) {
        kprintf("[vfs] cannot open /dev/ttyS0: %d\n", rc);
        return rc;
    }
    vfs_write(sf, msg, strlen(msg));
    vfs_close(sf);

    /* Stat and directory offsets. */
    vfs_stat_t st;
    rc = vfs_stat("/kernel.elf", &st);
    if (rc != 0 || st.size == 0 || st.type != VFS_TYPE_FILE) {
        kprintf("[vfs] stat failed: %d\n", rc);
        return rc;
    }
    ksnprintf(path, sizeof(path), "[vfs] /kernel.elf size %u, /dev/zero ready\n",
              (u32)st.size);
    kputs(path);

    kprintf("VFS_TEST_OK\n");
    return 0;
}

/* ---- init ----------------------------------------------------------- */

int vfs_init(void) {
    g_mount_count = 0;

    block_device_t *dev = block_root();
    int rc = -ENODEV;
    if (dev) {
        rc = fat32_mount(&g_fat, dev);
        if (rc == 0) {
            fat32_dump(&g_fat);
            vfs_mount_add("/", 1, &g_fat, &g_fat_mount_ops);
            kprintf("[vfs] mounted FAT32 %s at /\n", dev->name);
        } else {
            kprintf("[vfs] FAT32 mount failed: %d\n", rc);
        }
    } else {
        kprintf("[vfs] no block device, root filesystem unavailable\n");
    }

    g_tmpfs.root = kzalloc(sizeof(*g_tmpfs.root));
    if (!g_tmpfs.root) return -ENOMEM;
    g_tmpfs.root->type = VFS_TYPE_DIR;
    g_tmpfs.root->name[0] = 0;
    vfs_mount_add("/tmp", 2, &g_tmpfs, &g_tmpfs_mount_ops);

    vfs_mount_add("/dev", 3, NULL, &g_devfs_mount_ops);

    kprintf("[vfs] mounts: / (fat32), /tmp (tmpfs), /dev (devfs)\n");
    return rc;
}
