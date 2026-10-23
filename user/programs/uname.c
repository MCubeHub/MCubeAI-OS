/* uname: print the system name. */

#include <mcube/libc.h>

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        printf("MCubeAIOS 0.1.0 Genesis x86_64\n");
    } else {
        printf("MCubeAIOS\n");
    }
    return 0;
}
