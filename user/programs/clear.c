/* clear: clear the console (ANSI erase display). */

#include <mcube/libc.h>

int main(void) {
    fputs("\033[2J\033[H", 1);
    return 0;
}
