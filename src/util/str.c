#include "str.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

cb_span cb_span_cstr(const char *s) { cb_span v = { s ? s : "", s ? strlen(s) : 0 }; return v; }

bool cb_span_eq(cb_span a, const char *cstr) {
    size_t n = strlen(cstr);
    return a.n == n && memcmp(a.p, cstr, n) == 0;
}

bool cb_span_eq_nocase(cb_span a, const char *cstr) {
    size_t n = strlen(cstr);
    return a.n == n && strncasecmp(a.p, cstr, n) == 0;
}

void cb_buf_init(cb_buf *b) { b->p = NULL; b->len = 0; b->cap = 0; b->oom = false; }

void cb_buf_free(cb_buf *b) { free(b->p); cb_buf_init(b); }

void cb_buf_reset(cb_buf *b) { b->len = 0; if (b->p) b->p[0] = 0; }

bool cb_buf_reserve(cb_buf *b, size_t extra) {
    if (b->oom) return false;
    if (extra > SIZE_MAX / 2 - b->len) { b->oom = true; return false; }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p) { b->oom = true; return false; }
    b->p = p; b->cap = cap;
    return true;
}

void cb_buf_append(cb_buf *b, const void *p, size_t n) {
    if (!cb_buf_reserve(b, n)) return;
    if (n) memcpy(b->p + b->len, p, n);
    b->len += n;
    b->p[b->len] = 0;
}

void cb_buf_puts(cb_buf *b, const char *s) { cb_buf_append(b, s, strlen(s)); }

void cb_buf_putc(cb_buf *b, char c) { cb_buf_append(b, &c, 1); }

void cb_buf_vprintf(cb_buf *b, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0 || !cb_buf_reserve(b, (size_t)n)) return;
    vsnprintf((char *)b->p + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void cb_buf_printf(cb_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cb_buf_vprintf(b, fmt, ap);
    va_end(ap);
}

void cb_buf_consume(cb_buf *b, size_t n) {
    if (n >= b->len) { cb_buf_reset(b); return; }
    memmove(b->p, b->p + n, b->len - n);
    b->len -= n;
    b->p[b->len] = 0;
}

uint8_t *cb_buf_take(cb_buf *b, size_t *len) {
    if (b->oom) { cb_buf_free(b); if (len) *len = 0; return NULL; }
    if (!b->p && !cb_buf_reserve(b, 0)) { if (len) *len = 0; return NULL; }
    b->p[b->len] = 0;
    uint8_t *p = b->p;
    if (len) *len = b->len;
    cb_buf_init(b);
    return p;
}

const char *cb_buf_cstr(const cb_buf *b) { return b->p ? (const char *)b->p : ""; }

size_t cb_strlcpy(char *dst, const char *src, size_t cap) {
    size_t n = strlen(src);
    if (cap) {
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return n;
}

bool cb_strncpy0(char *dst, size_t cap, const char *src, size_t n) {
    if (cap == 0) return false;
    if (n >= cap) { dst[0] = 0; return false; }
    memcpy(dst, src, n);
    dst[n] = 0;
    return true;
}

bool cb_streq_nocase(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

bool cb_starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int cb_url_decode(const char *in, size_t n, char *out, size_t cap, bool plus_is_space) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= n) return -1;
            int hi = hexval((unsigned char)in[i + 1]), lo = hexval((unsigned char)in[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            c = (char)(hi * 16 + lo);
            i += 2;
        } else if (c == '+' && plus_is_space) {
            c = ' ';
        }
        if (o + 1 >= cap) return -1;
        out[o++] = c;
    }
    if (cap == 0) return -1;
    out[o] = 0;
    return (int)o;
}
