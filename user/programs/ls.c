/* ls: list directory entries. */

#include <mcube/libc.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    mcube_stat_t st;
    if (stat(path, &st) != 0) {
        fprintf(2, "ls: %s: not found\n", path);
        return 1;
    }
    if (st.type != VFS_TYPE_DIR) {
        printf("%s\n", path);
        return 0;
    }

    mcube_dirent_t ent;
    int count = 0;
    for (int i = 0; mc_readdir(path, i, &ent) == 0; i++) {
        if (ent.namelen == 0) continue;
        printf("%s%s", ent.name, ent.type == VFS_TYPE_DIR ? "/" : "");
        if (++count % 4 == 0) putchar('\n');
        else putchar(' ');
    }
    if (count % 4 != 0) putchar('\n');
    return 0;
}
