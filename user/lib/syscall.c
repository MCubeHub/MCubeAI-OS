/* Raw syscall entry and POSIX-style wrappers. */

#include <mcube/libc.h>

int errno;

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

static long sys0(long n) { return __syscall6(n, 0, 0, 0, 0, 0, 0); }
static long sys1(long n, long a) { return __syscall6(n, a, 0, 0, 0, 0, 0); }
static long sys2(long n, long a, long b) {
    return __syscall6(n, a, b, 0, 0, 0, 0);
}
static long sys3(long n, long a, long b, long c) {
    return __syscall6(n, a, b, c, 0, 0, 0);
}

static int fail(long rc) {
    if (rc < 0 && rc > -4096) {
        errno = (int)-rc;
        return -1;
    }
    return (int)rc;
}

int open(const char *path, int flags, ...) {
    return fail(sys3(SYS_OPEN, (long)path, flags, 0));
}

int close(int fd) { return fail(sys1(SYS_CLOSE, fd)); }

ssize_t read(int fd, void *buf, size_t len) {
    return (ssize_t)fail(sys3(SYS_READ, fd, (long)buf, (long)len));
}

ssize_t write(int fd, const void *buf, size_t len) {
    return (ssize_t)fail(sys3(SYS_WRITE, fd, (long)buf, (long)len));
}

int stat(const char *path, mcube_stat_t *st) {
    return fail(sys2(SYS_STAT, (long)path, (long)st));
}

int fstat(int fd, mcube_stat_t *st) {
    return fail(sys2(SYS_FSTAT, fd, (long)st));
}

int mkdir(const char *path, int mode) {
    (void)mode;
    return fail(sys1(SYS_MKDIR, (long)path));
}

int rmdir(const char *path) { return fail(sys1(SYS_RMDIR, (long)path)); }

int unlink(const char *path) { return fail(sys1(SYS_UNLINK, (long)path)); }

int getdents64(int fd, mcube_dirent_t *ent, size_t len) {
    return fail(sys3(SYS_GETDENTS64, fd, (long)ent, (long)len));
}

int chdir(const char *path) { return fail(sys1(SYS_CHDIR, (long)path)); }

char *getcwd(char *buf, size_t size) {
    long rc = sys2(SYS_GETCWD, (long)buf, (long)size);
    if (rc < 0) {
        errno = (int)-rc;
        return NULL;
    }
    return buf;
}

int dup(int fd) { return fail(sys1(SYS_DUP, fd)); }
int dup2(int oldfd, int newfd) { return fail(sys2(SYS_DUP2, oldfd, newfd)); }

int pipe(int fds[2]) { return fail(sys1(SYS_PIPE, (long)fds)); }

pid_t getpid(void) { return (pid_t)sys0(SYS_GETPID); }
pid_t getppid(void) { return (pid_t)sys0(SYS_GETPPID); }

pid_t fork(void) { return (pid_t)fail(sys0(SYS_FORK)); }

int execve(const char *path, char *const argv[], char *const envp[]) {
    return fail(sys3(SYS_EXECVE, (long)path, (long)argv, (long)envp));
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    return (pid_t)fail(sys3(SYS_WAIT4, pid, (long)status, options));
}

void sleep_ms(u64 ms) { sys1(SYS_NANOSLEEP, (long)ms); }

void *brk(void *addr) {
    long rc = sys1(SYS_BRK, (long)addr);
    if (rc < 0) {
        errno = (int)-rc;
        return (void *)-1;
    }
    return (void *)rc;
}

void *sbrk(long delta) {
    char *cur = (char *)brk(NULL);
    if (cur == (char *)-1) return (void *)-1;
    return brk(cur + delta);
}

int socket(int domain, int type, int protocol) {
    return fail(sys3(SYS_SOCKET, domain, type, protocol));
}

int bind(int fd, const mcube_sockaddr_in_t *addr, u32 len) {
    return fail(sys3(SYS_BIND, fd, (long)addr, (long)len));
}

int connect(int fd, const mcube_sockaddr_in_t *addr, u32 len) {
    return fail(sys3(SYS_CONNECT, fd, (long)addr, (long)len));
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const mcube_sockaddr_in_t *addr, u32 addrlen) {
    return (ssize_t)fail(__syscall6(SYS_SENDTO, fd, (long)buf, (long)len,
                                    flags, (long)addr, (long)addrlen));
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 mcube_sockaddr_in_t *addr, u32 *addrlen) {
    return (ssize_t)fail(__syscall6(SYS_RECVFROM, fd, (long)buf, (long)len,
                                    flags, (long)addr, (long)addrlen));
}
