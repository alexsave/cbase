// hex.h - hex text of bytes, and the digit helpers every hand-rolled copy of it
// carried: a session id, a game id, a uuid, a SigV4 digest, a pagination cursor
// and a percent-escape all spell bytes this way.
//
// Freestanding: no allocation, no libc.
#ifndef SHARED_HEX_H
#define SHARED_HEX_H

#include <stddef.h>
#include <stdint.h>

// The digit for a nibble (0..15): lowercase, or uppercase as a percent-escape
// (RFC 3986 section 2.1) writes it.
static inline char hex_digit(unsigned v) { return "0123456789abcdef"[v & 15]; }
static inline char hex_digit_upper(unsigned v) { return "0123456789ABCDEF"[v & 15]; }

// A hex digit's value, either case, or -1.
static inline int hex_nibble(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Lowercase hex of n bytes and a NUL. Returns 2n, or -1 when cap < 2n + 1.
int hex_encode(const uint8_t *in, size_t n, char *out, size_t cap);
// Hex of either case. Returns the bytes written, or -1 for an odd length, a
// non-hex digit, or cap too small.
int hex_decode(const char *in, size_t n, uint8_t *out, size_t cap);

#endif
