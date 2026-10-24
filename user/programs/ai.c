/* ai - command line client for the MCubeAIOS AI service (M9).
 *
 *   ai "hello"                 single turn (tools + confirmation enabled)
 *   ai chat                    multi-turn REPL against one session
 *   ai exec "system info"      non-interactive; writes auto-deny unless --yes
 *   ai status | tools | model | audit | memory [clear]
 */

#include <mcube/libc.h>

static int g_req = -1;
static int g_resp = -1;
static u32 g_session;
static int g_yes;
static int g_interactive;

static int send_req(u32 type, const char *text) {
    ai_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.session = g_session;
    if (text) {
        u32 n = (u32)strlen(text);
        if (n > AI_MSG_DATA) n = AI_MSG_DATA;
        memcpy(m.data, text, n);
        m.len = n;
    }
    return ai_send(g_req, &m) == (int)sizeof(m) ? 0 : -1;
}

static int read_line(char *buf, int cap) {
    int n = 0;
    for (;;) {
        char c;
        long r = read(0, &c, 1);
        if (r <= 0) {
            if (n == 0) return -1;
            break;
        }
        if (c == '\n') break;
        if (c != '\r' && n < cap - 1) buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

static int ask_confirm(const char *what) {
    if (g_yes) {
        printf("approve %s: yes (--yes)\n", what);
        return 1;
    }
    if (!g_interactive) {
        printf("approve %s: denied (non-interactive, use --yes)\n", what);
        return 0;
    }
    printf("approve %s? [y/N] ", what);
    char line[16];
    int n = read_line(line, sizeof(line));
    return n > 0 && (line[0] == 'y' || line[0] == 'Y');
}

/* Read events until DONE/ERROR. Returns 1 when the daemon answered. */
static int read_reply(void) {
    ai_msg_t m;
    char last = 0;
    int printed = 0;

    for (;;) {
        int rc = ai_recv(g_resp, &m);
        if (rc <= 0) {
            printf("ai: daemon disconnected\n");
            return -1;
        }

        u32 n = m.len;
        if (n > AI_MSG_DATA - 1) n = AI_MSG_DATA - 1;
        m.data[n] = 0;

        switch (m.type) {
        case AI_EVT_TOKEN:
        case AI_EVT_INFO:
        case AI_EVT_STATS:
            if (n) {
                write(1, m.data, n);
                last = m.data[n - 1];
                printed = 1;
            }
            break;
        case AI_EVT_TOOL:
            if (n) {
                write(1, m.data, n);
                last = m.data[n - 1];
                printed = 1;
            }
            if ((m.flags & 2u) && last != '\n') {
                putchar('\n');
                last = '\n';
            }
            break;
        case AI_EVT_CONFIRM: {
            int ok = ask_confirm(m.data);
            ai_msg_t con;
            memset(&con, 0, sizeof(con));
            con.type = AI_REQ_CONFIRM;
            con.session = g_session;
            con.flags = ok ? 1u : 0u;
            ai_send(g_req, &con);
            break;
        }
        case AI_EVT_ERROR:
            if (n) write(1, m.data, n);
            if (last != '\n') putchar('\n');
            return -1;
        case AI_EVT_DONE:
            if (printed && last != '\n') putchar('\n');
            return 1;
        default:
            break;
        }
    }
}

static int open_channel(void) {
    int fds[2];
    int sid = ai_connect(fds);
    if (sid < 0) {
        fprintf(2, "ai: cannot open channel (errno=%d)\n", errno);
        return -1;
    }
    g_req = fds[0];
    g_resp = fds[1];
    g_session = (u32)sid;

    ai_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = AI_REQ_HELLO;
    m.session = g_session;
    if (ai_send(g_req, &m) != (int)sizeof(m)) {
        fprintf(2, "ai: cannot send hello\n");
        return -1;
    }

    int rc = ai_recv_timeout(g_resp, &m, 3000);
    if (rc != 1 || m.type != AI_EVT_INFO) {
        fprintf(2, "ai: aiosd not responding (rc=%d type=%u)\n", rc, m.type);
        return -1;
    }
    return 0;
}

static int join_args(int argc, char **argv, int start, char *out, int cap) {
    int n = 0;
    for (int i = start; i < argc; i++) {
        if (i > start && n < cap - 1) out[n++] = ' ';
        const char *s = argv[i];
        while (*s && n < cap - 1) out[n++] = *s++;
    }
    out[n] = 0;
    return n;
}

static int run_chat(void) {
    if (open_channel() != 0) return 1;

    printf("MCubeAIOS ai chat (empty line or 'exit' quits)\n");
    for (;;) {
        printf("ai> ");
        char line[AI_MSG_DATA];
        int n = read_line(line, sizeof(line));
        if (n < 0) break;
        if (n == 0) continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;
        if (send_req(AI_REQ_CHAT, line) != 0) {
            printf("ai: send failed\n");
            return 1;
        }
        if (read_reply() < 0) return 1;
    }
    return 0;
}

static int run_one(const char *prompt) {
    if (open_channel() != 0) return 1;
    if (send_req(AI_REQ_CHAT, prompt) != 0) {
        printf("ai: send failed\n");
        return 1;
    }
    return read_reply() < 0 ? 1 : 0;
}

static int run_simple(u32 type, const char *arg) {
    if (open_channel() != 0) return 1;
    if (send_req(type, arg) != 0) {
        printf("ai: send failed\n");
        return 1;
    }
    return read_reply() < 0 ? 1 : 0;
}

static void usage(void) {
    printf("usage: ai \"prompt\"            single turn\n"
           "       ai chat                  multi-turn session\n"
           "       ai exec [--yes] \"prompt\" non-interactive\n"
           "       ai status | tools | model | audit\n"
           "       ai memory [clear]\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc < 2) {
        usage();
        return 2;
    }

    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(argv[1], "chat") == 0) {
        g_interactive = 1;
        return run_chat();
    }
    if (strcmp(argv[1], "exec") == 0) {
        int start = 2;
        g_interactive = 0;
        while (start < argc && (strcmp(argv[start], "--yes") == 0 ||
                                strcmp(argv[start], "-y") == 0)) {
            g_yes = 1;
            start++;
        }
        char prompt[AI_MSG_DATA];
        if (join_args(argc, argv, start, prompt, sizeof(prompt)) == 0) {
            usage();
            return 2;
        }
        return run_one(prompt);
    }
    if (strcmp(argv[1], "status") == 0) return run_simple(AI_REQ_STATUS, NULL);
    if (strcmp(argv[1], "tools") == 0) return run_simple(AI_REQ_TOOL_LIST, NULL);
    if (strcmp(argv[1], "model") == 0) return run_simple(AI_REQ_MODEL, "info");
    if (strcmp(argv[1], "audit") == 0) return run_simple(AI_REQ_AUDIT, NULL);
    if (strcmp(argv[1], "memory") == 0) {
        const char *a = (argc > 2 && strcmp(argv[2], "clear") == 0) ? "clear" : "show";
        return run_simple(AI_REQ_MEMORY, a);
    }

    g_interactive = 1;
    char prompt[AI_MSG_DATA];
    if (join_args(argc, argv, 1, prompt, sizeof(prompt)) == 0) {
        usage();
        return 2;
    }
    return run_one(prompt);
}
