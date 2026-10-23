/* cat: copy files (or stdin) to stdout. */

#include <mcube/libc.h>

static int cat_fd(int fd) {
    char buf[512];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write(1, buf, (size_t)n) != n) return 1;
    }
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return cat_fd(0);

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], VFS_O_RDONLY);
        if (fd < 0) {
            fprintf(2, "cat: %s: not found\n", argv[i]);
            rc = 1;
            continue;
        }
        if (cat_fd(fd)) rc = 1;
        close(fd);
    }
    return rc;
}
