// hex.h - hex text, and Postgres's '\x' bytea spelling of it.
//
// PostgREST sends a BYTEA column as the text "\x" followed by lowercase hex,
// which inside a JSON string is written "\\x...". The clients read view rows,
// replay codes and extras that way, and the migration export writes them that
// way, so c_base speaks it on both edges.
#ifndef CB_HEX_H
#define CB_HEX_H

#include <stddef.h>
#include <stdint.h>

// Lowercase hex of n bytes and a NUL. Returns 2n, or -1 when cap < 2n + 1.
int cb_hex_encode(const uint8_t *in, size_t n, char *out, size_t cap);
// "\x" + lowercase hex + NUL. Returns 2 + 2n, or -1.
int cb_bytea_encode(const uint8_t *in, size_t n, char *out, size_t cap);
// Hex of either case, optionally prefixed with "\x" (one backslash). Returns
// the bytes written, or -1 for an odd length, a non-hex digit, or cap too small.
int cb_hex_decode(const char *in, size_t n, uint8_t *out, size_t cap);

#endif
