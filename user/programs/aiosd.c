/* aiosd - resident AI service for MCubeAIOS (M9).
 *
 * The kernel only moves fixed-size ai_msg_t records between the client and
 * this daemon. Everything else (GGUF model, tokenizer, sampler, rule-based
 * tool routing, session memory, permission gate and audit log) lives here in
 * user space, so a crashed model cannot take the kernel down.
 */

#include <mcube/libc.h>
#include <mcube/ai.h>
#include "ai_runtime.h"

/* 8.3 short name: the FAT32 driver does not parse long file names yet. */
#define AI_MODEL_PATH "/models/tiny.ggu"
#define AI_TOOL_OUT   2048
#define AI_PROMPT_MAX 300

static ai_model_t  g_model;
static ai_params_t g_params;
static int         g_model_loaded;

/* ---- transport helpers ---------------------------------------------- */

static int send_msg(int fd, u32 type, u32 session, u32 flags,
                    const char *data, u32 len) {
    ai_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.session = session;
    m.flags = flags;
    if (data && len) {
        if (len > AI_MSG_DATA) len = AI_MSG_DATA;
        memcpy(m.data, data, len);
    }
    m.len = len;
    return ai_send(fd, &m) == (int)sizeof(m) ? 0 : -1;
}

static int send_text(int fd, u32 type, u32 session, const char *text) {
    if (!text) text = "";
    u32 len = (u32)strlen(text);
    if (len == 0) return send_msg(fd, type, session, 0, NULL, 0);

    u32 off = 0;
    while (off < len) {
        u32 chunk = len - off;
        if (chunk > AI_MSG_DATA) chunk = AI_MSG_DATA;
        u32 flags = (off == 0 ? 1u : 0u) | (off + chunk >= len ? 2u : 0u);
        if (send_msg(fd, type, session, flags, text + off, chunk) != 0) return -1;
        off += chunk;
    }
    return 0;
}

static int send_done(int fd, u32 session, int tokens, u64 ms) {
    char buf[96];
    snprintf(buf, sizeof(buf), "tokens=%d ms=%u", tokens, (unsigned)ms);
    return send_msg(fd, AI_EVT_DONE, session, 0, buf, (u32)strlen(buf));
}

/* ---- model prompt + streaming --------------------------------------- */

typedef struct {
    int  fd;
    u32  session;
    int  n;
    int  failed;
    char text[192];
} stream_t;

static int on_token(void *ud, const char *piece, int npiece) {
    stream_t *s = (stream_t *)ud;
    for (int i = 0; i < npiece; i++) {
        if (send_msg(s->fd, AI_EVT_TOKEN, s->session, 0, &piece[i], 1) != 0) {
            s->failed = 1;
            return 1;
        }
        if (s->n < (int)sizeof(s->text) - 1) s->text[s->n++] = piece[i];
    }
    return 0;
}

/* ---- request handlers ----------------------------------------------- */

static void done_or_error(int fd, u32 session, const char *err) {
    if (err) send_text(fd, AI_EVT_ERROR, session, err);
    send_done(fd, session, 0, 0);
}

static void handle_status(int fd, u32 session) {
    mcube_ai_status_t st;
    memset(&st, 0, sizeof(st));
    ai_status(&st);

    char buf[512];
    snprintf(buf, sizeof(buf),
             "daemon pid=%u pending=%u connects=%u accepts=%u\n"
             "model=%s\nloaded=%d path=%s\n"
             "params: temperature=%d/100 top_k=%d max_tokens=%d\n",
             (unsigned)st.daemon_pid, (unsigned)st.pending,
             (unsigned)st.connects, (unsigned)st.accepts,
             g_model_loaded ? "tiny-bigram" : "none", g_model_loaded,
             AI_MODEL_PATH, (int)(g_params.temperature * 100.0f),
             g_params.top_k, g_params.max_tokens);
    send_text(fd, AI_EVT_INFO, session, buf);
    send_done(fd, session, 0, 0);
}

