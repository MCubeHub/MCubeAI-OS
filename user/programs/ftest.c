/* Fault injection helper used to check that an application crash only takes
 * down that application. The desktop keeps running, the window disappears and
 * the kernel log shows the fault instead of a panic.
 *
 *   ftest           write to a null pointer  (#PF)
 *   ftest ud2       execute an invalid opcode (#UD)
 *   ftest gp        run a privileged instruction from CPL3 (#GP)
 *
 * The name is kept to 8.3 because the FAT32 driver cannot read long file
 * names yet; a longer name is packed as FAULTT~1.LGY and the shell's exec
 * path lookup (which uses the name the user typed) would not find it.
 */

#include <mcube/libc.h>

int main(int argc, char **argv, char **envp) {
    (void)envp;
    const char *what = (argc > 1 && argv[1]) ? argv[1] : "null";
    printf("ftest: injecting %s\n", what);

    if (strcmp(what, "ud2") == 0) {
        __asm__ __volatile__("ud2");
    } else if (strcmp(what, "gp") == 0) {
        __asm__ __volatile__("cli");
    } else {
        *(volatile unsigned long *)0 = 1;
    }

    printf("ftest: no fault was raised\n");
    return 1;
}
