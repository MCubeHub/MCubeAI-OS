/* grep: print lines containing a pattern. */

#include <mcube/libc.h>

static int match_line(const char *line, const char *pat, int invert,
                      int ignore_case) {
    char a[512], b[256];
    usize n = strlen(line);
    if (n >= sizeof(a)) n = sizeof(a) - 1;
    memcpy(a, line, n);
    a[n] = 0;

    usize m = strlen(pat);
    if (m >= sizeof(b)) m = sizeof(b) - 1;
    memcpy(b, pat, m);
    b[m] = 0;

    if (ignore_case) {
        for (usize i = 0; a[i]; i++) if (a[i] >= 'A' && a[i] <= 'Z') a[i] += 32;
        for (usize i = 0; b[i]; i++) if (b[i] >= 'A' && b[i] <= 'Z') b[i] += 32;
    }

    int found = strstr(a, b) != NULL;
    return invert ? !found : found;
}

static int grep_fd(int fd, const char *pat, int invert, int ignore_case,
                   int number) {
    char line[512];
    usize n = 0;
    int lineno = 0;
    int rc = 1;
    char c;

    for (;;) {
        long r = read(fd, &c, 1);
        if (r <= 0) {
            if (n) {
                line[n] = 0;
                lineno++;
                if (match_line(line, pat, invert, ignore_case)) {
                    if (number) printf("%d:", lineno);
                    printf("%s\n", line);
                    rc = 0;
                }
            }
            break;
        }
        if (c == '\n') {
            line[n] = 0;
            lineno++;
            if (match_line(line, pat, invert, ignore_case)) {
                if (number) printf("%d:", lineno);
                printf("%s\n", line);
                rc = 0;
            }
            n = 0;
        } else if (n + 1 < sizeof(line)) {
            line[n++] = c;
        }
    }
    return rc;
}

int main(int argc, char **argv) {
    int invert = 0, ignore_case = 0, number = 0;
    int start = 1;

    while (start < argc && argv[start][0] == '-' && argv[start][1]) {
        for (const char *f = argv[start] + 1; *f; f++) {
            if (*f == 'v') invert = 1;
            else if (*f == 'i') ignore_case = 1;
            else if (*f == 'n') number = 1;
        }
        start++;
    }

    if (start >= argc) {
        fprintf(2, "usage: grep [-vin] PATTERN [FILE...]\n");
        return 2;
    }
    const char *pat = argv[start++];

    if (start >= argc) return grep_fd(0, pat, invert, ignore_case, number);

    int rc = 1;
    for (int i = start; i < argc; i++) {
        int fd = open(argv[i], VFS_O_RDONLY);
        if (fd < 0) {
            fprintf(2, "grep: %s: not found\n", argv[i]);
            continue;
        }
        if (grep_fd(fd, pat, invert, ignore_case, number) == 0) rc = 0;
        close(fd);
    }
    return rc;
}
