/* MCubeAIOS shell: tokenising, quoting, variables, pipes, redirection,
 * background jobs and a small set of builtins.
 *
 * This is deliberately small but real: it parses a line into pipeline stages,
 * expands $VAR/$?/$$, forks one child per stage, wires the pipes with dup2 and
 * execs /bin/<name>.elf. Builtins run in the shell process.
 */

#include <mcube/libc.h>

#define MAX_LINE   256
#define MAX_ARGS   32
#define MAX_STAGES 4
#define MAX_JOBS   8
#define MAX_VARS   48

typedef struct {
    char *argv[MAX_ARGS + 1];
    int argc;
    char *in_file;
    char *out_file;
    int append;
} stage_t;

typedef struct {
    pid_t pid;
    char cmd[64];
    int used;
} job_t;

static job_t g_jobs[MAX_JOBS];
static int g_last_status;
static int g_interactive;
static pid_t g_shell_pid;

/* ---- small helpers -------------------------------------------------- */

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static char *skip_space(char *p) {
    while (is_space(*p)) p++;
    return p;
}

static int parse_int(const char *s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* Expand $VAR, ${VAR}, $? and $$ into out. */
static void expand(const char *in, char *out, usize cap) {
    usize o = 0;
    while (*in && o + 1 < cap) {
        if (*in != '$') {
            out[o++] = *in++;
            continue;
        }
        in++;
        char name[64];
        usize n = 0;
        if (*in == '{') {
            in++;
            while (*in && *in != '}' && n + 1 < sizeof(name)) name[n++] = *in++;
            if (*in == '}') in++;
        } else if (*in == '?') {
            in++;
            name[n++] = '?';
        } else if (*in == '$') {
            in++;
            name[n++] = '$';
        } else {
            while ((*in >= 'A' && *in <= 'Z') || (*in >= 'a' && *in <= 'z') ||
                   (*in >= '0' && *in <= '9') || *in == '_') {
                if (n + 1 < sizeof(name)) name[n++] = *in;
                in++;
            }
        }
        name[n] = 0;

        const char *val = "";
        char numbuf[16];
        if (n == 1 && name[0] == '?') {
            snprintf(numbuf, sizeof(numbuf), "%d", g_last_status);
            val = numbuf;
        } else if (n == 1 && name[0] == '$') {
            snprintf(numbuf, sizeof(numbuf), "%d", (int)g_shell_pid);
            val = numbuf;
        } else if (n > 0) {
            val = getenv(name);
            if (!val) val = "";
        }
        while (*val && o + 1 < cap) out[o++] = *val++;
    }
    out[o] = 0;
}

/* ---- parsing -------------------------------------------------------- */

static int tokenize(char *line, stage_t *st) {
    st->argc = 0;
    st->in_file = NULL;
    st->out_file = NULL;
    st->append = 0;

    static char expanded[MAX_LINE * 2];
    char *p = line;

    while (*p) {
        p = skip_space(p);
        if (!*p) break;

        char *word;
        char quote = 0;
        usize w = 0;

        if (*p == '<' || *p == '>') {
            /* Redirection with a separate filename. */
            int append = 0;
            char op = *p++;
            if (op == '>' && *p == '>') { append = 1; p++; }
            p = skip_space(p);
            if (!*p) return -1;
            char *fname = p;
            while (*p && !is_space(*p)) p++;
            if (*p) *p++ = 0;
            if (op == '<') st->in_file = fname;
            else { st->out_file = fname; st->append = append; }
            continue;
        }

        word = &expanded[w];
        while (*p && !is_space(*p)) {
            if (*p == '\'' || *p == '"') {
                quote = *p++;
                while (*p && *p != quote) {
                    if (quote == '"' && *p == '\\' && p[1]) p++;
                    expanded[w++] = *p++;
                }
                if (*p == quote) p++;
                continue;
            }
            if (*p == '\\' && p[1]) {
                p++;
                expanded[w++] = *p++;
                continue;
            }
            if (*p == '>' || *p == '<') break;
            expanded[w++] = *p++;
        }
        expanded[w] = 0;

        static char tmp[MAX_LINE * 2];
        expand(word, tmp, sizeof(tmp));
        if (st->argc < MAX_ARGS) st->argv[st->argc++] = strdup(tmp);
    }

    st->argv[st->argc] = NULL;
    return st->argc;
}

/* Handle "<file", ">file", ">>file" arguments produced by tokenize. */
static void extract_redirections(stage_t *st) {
    int out = 0;
    for (int i = 0; i < st->argc; i++) {
        char *a = st->argv[i];
        if (a[0] == '<' && a[1]) {
            st->in_file = a + 1;
        } else if (a[0] == '>' && a[1] == '>' && a[2]) {
            st->out_file = a + 2;
            st->append = 1;
        } else if (a[0] == '>' && a[1]) {
            st->out_file = a + 1;
        } else if (a[0] == '<' && !a[1]) {
            if (i + 1 < st->argc) st->in_file = st->argv[++i];
        } else if (a[0] == '>' && !a[1]) {
            if (i + 1 < st->argc) st->out_file = st->argv[++i];
        } else {
            st->argv[out++] = a;
        }
    }
    st->argc = out;
    st->argv[out] = NULL;
}

/* ---- command execution ---------------------------------------------- */

static int is_builtin(const char *name) {
    static const char *names[] = {
        "cd", "pwd", "exit", "export", "unset", "jobs", "help", "true",
        "false", "echo", "set", NULL
    };
    for (int i = 0; names[i]; i++) if (strcmp(name, names[i]) == 0) return 1;
    return 0;
}

static int run_builtin(stage_t *st) {
    const char *name = st->argv[0];

    if (strcmp(name, "cd") == 0) {
        const char *dir = st->argc > 1 ? st->argv[1] : "/";
        if (chdir(dir) != 0) {
            fprintf(2, "cd: %s: no such directory\n", dir);
            return 1;
        }
        return 0;
    }
    if (strcmp(name, "pwd") == 0) {
        char buf[VFS_PATH_MAX];
        if (getcwd(buf, sizeof(buf))) printf("%s\n", buf);
        return 0;
    }
    if (strcmp(name, "exit") == 0) {
        exit(st->argc > 1 ? parse_int(st->argv[1]) : g_last_status);
    }
    if (strcmp(name, "export") == 0) {
        for (int i = 1; i < st->argc; i++) {
            char *eq = strchr(st->argv[i], '=');
            if (eq) {
                *eq = 0;
                setenv(st->argv[i], eq + 1, 1);
                *eq = '=';
            }
        }
        return 0;
    }
    if (strcmp(name, "unset") == 0) {
        for (int i = 1; i < st->argc; i++) unsetenv(st->argv[i]);
        return 0;
    }
    if (strcmp(name, "set") == 0) {
        if (environ) {
            for (char **e = environ; *e; e++) printf("%s\n", *e);
        }
        return 0;
    }
    if (strcmp(name, "jobs") == 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (g_jobs[i].used) {
                printf("[%d] pid %d %s\n", i + 1, (int)g_jobs[i].pid,
                       g_jobs[i].cmd);
            }
        }
        return 0;
    }
    if (strcmp(name, "help") == 0) {
        printf("MCubeAIOS shell builtins: cd pwd exit export unset set jobs "
               "help echo true false\n");
        printf("operators: | > >> < ; &   programs live in /bin\n");
        return 0;
    }
    if (strcmp(name, "true") == 0) return 0;
    if (strcmp(name, "false") == 0) return 1;
    if (strcmp(name, "echo") == 0) {
        int newline = 1;
        int start = 1;
        if (st->argc > 1 && strcmp(st->argv[1], "-n") == 0) {
            newline = 0;
            start = 2;
        }
        for (int i = start; i < st->argc; i++) {
            if (i > start) putchar(' ');
            fputs(st->argv[i], 1);
        }
        if (newline) putchar('\n');
        return 0;
    }
    return 0;
}

