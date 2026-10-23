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
#include <mcube/net.h>
#include <mcube/pipe.h>
#include <mcube/mm.h>

/* Selectors: kernel code 0x08, kernel data 0x10, user data 0x18, user code
 * 0x20. STAR encodes both the syscall and sysret pairs. */
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10

#define USER_IO_MAX (1ULL << 20)
#define EXEC_MAX_VEC 32

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

/* Resolve a user path against the task's working directory. */
static int make_path(char *dst, u64 upath) {
    char raw[VFS_PATH_MAX];
    int rc = copy_path_from_user(raw, upath, sizeof(raw));
    if (rc != 0) return rc;

    task_t *t = current_task();
    if (raw[0] == '/' || !t || t->cwd[0] != '/') {
        usize n = strlen(raw);
        if (n >= VFS_PATH_MAX) return -ENAMETOOLONG;
        memcpy(dst, raw, n + 1);
        return 0;
    }

    usize clen = strlen(t->cwd);
    usize rlen = strlen(raw);
    if (clen + 1 + rlen >= VFS_PATH_MAX) return -ENAMETOOLONG;
    memcpy(dst, t->cwd, clen);
    dst[clen] = '/';
    memcpy(dst + clen + 1, raw, rlen + 1);
    return 0;
}

static int fd_alloc(vfs_file_t **files, vfs_file_t *f) {
    if (!files) return -ESRCH;
    task_t *t = current_task();
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!files[i] && !(t && t->socks[i]) && !(t && t->pipes[i])) {
            files[i] = f;
            return i;
        }
    }
    return -ENFILE;
}

static vfs_file_t *fd_get(vfs_file_t **files, int fd) {
    if (!files || fd < 0 || fd >= VFS_MAX_FDS) return NULL;
    return files[fd];
}

/* ---- sockets ------------------------------------------------------- */

static struct net_socket *sock_get(int fd) {
    task_t *t = current_task();
    if (!t || fd < 3 || fd >= VFS_MAX_FDS) return NULL;
    return t->socks[fd];
}

static int fd_alloc_socket(struct net_socket *s) {
    task_t *t = current_task();
    if (!t) return -ESRCH;
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!t->files[i] && !t->socks[i]) {
            t->socks[i] = s;
            return i;
        }
    }
    return -ENFILE;
}

static u32 be32_to_host(u32 v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}

static u16 be16_to_host(u16 v) {
    return (u16)((v >> 8) | (v << 8));
}

static u32 host_to_be32(u32 v) { return be32_to_host(v); }
static u16 host_to_be16(u16 v) { return be16_to_host(v); }

static int sockaddr_from_user(u64 uaddr, u64 addrlen, u32 *ip, u16 *port) {
    mcube_sockaddr_in_t sa;
    if (!uaddr) return -EINVAL;
    if (addrlen < sizeof(sa)) return -EINVAL;
    if (copy_from_user(&sa, (const void *)(uptr)uaddr, sizeof(sa)) != 0) {
        return -EFAULT;
    }
    if (sa.sin_family != AF_INET) return -EAFNOSUPPORT;
    *ip = be32_to_host(sa.sin_addr);
    *port = be16_to_host(sa.sin_port);
    return 0;
}

static long sys_socket(int domain, int type, int protocol) {
    UNUSED(protocol);
    if (domain != AF_INET) return -EAFNOSUPPORT;
    if (type != SOCK_DGRAM && type != SOCK_STREAM) return -EPROTONOSUPPORT;

    struct net_socket *s = net_socket_open(type);
    if (!s) return -ENFILE;

    int fd = fd_alloc_socket(s);
    if (fd < 0) {
        net_socket_close(s);
        return fd;
    }
    return fd;
}

static long sys_bind(int fd, u64 uaddr, u64 addrlen) {
    struct net_socket *s = sock_get(fd);
    if (!s) return -ENOTSOCK;

    u32 ip;
    u16 port;
    int rc = sockaddr_from_user(uaddr, addrlen, &ip, &port);
    if (rc != 0) return rc;
    return net_socket_bind(s, ip, port);
}

static long sys_connect(int fd, u64 uaddr, u64 addrlen) {
    struct net_socket *s = sock_get(fd);
    if (!s) return -ENOTSOCK;

    u32 ip;
    u16 port;
    int rc = sockaddr_from_user(uaddr, addrlen, &ip, &port);
    if (rc != 0) return rc;
    return net_socket_connect(s, ip, port);
}

