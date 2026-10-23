/* env: print the environment. */

#include <mcube/libc.h>

int main(void) {
    if (environ) {
        for (char **e = environ; *e; e++) printf("%s\n", *e);
    }
    return 0;
}
