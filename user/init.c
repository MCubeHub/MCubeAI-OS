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

static long sys6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    long r;
    __asm__ __volatile__("syscall"
                         : "=a"(r)
                         : "a"(n), "D"(a), "S"(b), "d"(c),
                           "r"(r10), "r"(r8), "r"(r9)
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

    /* BSD socket path: ask the user-mode gateway DNS server to resolve a
     * name. This exercises socket/bind/sendto/recvfrom/close end to end. */
    long sfd = sys3(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0);
    if (sfd >= 3) {
        u8 query[32];
        u64 qlen = 0;
        const u8 hdr[12] = { 0x4D, 0x43, 0x01, 0x00, 0x00, 0x01,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        for (int i = 0; i < 12; i++) query[qlen++] = hdr[i];
        const char *name = "example.com";
        while (*name) {
            const char *dot = name;
            while (*dot && *dot != '.') dot++;
            query[qlen++] = (u8)(dot - name);
            while (name < dot) query[qlen++] = (u8)*name++;
            if (*dot == '.') name = dot + 1;
        }
        query[qlen++] = 0;
        query[qlen++] = 0; query[qlen++] = 1;
        query[qlen++] = 0; query[qlen++] = 1;

        mcube_sockaddr_in_t sa;
        for (u64 i = 0; i < sizeof(sa); i++) ((u8 *)&sa)[i] = 0;
        sa.sin_family = AF_INET;
        sa.sin_port = (u16)((53 >> 8) | (53 << 8));
        sa.sin_addr = 10u | (0u << 8) | (2u << 16) | (3u << 24);

        long sent = sys6(SYS_SENDTO, sfd, (long)query, (long)qlen,
                         0, (long)&sa, (long)sizeof(sa));
        int ok = 0;
        if (sent == (long)qlen) {
            u8 reply[256];
            mcube_sockaddr_in_t from;
            for (int tries = 0; tries < 100; tries++) {
                u32 alen = sizeof(from);
                long n = sys6(SYS_RECVFROM, sfd, (long)reply,
                              (long)sizeof(reply), 0, (long)&from,
                              (long)&alen);
                if (n > 12) {
                    ok = (reply[0] == 0x4D && reply[1] == 0x43);
                    break;
                }
                sys1(SYS_NANOSLEEP, 5);
            }
        }
        sys3(SYS_CLOSE, sfd, 0, 0);
        if (ok) swrite("USER_NET_OK\n");
        else swrite("USER_NET_FAIL\n");
    }

    sys1(SYS_NANOSLEEP, 40);
    swrite("USER_INIT_OK\n");

    sys1(SYS_EXIT, 0);
    for (;;) { }
}
