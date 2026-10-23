/* Minimal freestanding printf implementation shared by kernel and drivers. */

#include <mcube/kernel.h>
#include <stdarg.h>

static usize fmt_uint(char *out, usize cap, u64 value, unsigned base,
                      int width, int zero_pad, int left_align, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value) {
            tmp[n++] = digits[value % base];
            value /= base;
        }
    }
    usize written = 0;
    int pad = width > n ? width - n : 0;
    if (!left_align) {
        while (pad-- > 0 && written < cap) out[written++] = zero_pad ? '0' : ' ';
    }
    while (n > 0 && written < cap) out[written++] = tmp[--n];
    if (left_align) {
        while (pad-- > 0 && written < cap) out[written++] = ' ';
    }
    return written;
}

int kvsnprintf(char *buf, usize size, const char *fmt, va_list ap) {
    usize pos = 0;
#define EMIT(c) do { if (pos + 1 < size) buf[pos] = (c); pos++; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { EMIT(*p); continue; }
        p++;
        int zero_pad = 0, left_align = 0, width = 0;
        if (*p == '-') { left_align = 1; p++; }
        if (*p == '0') { zero_pad = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        int is_long = 0;
        while (*p == 'l') { is_long++; p++; }
        int is_size = 0;
        if (*p == 'z') { is_size = 1; p++; }
        char tmp[32];
        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            usize len = strlen(s);
            int pad = width > (int)len ? width - (int)len : 0;
            if (!left_align) while (pad-- > 0) EMIT(' ');
            while (*s) EMIT(*s++);
            if (left_align) while (pad-- > 0) EMIT(' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            EMIT(c);
            break;
        }
        case 'd':
        case 'i': {
            i64 v;
            if (is_size) v = (i64)va_arg(ap, usize);
            else if (is_long) v = va_arg(ap, i64);
            else v = (i64)va_arg(ap, int);
            if (v < 0) {
                EMIT('-');
                u64 uv = (u64)(-(v + 1)) + 1;
                usize n = fmt_uint(tmp, sizeof(tmp), uv, 10, width - 1, zero_pad, left_align, 0);
                for (usize i = 0; i < n; i++) EMIT(tmp[i]);
            } else {
                usize n = fmt_uint(tmp, sizeof(tmp), (u64)v, 10, width, zero_pad, left_align, 0);
                for (usize i = 0; i < n; i++) EMIT(tmp[i]);
            }
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            u64 v;
            if (is_size) v = va_arg(ap, usize);
            else v = is_long ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned int);
            unsigned base = (*p == 'u') ? 10 : 16;
            usize n = fmt_uint(tmp, sizeof(tmp), v, base, width, zero_pad, left_align, *p == 'X');
            for (usize i = 0; i < n; i++) EMIT(tmp[i]);
            break;
        }
        case 'p': {
            u64 v = (u64)(uptr)va_arg(ap, void *);
            EMIT('0'); EMIT('x');
            usize n = fmt_uint(tmp, sizeof(tmp), v, 16, 16, 1, 0, 0);
            for (usize i = 0; i < n; i++) EMIT(tmp[i]);
            break;
        }
        case '%': EMIT('%'); break;
        default:
            EMIT('%');
            if (*p) EMIT(*p);
            break;
        }
    }
    if (size) buf[pos < size ? pos : size - 1] = 0;
#undef EMIT
    return (int)pos;
}

int ksnprintf(char *buf, usize size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int kprintf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (usize i = 0; buf[i] && i < sizeof(buf); i++) kputc(buf[i]);
    return n;
}

void kputs(const char *s) {
    while (*s) kputc(*s++);
}

/* console multiplexer lives here to keep kprintf dependency-free */
static bool g_fb_ready;

void console_init(const boot_info_t *bi) {
    serial_init();
    fb_init(bi);
    g_fb_ready = bi && bi->fb_base;
}

void kputc(char c) {
    serial_putc(c);
    if (g_fb_ready) fb_putc(c);
}

void console_clear(void) {
    if (g_fb_ready) fb_clear();
}