static void handle_tools(int fd, u32 session) {
    char buf[1536];
    int n = 0;
    int count = ai_tool_count();
    for (int i = 0; i < count && n < (int)sizeof(buf); i++) {
        const ai_tool_t *t = ai_tool_at(i);
        if (!t) continue;
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%-10s %s%s\n",
                      t->name, t->help,
                      t->needs_confirm ? "  [confirm]" : "");
    }
    if (n == 0) snprintf(buf, sizeof(buf), "no tools registered");
    send_text(fd, AI_EVT_INFO, session, buf);
    send_done(fd, session, 0, 0);
}

static void handle_memory(int fd, u32 session, const char *arg) {
    char buf[640];
    if (arg && strcmp(arg, "clear") == 0) {
        ai_session_reset(session);
        send_text(fd, AI_EVT_INFO, session, "session memory cleared");
        send_done(fd, session, 0, 0);
        return;
    }

    ai_session_brief(session, buf, sizeof(buf));
    if (buf[0] == 0) snprintf(buf, sizeof(buf), "(no turns in this session)");
    send_text(fd, AI_EVT_INFO, session, buf);
    send_done(fd, session, 0, 0);
}

static void handle_audit(int fd, u32 session) {
    mcube_ai_audit_t ev[16];
    int n = ai_audit(ev, 16);
    if (n < 0) n = 0;

    char buf[1024];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                    "kernel audit ring: %d event(s)\n", n);
    for (int i = 0; i < n && off < (int)sizeof(buf); i++) {
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        "%u pid=%u kind=%u %s\n", (unsigned)ev[i].ts,
                        (unsigned)ev[i].pid, (unsigned)ev[i].kind, ev[i].text);
    }
    send_text(fd, AI_EVT_INFO, session, buf);
    send_done(fd, session, 0, 0);
}

static void handle_model(int fd, u32 session, const char *arg) {
    (void)arg;
    char buf[320];
    if (!g_model_loaded) {
        snprintf(buf, sizeof(buf), "model not loaded (expected %s)", AI_MODEL_PATH);
    } else {
        snprintf(buf, sizeof(buf), "%s\npath=%s size=%u KiB",
                 g_model.desc, AI_MODEL_PATH, (unsigned)(g_model.size / 1024));
    }
    send_text(fd, AI_EVT_INFO, session, buf);
    send_done(fd, session, 0, 0);
}

static void handle_chat(int rfd, int wfd, u32 session, const char *prompt) {
    ai_session_append(session, "user", prompt);
    ai_log(AI_AUDIT_CHAT, prompt);

    /* Rule-based routing keeps tool selection deterministic for the tiny
     * model; a real function-calling model can replace ai_route() later. */
    char args[160];
    const ai_tool_t *tool = ai_route(prompt, args, sizeof(args));
    char *tool_out = NULL;
    char  tool_name[32];
    tool_name[0] = 0;

    if (tool) {
        snprintf(tool_name, sizeof(tool_name), "%s", tool->name);

        if (tool->needs_confirm) {
            char q[AI_MSG_DATA];
            snprintf(q, sizeof(q), "%s %s", tool->name, args);
            send_text(wfd, AI_EVT_CONFIRM, session, q);

            ai_msg_t con;
            memset(&con, 0, sizeof(con));
            int rc = ai_recv(rfd, &con);
            if (rc != 1 || con.type != AI_REQ_CONFIRM || !(con.flags & 1)) {
                ai_log(AI_AUDIT_DENY, tool_name);
                ai_session_append(session, "tool", "denied by user");
                send_text(wfd, AI_EVT_INFO, session, "tool denied by user\n");
                send_done(wfd, session, 0, 0);
                return;
            }
        }

        tool_out = malloc(AI_TOOL_OUT);
        if (!tool_out) {
            done_or_error(wfd, session, "out of memory");
            return;
        }

        int rc = ai_tool_invoke(tool, args, tool_out, AI_TOOL_OUT);
        ai_log(AI_AUDIT_TOOL, tool_name);

        char hdr[128];
        snprintf(hdr, sizeof(hdr), "-> %s %s%s\n", tool->name, args,
                 rc != 0 ? " (error)" : "");
        send_text(wfd, AI_EVT_INFO, session, hdr);
        send_text(wfd, AI_EVT_TOOL, session, tool_out);
        ai_session_append(session, "tool", tool_out);
    }

    if (!g_model_loaded) {
        if (!tool) send_text(wfd, AI_EVT_ERROR, session,
                             "model not loaded; only tool requests work");
        send_done(wfd, session, 0, 0);
        free(tool_out);
        return;
    }

    /* Condition the byte-level model on the prompt or the tool result. */
    char prompt_buf[AI_PROMPT_MAX];
    if (tool_out) {
        snprintf(prompt_buf, sizeof(prompt_buf), "Tool %s result:\n%.180s\nAI:",
                 tool->name, tool_out);
    } else {
        snprintf(prompt_buf, sizeof(prompt_buf), "User: %.240s\nAI:", prompt);
    }

    int tokens[AI_PROMPT_MAX];
    int nt = ai_encode(&g_model, prompt_buf, tokens, AI_PROMPT_MAX);
    if (nt <= 0) {
        done_or_error(wfd, session, "empty prompt");
        free(tool_out);
        return;
    }

    ai_params_t p = g_params;
    if (p.max_tokens <= 0) p.max_tokens = AI_DEF_MAX_TOKENS;
    if (p.max_tokens > (int)g_model.n_ctx - nt) {
        p.max_tokens = (int)g_model.n_ctx - nt;
    }

    stream_t s;
    memset(&s, 0, sizeof(s));
    s.fd = wfd;
    s.session = session;

    int nout = 0;
    u64 ms = 0;
    int rc = ai_generate(&g_model, tokens, nt, &p, on_token, &s, &nout, &ms);

    if (s.n > 0) {
        s.text[s.n] = 0;
        ai_session_append(session, "assistant", s.text);
    }
    if (rc != 0 || s.failed) {
        send_text(wfd, AI_EVT_ERROR, session, "inference failed");
    }
    send_done(wfd, session, nout, ms);
    free(tool_out);
}

