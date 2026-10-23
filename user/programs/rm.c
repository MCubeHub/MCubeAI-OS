/* rm: remove files or empty directories. */

#include <mcube/libc.h>

int main(int argc, char **argv) {
    int start = 1;
    int force = 0;
    if (argc > 1 && strcmp(argv[1], "-f") == 0) { force = 1; start = 2; }

    if (argc <= start) {
        fprintf(2, "usage: rm [-f] FILE...\n");
        return 1;
    }

    int rc = 0;
    for (int i = start; i < argc; i++) {
        mcube_stat_t st;
        if (stat(argv[i], &st) != 0) {
            if (!force) { fprintf(2, "rm: %s: not found\n", argv[i]); rc = 1; }
            continue;
        }
        int err = (st.type == VFS_TYPE_DIR) ? rmdir(argv[i]) : unlink(argv[i]);
        if (err != 0) {
            fprintf(2, "rm: %s: cannot remove\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
