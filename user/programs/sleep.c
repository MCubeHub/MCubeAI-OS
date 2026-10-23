/* sleep: pause for N seconds (fractions via ms suffix). */

#include <mcube/libc.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(2, "usage: sleep SECONDS\n");
        return 1;
    }
    int ms = atoi(argv[1]) * 1000;
    if (strstr(argv[1], "ms")) ms = atoi(argv[1]);
    if (ms > 0) sleep_ms((u64)ms);
    return 0;
}
