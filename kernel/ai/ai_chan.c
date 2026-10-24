/* AI-native transport (M9).
 *
 * The kernel does not parse prompts or models. It only brokers connections
 * between clients and the `aiosd` daemon: every connection is two anonymous
 * pipes (request client->daemon, response daemon->client), so read/write/poll
 * and close keep their existing semantics. A small pending queue lets a client
 * connect before the daemon has reached accept().
 */

#include <mcube/ai.h>
#include <mcube/pipe.h>
#include <mcube/sched.h>
#include <mcube/timer.h>
#include <mcube/kernel.h>
#include <mcube/syscall.h>

#define AI_PENDING_MAX 16

typedef struct ai_pending {
    struct pipe_end *req_r;   /* daemon reads requests here   */
    struct pipe_end *resp_w;  /* daemon writes responses here */
    u32 client_pid;
    u32 session;
    int used;
} ai_pending_t;

static ai_pending_t g_pending[AI_PENDING_MAX];
static u32 g_next_session = 1;
static u32 g_daemon_pid;
static u32 g_connects;
static u32 g_accepts;

static mcube_ai_audit_t g_audit[AI_AUDIT_RING];
static u32 g_audit_head;
static u32 g_audit_count;

static void pending_close(ai_pending_t *p) {
    if (!p->used) return;
    pipe_end_close(p->req_r);
    pipe_end_close(p->resp_w);
    p->req_r = NULL;
    p->resp_w = NULL;
    p->used = 0;
}

static ai_pending_t *pending_alloc(void) {
    for (int i = 0; i < AI_PENDING_MAX; i++) {
        if (!g_pending[i].used) return &g_pending[i];
    }
    return NULL;
}

int ai_chan_connect(struct pipe_end **req_w, struct pipe_end **resp_r,
                    u32 *session) {
    if (!req_w || !resp_r) return -EINVAL;

    struct pipe_end *req_r = NULL;
    struct pipe_end *rsp_w = NULL;
    int rc = pipe_create(&req_r, req_w);
    if (rc != 0) return rc;

    rc = pipe_create(resp_r, &rsp_w);
    if (rc != 0) {
        pipe_end_close(req_r);
        pipe_end_close(*req_w);
        *req_w = NULL;
        return rc;
    }

    ai_pending_t *p = pending_alloc();
    if (!p) {
        pipe_end_close(req_r);
        pipe_end_close(*req_w);
        pipe_end_close(*resp_r);
        pipe_end_close(rsp_w);
        *req_w = NULL;
        *resp_r = NULL;
        return -ENFILE;
    }

    p->req_r = req_r;
    p->resp_w = rsp_w;
    p->client_pid = current_task() ? current_task()->pid : 0;
    p->session = g_next_session++;
    if (g_next_session == 0) g_next_session = 1;
    p->used = 1;

    g_connects++;
    ai_audit_log(AI_AUDIT_CONNECT, "client connected");
    if (session) *session = p->session;
    return 0;
}

/* Drop a pending connection by session id. Used when installing descriptors
 * into the client fails after ai_chan_connect() succeeded. */
void ai_chan_cancel(u32 session) {
    for (int i = 0; i < AI_PENDING_MAX; i++) {
        if (g_pending[i].used && g_pending[i].session == session) {
            pending_close(&g_pending[i]);
            return;
        }
    }
}

int ai_chan_accept(struct pipe_end **req_r, struct pipe_end **resp_w,
                   u32 *client_pid) {
    if (!req_r || !resp_w) return -EINVAL;

    task_t *me = current_task();
    if (!me) return -ESRCH;

    if (g_daemon_pid == 0) {
        g_daemon_pid = me->pid;
    } else if (g_daemon_pid != me->pid) {
        return -EPERM;
    }

    for (int i = 0; i < AI_PENDING_MAX; i++) {
        ai_pending_t *p = &g_pending[i];
        if (!p->used) continue;

        /* A client may die between connect() and accept(). Drop its stale
         * descriptors instead of handing the daemon an immediate EOF. */
        if (p->client_pid && !task_exists(p->client_pid)) {
            pending_close(p);
            continue;
        }

        *req_r = p->req_r;
        *resp_w = p->resp_w;
        if (client_pid) *client_pid = p->client_pid;
        p->req_r = NULL;
        p->resp_w = NULL;
        p->used = 0;
        g_accepts++;
        ai_audit_log(AI_AUDIT_ACCEPT, "daemon accepted client");
        return 0;
    }

    return -EAGAIN;
}

void ai_chan_status(mcube_ai_status_t *st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->daemon_pid = g_daemon_pid;
    for (int i = 0; i < AI_PENDING_MAX; i++) {
        if (g_pending[i].used) st->pending++;
    }
    st->connects = g_connects;
    st->accepts = g_accepts;
    st->audit_count = g_audit_count;
    st->sessions = g_accepts;
}

/* Only the daemon that has accepted a connection may append audit events, so
 * an ordinary client cannot forge tool decisions in the kernel ring. */
int ai_chan_audit(u32 kind, const char *text) {
    task_t *t = current_task();
    if (!t || g_daemon_pid == 0 || g_daemon_pid != t->pid) return -EPERM;
    ai_audit_log(kind, text);
    return 0;
}

void ai_audit_log(u32 kind, const char *text) {
    mcube_ai_audit_t *e;
    if (g_audit_count < AI_AUDIT_RING) {
        e = &g_audit[(g_audit_head + g_audit_count) % AI_AUDIT_RING];
        g_audit_count++;
    } else {
        e = &g_audit[g_audit_head];
        g_audit_head = (g_audit_head + 1) % AI_AUDIT_RING;
    }

    memset(e, 0, sizeof(*e));
    e->ts = timer_uptime_ms();
    task_t *t = current_task();
    e->pid = t ? t->pid : 0;
    e->kind = kind;
    if (text) {
        strncpy(e->text, text, sizeof(e->text) - 1);
        e->text[sizeof(e->text) - 1] = 0;
    }
}

u32 ai_audit_read(mcube_ai_audit_t *out, u32 max) {
    if (!out || max == 0) return 0;
    u32 n = g_audit_count < max ? g_audit_count : max;
    for (u32 i = 0; i < n; i++) {
        out[i] = g_audit[(g_audit_head + i) % AI_AUDIT_RING];
    }
    return n;
}

void ai_chan_task_cleanup(struct task *t) {
    task_t *task = (task_t *)t;
    if (!task) return;

    for (int i = 0; i < AI_PENDING_MAX; i++) {
        if (g_pending[i].used && g_pending[i].client_pid == task->pid) {
            pending_close(&g_pending[i]);
        }
    }
    if (g_daemon_pid == task->pid) g_daemon_pid = 0;
}