/* Resolve a command name to an executable path. */
static int resolve(const char *name, char *out, usize cap) {
    if (strchr(name, '/')) {
        snprintf(out, cap, "%s", name);
        return 0;
    }
    snprintf(out, cap, "/bin/%s.elf", name);
    mcube_stat_t st;
    if (stat(out, &st) == 0 && st.type == VFS_TYPE_FILE) return 0;

    snprintf(out, cap, "/bin/%s", name);
    if (stat(out, &st) == 0 && st.type == VFS_TYPE_FILE) return 0;

    snprintf(out, cap, "/%s", name);
    if (stat(out, &st) == 0 && st.type == VFS_TYPE_FILE) return 0;

    return -1;
}

static void spawn_job(stage_t *st, int background) {
    char path[VFS_PATH_MAX];
    if (resolve(st->argv[0], path, sizeof(path)) != 0) {
        fprintf(2, "%s: command not found\n", st->argv[0]);
        g_last_status = 127;
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(2, "fork failed\n");
        g_last_status = 1;
        return;
    }

    if (pid == 0) {
        if (st->in_file) {
            int fd = open(st->in_file, VFS_O_RDONLY);
            if (fd < 0) { fprintf(2, "cannot open %s\n", st->in_file); _exit(1); }
            dup2(fd, 0);
            close(fd);
        }
        if (st->out_file) {
            int flags = VFS_O_WRONLY | VFS_O_CREAT |
                        (st->append ? VFS_O_APPEND : VFS_O_TRUNC);
            int fd = open(st->out_file, flags);
            if (fd < 0) { fprintf(2, "cannot create %s\n", st->out_file); _exit(1); }
            dup2(fd, 1);
            close(fd);
        }
        execv(path, st->argv);
        fprintf(2, "%s: exec failed\n", path);
        _exit(127);
    }

    if (background) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (!g_jobs[i].used) {
                g_jobs[i].used = 1;
                g_jobs[i].pid = pid;
                snprintf(g_jobs[i].cmd, sizeof(g_jobs[i].cmd), "%s",
                         st->argv[0]);
                printf("[%d] %d\n", i + 1, (int)pid);
                break;
            }
        }
        g_last_status = 0;
        return;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == pid) g_last_status = status;
}

