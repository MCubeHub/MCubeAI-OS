/* Fault injection helper used to check that an application crash only takes
 * down that application. The desktop keeps running, the window disappears and
 * the kernel log shows the fault instead of a panic.
 *
 *   faulttest           write to a null pointer  (#PF)
 *   faulttest ud2       execute an invalid opcode (#UD)
 *   faulttest gp        run a privileged instruction from CPL3 (#GP)
 */

#include <mcube/libc.h>

int main(int argc, char **argv, char **envp) {
    (void)envp;
    const char *what = (argc > 1 && argv[1]) ? argv[1] : "null";
    printf("faulttest: injecting %s\n", what);

    if (strcmp(what, "ud2") == 0) {
        __asm__ __volatile__("ud2");
    } else if (strcmp(what, "gp") == 0) {
        __asm__ __volatile__("cli");
    } else {
        *(volatile unsigned long *)0 = 1;
    }

    printf("faulttest: no fault was raised\n");
    return 1;
}
