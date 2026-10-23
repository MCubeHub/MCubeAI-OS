/* The kernel console font is shared with user space so the desktop can render
 * text without a second generated copy. The source file only depends on
 * <mcube/types.h>, so including it here is safe. */

#include "../../kernel/font/font8x16.c"
