/* PID 1 for MCubeAIOS.
 *
 * Runs the syscall/VFS/socket acceptance tests, then forks a shell to verify
 * fork/exec/pipe/wait end to end and hands the console to an interactive
 * shell.
 */

#include <mcube/libc.h>

static long raw3(long n, long a, long b, long c) {
    long r;
    __asm__ __volatile__("syscall"
                         : "=a"(r)
                         : "a"(n), "D"(a), "S"(b), "d"(c)
                         : "rcx", "r11", "cc", "memory");
    return r;
}

static int run_shell_test(void) {
    static char *argv[] = { "sh", "-c", "ls /bin | wc -l", NULL };

    pid_t pid = fork();
    if (pid < 0) {
        printf("USER_SHELL_FAIL: fork\n");
        return -1;
    }
    if (pid == 0) {
        execv("/bin/sh.elf", argv);
        printf("USER_SHELL_FAIL: exec\n");
        _exit(127);
    }

    int status = -1;
    if (waitpid(pid, &status, 0) != pid) {
        printf("USER_SHELL_FAIL: wait\n");
        return -1;
    }
    if (status != 0) {
        printf("USER_SHELL_FAIL: status %d\n", status);
        return -1;
    }
    return 0;
}

/* Exercise shell redirection through a builtin (open + dup2 + restore). */
static int run_redirect_test(void) {
    static char *argv[] = { "sh", "-c", "echo m7-redirect > /tmp/m7.txt", NULL };

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv("/bin/sh.elf", argv);
        _exit(127);
    }
    int status = -1;
    if (waitpid(pid, &status, 0) != pid || status != 0) return -1;

    int fd = open("/tmp/m7.txt", VFS_O_RDONLY);
    if (fd < 3) return -1;
    char buf[32];
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return strcmp(buf, "m7-redirect\n") == 0 ? 0 : -1;
}

/* ---- M9: AI daemon + client end-to-end ------------------------------ */

/* Capture a child's stdout without a pipe: a dead child stays a zombie until
 * waitpid(), and its pipe write end is only released on fd cleanup, so reading
 * to EOF before reaping would deadlock. tmpfs removes the ordering issue. */
static int capture(const char *path, char *const argv[], char *out, int cap) {
    const char *tmp = "/tmp/ai-cap.txt";
    out[0] = 0;
    if (cap < 2) return -1;

    int fd = open(tmp, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 3) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fd);
        return -1;
    }
    if (pid == 0) {
        dup2(fd, 1);
        close(fd);
        execv(path, argv);
        _exit(127);
    }
    close(fd);

    int status = -1;
    if (waitpid(pid, &status, 0) != pid || status != 0) return -1;

    fd = open(tmp, VFS_O_RDONLY);
    if (fd < 3) return -1;
    int n = 0;
    while (n < cap - 1) {
        long r = read(fd, out + n, (size_t)(cap - 1 - n));
        if (r <= 0) break;
        n += (int)r;
    }
    close(fd);
    out[n] = 0;
    unlink(tmp);
    return n;
}

static int wait_ai_ready(void) {
    for (int i = 0; i < 500; i++) {
        mcube_ai_status_t st;
        if (ai_status(&st) == 0 && st.daemon_pid != 0) return 0;
        sleep_ms(10);
    }
    return -1;
}

