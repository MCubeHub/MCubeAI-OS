/* head: print the first N lines (default 10). */

#include <mcube/libc.h>

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
            fprintf(2, "head: %s: not found\n", argv[start]);
            return 1;
        }
    }

    char c;
    int lines = 0;
    while (lines < want) {
        long n = read(fd, &c, 1);
        if (n <= 0) break;
        putchar(c);
        if (c == '\n') lines++;
    }
    if (fd > 0) close(fd);
    return 0;
}