static long sys_sendto(int fd, u64 ubuf, u64 len, u64 uaddr, u64 addrlen) {
    struct net_socket *s = sock_get(fd);
    if (!s) return -ENOTSOCK;
    if (len > NET_MTU - 8 - 20) return -EINVAL;

    u32 ip = 0;
    u16 port = 0;
    if (uaddr) {
        int rc = sockaddr_from_user(uaddr, addrlen, &ip, &port);
        if (rc != 0) return rc;
    }

    u8 kbuf[NET_MTU];
    if (len && copy_from_user(kbuf, (const void *)(uptr)ubuf, len) != 0) {
        return -EFAULT;
    }

    int rc;
    if (uaddr) rc = net_socket_sendto(s, kbuf, len, ip, port);
    else       rc = net_socket_send(s, kbuf, len);
    return rc;
}

static long sys_recvfrom(int fd, u64 ubuf, u64 len, u64 uaddr, u64 uaddrlen) {
    struct net_socket *s = sock_get(fd);
    if (!s) return -ENOTSOCK;
    if (len > USER_IO_MAX) return -EINVAL;

    u8 kbuf[NET_MTU];
    u64 want = MIN(len, (u64)sizeof(kbuf));
    u32 ip = 0;
    u16 port = 0;

    int rc = net_socket_recvfrom(s, kbuf, want, &ip, &port);
    if (rc < 0) return rc;
    if (copy_to_user((void *)(uptr)ubuf, kbuf, (u64)rc) != 0) return -EFAULT;

    if (uaddr) {
        u32 alen = 0;
        if (copy_from_user(&alen, (const void *)(uptr)uaddrlen, sizeof(alen)) != 0) {
            return -EFAULT;
        }
        if (alen >= sizeof(mcube_sockaddr_in_t)) {
            mcube_sockaddr_in_t sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = host_to_be16(port);
            sa.sin_addr = host_to_be32(ip);
            if (copy_to_user((void *)(uptr)uaddr, &sa, sizeof(sa)) != 0) {
                return -EFAULT;
            }
            alen = sizeof(sa);
            if (copy_to_user((void *)(uptr)uaddrlen, &alen, sizeof(alen)) != 0) {
                return -EFAULT;
            }
        }
    }
    return rc;
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
    int rc = make_path(path, upath);
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
    task_t *t = current_task();
    if (t && fd >= 3 && fd < VFS_MAX_FDS) {
        if (t->socks[fd]) {
            net_socket_close(t->socks[fd]);
            t->socks[fd] = NULL;
            return 0;
        }
        if (t->pipes[fd]) {
            pipe_end_close(t->pipes[fd]);
            t->pipes[fd] = NULL;
            return 0;
        }
    }

    vfs_file_t **files = current_files();
    vfs_file_t *f = fd_get(files, fd);
    if (!f) return -EBADF;
    files[fd] = NULL;
    return vfs_close(f);
}

