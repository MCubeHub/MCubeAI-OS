/* wc: count lines, words and bytes. */

#include <mcube/libc.h>

static void count_fd(int fd, u64 *lines, u64 *words, u64 *bytes) {
    char buf[512];
    long n;
    int in_word = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        *bytes += (u64)n;
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') (*lines)++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                (*words)++;
            }
        }
    }
}

int main(int argc, char **argv) {
    int show_lines = 1, show_words = 1, show_bytes = 1;
    int start = 1;

    while (start < argc && argv[start][0] == '-') {
        const char *f = argv[start] + 1;
        show_lines = show_words = show_bytes = 0;
        for (; *f; f++) {
            if (*f == 'l') show_lines = 1;
            else if (*f == 'w') show_words = 1;
            else if (*f == 'c') show_bytes = 1;
        }
        start++;
    }

    u64 lines = 0, words = 0, bytes = 0;
    if (start >= argc) {
        count_fd(0, &lines, &words, &bytes);
    } else {
        for (int i = start; i < argc; i++) {
            int fd = open(argv[i], VFS_O_RDONLY);
            if (fd < 0) {
                fprintf(2, "wc: %s: not found\n", argv[i]);
                return 1;
            }
            count_fd(fd, &lines, &words, &bytes);
            close(fd);
        }
    }

    if (show_lines) printf("%d", (int)lines);
    if (show_words) printf(show_lines ? " %d" : "%d", (int)words);
    if (show_bytes) printf((show_lines || show_words) ? " %d" : "%d", (int)bytes);
    putchar('\n');
    return 0;
}