static void reap_jobs(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!g_jobs[i].used) continue;
        int status = 0;
        pid_t rc = waitpid(g_jobs[i].pid, &status, 0);
        if (rc == g_jobs[i].pid) {
            printf("[%d] done %s\n", i + 1, g_jobs[i].cmd);
            g_jobs[i].used = 0;
        }
    }
}

static void run_pipeline(stage_t *stages, int nstages, int background) {
    if (nstages == 1 && is_builtin(stages[0].argv[0])) {
        int had_in = 0, had_out = 0;
        if (stages[0].in_file) {
            int fd = open(stages[0].in_file, VFS_O_RDONLY);
            if (fd < 0) { fprintf(2, "cannot open %s\n", stages[0].in_file); g_last_status = 1; return; }
            dup2(fd, 0);
            close(fd);
            had_in = 1;
        }
        if (stages[0].out_file) {
            int flags = VFS_O_WRONLY | VFS_O_CREAT |
                        (stages[0].append ? VFS_O_APPEND : VFS_O_TRUNC);
            int fd = open(stages[0].out_file, flags);
            if (fd < 0) { fprintf(2, "cannot create %s\n", stages[0].out_file); g_last_status = 1; return; }
            dup2(fd, 1);
            close(fd);
            had_out = 1;
        }
        g_last_status = run_builtin(&stages[0]);
        if (had_in) {
            int fd = open("/dev/console", VFS_O_RDONLY);
            if (fd >= 0) { dup2(fd, 0); close(fd); }
        }
        if (had_out) {
            int fd = open("/dev/console", VFS_O_WRONLY);
            if (fd >= 0) { dup2(fd, 1); close(fd); }
        }
        return;
    }

    if (nstages == 1) {
        spawn_job(&stages[0], background);
        return;
    }

    int prev_read = -1;
    for (int i = 0; i < nstages; i++) {
        int fds[2] = { -1, -1 };
        if (i + 1 < nstages && pipe(fds) != 0) {
            fprintf(2, "pipe failed\n");
            return;
        }

        char path[VFS_PATH_MAX];
        if (resolve(stages[i].argv[0], path, sizeof(path)) != 0) {
            fprintf(2, "%s: command not found\n", stages[i].argv[0]);
            g_last_status = 127;
            if (prev_read >= 0) close(prev_read);
            if (fds[0] >= 0) { close(fds[0]); close(fds[1]); }
            return;
        }

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(2, "fork failed\n");
            return;
        }
        if (pid == 0) {
            if (prev_read >= 0) { dup2(prev_read, 0); close(prev_read); }
            if (fds[1] >= 0) { dup2(fds[1], 1); close(fds[1]); }
            if (fds[0] >= 0) close(fds[0]);
            if (stages[i].in_file) {
                int fd = open(stages[i].in_file, VFS_O_RDONLY);
                if (fd >= 0) { dup2(fd, 0); close(fd); }
            }
            if (stages[i].out_file) {
                int flags = VFS_O_WRONLY | VFS_O_CREAT |
                            (stages[i].append ? VFS_O_APPEND : VFS_O_TRUNC);
                int fd = open(stages[i].out_file, flags);
                if (fd >= 0) { dup2(fd, 1); close(fd); }
            }
            execv(path, stages[i].argv);
            _exit(127);
        }

        if (prev_read >= 0) close(prev_read);
        if (fds[1] >= 0) close(fds[1]);
        prev_read = fds[0];
    }
    if (prev_read >= 0) close(prev_read);

    for (int i = 0; i < nstages; i++) {
        int status = 0;
        if (waitpid(0, &status, 0) > 0) g_last_status = status;
    }
}

