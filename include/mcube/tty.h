#pragma once
/* Console input multiplexer: PS/2 keyboard + serial RX feed one queue. */

#include <mcube/types.h>

void tty_init(void);
void tty_input(char c);
int  tty_pending(void);
long tty_read(char *buf, u64 len);        /* -EAGAIN when empty            */
void tty_write(const char *buf, u64 len); /* serial + framebuffer console  */