static long sys_read(int fd, u64 ubuf, u64 len) {
    if (len > USER_IO_MAX) return -EINVAL;
    if (len == 0) return 0;

    task_t *t = current_task();
    if (t && fd >= 0 && fd < VFS_MAX_FDS && t->pipes[fd]) {
        char buf[512];
        long n = pipe_read(t->pipes[fd], buf, MIN(len, (u64)sizeof(buf)));
        if (n <= 0) return n;
        if (copy_to_user((void *)(uptr)ubuf, buf, (u64)n) != 0) return -EFAULT;
        return n;
    }

    vfs_file_t *f = fd_get(current_files(), fd);
    if (f) {
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

    if (fd == 0) {
        char buf[256];
        long n = tty_read(buf, MIN(len, (u64)sizeof(buf)));
        if (n <= 0) return n;
        if (copy_to_user((void *)(uptr)ubuf, buf, (u64)n) != 0) return -EFAULT;
        return n;
    }

    return -EBADF;
}

static long sys_write(int fd, u64 ubuf, u64 len) {
    if (len > USER_IO_MAX) return -EINVAL;

    task_t *t = current_task();
    if (t && fd >= 0 && fd < VFS_MAX_FDS && t->pipes[fd]) {
        char buf[512];
        u64 done = 0;
        while (done < len) {
            u64 chunk = MIN(len - done, (u64)sizeof(buf));
            if (copy_from_user(buf, (const void *)(uptr)(ubuf + done), chunk) != 0) {
                return done ? (long)done : -EFAULT;
            }
            long n = pipe_write(t->pipes[fd], buf, chunk);
            if (n < 0) return done ? (long)done : n;
            done += (u64)n;
        }
        return (long)done;
    }

    vfs_file_t *f = fd_get(current_files(), fd);
    if (!f && (fd == 1 || fd == 2)) return console_write(ubuf, len);
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
    int rc = make_path(path, upath);
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
    int rc = make_path(path, upath);
    if (rc != 0) return rc;
    return vfs_unlink(path);
}

static long sys_mkdir(u64 upath) {
    char path[VFS_PATH_MAX];
    int rc = make_path(path, upath);
    if (rc != 0) return rc;
    return vfs_mkdir(path);
}

static long sys_chdir(u64 upath) {
    char path[VFS_PATH_MAX];
    int rc = make_path(path, upath);
    if (rc != 0) return rc;

    vfs_stat_t st;
    rc = vfs_stat(path, &st);
    if (rc != 0) return rc;
    if (st.type != VFS_TYPE_DIR) return -ENOTDIR;

    task_t *t = current_task();
    if (!t) return -ESRCH;
    strncpy(t->cwd, path, sizeof(t->cwd) - 1);
    t->cwd[sizeof(t->cwd) - 1] = 0;
    return 0;
}

static long sys_getcwd(u64 ubuf, u64 size) {
    task_t *t = current_task();
    if (!t || !ubuf) return -EINVAL;
    usize n = strlen(t->cwd) + 1;
    if (size < n) return -ERANGE;
    if (copy_to_user((void *)(uptr)ubuf, t->cwd, n) != 0) return -EFAULT;
    return (long)n;
}

static NORETURN void sys_exit(int status) {
    task_t *t = current_task();
    if (t) {
        task_set_exit_status(status);
        kprintf("[user] pid %u (%s) exited, status %d\n", t->pid, t->name, status);
    }
    thread_exit();
}

/* ---- process control ----------------------------------------------- */

static long sys_brk(u64 addr) {
    task_t *t = current_task();
    if (!t) return -ESRCH;
    if (addr == 0) return (long)t->brk;
    int rc = user_brk_set(t->pml4_phys, &t->brk, &t->brk_end, addr);
    if (rc != 0) return rc;
    return (long)t->brk;
}

static long sys_fork(syscall_frame_t *f) {
    task_t *t = current_task();
    if (!t || !t->pml4_phys) return -EINVAL;

    u64 child_as = user_as_clone(t->pml4_phys);
    if (!child_as) return -ENOMEM;

    task_t *child = task_create_user_fork(t->name, f, child_as);
    if (!child) {
        user_as_destroy(child_as);
        return -ENOMEM;
    }
    child->ppid = t->pid;
    child->brk = t->brk;
    child->brk_end = t->brk_end;
    memcpy(child->cwd, t->cwd, sizeof(child->cwd));

    /* The child shares the parent's open descriptors (like POSIX fork). */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (t->files[i]) {
            t->files[i]->refs++;
            child->files[i] = t->files[i];
        }
        if (t->socks[i]) {
            net_socket_ref(t->socks[i]);
            child->socks[i] = t->socks[i];
        }
        if (t->pipes[i]) {
            pipe_end_ref(t->pipes[i]);
            child->pipes[i] = t->pipes[i];
        }
    }
    return (long)child->pid;
}

static int copy_user_str(char *dst, usize cap, u64 usrc) {
    for (usize i = 0; i < cap; i++) {
        if (copy_from_user(&dst[i], (const void *)(uptr)(usrc + i), 1) != 0) {
            return -EFAULT;
        }
        if (dst[i] == 0) return 0;
    }
    return -ENAMETOOLONG;
}

