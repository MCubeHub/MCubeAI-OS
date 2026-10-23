/* Minimal PID 1 for MCubeAIOS.
 *
 * Freestanding, statically linked at USER_BASE. It exercises the syscall ABI:
 * console write, getpid, invalid-pointer and oversized-length rejection,
 * nanosleep and exit. Real init/exec arrives with the VFS in M5.
 */

#include <mcube/syscall.h>

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

    sys1(SYS_NANOSLEEP, 40);
    swrite("USER_INIT_OK\n");

    sys1(SYS_EXIT, 0);
    for (;;) { }
}
