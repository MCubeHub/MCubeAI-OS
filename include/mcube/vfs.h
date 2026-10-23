#pragma once
/* Virtual file system: mounts, inodes, file descriptors.
 *
 * The kernel keeps a small per-task descriptor table. Paths are absolute and
 * resolved by longest-prefix mount matching, so "/tmp/x" lands in the tmpfs
 * mounted at "/tmp" while "/kernel.elf" goes to the FAT32 root.
 */

#include <mcube/types.h>

#define VFS_MAX_FDS   32
#define VFS_PATH_MAX  256
#define VFS_NAME_MAX  64

/* open(2) flags (values match Linux where possible). */
#define VFS_O_RDONLY  0
#define VFS_O_WRONLY  1
#define VFS_O_RDWR    2
#define VFS_O_ACCMODE 3
#define VFS_O_CREAT   0x40
#define VFS_O_TRUNC   0x200
#define VFS_O_APPEND  0x400

enum vfs_type {
    VFS_TYPE_NONE = 0,
    VFS_TYPE_FILE,
    VFS_TYPE_DIR,
    VFS_TYPE_CHR,
};

struct vfs_inode;
struct vfs_file;
struct task;

typedef struct vfs_inode_ops {
    int (*read)(struct vfs_inode *in, u64 off, void *buf, u64 len);
    int (*write)(struct vfs_inode *in, u64 off, const void *buf, u64 len);
    int (*truncate)(struct vfs_inode *in);
    int (*readdir)(struct vfs_inode *in, u32 index, char *name, u64 namesz,
                   u32 *type);
    void (*destroy)(struct vfs_inode *in);
} vfs_inode_ops_t;

typedef struct vfs_inode {
    int type;
    u32 mode;
    u64 size;
    void *priv;
    const vfs_inode_ops_t *ops;
} vfs_inode_t;

typedef struct vfs_file {
    vfs_inode_t *inode;
    u64 offset;
    int flags;
    int refs;
} vfs_file_t;

typedef struct vfs_stat {
    u64 size;
    u32 type;
    u32 mode;
} vfs_stat_t;

/* Directory entry returned by getdents64. Fixed-size and 8-byte aligned. */
typedef struct PACKED vfs_dirent {
    u64 ino;
    u32 type;
    u32 namelen;
    char name[VFS_NAME_MAX];
} vfs_dirent_t;

/* Mount table. A filesystem provides these three operations. */
typedef struct vfs_mount_ops {
    int (*lookup)(void *fs, const char *rel, vfs_inode_t **out);
    int (*create)(void *fs, const char *rel, u32 type, vfs_inode_t **out);
    int (*unlink)(void *fs, const char *rel);
} vfs_mount_ops_t;

int  vfs_init(void);
int  vfs_mount_add(const char *path, int type, void *fs,
                   const vfs_mount_ops_t *ops);

int  vfs_open(const char *path, int flags, vfs_file_t **out);
long vfs_read(vfs_file_t *f, void *buf, u64 len);
long vfs_write(vfs_file_t *f, const void *buf, u64 len);
int  vfs_close(vfs_file_t *f);
int  vfs_stat(const char *path, vfs_stat_t *st);
int  vfs_fstat(vfs_file_t *f, vfs_stat_t *st);
int  vfs_readdir(vfs_file_t *f, u32 index, vfs_dirent_t *out);
int  vfs_unlink(const char *path);
int  vfs_mkdir(const char *path);
void vfs_task_cleanup(struct task *t);
int  vfs_selftest(void);
