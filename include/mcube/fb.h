#pragma once
/* Framebuffer ABI shared by the kernel console and the user-space desktop.
 *
 * The kernel owns the GOP linear framebuffer and uses it as a text console
 * during boot. Once the desktop maps it with SYS_FB_MAP the kernel console
 * stops drawing (serial output continues) and user space becomes the only
 * writer. The mapping is placed at a fixed virtual address so the desktop can
 * use it without any further bookkeeping.
 */

#include <mcube/types.h>

#define MCUBE_FB_USER_BASE 0x0000000030000000ULL

typedef struct PACKED mcube_fb_info {
    u64 phys;      /* physical base address of the linear framebuffer */
    u64 size;      /* mapped size in bytes (pitch * height, page aligned) */
    u32 width;
    u32 height;
    u32 pitch;     /* bytes per scanline */
    u32 format;    /* enum boot_pixel_format */
    u32 bpp;       /* bits per pixel (32 for the supported formats) */
    u32 reserved;
} mcube_fb_info_t;

/* Kernel side (drivers/fb.c). */
int  fb_get_info(mcube_fb_info_t *out);
void fb_console_disable(void);
