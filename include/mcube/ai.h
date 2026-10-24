#pragma once
/* AI-native channel (M9).
 *
 * `aiosd` runs in user space and speaks a small fixed-size message protocol
 * with clients. The kernel provides only the transport: a connection is two
 * anonymous pipes (request: client -> daemon, response: daemon -> client), so
 * the existing read/write/poll/close paths work unchanged.
 *
 *   client: ai_connect() -> fd 3 = request write, fd 4 = response read
 *   daemon: ai_accept()  -> fd 3 = request read,  fd 4 = response write
 *
 * The protocol is intentionally plain (no JSON parser in the kernel or libc):
 * one ai_msg_t per read/write, streamed tokens are separate AI_EVT_TOKEN
 * messages.
 */

#include <mcube/types.h>

#define AI_MSG_DATA 512

/* client -> daemon */
#define AI_REQ_HELLO     1
#define AI_REQ_CHAT      2
#define AI_REQ_CANCEL    3
#define AI_REQ_STATUS    4
#define AI_REQ_TOOL_LIST 5
#define AI_REQ_MEMORY    6
#define AI_REQ_AUDIT     7
#define AI_REQ_MODEL     8
#define AI_REQ_CONFIRM   9

/* daemon -> client */
#define AI_EVT_TOKEN   64
#define AI_EVT_TOOL    65
#define AI_EVT_CONFIRM 66
#define AI_EVT_DONE    67
#define AI_EVT_ERROR   68
#define AI_EVT_INFO    69
#define AI_EVT_STATS   70

/* audit kinds (kernel ring + daemon log) */
#define AI_AUDIT_CONNECT 1
#define AI_AUDIT_ACCEPT  2
#define AI_AUDIT_TOOL    3
#define AI_AUDIT_DENY    4
#define AI_AUDIT_CHAT    5

typedef struct PACKED ai_msg {
    u32 type;
    u32 session;
    u32 flags;
    u32 len;
    char data[AI_MSG_DATA];
} ai_msg_t;

typedef struct PACKED mcube_ai_status {
    u32 daemon_pid;
    u32 pending;
    u32 connects;
    u32 accepts;
    u32 audit_count;
    u32 sessions;
} mcube_ai_status_t;

typedef struct PACKED mcube_ai_audit {
    u64 ts;
    u32 pid;
    u32 kind;
    char text[64];
} mcube_ai_audit_t;

#define AI_AUDIT_RING 64

struct pipe_end;
struct task;

/* Kernel-side transport (kernel/ai/ai_chan.c). */
int  ai_chan_connect(struct pipe_end **req_w, struct pipe_end **resp_r,
                     u32 *session);
void ai_chan_cancel(u32 session);
int  ai_chan_accept(struct pipe_end **req_r, struct pipe_end **resp_w,
                    u32 *client_pid);
void ai_chan_status(mcube_ai_status_t *st);
void ai_chan_task_cleanup(struct task *t);
int  ai_chan_audit(u32 kind, const char *text);
void ai_audit_log(u32 kind, const char *text);
u32  ai_audit_read(mcube_ai_audit_t *out, u32 max);

/* User-side helpers (user/lib/ai.c). */
int  ai_connect(int fds[2]);
int  ai_accept(int fds[2]);
int  ai_status(mcube_ai_status_t *st);
int  ai_audit(mcube_ai_audit_t *out, int max);
int  ai_log(u32 kind, const char *text);  /* daemon only */
int  ai_send(int fd, ai_msg_t *msg);
int  ai_recv(int fd, ai_msg_t *msg);      /* 1 message, 0 EOF, -1 error */
int  ai_recv_timeout(int fd, ai_msg_t *msg, int timeout_ms);
