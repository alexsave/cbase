/* fnv.h - FNV-1a, the one non-cryptographic hash every kernel folds bytes
 * with: a game's hash for a bot's table, a seed salt, a roster hash, a lock
 * stripe, a texture pin, a golden's checksum.
 *
 * The step is (h ^ v) * prime. Most callers fold a byte at a time; a few fold a
 * whole value at once (a bitboard, a field wider than a byte), which is the same
 * step with a wider v, so the step takes the hash's own width. Whatever a caller
 * feeds and whatever basis it starts from (the standard offset, or one salted
 * with a seed) stays the caller's: those choices are what its frozen outputs
 * depend on, and this file only owns the arithmetic.
 *
 * Header-only and freestanding. NOT for anything an adversary chooses the
 * input of where collisions matter: it is not a keyed hash. */
#ifndef SHARED_FNV_H
#define SHARED_FNV_H

#include <stddef.h>
#include <stdint.h>

#define FNV32_OFFSET UINT32_C(2166136261)
#define FNV32_PRIME  UINT32_C(16777619)
#define FNV64_OFFSET UINT64_C(14695981039346656037)
#define FNV64_PRIME  UINT64_C(1099511628211)

static inline uint32_t fnv1a32_step(uint32_t h, uint32_t v) { return (h ^ v) * FNV32_PRIME; }
static inline uint64_t fnv1a64_step(uint64_t h, uint64_t v) { return (h ^ v) * FNV64_PRIME; }

/* n bytes folded into h, one at a time. */
static inline uint32_t fnv1a32(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = fnv1a32_step(h, b[i]);
    return h;
}
static inline uint64_t fnv1a64(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = fnv1a64_step(h, b[i]);
    return h;
}

/* A NUL-terminated string's bytes folded into h. */
static inline uint32_t fnv1a32_str(uint32_t h, const char *s)
{
    for (; *s; s++) h = fnv1a32_step(h, (uint8_t)*s);
    return h;
}
static inline uint64_t fnv1a64_str(uint64_t h, const char *s)
{
    for (; *s; s++) h = fnv1a64_step(h, (uint8_t)*s);
    return h;
}

#endif
