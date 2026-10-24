/* User-side AI channel helpers (M9). */

#include <mcube/libc.h>
#include <mcube/ai.h>

static long __syscall6(long n, long a1, long a2, long a3, long a4, long a5,
                       long a6) {
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    long ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                           "r"(r10), "r"(r8), "r"(r9)
                         : "rcx", "r11", "cc", "memory");
    return ret;
}

static int fail(long rc) {
    if (rc < 0 && rc > -4096) {
        errno = (int)-rc;
        return -1;
    }
    return (int)rc;
}

int ai_connect(int fds[2]) {
    if (!fds) return -1;
    return fail(__syscall6(SYS_AI_CONNECT, (long)fds, 0, 0, 0, 0, 0));
}

int ai_accept(int fds[2]) {
    if (!fds) return -1;
    return fail(__syscall6(SYS_AI_ACCEPT, (long)fds, 0, 0, 0, 0, 0));
}

int ai_status(mcube_ai_status_t *st) {
    if (!st) return -1;
    return fail(__syscall6(SYS_AI_STATUS, (long)st, 0, 0, 0, 0, 0));
}

int ai_audit(mcube_ai_audit_t *out, int max) {
    if (!out || max <= 0) return -1;
    if (max > 64) max = 64;
    return fail(__syscall6(SYS_AI_AUDIT, (long)out, (long)max, 0, 0, 0, 0));
}

int ai_log(u32 kind, const char *text) {
    if (!text) return -1;
    return fail(__syscall6(SYS_AI_LOG, (long)kind, (long)text, 0, 0, 0, 0));
}

int ai_send(int fd, ai_msg_t *msg) {
    if (fd < 0 || !msg) return -1;
    const u8 *p = (const u8 *)msg;
    usize sent = 0;
    while (sent < sizeof(*msg)) {
        long n = write(fd, p + sent, sizeof(*msg) - sent);
        if (n <= 0) return -1;
        sent += (usize)n;
    }
    return (int)sent;
}

int ai_recv(int fd, ai_msg_t *msg) {
    if (fd < 0 || !msg) return -1;

    /* The kernel pipe path moves data in 512-byte chunks, so a single read()
     * may only deliver part of a 528-byte message. Gather exactly one. */
    u8 *p = (u8 *)msg;
    usize got = 0;
    while (got < sizeof(*msg)) {
        long n = read(fd, p + got, sizeof(*msg) - got);
        if (n == 0) return got ? -1 : 0;
        if (n < 0) return -1;
        got += (usize)n;
    }
    return 1;
}

/* Wait for a message without blocking forever: 1 message, 0 timeout or EOF,
 * -1 error. Used by clients so they never hang if aiosd is not up yet. */
int ai_recv_timeout(int fd, ai_msg_t *msg, int timeout_ms) {
    if (fd < 0 || !msg) return -1;
    mcube_pollfd_t p;
    p.fd = fd;
    p.events = MCUBE_POLLIN;
    p.revents = 0;
    int rc = poll(&p, 1, timeout_ms);
    if (rc <= 0) return rc;
    return ai_recv(fd, msg);
}
