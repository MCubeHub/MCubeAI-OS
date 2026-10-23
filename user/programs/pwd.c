/* pwd: print the working directory. */

#include <mcube/libc.h>

int main(void) {
    char buf[VFS_PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) return 1;
    printf("%s\n", buf);
    return 0;
}