static int copy_user_vec(char **kvec, int max, char *buf, usize cap, u64 uvec) {
    if (!uvec) return 0;

    usize used = 0;
    int n = 0;
    while (n < max) {
        u64 ptr = 0;
        if (copy_from_user(&ptr, (const void *)(uptr)(uvec + (u64)n * 8),
                           sizeof(ptr)) != 0) {
            return -EFAULT;
        }
        if (!ptr) break;
        int rc = copy_user_str(buf + used, cap - used, ptr);
        if (rc != 0) return rc;
        kvec[n] = buf + used;
        used += strlen(buf + used) + 1;
        n++;
    }
    kvec[n] = NULL;
    return n;
}

static long sys_execve(u64 upath, u64 uargv, u64 uenvp, syscall_frame_t *f) {
    char path[VFS_PATH_MAX];
    int rc = make_path(path, upath);
    if (rc != 0) return rc;

    vfs_file_t *vf = NULL;
    rc = vfs_open(path, VFS_O_RDONLY, &vf);
    if (rc != 0) return rc;

    vfs_stat_t st;
    rc = vfs_fstat(vf, &st);
    if (rc != 0) { vfs_close(vf); return rc; }
    if (st.type != VFS_TYPE_FILE || st.size < 64 || st.size > (4u << 20)) {
        vfs_close(vf);
        return -EINVAL;
    }

    u8 *image = kmalloc(st.size);
    char *argbuf = kmalloc(2048);
    char *envbuf = kmalloc(2048);
    char *kargv[EXEC_MAX_VEC + 1];
    char *kenvp[EXEC_MAX_VEC + 1];
    if (!image || !argbuf || !envbuf) {
        kfree(image); kfree(argbuf); kfree(envbuf);
        vfs_close(vf);
        return -ENOMEM;
    }

    u64 done = 0;
    while (done < st.size) {
        long n = vfs_read(vf, image + done, st.size - done);
        if (n <= 0) break;
        done += (u64)n;
    }
    vfs_close(vf);
    if (done != st.size) {
        kfree(image); kfree(argbuf); kfree(envbuf);
        return -EIO;
    }

    rc = copy_user_vec(kargv, EXEC_MAX_VEC, argbuf, 2048, uargv);
    if (rc < 0) goto out;
    if (rc == 0) {
        kargv[0] = path;
        kargv[1] = NULL;
    }
    rc = copy_user_vec(kenvp, EXEC_MAX_VEC, envbuf, 2048, uenvp);
    if (rc < 0) goto out;

    u64 entry = 0, rsp = 0, new_as = 0;
    rc = user_exec(image, st.size, kargv, kenvp, &entry, &rsp, &new_as);
    if (rc != 0) goto out;

    task_t *t = current_task();
    u64 old_as = t->pml4_phys;

    t->pml4_phys = new_as;
    t->user_entry = entry;
    t->user_stack_top = USER_STACK_TOP;
    t->brk = user_brk_default();
    t->brk_end = user_brk_default();
    write_cr3(new_as);

    f->user_rip = entry;
    f->user_rsp = rsp;

    if (old_as) user_as_destroy(old_as);
    rc = 0;

out:
    kfree(image);
    kfree(argbuf);
    kfree(envbuf);
    return rc;
}

static long sys_wait4(int pid, u64 ustatus, int options) {
    UNUSED(options);
    if (pid < 0) pid = 0;
    int status = 0;
    int rc = task_wait((u32)pid, &status);
    if (rc < 0) return rc;
    if (ustatus && copy_to_user((void *)(uptr)ustatus, &status,
                                sizeof(status)) != 0) {
        return -EFAULT;
    }
    return rc;
}

static long sys_getppid(void) {
    task_t *t = current_task();
    return t ? (long)t->ppid : -ESRCH;
}

/* ---- pipes and descriptor duplication ------------------------------ */

static int fd_slot_free(task_t *t, int i) {
    return !t->files[i] && !t->socks[i] && !t->pipes[i];
}

static void fd_copy(task_t *t, int from, int to) {
    t->files[to] = t->files[from];
    t->socks[to] = t->socks[from];
    t->pipes[to] = t->pipes[from];

    if (t->files[to]) t->files[to]->refs++;
    else if (t->socks[to]) net_socket_ref(t->socks[to]);
    else if (t->pipes[to]) pipe_end_ref(t->pipes[to]);
}