static void run_ai_tests(void) {
    pid_t pid = fork();
    if (pid < 0) {
        printf("AI_DAEMON_FAIL: fork\n");
        return;
    }
    if (pid == 0) {
        static char *argv[] = { "aiosd", NULL };
        execv("/bin/aiosd.elf", argv);
        printf("AI_DAEMON_EXEC_FAIL\n");
        _exit(127);
    }

    if (wait_ai_ready() != 0) {
        printf("AI_DAEMON_FAIL: timeout\n");
        return;
    }

    char buf[4096];
    static char *tool_argv[] = { "ai", "exec", "system info", NULL };
    int n = capture("/bin/ai.elf", tool_argv, buf, sizeof(buf));
    if (n > 0 && strstr(buf, "-> sys.info")) printf("AI_TOOL_OK\n");
    else printf("AI_TOOL_FAIL\n");

    static char *chat_argv[] = { "ai", "exec", "hello", NULL };
    n = capture("/bin/ai.elf", chat_argv, buf, sizeof(buf));
    if (n > 16) printf("AI_CHAT_OK\n");
    else printf("AI_CHAT_FAIL\n");
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    printf("hello from user mode\n");
    printf("user init pid %d\n", (int)getpid());

    if (raw3(SYS_WRITE, 1, 0x1000, 4) == -EFAULT) printf("USER_EFAULT_OK\n");
    if (raw3(SYS_WRITE, 1, (long)"x", 1L << 21) == -EINVAL) {
        printf("USER_EINVAL_OK\n");
    }

    int fd = open("/TEST.TXT", VFS_O_RDONLY);
    if (fd >= 3) {
        char buf[64];
        long n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n > 0) {
            printf("USER_VFS_OK: ");
            write(1, buf, (size_t)n);
        }
    } else {
        printf("USER_VFS_FAIL\n");
    }

    int dfd = open("/", VFS_O_RDONLY);
    if (dfd >= 3) {
        mcube_dirent_t ent;
        int count = 0;
        while (getdents64(dfd, &ent, sizeof(ent)) > 0) {
            printf("  /%s\n", ent.name);
            if (++count > 32) break;
        }
        close(dfd);
        if (count >= 2) printf("USER_DIR_OK\n");
    }

    mcube_stat_t st;
    if (stat("/kernel.elf", &st) == 0 && st.size > 0) printf("USER_STAT_OK\n");

    int tfd = open("/dev/ttyS0", VFS_O_WRONLY);
    if (tfd >= 3) {
        const char *msg = "USER_TTY_OK\n";
        write(tfd, msg, strlen(msg));
        close(tfd);
    }

    /* BSD socket path: resolve a name through the gateway DNS server. */
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd >= 3) {
        u8 query[32];
        usize qlen = 0;
        static const u8 hdr[12] = { 0x4D, 0x43, 0x01, 0x00, 0x00, 0x01,
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
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = (u16)((53 >> 8) | (53 << 8));
        sa.sin_addr = 10u | (0u << 8) | (2u << 16) | (3u << 24);

        int ok = 0;
        if (sendto(sfd, query, qlen, 0, &sa, sizeof(sa)) == (long)qlen) {
            u8 reply[256];
            for (int tries = 0; tries < 100; tries++) {
                mcube_sockaddr_in_t from;
                u32 alen = sizeof(from);
                long n = recvfrom(sfd, reply, sizeof(reply), 0, &from, &alen);
                if (n > 12) {
                    ok = (reply[0] == 0x4D && reply[1] == 0x43);
                    break;
                }
                sleep_ms(5);
            }
        }
        close(sfd);
        printf(ok ? "USER_NET_OK\n" : "USER_NET_FAIL\n");
    }

    if (run_shell_test() == 0) printf("USER_SHELL_OK\n");
    if (run_redirect_test() == 0) printf("USER_REDIR_OK\n");

    run_ai_tests();

    sleep_ms(40);
    printf("USER_INIT_OK\n");

    /* Start the desktop in the background. The serial console stays with the
     * shell below, which keeps automated tests interactive. */
    pid_t dpid = fork();
    if (dpid == 0) {
        static char *desk_argv[] = { "desktop", NULL };
        execv("/bin/desktop.elf", desk_argv);
        printf("USER_DESKTOP_EXEC_FAIL\n");
        _exit(127);
    }

    /* Hand the console to an interactive shell. */
    static char *sh_argv[] = { "sh", NULL };
    execv("/bin/sh.elf", sh_argv);

    printf("USER_EXEC_FAIL\n");
    _exit(1);
}
