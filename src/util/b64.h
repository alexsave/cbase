// b64.h - base64, both alphabets (RFC 4648 sections 4 and 5).
//
// Standard base64 with '=' padding is what a Realtime broadcast carries
// ({b: <base64 cast>}) and what the WebSocket handshake answers with. The
// URL-safe alphabet without padding is what a JWT's three parts and a refresh
// token are written in. The decoder reads either alphabet, with or without
// padding, because a reader that is picky about which one it was handed buys
// nothing.
#ifndef CB_B64_H
#define CB_B64_H

#include <stddef.h>
#include <stdint.h>

// Characters cb_b64_encode writes for n bytes (padded), not counting the NUL.
size_t cb_b64_len(size_t n);
// Characters cb_b64url_encode writes for n bytes (unpadded), not counting the NUL.
size_t cb_b64url_len(size_t n);

// Writes the encoding and a NUL. Returns the characters written (not the NUL),
// or -1 when cap cannot hold them and the NUL.
int cb_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);
int cb_b64url_encode(const uint8_t *in, size_t n, char *out, size_t cap);

// Decodes `n` characters of either alphabet; trailing '=' optional. Returns
// the bytes written, or -1 for a character outside both alphabets, a length
// no encoding produces (n % 4 == 1), padding in the middle, or cap too small.
int cb_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap);

#endif