static void fd_close_slot(task_t *t, int fd) {
    if (t->files[fd]) {
        vfs_close(t->files[fd]);
        t->files[fd] = NULL;
    } else if (t->socks[fd]) {
        net_socket_close(t->socks[fd]);
        t->socks[fd] = NULL;
    } else if (t->pipes[fd]) {
        pipe_end_close(t->pipes[fd]);
        t->pipes[fd] = NULL;
    }
}

static long sys_pipe(u64 ufds) {
    task_t *t = current_task();
    if (!t || !ufds) return -EINVAL;

    struct pipe_end *r = NULL;
    struct pipe_end *w = NULL;
    int rc = pipe_create(&r, &w);
    if (rc != 0) return rc;

    int fds[2] = { -1, -1 };
    for (int i = 3; i < VFS_MAX_FDS && fds[1] < 0; i++) {
        if (!fd_slot_free(t, i)) continue;
        if (fds[0] < 0) fds[0] = i;
        else fds[1] = i;
    }
    if (fds[1] < 0) {
        pipe_end_close(r);
        pipe_end_close(w);
        return -ENFILE;
    }

    t->pipes[fds[0]] = r;
    t->pipes[fds[1]] = w;

    int out[2] = { fds[0], fds[1] };
    if (copy_to_user((void *)(uptr)ufds, out, sizeof(out)) != 0) {
        fd_close_slot(t, fds[0]);
        fd_close_slot(t, fds[1]);
        return -EFAULT;
    }
    return 0;
}

static long sys_dup(int oldfd) {
    task_t *t = current_task();
    if (!t || oldfd < 0 || oldfd >= VFS_MAX_FDS) return -EBADF;
    if (fd_slot_free(t, oldfd)) return -EBADF;

    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (fd_slot_free(t, i)) {
            fd_copy(t, oldfd, i);
            return i;
        }
    }
    return -ENFILE;
}

static long sys_dup2(int oldfd, int newfd) {
    task_t *t = current_task();
    if (!t || oldfd < 0 || oldfd >= VFS_MAX_FDS ||
        newfd < 0 || newfd >= VFS_MAX_FDS) {
        return -EBADF;
    }
    if (fd_slot_free(t, oldfd)) return -EBADF;
    if (oldfd == newfd) return newfd;

    fd_close_slot(t, newfd);
    fd_copy(t, oldfd, newfd);
    return newfd;
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
    case SYS_SOCKET:
        ret = sys_socket((int)f->rdi, (int)f->rsi, (int)f->rdx);
        break;
    case SYS_BIND:
        ret = sys_bind((int)f->rdi, f->rsi, f->rdx);
        break;
    case SYS_CONNECT:
        ret = sys_connect((int)f->rdi, f->rsi, f->rdx);
        break;
    case SYS_SENDTO:
        ret = sys_sendto((int)f->rdi, f->rsi, f->rdx, f->r8, f->r9);
        break;
    case SYS_RECVFROM:
        ret = sys_recvfrom((int)f->rdi, f->rsi, f->rdx, f->r8, f->r9);
        break;
    case SYS_SETSOCKOPT:
    case SYS_GETSOCKOPT:
        ret = -EOPNOTSUPP;
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
        ret = -ENOSYS;
        break;
    case SYS_BRK:
        ret = sys_brk(f->rdi);
        break;
    case SYS_IOCTL:
        ret = -ENOSYS;
        break;
    case SYS_FORK:
    case SYS_VFORK:
        ret = sys_fork(f);
        break;
    case SYS_EXECVE:
        ret = sys_execve(f->rdi, f->rsi, f->rdx, f);
        break;
    case SYS_WAIT4:
        ret = sys_wait4((int)f->rdi, f->rsi, (int)f->rdx);
        break;
    case SYS_GETPPID:
        ret = sys_getppid();
        break;
    case SYS_PIPE:
        ret = sys_pipe(f->rdi);
        break;
    case SYS_DUP:
        ret = sys_dup((int)f->rdi);
        break;
    case SYS_DUP2:
        ret = sys_dup2((int)f->rdi, (int)f->rsi);
        break;
    case SYS_CHDIR:
        ret = sys_chdir(f->rdi);
        break;
    case SYS_GETCWD:
        ret = sys_getcwd(f->rdi, f->rsi);
        break;
    case SYS_RMDIR:
        ret = sys_unlink(f->rdi);
        break;
    case SYS_KILL:
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
