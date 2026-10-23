/* Process startup, heap allocator and environment. */

#include <mcube/libc.h>

int main(int argc, char **argv, char **envp);

char **environ;

/* ---- exit ---------------------------------------------------------- */

#define MAX_ATEXIT 8
static void (*g_atexit[MAX_ATEXIT])(void);
static int g_atexit_count;

int atexit(void (*fn)(void)) {
    if (g_atexit_count >= MAX_ATEXIT) return -1;
    g_atexit[g_atexit_count++] = fn;
    return 0;
}

void _exit(int status) {
    __asm__ __volatile__("syscall"
                         :
                         : "a"((long)SYS_EXIT), "D"((long)status)
                         : "rcx", "r11", "memory");
    for (;;) { }
}

void exit(int status) {
    while (g_atexit_count > 0) {
        void (*fn)(void) = g_atexit[--g_atexit_count];
        if (fn) fn();
    }
    _exit(status);
}

void __libc_start_main(int argc, char **argv, char **envp) {
    environ = envp;
    int rc = main(argc, argv, envp);
    exit(rc);
}

/* ---- bump allocator on top of brk ---------------------------------- */

static u64 g_heap_ptr;
static u64 g_heap_end;
static int g_heap_ready;

static int heap_init(void) {
    if (g_heap_ready) return 0;
    void *cur = brk(NULL);
    if (cur == (void *)-1) return -1;
    g_heap_ptr = (u64)(uptr)cur;
    g_heap_end = g_heap_ptr;
    g_heap_ready = 1;
    return 0;
}

void *malloc(size_t size) {
    if (size == 0) size = 1;
    if (heap_init() != 0) return NULL;

    u64 need = (u64)size;
    if (need > ~0ULL - 15) return NULL;
    need = (need + 15) & ~15ULL;

    if (g_heap_ptr + need > g_heap_end) {
        u64 want = (g_heap_ptr + need + 0xFFFF) & ~0xFFFFULL;
        void *rc = brk((void *)want);
        if (rc == (void *)-1) return NULL;
        g_heap_end = want;
    }

    void *p = (void *)(uptr)g_heap_ptr;
    g_heap_ptr += need;
    return p;
}

void free(void *ptr) {
    (void)ptr;  /* bump allocator: released when the process exits */
}

void *calloc(size_t n, size_t size) {
    if (size && n > (~0ULL) / size) return NULL;
    size_t total = n * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) return NULL;
    void *p = malloc(size);
    if (!p) return NULL;
    /* The old block size is unknown to a bump allocator; copy conservatively
     * up to the new size, which is safe because the heap never reuses memory. */
    memcpy(p, ptr, size);
    return p;
}

/* ---- environment --------------------------------------------------- */

#define ENV_MAX 32

static char *g_env[ENV_MAX + 1];

static void env_sync(void) {
    environ = g_env;
}

char *getenv(const char *name) {
    if (!name || !environ) return NULL;
    size_t n = strlen(name);
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return *e + n + 1;
    }
    return NULL;
}

static int env_index(const char *name) {
    if (!environ) return -1;
    size_t n = strlen(name);
    int i = 0;
    for (char **e = environ; *e; e++, i++) {
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return i;
    }
    return -1;
}

static int env_count(void) {
    int n = 0;
    if (environ) while (environ[n]) n++;
    return n;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value || !*name || strchr(name, '=')) return -1;

    int idx = env_index(name);
    if (idx >= 0 && !overwrite) return 0;

    size_t len = strlen(name) + 1 + strlen(value) + 1;
    char *entry = malloc(len);
    if (!entry) return -1;
    strcpy(entry, name);
    strcat(entry, "=");
    strcat(entry, value);

    if (!environ) {
        memset(g_env, 0, sizeof(g_env));
        env_sync();
    }

    int n = env_count();
    if (idx >= 0) {
        environ[idx] = entry;
        return 0;
    }
    if (n >= ENV_MAX) return -1;
    environ[n] = entry;
    environ[n + 1] = NULL;
    return 0;
}

int unsetenv(const char *name) {
    int idx = env_index(name);
    if (idx < 0) return 0;
    int n = env_count();
    for (int i = idx; i < n - 1; i++) environ[i] = environ[i + 1];
    environ[n - 1] = NULL;
    return 0;
}

/* ---- directory helpers --------------------------------------------- */

int mc_readdir(const char *path, int index, mcube_dirent_t *out) {
    int fd = open(path, VFS_O_RDONLY);
    if (fd < 0) return -1;

    mcube_dirent_t ent;
    int seen = 0;
    int rc = -1;
    while (getdents64(fd, &ent, sizeof(ent)) > 0) {
        if (seen++ == index) {
            *out = ent;
            out->name[(out->namelen < sizeof(out->name)) ? out->namelen
                                                         : sizeof(out->name) - 1] = 0;
            rc = 0;
            break;
        }
    }
    close(fd);
    return rc;
}

int path_is_dir(const char *path) {
    mcube_stat_t st;
    if (stat(path, &st) != 0) return 0;
    return st.type == VFS_TYPE_DIR;
}
