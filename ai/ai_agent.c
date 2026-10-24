/* Tools, intent routing and conversation memory for the AI layer (M9).
 *
 * The router is rules-based on purpose: a byte-level tiny model cannot emit
 * reliable function calls, so `ai_route()` maps an utterance onto a tool and
 * the daemon audits every invocation. The session table keeps per-client
 * history keyed by an opaque u32 supplied by the client (its pid by default).
 */

#include <mcube/libc.h>
#include <mcube/ai.h>
#include "ai_runtime.h"

#include <stdarg.h>

/* ---------------------------------------------------------------------
 * tiny string builder
 * ------------------------------------------------------------------- */

typedef struct {
    char *p;
    int cap;
    int n;
} sbuf_t;

static void sb_init(sbuf_t *b, char *p, int cap) {
    b->p = p;
    b->cap = cap;
    b->n = 0;
    if (cap > 0) p[0] = 0;
}

static void sb_add(sbuf_t *b, const char *fmt, ...) {
    if (!b->p || b->cap <= 0 || b->n >= b->cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(b->p + b->n, (size_t)(b->cap - b->n), fmt, ap);
    va_end(ap);
    if (m > 0) {
        b->n += m;
        if (b->n > b->cap - 1) b->n = b->cap - 1;
    }
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Copy the first whitespace-delimited token of `s` into out. */
static void first_token(const char *s, char *out, int cap) {
    s = skip_ws(s);
    int n = 0;
    while (*s && *s != ' ' && *s != '\t') {
        if (n + 1 < cap) out[n++] = *s;
        s++;
    }
    out[n] = 0;
}

/* Find the first absolute path inside `s`; empty when there is none. */
static void first_path(const char *s, char *out, int cap) {
    int n = 0;
    while (*s) {
        if (*s == '/') {
            while (*s && *s != ' ' && *s != '\t' && *s != '\n') {
                if (n + 1 < cap) out[n++] = *s;
                s++;
            }
            break;
        }
        s++;
    }
    out[n] = 0;
}

/* ---------------------------------------------------------------------
 * tools
 * ------------------------------------------------------------------- */

static int tool_sys_info(const char *args, char *out, int cap) {
    (void)args;
    mcube_time_t ts;
    memset(&ts, 0, sizeof(ts));
    mc_time(&ts);
    mcube_task_info_t ti[16];
    int nt = taskinfo(ti, 16);

    sbuf_t b;
    sb_init(&b, out, cap);
    sb_add(&b, "MCubeAIOS 0.1.0 x86_64 UEFI\n");
    sb_add(&b, "kernel: higher-half, RR scheduler, tmpfs+FAT32\n");
    sb_add(&b, "uptime: %u ms\n", (unsigned)ts.uptime_ms);
    sb_add(&b, "tasks: %d\n", nt);
    sb_add(&b, "ai: local GGUF runtime, aiosd resident\n");
    return 0;
}

static int tool_sys_time(const char *args, char *out, int cap) {
    (void)args;
    mcube_time_t ts;
    memset(&ts, 0, sizeof(ts));
    if (mc_time(&ts) != 0) {
        snprintf(out, (size_t)cap, "time unavailable");
        return -1;
    }
    snprintf(out, (size_t)cap, "uptime %u ms, epoch %u s, ticks %u",
             (unsigned)ts.uptime_ms, (unsigned)ts.epoch, (unsigned)ts.ticks);
    return 0;
}

static int tool_sys_disk(const char *args, char *out, int cap) {
    (void)args;
    mcube_stat_t st;
    int bin_count = 0;
    mcube_dirent_t ent;
    while (mc_readdir("/bin", bin_count, &ent) == 0) bin_count++;

    sbuf_t b;
    sb_init(&b, out, cap);
    if (stat("/", &st) == 0) {
        sb_add(&b, "root: %s (%u bytes)\n",
               st.type == VFS_TYPE_DIR ? "directory" : "file",
               (unsigned)st.size);
    }
    sb_add(&b, "bin: %d programs\n", bin_count);
    sb_add(&b, "models: /models/tiny.ggu\n");
    sb_add(&b, "volumes: FAT32 ESP (read-only), tmpfs (rw)\n");
    return 0;
}

static int tool_proc_list(const char *args, char *out, int cap) {
    (void)args;
    mcube_task_info_t ti[24];
    int n = taskinfo(ti, 24);
    if (n < 0) n = 0;

    sbuf_t b;
    sb_init(&b, out, cap);
    sb_add(&b, "pid  ppid state name\n");
    for (int i = 0; i < n; i++) {
        sb_add(&b, "%3d  %4d %5d %s\n", (int)ti[i].pid, (int)ti[i].ppid,
               (int)ti[i].state, ti[i].name);
    }
    sb_add(&b, "%d task(s)", n);
    return 0;
}

static int tool_fs_list(const char *args, char *out, int cap) {
    char path[VFS_PATH_MAX];
    const char *a = skip_ws(args ? args : "");
    if (*a == '/') first_token(a, path, sizeof(path));
    else snprintf(path, sizeof(path), "%s", "/");
    if (path[0] == 0) snprintf(path, sizeof(path), "%s", "/");

    mcube_dirent_t ent;
    int i = 0;
    sbuf_t b;
    sb_init(&b, out, cap);
    sb_add(&b, "%s:\n", path);
    while (i < 32 && mc_readdir(path, i, &ent) == 0) {
        sb_add(&b, "  %s%s\n", ent.name,
               ent.type == VFS_TYPE_DIR ? "/" : "");
        i++;
    }
    if (i == 0) sb_add(&b, "  (empty or unreadable)\n");
    sb_add(&b, "%d entr%s", i, i == 1 ? "y" : "ies");
    return 0;
}

static int tool_fs_read(const char *args, char *out, int cap) {
    char path[VFS_PATH_MAX];
    first_path(args ? args : "", path, sizeof(path));
    if (path[0] == 0) {
        snprintf(out, (size_t)cap, "fs.read: no path in request");
        return -1;
    }

    int fd = open(path, VFS_O_RDONLY);
    if (fd < 0) {
        snprintf(out, (size_t)cap, "fs.read: cannot open %s", path);
        return -1;
    }

    int want = cap - 1;
    if (want > 1536) want = 1536;
    long n = read(fd, out, (size_t)want);
    close(fd);
    if (n < 0) {
        snprintf(out, (size_t)cap, "fs.read: read failed");
        return -1;
    }
    out[n] = 0;
    return 0;
}

/* Sensitive: overwrite a file. The daemon asks the client before running. */
static int tool_fs_write(const char *args, char *out, int cap) {
    char path[VFS_PATH_MAX];
    first_token(args ? args : "", path, sizeof(path));
    if (path[0] != '/') {
        snprintf(out, (size_t)cap, "fs.write: need an absolute path");
        return -1;
    }

    const char *text = args + strlen(path);
    while (*text == ' ' || *text == '\t') text++;

    int fd = open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        snprintf(out, (size_t)cap, "fs.write: cannot create %s", path);
        return -1;
    }
    size_t len = strlen(text);
    long n = len ? write(fd, text, len) : 0;
    close(fd);
    if (n < 0) {
        snprintf(out, (size_t)cap, "fs.write: write failed");
        return -1;
    }
    snprintf(out, (size_t)cap, "wrote %d byte(s) to %s", (int)len, path);
    return 0;
}

static int tool_fs_search(const char *args, char *out, int cap) {
    char dir[VFS_PATH_MAX];
    char pat[VFS_NAME_MAX];
    const char *a = skip_ws(args ? args : "");

    if (*a == '/') {
        first_token(a, dir, sizeof(dir));
        a += strlen(dir);
        a = skip_ws(a);
        first_token(a, pat, sizeof(pat));
    } else {
        snprintf(dir, sizeof(dir), "%s", "/");
        first_token(a, pat, sizeof(pat));
    }
    if (pat[0] == 0) {
        snprintf(out, (size_t)cap, "fs.search: no pattern");
        return -1;
    }

    mcube_dirent_t ent;
    int i = 0, hits = 0;
    sbuf_t b;
    sb_init(&b, out, cap);
    sb_add(&b, "search '%s' in %s:\n", pat, dir);
    while (i < 64 && mc_readdir(dir, i, &ent) == 0) {
        if (strstr(ent.name, pat)) {
            sb_add(&b, "  %s\n", ent.name);
            hits++;
        }
        i++;
    }
    sb_add(&b, "%d hit(s)", hits);
    return 0;
}

/* ---- calc.eval ------------------------------------------------------ */

typedef struct {
    const char *p;
    int err;
} ep_t;

static long e_expr(ep_t *e);

static long e_atom(ep_t *e) {
    e->p = skip_ws(e->p);
    if (*e->p == '(') {
        e->p++;
        long v = e_expr(e);
        e->p = skip_ws(e->p);
        if (*e->p == ')') e->p++;
        else e->err = 1;
        return v;
    }
    int neg = 0;
    if (*e->p == '-') { neg = 1; e->p++; }
    if (*e->p < '0' || *e->p > '9') { e->err = 1; return 0; }
    long v = 0;
    while (*e->p >= '0' && *e->p <= '9') v = v * 10 + (*e->p++ - '0');
    return neg ? -v : v;
}

static long e_term(ep_t *e) {
    long v = e_atom(e);
    for (;;) {
        e->p = skip_ws(e->p);
        char op = *e->p;
        if (op != '*' && op != '/' && op != '%') break;
        e->p++;
        long r = e_atom(e);
        if ((op == '/' || op == '%') && r == 0) { e->err = 1; return 0; }
        if (op == '*') v *= r;
        else if (op == '/') v /= r;
        else v %= r;
    }
    return v;
}

static long e_expr(ep_t *e) {
    long v = e_term(e);
    for (;;) {
        e->p = skip_ws(e->p);
        char op = *e->p;
        if (op != '+' && op != '-') break;
        e->p++;
        long r = e_term(e);
        v = (op == '+') ? v + r : v - r;
    }
    return v;
}

static int tool_calc_eval(const char *args, char *out, int cap) {
    ep_t e;
    e.p = args ? args : "";
    e.err = 0;
    long v = e_expr(&e);
    e.p = skip_ws(e.p);
    if (e.err || *e.p) {
        snprintf(out, (size_t)cap, "calc.eval: cannot parse '%s'", args ? args : "");
        return -1;
    }
    snprintf(out, (size_t)cap, "%d", (int)v);
    return 0;
}

/* ---------------------------------------------------------------------
 * registry
 * ------------------------------------------------------------------- */

static ai_tool_t g_tools[AI_TOOL_MAX];
static int g_tool_count;
static int g_agent_ready;

static void reg(const char *name, const char *schema, const char *help,
                int confirm, int (*run)(const char *, char *, int)) {
    if (g_tool_count >= AI_TOOL_MAX) return;
    ai_tool_t *t = &g_tools[g_tool_count++];
    t->name = name;
    t->schema = schema;
    t->help = help;
    t->needs_confirm = confirm;
    t->run = run;
}

void ai_agent_init(void) {
    if (g_agent_ready) return;
    g_agent_ready = 1;
    g_tool_count = 0;

    reg("sys.info", "{\"args\":[]}", "system identity and uptime", 0,
        tool_sys_info);
    reg("sys.time", "{\"args\":[]}", "uptime, epoch seconds and ticks", 0,
        tool_sys_time);
    reg("sys.disk", "{\"args\":[]}", "storage and program inventory", 0,
        tool_sys_disk);
    reg("proc.list", "{\"args\":[]}", "list running tasks", 0,
        tool_proc_list);
    reg("fs.list", "{\"path\":\"dir\"}", "list a directory", 0,
        tool_fs_list);
    reg("fs.read", "{\"path\":\"file\"}", "read a text file", 0,
        tool_fs_read);
    reg("fs.write", "{\"path\":\"file\",\"text\":\"data\"}",
        "overwrite a file (needs confirmation)", 1, tool_fs_write);
    reg("fs.search", "{\"dir\":\"dir\",\"pattern\":\"text\"}",
        "search file names in a directory", 0, tool_fs_search);
    reg("calc.eval", "{\"expr\":\"a + b\"}", "evaluate integer arithmetic", 0,
        tool_calc_eval);
}

int ai_tool_count(void) {
    ai_agent_init();
    return g_tool_count;
}

const ai_tool_t *ai_tool_at(int i) {
    ai_agent_init();
    if (i < 0 || i >= g_tool_count) return NULL;
    return &g_tools[i];
}

const ai_tool_t *ai_tool_find(const char *name) {
    ai_agent_init();
    if (!name) return NULL;
    for (int i = 0; i < g_tool_count; i++) {
        if (strcmp(g_tools[i].name, name) == 0) return &g_tools[i];
    }
    return NULL;
}

int ai_tool_invoke(const ai_tool_t *t, const char *args, char *out, int cap) {
    if (!t || !t->run || !out || cap <= 0) return -1;
    out[0] = 0;
    return t->run(args, out, cap);
}

/* ---------------------------------------------------------------------
 * intent routing
 * ------------------------------------------------------------------- */

static int has(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

static int has_digit_or_op(const char *s) {
    int digit = 0, op = 0;
    for (const char *p = s; *p; p++) {
        if (*p >= '0' && *p <= '9') digit = 1;
        if (*p == '+' || *p == '-' || *p == '*' || *p == '/') op = 1;
    }
    return digit && op;
}

const ai_tool_t *ai_route(const char *prompt, char *args, int argcap) {
    ai_agent_init();
    if (!prompt || !args || argcap <= 0) return NULL;
    args[0] = 0;

    /* Order matters: specific intents first. */
    if (has(prompt, "write file") || has(prompt, "save to") ||
        has(prompt, "写入") || has(prompt, "保存到")) {
        const char *a = skip_ws(prompt);
        while (*a && *a != '/') a++;
        snprintf(args, (size_t)argcap, "%s", *a ? a : "");
        return ai_tool_find("fs.write");
    }

    if (has(prompt, "read file") || has(prompt, "read /") ||
        has(prompt, "cat /") || has(prompt, "读取")) {
        first_path(prompt, args, argcap);
        return ai_tool_find("fs.read");
    }

    if (has(prompt, "search") || has(prompt, "find ") || has(prompt, "查找") ||
        has(prompt, "搜索")) {
        const char *a = skip_ws(prompt);
        while (*a && !has(a, "search") && !has(a, "find") && *a != '/') a++;
        if (has(a, "search")) a += 6;
        else if (has(a, "find")) a += 4;
        snprintf(args, (size_t)argcap, "%s", a);
        return ai_tool_find("fs.search");
    }

    if (has(prompt, "list ") || has(prompt, "list /") || has(prompt, "ls ") ||
        has(prompt, "列出") || has(prompt, "目录")) {
        char path[VFS_PATH_MAX];
        first_path(prompt, path, sizeof(path));
        snprintf(args, (size_t)argcap, "%s", path[0] ? path : "/");
        return ai_tool_find("fs.list");
    }

    if (has(prompt, "process") || has(prompt, "task") || has(prompt, "ps ") ||
        has(prompt, "进程")) {
        return ai_tool_find("proc.list");
    }

    if (has(prompt, "disk") || has(prompt, "space") || has(prompt, "磁盘") ||
        has(prompt, "空间")) {
        return ai_tool_find("sys.disk");
    }

    if (has(prompt, "time") || has(prompt, "clock") || has(prompt, "时间")) {
        return ai_tool_find("sys.time");
    }

    if (has(prompt, "system") || has(prompt, "version") || has(prompt, "uname") ||
        has(prompt, "系统")) {
        return ai_tool_find("sys.info");
    }

    if (has(prompt, "calc") || has(prompt, "compute") || has(prompt, "算") ||
        has_digit_or_op(prompt)) {
        snprintf(args, (size_t)argcap, "%s", prompt);
        return ai_tool_find("calc.eval");
    }

    return NULL;
}

/* ---------------------------------------------------------------------
 * session memory
 * ------------------------------------------------------------------- */

typedef struct {
    char role[10];
    char text[160];
} ai_turn_t;

typedef struct {
    u32 key;
    int used;
    int n;
    ai_turn_t turns[AI_SESSION_TURNS];
} ai_session_t;

static ai_session_t g_sessions[AI_SESSIONS];

static ai_session_t *session_slot(u32 key, int create) {
    for (int i = 0; i < AI_SESSIONS; i++) {
        if (g_sessions[i].used && g_sessions[i].key == key) return &g_sessions[i];
    }
    if (!create) return NULL;

    for (int i = 0; i < AI_SESSIONS; i++) {
        if (!g_sessions[i].used) {
            memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
            g_sessions[i].used = 1;
            g_sessions[i].key = key;
            return &g_sessions[i];
        }
    }

    /* Table full: recycle the oldest entry (index 0 keeps insertion order). */
    ai_session_t *s = &g_sessions[0];
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->key = key;
    return s;
}

void ai_session_reset(u32 key) {
    ai_session_t *s = session_slot(key, 0);
    if (!s) return;
    memset(s, 0, sizeof(*s));
}

int ai_session_turns(u32 key) {
    ai_session_t *s = session_slot(key, 0);
    return s ? s->n : 0;
}

int ai_session_append(u32 key, const char *role, const char *text) {
    if (!role || !text) return -1;
    ai_session_t *s = session_slot(key, 1);
    if (!s) return -1;

    if (s->n >= AI_SESSION_TURNS) {
        for (int i = 1; i < AI_SESSION_TURNS; i++) s->turns[i - 1] = s->turns[i];
        s->n = AI_SESSION_TURNS - 1;
    }

    ai_turn_t *t = &s->turns[s->n++];
    memset(t, 0, sizeof(*t));
    snprintf(t->role, sizeof(t->role), "%s", role);
    snprintf(t->text, sizeof(t->text), "%s", text);
    return 0;
}

void ai_session_brief(u32 key, char *out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = 0;

    ai_session_t *s = session_slot(key, 0);
    if (!s) return;

    sbuf_t b;
    sb_init(&b, out, cap);
    for (int i = 0; i < s->n; i++) {
        sb_add(&b, "%s: %s\n", s->turns[i].role, s->turns[i].text);
    }
}

int ai_session_count(void) {
    int n = 0;
    for (int i = 0; i < AI_SESSIONS; i++) {
        if (g_sessions[i].used) n++;
    }
    return n;
}