/* ---- line execution -------------------------------------------------- */

static void run_line(char *line) {
    /* Split on ';' at the top level (quotes are handled by tokenize). */
    char *cmd = line;
    while (cmd) {
        char *semi = strchr(cmd, ';');
        if (semi) { *semi = 0; }

        stage_t stages[MAX_STAGES];
        int nstages = 0;
        char *stage_src = cmd;
        int background = 0;

        while (stage_src && nstages < MAX_STAGES) {
            char *bar = strchr(stage_src, '|');
            if (bar) *bar = 0;

            char *s = skip_space(stage_src);
            usize len = strlen(s);
            while (len > 0 && is_space(s[len - 1])) s[--len] = 0;
            if (len > 0 && s[len - 1] == '&') {
                background = 1;
                s[--len] = 0;
                while (len > 0 && is_space(s[len - 1])) s[--len] = 0;
            }

            if (*s) {
                if (tokenize(s, &stages[nstages]) > 0) {
                    extract_redirections(&stages[nstages]);
                    nstages++;
                }
            }
            stage_src = bar ? bar + 1 : NULL;
        }

        if (nstages > 0) run_pipeline(stages, nstages, background);

        /* Free the argument copies. */
        for (int i = 0; i < nstages; i++) {
            for (int a = 0; a < stages[i].argc; a++) free(stages[i].argv[a]);
        }

        cmd = semi ? semi + 1 : NULL;
    }
}

/* ---- interactive loop ------------------------------------------------ */

static int read_line(char *buf, usize cap) {
    usize n = 0;
    for (;;) {
        char c;
        long rc = read(0, &c, 1);
        if (rc == 1) {
            if (c == '\n') {
                buf[n] = 0;
                return (int)n;
            }
            if (c == '\r') continue;
            if (c == 4) {  /* Ctrl-D */
                if (n == 0) return -1;
                buf[n] = 0;
                return (int)n;
            }
            if (c == 8 || c == 127) {
                if (n > 0) {
                    n--;
                    fputs("\b \b", 1);
                }
                continue;
            }
            if (n + 1 < cap) {
                buf[n++] = c;
                fputc(c, 1);
            }
            continue;
        }
        if (rc < 0 && errno == EAGAIN) {
            sleep_ms(2);
            continue;
        }
        if (n == 0) return -1;
        buf[n] = 0;
        return (int)n;
    }
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    g_shell_pid = getpid();
    g_interactive = 1;

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        g_interactive = 0;
        char buf[MAX_LINE];
        snprintf(buf, sizeof(buf), "%s", argv[2]);
        run_line(buf);
        return g_last_status;
    }

    if (argc >= 2) {
        int fd = open(argv[1], VFS_O_RDONLY);
        if (fd < 0) {
            fprintf(2, "sh: cannot open %s\n", argv[1]);
            return 1;
        }
        g_interactive = 0;
        char buf[MAX_LINE];
        usize n = 0;
        for (;;) {
            char c;
            long rc = read(fd, &c, 1);
            if (rc <= 0) break;
            if (c == '\n') {
                buf[n] = 0;
                run_line(buf);
                n = 0;
                continue;
            }
            if (n + 1 < sizeof(buf)) buf[n++] = c;
        }
        if (n) { buf[n] = 0; run_line(buf); }
        close(fd);
        return g_last_status;
    }

    printf("MCubeAIOS shell 0.1 -- type 'help' for builtins\n");
    char buf[MAX_LINE];
    for (;;) {
        reap_jobs();
        printf("mcube$ ");
        int n = read_line(buf, sizeof(buf));
        if (n < 0) {
            putchar('\n');
            break;
        }
        if (n == 0) continue;
        run_line(buf);
    }
    return g_last_status;
}
