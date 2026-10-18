// bytea.h - Postgres's '\x' bytea spelling of hex (shared/c/hex for the hex).
//
// PostgREST sends a BYTEA column as the text "\x" followed by lowercase hex,
// which inside a JSON string is written "\\x...". The clients read view rows,
// replay codes and extras that way, and the migration export writes them that
// way, so c_base speaks it on both edges.
#ifndef CB_BYTEA_H
#define CB_BYTEA_H

#include <stddef.h>
#include <stdint.h>

// "\x" + lowercase hex + NUL. Returns 2 + 2n, or -1.
int cb_bytea_encode(const uint8_t *in, size_t n, char *out, size_t cap);
// Hex of either case, optionally prefixed with "\x" (one backslash). Returns
// the bytes written, or -1 for an odd length, a non-hex digit, or cap too small.
int cb_bytea_decode(const char *in, size_t n, uint8_t *out, size_t cap);

#endif
