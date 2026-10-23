/* Formatted output for user programs (subset of printf). */

#include <mcube/libc.h>
#include <stdarg.h>

static size_t fmt_uint(char *out, size_t cap, u64 value, unsigned base,
                       int width, int zero_pad, int left, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int n = 0;
    if (value == 0) tmp[n++] = '0';
    while (value) {
        tmp[n++] = digits[value % base];
        value /= base;
    }

    size_t w = 0;
    int pad = width > n ? width - n : 0;
    if (!left) {
        while (pad-- > 0 && w < cap) out[w++] = zero_pad ? '0' : ' ';
    }
    while (n > 0 && w < cap) out[w++] = tmp[--n];
    if (left) {
        while (pad-- > 0 && w < cap) out[w++] = ' ';
    }
    return w;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;
#define EMIT(c) do { if (size && pos + 1 < size) buf[pos] = (char)(c); pos++; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { EMIT(*p); continue; }
        p++;
        int zero_pad = 0, left = 0, width = 0, lng = 0;
        if (*p == '-') { left = 1; p++; }
        if (*p == '0') { zero_pad = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        while (*p == 'l') { lng++; p++; }
        if (*p == 'z') { lng = 2; p++; }

        char tmp[32];
        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t len = strlen(s);
            int pad = width > (int)len ? width - (int)len : 0;
            if (!left) while (pad-- > 0) EMIT(' ');
            while (*s) EMIT(*s++);
            if (left) while (pad-- > 0) EMIT(' ');
            break;
        }
        case 'c':
            EMIT((char)va_arg(ap, int));
            break;
        case 'd':
        case 'i': {
            i64 v = lng ? va_arg(ap, i64) : (i64)va_arg(ap, int);
            if (v < 0) {
                EMIT('-');
                u64 uv = (u64)(-(v + 1)) + 1;
                size_t n = fmt_uint(tmp, sizeof(tmp), uv, 10, width - 1,
                                    zero_pad, left, 0);
                for (size_t i = 0; i < n; i++) EMIT(tmp[i]);
            } else {
                size_t n = fmt_uint(tmp, sizeof(tmp), (u64)v, 10, width,
                                    zero_pad, left, 0);
                for (size_t i = 0; i < n; i++) EMIT(tmp[i]);
            }
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            u64 v = lng ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned int);
            unsigned base = (*p == 'u') ? 10 : 16;
            size_t n = fmt_uint(tmp, sizeof(tmp), v, base, width, zero_pad,
                                left, *p == 'X');
            for (size_t i = 0; i < n; i++) EMIT(tmp[i]);
            break;
        }
        case 'p': {
            u64 v = (u64)(uptr)va_arg(ap, void *);
            EMIT('0'); EMIT('x');
            size_t n = fmt_uint(tmp, sizeof(tmp), v, 16, 16, 1, 0, 0);
            for (size_t i = 0; i < n; i++) EMIT(tmp[i]);
            break;
        }
        case '%':
            EMIT('%');
            break;
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

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(int fd, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) write(fd, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    return n;
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) write(1, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    return n;
}

int puts(const char *s) {
    write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int fputs(const char *s, int fd) {
    write(fd, s, strlen(s));
    return 0;
}

int fputc(int c, int fd) {
    char ch = (char)c;
    write(fd, &ch, 1);
    return c;
}
