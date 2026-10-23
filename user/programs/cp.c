/* cp: copy one file to another. */

#include <mcube/libc.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(2, "usage: cp SRC DST\n");
        return 1;
    }

    int in = open(argv[1], VFS_O_RDONLY);
    if (in < 0) {
        fprintf(2, "cp: %s: not found\n", argv[1]);
        return 1;
    }
    int out = open(argv[2], VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (out < 0) {
        fprintf(2, "cp: %s: cannot create\n", argv[2]);
        close(in);
        return 1;
    }

    char buf[512];
    long n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) { rc = 1; break; }
    }
    if (n < 0) rc = 1;

    close(in);
    close(out);
    return rc;
}
