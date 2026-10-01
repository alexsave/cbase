/* le_bytes.h - fixed-width little-endian integers in a byte buffer: u16, u32
 * and u48, read and written at a pointer with no alignment assumed.
 *
 * WHY U48 AND NOT U64. A clock in a row's bytes is u48 little-endian epoch ms
 * because the session log already stores its record timestamps that way, and
 * 48 bits of ms is about 8,900 years, so every clock in the row has one width.
 *
 * THE OUT-OF-RANGE RULE. A put writes the value's low bytes and drops the rest:
 * le_put_u48 keeps the low 48 bits of its uint64_t, the narrower puts take
 * their argument already narrowed by the usual C conversion (modulo 2^n). A
 * caller with a signed value decides what a negative one means before it calls
 * (one that wants "negative is never" clamps to 0 first). A get never fails and
 * never sign-extends: le_get_u48 is always in [0, 2^48 - 1].
 *
 * Header-only and freestanding (stdint.h only), so a wasm32 build needs nothing
 * from shared/c/wasm for it. A product includes it by a relative path. */
#ifndef SHARED_LE_BYTES_H
#define SHARED_LE_BYTES_H

#include <stdint.h>

#define LE_U48_MAX ((uint64_t)0xffffffffffffull)

static inline void le_put_u16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)(v >> 8);
}

static inline void le_put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)(v >> 24);
}

static inline void le_put_u48(unsigned char *p, uint64_t v) {
    le_put_u32(p, (uint32_t)(v & 0xffffffffu));
    le_put_u16(p + 4, (uint16_t)((v >> 32) & 0xffff));
}

static inline uint16_t le_get_u16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t le_get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t le_get_u48(const unsigned char *p) {
    return (uint64_t)le_get_u32(p) | ((uint64_t)le_get_u16(p + 4) << 32);
}

#endif
