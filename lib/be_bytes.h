/* be_bytes.h - fixed-width big-endian integers in a byte buffer: u16 and u32,
 * the order of PNG chunks, TrueType tables and the uttt message headers. The
 * little-endian half is le_bytes.h; the out-of-range rule is the same (a put
 * keeps the low bytes, a get never sign-extends).
 *
 * Header-only and freestanding (stdint.h only). A product includes it by a
 * relative path. */
#ifndef SHARED_BE_BYTES_H
#define SHARED_BE_BYTES_H

#include <stdint.h>

static inline void be_put_u16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xff);
}

static inline void be_put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

static inline uint16_t be_get_u16(const unsigned char *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t be_get_u32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

#endif
