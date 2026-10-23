/* Minimal PID 1 for MCubeAIOS.
 *
 * Freestanding, statically linked at USER_BASE. It exercises the syscall ABI:
 * console write, getpid, invalid-pointer and oversized-length rejection,
 * nanosleep, FAT32 read, directory listing, stat, and writes through the
 * /dev/ttyS0 device node. Real init/exec arrives with the POSIX libc in M7.
 */

#include <mcube/syscall.h>
#include <mcube/vfs.h>

static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ __volatile__("syscall"
                         : "=a"(r)
                         : "a"(n), "D"(a), "S"(b), "d"(c)
                         : "rcx", "r11", "cc", "memory");
    return r;
}

static long sys1(long n, long a) { return sys3(n, a, 0, 0); }
static long sys0(long n) { return sys3(n, 0, 0, 0); }

static u64 slen(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

static void swrite(const char *s) {
    sys3(SYS_WRITE, 1, (long)s, (long)slen(s));
}

static void swrite_n(const char *s, u64 n) {
    sys3(SYS_WRITE, 1, (long)s, (long)n);
}

static void swrite_dec(long v) {
    char buf[24];
    int i = 0;
    if (v == 0) {
        buf[i++] = '0';
    } else {
        char tmp[24];
        int j = 0;
        if (v < 0) {
            swrite("-");
            v = -v;
        }
        while (v > 0) {
            tmp[j++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (j > 0) buf[i++] = tmp[--j];
    }
    buf[i++] = '\n';
    swrite_n(buf, (u64)i);
}

__attribute__((section(".text.entry"), noreturn))
void _start(void) {
    swrite("hello from user mode\n");

    swrite("user init pid ");
    swrite_dec(sys0(SYS_GETPID));

    long bad = sys3(SYS_WRITE, 1, 0x1000, 4);
    if (bad == -EFAULT) swrite("USER_EFAULT_OK\n");

    long huge = sys3(SYS_WRITE, 1, (long)"x", 1L << 21);
    if (huge == -EINVAL) swrite("USER_EINVAL_OK\n");

    /* Read the file the kernel wrote during its VFS self test. */
    long fd = sys3(SYS_OPEN, (long)"/TEST.TXT", VFS_O_RDONLY, 0);
    if (fd >= 3) {
        char buf[64];
        long n = sys3(SYS_READ, fd, (long)buf, (long)sizeof(buf));
        sys3(SYS_CLOSE, fd, 0, 0);
        if (n > 0) {
            swrite("USER_VFS_OK: ");
            swrite_n(buf, (u64)n);
        }
    } else {
        swrite("USER_VFS_FAIL\n");
    }

    /* Enumerate the FAT32 root through getdents64. */
    long dfd = sys3(SYS_OPEN, (long)"/", VFS_O_RDONLY, 0);
    if (dfd >= 3) {
        mcube_dirent_t ent;
        long n;
        int count = 0;
        while ((n = sys3(SYS_GETDENTS64, dfd, (long)&ent, (long)sizeof(ent))) > 0) {
            swrite("  /");
            swrite_n(ent.name, ent.namelen);
            swrite("\n");
            if (++count > 32) break;
        }
        sys3(SYS_CLOSE, dfd, 0, 0);
        if (count >= 2) swrite("USER_DIR_OK\n");
    }

    mcube_stat_t st;
    if (sys3(SYS_STAT, (long)"/kernel.elf", (long)&st, 0) == 0 && st.size > 0) {
        swrite("USER_STAT_OK\n");
    }

    /* Character device write path. */
    long tfd = sys3(SYS_OPEN, (long)"/dev/ttyS0", VFS_O_WRONLY, 0);
    if (tfd >= 3) {
        const char *msg = "USER_TTY_OK\n";
        sys3(SYS_WRITE, tfd, (long)msg, (long)slen(msg));
        sys3(SYS_CLOSE, tfd, 0, 0);
    }

    sys1(SYS_NANOSLEEP, 40);
    swrite("USER_INIT_OK\n");

    sys1(SYS_EXIT, 0);
    for (;;) { }
}
