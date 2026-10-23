/* help: list the programs shipped in /bin. */

#include <mcube/libc.h>

int main(void) {
    printf("MCubeAIOS programs:\n");
    mcube_dirent_t ent;
    for (int i = 0; mc_readdir("/bin", i, &ent) == 0; i++) {
        if (ent.type == VFS_TYPE_FILE) printf("  %s\n", ent.name);
    }
    return 0;
}
