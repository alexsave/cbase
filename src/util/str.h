// str.h - byte buffers, spans and the small string helpers every module needs.
//
// cb_buf is the one growable buffer in c_base: response bodies, JSON being
// written, a WebSocket's outbound queue. It always keeps one spare byte past
// `len` and writes a NUL there, so a buffer holding text can be handed to a
// function that wants a C string without a copy.
#ifndef CB_STR_H
#define CB_STR_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A view of bytes someone else owns. Not NUL-terminated in general.
typedef struct {
    const char *p;
    size_t      n;
} cb_span;

#define CB_SPAN_LIT(s) ((cb_span){ (s), sizeof(s) - 1 })
static inline cb_span cb_span_of(const char *s, size_t n) { cb_span v = { s, n }; return v; }
cb_span cb_span_cstr(const char *s);                   // NULL -> empty span
bool    cb_span_eq(cb_span a, const char *cstr);       // exact bytes
bool    cb_span_eq_nocase(cb_span a, const char *cstr);  // ASCII case-insensitive

typedef struct {
    uint8_t *p;     // NULL until the first append
    size_t   len;
    size_t   cap;
    bool     oom;   // an allocation failed; every later append is a no-op
} cb_buf;

void   cb_buf_init(cb_buf *b);
void   cb_buf_free(cb_buf *b);                 // and re-inits it
void   cb_buf_reset(cb_buf *b);                // len = 0, keeps the memory
bool   cb_buf_reserve(cb_buf *b, size_t extra); // room for `extra` more bytes (+ the NUL)
void   cb_buf_append(cb_buf *b, const void *p, size_t n);
void   cb_buf_puts(cb_buf *b, const char *s);
void   cb_buf_putc(cb_buf *b, char c);
void   cb_buf_printf(cb_buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void   cb_buf_vprintf(cb_buf *b, const char *fmt, va_list ap);
// Drops the first n bytes (a consumed request, a flushed write).
void   cb_buf_consume(cb_buf *b, size_t n);
// Hands the memory to the caller (free() it) and re-inits the buffer. Never
// NULL for a buffer that has not run out of memory: an empty one gives "".
uint8_t *cb_buf_take(cb_buf *b, size_t *len);
// The contents as a C string (the trailing NUL is always there). "" when empty.
const char *cb_buf_cstr(const cb_buf *b);

// strlcpy: copies at most cap-1 bytes and always terminates (cap > 0).
// Returns strlen(src), so a result >= cap means it was cut.
size_t cb_strlcpy(char *dst, const char *src, size_t cap);
// Copies exactly n bytes of src and terminates; false (and "" in dst) when n >= cap.
// src may be NULL when n is 0 (an empty span).
bool   cb_strncpy0(char *dst, size_t cap, const char *src, size_t n);
bool   cb_streq_nocase(const char *a, const char *b);
bool   cb_starts_with(const char *s, const char *prefix);

// Percent-decoding of a URL component. `plus_is_space` for a query string
// (application/x-www-form-urlencoded). Returns the decoded length, or -1 for a
// bad escape or an output that does not fit (cap includes the NUL written).
int    cb_url_decode(const char *in, size_t n, char *out, size_t cap, bool plus_is_space);

#endif