/* ---- connection loop ------------------------------------------------ */

static void serve_client(int rfd, int wfd) {
    for (;;) {
        ai_msg_t req;
        memset(&req, 0, sizeof(req));
        int rc = ai_recv(rfd, &req);
        if (rc <= 0) break;

        u32 session = req.session;
        req.data[AI_MSG_DATA - 1] = 0;
        const char *arg = req.data;

        switch (req.type) {
        case AI_REQ_HELLO: {
            char hello[160];
            snprintf(hello, sizeof(hello), "MCubeAIOS aiosd ready; model=%s",
                     g_model_loaded ? "tiny-bigram" : "none");
            send_text(wfd, AI_EVT_INFO, session, hello);
            break;
        }
        case AI_REQ_CHAT:
            handle_chat(rfd, wfd, session, arg);
            break;
        case AI_REQ_STATUS:
            handle_status(wfd, session);
            break;
        case AI_REQ_TOOL_LIST:
            handle_tools(wfd, session);
            break;
        case AI_REQ_MEMORY:
            handle_memory(wfd, session, arg);
            break;
        case AI_REQ_AUDIT:
            handle_audit(wfd, session);
            break;
        case AI_REQ_MODEL:
            handle_model(wfd, session, arg);
            break;
        case AI_REQ_CANCEL:
            send_text(wfd, AI_EVT_INFO, session, "cancel: request already finished\n");
            send_done(wfd, session, 0, 0);
            break;
        default:
            send_text(wfd, AI_EVT_ERROR, session, "unknown request");
            send_done(wfd, session, 0, 0);
            break;
        }
    }

    close(rfd);
    close(wfd);
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    ai_agent_init();
    ai_params_default(&g_params);

    int mrc = ai_model_open(&g_model, AI_MODEL_PATH);
    if (mrc == 0) {
        g_model_loaded = 1;
        printf("AI_MODEL_OK %s\n", g_model.desc);
    } else {
        /* Negative rc identifies the loader stage; see ai_engine.c. */
        printf("AI_MODEL_MISSING %s rc=%d\n", AI_MODEL_PATH, mrc);
    }
    printf("AI_DAEMON_OK pid=%d\n", (int)getpid());

    for (;;) {
        int fds[2] = { -1, -1 };
        int pid = ai_accept(fds);
        if (pid < 0) {
            if (errno == EAGAIN) {
                sleep_ms(5);
                continue;
            }
            if (errno == EPERM) {
                printf("aiosd: another daemon is already running\n");
                return 1;
            }
            sleep_ms(20);
            continue;
        }
        serve_client(fds[0], fds[1]);
    }
}
