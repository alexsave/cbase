/* The freestanding C library for a wasm32 kernel built -nostdlib
 * -ffreestanding: the string functions, and what clang lowers struct copies
 * and zeroing to. Declared in include/string.h, include/stdio.h and
 * include/stdlib.h.
 *
 * A product links this file and keeps what it reaches; the linker drops the
 * rest. The allocator is beside it in bump_alloc.c, a file of its own because
 * a free that does nothing is a choice a product makes by linking it. What is
 * a product's own policy rather than a C library - getenv, a stdio with
 * semantics of its own - stays with that product. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Overlap allowed, either direction (utb_adopt moves a board onto itself). */
void *memmove(void *dst, const void *src, size_t n)
{
    __builtin_memmove(dst, src, n);
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

/* The leading whitespace is C's isspace set, as the host's atoi skips it. The
 * digits accumulate unsigned so a value past the type's range wraps instead
 * of being undefined; the host promises nothing there either. */
long atol(const char *s)
{
    unsigned long v = 0;
    int neg = 0;
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10ul + (unsigned long)(*s++ - '0');
    return (long)(neg ? 0ul - v : v);
}

int atoi(const char *s) { return (int)atol(s); }

/* A small snprintf: %s, %d and %%, nothing else - all its users write. Like
 * the real one it returns the length it wanted and always terminates, and a
 * conversion that does not fit is cut short, never looped on.
 *
 * Any other conversion (%u, %x, %c, %ld, a width) traps: skipping it would
 * leave its argument unread and hand every later conversion the wrong one,
 * which prints memory. Trapping makes the misuse fail on its first test run.
 *
 * libc_test.c beside this file runs it natively. */
int snprintf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = 0;
    /* ch is evaluated exactly once, whether or not it fits: the callers pass
     * *s++ and d[--k], which must advance even once the buffer is full. */
#define PUT(ch) do { char c_ = (char)(ch); if (n + 1 < cap) out[n] = c_; n++; } while (0)
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
        } else {
            __builtin_trap();
        }
    }
#undef PUT
    if (cap) out[n < cap ? n : cap - 1] = 0;
    va_end(ap);
    return (int)n;
}
