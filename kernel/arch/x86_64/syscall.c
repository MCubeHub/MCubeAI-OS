/* System call MSR setup and dispatcher.
 *
 * The heavy lifting (address-space checks, ELF loading) lives in
 * kernel/user/process.c; this file only validates the call number and routes
 * arguments to kernel services. Every pointer is bounded by copy_*_user.
 */

#include <mcube/syscall.h>
#include <mcube/kernel.h>
#include <mcube/io.h>
#include <mcube/sched.h>
#include <mcube/percpu.h>
#include <mcube/user.h>
#include <mcube/timer.h>
#include <mcube/vfs.h>
#include <mcube/tty.h>

/* Selectors: kernel code 0x08, kernel data 0x10, user data 0x18, user code
 * 0x20. STAR encodes both the syscall and sysret pairs. */
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10

#define USER_IO_MAX (1ULL << 20)

extern void syscall_entry(void);

void syscall_init(void) {
    u64 star = ((u64)KERNEL_DS << 48) | ((u64)KERNEL_CS << 32);

    write_msr(MSR_STAR, star);
    write_msr(MSR_LSTAR, (u64)(uptr)syscall_entry);
    /* Clear TF/IF/DF on entry; keep everything else as the user had it. */
    write_msr(MSR_FMASK, 0x700);

    u64 efer = read_msr(MSR_EFER);
    write_msr(MSR_EFER, efer | 1u); /* SCE */

    kprintf("[syscall] armed: STAR=%p LSTAR=%p FMASK=0x700, SCE=1\n",
            (void *)star, (void *)syscall_entry);
}

static int copy_path_from_user(char *dst, u64 usrc, usize max) {
    if (!usrc) return -EFAULT;
    for (usize i = 0; i < max; i++) {
        if (copy_from_user(&dst[i], (const void *)(uptr)(usrc + i), 1) != 0) {
            return -EFAULT;
        }
        if (dst[i] == 0) return 0;
    }
    return -ENAMETOOLONG;
}

static vfs_file_t **current_files(void) {
    task_t *t = current_task();
    return t ? t->files : NULL;
}

static int fd_alloc(vfs_file_t **files, vfs_file_t *f) {
    if (!files) return -ESRCH;
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!files[i]) {
            files[i] = f;
            return i;
        }
    }
    return -ENFILE;
}

static vfs_file_t *fd_get(vfs_file_t **files, int fd) {
    if (!files || fd < 3 || fd >= VFS_MAX_FDS) return NULL;
    return files[fd];
}

/* ---- console ------------------------------------------------------- */

static long console_write(u64 ubuf, u64 len) {
    if (len > USER_IO_MAX) return -EINVAL;

    char buf[256];
    u64 done = 0;
    while (done < len) {
        u64 chunk = MIN(len - done, (u64)sizeof(buf));
        if (copy_from_user(buf, (const void *)(uptr)(ubuf + done), chunk) != 0) {
            return done ? (long)done : -EFAULT;
        }
        for (u64 i = 0; i < chunk; i++) kputc(buf[i]);
        done += chunk;
    }
    return (long)done;
}

/* ---- file descriptor calls ---------------------------------------- */

static long sys_open(u64 upath, int flags) {
    char path[VFS_PATH_MAX];
    int rc = copy_path_from_user(path, upath, sizeof(path));
    if (rc != 0) return rc;

    flags &= (VFS_O_ACCMODE | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND);

    vfs_file_t *f = NULL;
    rc = vfs_open(path, flags, &f);
    if (rc != 0) return rc;

    vfs_file_t **files = current_files();
    int fd = fd_alloc(files, f);
    if (fd < 0) {
        vfs_close(f);
        return fd;
    }
    return fd;
}

static long sys_close(int fd) {
    vfs_file_t **files = current_files();
    vfs_file_t *f = fd_get(files, fd);
    if (!f) return -EBADF;
    files[fd] = NULL;
    return vfs_close(f);
}

static long sys_read(int fd, u64 ubuf, u64 len) {
    if (len > USER_IO_MAX) return -EINVAL;
    if (len == 0) return 0;

    if (fd == 0) {
        char buf[256];
        long n = tty_read(buf, MIN(len, (u64)sizeof(buf)));
        if (n <= 0) return n;
        if (copy_to_user((void *)(uptr)ubuf, buf, (u64)n) != 0) return -EFAULT;
        return n;
    }

    vfs_file_t *f = fd_get(current_files(), fd);
    if (!f) return -EBADF;

    char buf[512];
    u64 done = 0;
    while (done < len) {
        u64 chunk = MIN(len - done, (u64)sizeof(buf));
        long n = vfs_read(f, buf, chunk);
        if (n < 0) return done ? (long)done : n;
        if (n == 0) break;
        if (copy_to_user((void *)(uptr)(ubuf + done), buf, (u64)n) != 0) {
            return done ? (long)done : -EFAULT;
        }
        done += (u64)n;
        if ((u64)n < chunk) break;
    }
    return (long)done;
}

