/* echo: write arguments separated by spaces. */

#include <mcube/libc.h>

int main(int argc, char **argv) {
    int newline = 1;
    int start = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        start = 2;
    }
    for (int i = start; i < argc; i++) {
        if (i > start) putchar(' ');
        fputs(argv[i], 1);
    }
    if (newline) putchar('\n');
    return 0;
}
