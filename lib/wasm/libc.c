/* The freestanding C library for a wasm32 kernel built -nostdlib
 * -ffreestanding: the string functions, and what clang lowers struct copies
 * and zeroing to. Declared in include/string.h and include/stdio.h.
 *
 * A product links this file and keeps what it reaches; the linker drops the
 * rest. What is a product's own policy rather than a C library - an
 * allocator, getenv, a stdio with semantics of its own - stays with that
 * product. */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* With -mbulk-memory these __builtin calls - and every struct copy clang
 * lowers - compile to the single wasm memory.copy / memory.fill instruction.
 * The out-of-line definitions only back the calls clang chooses not to lower
 * inline. */
void *memcpy(void *dst, const void *src, size_t n)
{
    __builtin_memcpy(dst, src, n);
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    __builtin_memset(dst, c, n);
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

/* A small snprintf: %s, %d and %%, nothing else - all its users write. Like
 * the real one it returns the length it wanted and always terminates. */
int snprintf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = 0;
#define PUT(ch) do { if (n + 1 < cap) out[n] = (ch); n++; } while (0)
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++;
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "";
            while (*s) PUT(*s++);
        } else if (*f == 'd') {
            int v = va_arg(ap, int);
            unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
            char d[12]; int k = 0;
            do { d[k++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (v < 0) PUT('-');
            while (k) PUT(d[--k]);
        } else if (*f == '%') {
            PUT('%');
        } else if (!*f) {
            break;
        }
    }
#undef PUT
    if (cap) out[n < cap ? n : cap - 1] = 0;
    va_end(ap);
    return (int)n;
}