static long sys_write(int fd, u64 ubuf, u64 len) {
    if (len > USER_IO_MAX) return -EINVAL;

    if (fd == 1 || fd == 2) return console_write(ubuf, len);

    vfs_file_t *f = fd_get(current_files(), fd);
    if (!f) return -EBADF;

    char buf[512];
    u64 done = 0;
    while (done < len) {
        u64 chunk = MIN(len - done, (u64)sizeof(buf));
        if (copy_from_user(buf, (const void *)(uptr)(ubuf + done), chunk) != 0) {
            return done ? (long)done : -EFAULT;
        }
        long n = vfs_write(f, buf, chunk);
        if (n < 0) return done ? (long)done : n;
        if (n == 0) break;
        done += (u64)n;
        if ((u64)n < chunk) break;
    }
    return (long)done;
}

static long sys_stat(u64 upath, u64 ustat) {
    char path[VFS_PATH_MAX];
    int rc = copy_path_from_user(path, upath, sizeof(path));
    if (rc != 0) return rc;

    vfs_stat_t st;
    rc = vfs_stat(path, &st);
    if (rc != 0) return rc;
    if (copy_to_user((void *)(uptr)ustat, &st, sizeof(st)) != 0) return -EFAULT;
    return 0;
}

static long sys_fstat(int fd, u64 ustat) {
    vfs_file_t *f = fd_get(current_files(), fd);
    if (!f) return -EBADF;

    vfs_stat_t st;
    int rc = vfs_fstat(f, &st);
    if (rc != 0) return rc;
    if (copy_to_user((void *)(uptr)ustat, &st, sizeof(st)) != 0) return -EFAULT;
    return 0;
}

static long sys_getdents64(int fd, u64 ubuf, u64 len) {
    vfs_file_t *f = fd_get(current_files(), fd);
    if (!f) return -EBADF;

    u64 done = 0;
    while (done + sizeof(vfs_dirent_t) <= len) {
        vfs_dirent_t ent;
        int rc = vfs_readdir(f, (u32)f->offset, &ent);
        if (rc == -ENOENT) break;
        if (rc != 0) return done ? (long)done : rc;
        if (copy_to_user((void *)(uptr)(ubuf + done), &ent, sizeof(ent)) != 0) {
            return done ? (long)done : -EFAULT;
        }
        f->offset++;
        done += sizeof(ent);
    }
    return (long)done;
}

static long sys_unlink(u64 upath) {
    char path[VFS_PATH_MAX];
    int rc = copy_path_from_user(path, upath, sizeof(path));
    if (rc != 0) return rc;
    return vfs_unlink(path);
}

static long sys_mkdir(u64 upath) {
    char path[VFS_PATH_MAX];
    int rc = copy_path_from_user(path, upath, sizeof(path));
    if (rc != 0) return rc;
    return vfs_mkdir(path);
}

static NORETURN void sys_exit(int status) {
    task_t *t = current_task();
    if (t) kprintf("[user] pid %u (%s) exited, status %d\n", t->pid, t->name, status);
    thread_exit();
}

void syscall_handler(syscall_frame_t *f) {
    task_t *t = current_task();
    if (t) t->syscalls++;

    long ret;
    switch (f->nr) {
    case SYS_READ:
        ret = sys_read((int)f->rdi, f->rsi, f->rdx);
        break;
    case SYS_WRITE:
        ret = sys_write((int)f->rdi, f->rsi, f->rdx);
        break;
    case SYS_OPEN:
        ret = sys_open(f->rdi, (int)f->rsi);
        break;
    case SYS_CLOSE:
        ret = sys_close((int)f->rdi);
        break;
    case SYS_STAT:
        ret = sys_stat(f->rdi, f->rsi);
        break;
    case SYS_FSTAT:
        ret = sys_fstat((int)f->rdi, f->rsi);
        break;
    case SYS_GETDENTS64:
        ret = sys_getdents64((int)f->rdi, f->rsi, f->rdx);
        break;
    case SYS_MKDIR:
        ret = sys_mkdir(f->rdi);
        break;
    case SYS_UNLINK:
        ret = sys_unlink(f->rdi);
        break;
    case SYS_GETPID:
    case SYS_GETTID:
        ret = t ? (long)t->pid : -ESRCH;
        break;
    case SYS_NANOSLEEP:
        /* Argument is milliseconds for now; clock_nanosleep arrives with the
         * POSIX libc in M7. */
        if ((i64)f->rdi < 0) {
            ret = -EINVAL;
        } else {
            task_sleep_ms(f->rdi ? f->rdi : 1);
            ret = 0;
        }
        break;
    case SYS_YIELD:
        task_yield();
        ret = 0;
        break;
    case SYS_EXIT:
        sys_exit((int)f->rdi);
        __builtin_unreachable();
    case SYS_MMAP:
    case SYS_MUNMAP:
    case SYS_BRK:
    case SYS_IOCTL:
        ret = -ENOSYS;
        break;
    default:
        if (f->nr >= SYS_AI_BASE && f->nr <= SYS_AI_LAST) {
            /* aiosd endpoints are wired up with the AI layer (M9). */
            ret = -ENOSYS;
        } else {
            ret = -ENOSYS;
        }
        break;
    }
    f->nr = (u64)ret;
}
