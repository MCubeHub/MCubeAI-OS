/* tail: print the last N lines (default 10) of a file. */

#include <mcube/libc.h>

#define TAIL_MAX 65536
static char g_buf[TAIL_MAX];

int main(int argc, char **argv) {
    int want = 10;
    int start = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        want = atoi(argv[2]);
        start = 3;
    }

    int fd = 0;
    if (start < argc) {
        fd = open(argv[start], VFS_O_RDONLY);
        if (fd < 0) {
            fprintf(2, "tail: %s: not found\n", argv[start]);
            return 1;
        }
    }

    usize len = 0;
    long n;
    while (len < TAIL_MAX && (n = read(fd, g_buf + len, TAIL_MAX - len)) > 0) {
        len += (usize)n;
    }
    if (fd > 0) close(fd);

    /* Find the start of the last `want` lines. */
    usize pos = len;
    int seen = 0;
    while (pos > 0) {
        pos--;
        if (g_buf[pos] == '\n' && pos + 1 < len) {
            if (++seen >= want) { pos++; break; }
        }
    }
    if (seen < want) pos = 0;

    if (len > pos) write(1, g_buf + pos, len - pos);
    return 0;
}
