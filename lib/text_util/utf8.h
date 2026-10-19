/* utf8.h - strict UTF-8, header-only and freestanding: one decoder and one
 * scanner that every product's trust boundary (names, titles, ids, a response
 * body) calls, so "what is valid UTF-8" has one owner.
 *
 * STRICT means shortest form only (no overlong), no UTF-16 surrogates
 * (U+D800-DFFF), nothing past U+10FFFF, and a sequence that runs off the end
 * is invalid. The lenient stepper that turns bad bytes into U+FFFD is
 * text_util.h's text_next_cp, a different policy on purpose.
 *
 * THE POLICIES THAT DIFFER ARE FLAGS, not a second copy:
 *   TEXT_UTF8_NO_CONTROLS also refuses C0 controls, DEL and the C1 range
 *     (U+0000-001F, U+007F, U+0080-009F), as a person's display name does.
 *   Without it, controls are valid text (a title, an HTTP body).
 * The counts a caller wants are outputs, not policies: code points (what a
 * length limit in characters counts) and UTF-16 units (what a JS string's
 * length counts: a code point above U+FFFF is two). Either pointer may be NULL. */
#ifndef SHARED_TEXT_UTF8_H
#define SHARED_TEXT_UTF8_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEXT_UTF8_NO_CONTROLS 1u

/* One scalar value at s (at most `left` bytes): its length in bytes, or -1 when
 * the bytes are not strict UTF-8 (left == 0 included). *cp may be NULL. */
static inline int text_utf8_next(const unsigned char *s, size_t left, uint32_t *cp)
{
    if (left == 0) return -1;
    const unsigned c = s[0];
    if (c < 0x80) { if (cp) *cp = c; return 1; }
    int k;
    uint32_t v, min;
    if (c >= 0xc2 && c <= 0xdf)      { k = 2; v = c & 0x1f; min = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { k = 3; v = c & 0x0f; min = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { k = 4; v = c & 0x07; min = 0x10000; }
    else return -1;
    if ((size_t)k > left) return -1;
    for (int j = 1; j < k; j++) {
        if ((s[j] & 0xc0) != 0x80) return -1;
        v = (v << 6) | (s[j] & 0x3f);
    }
    if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return -1;
    if (cp) *cp = v;
    return k;
}

/* All n bytes of s as strict UTF-8 under `flags`: 0 when valid (with the code
 * point and UTF-16 unit counts in the outputs), -1 when not (outputs untouched).
 * ASCII runs are skipped eight bytes at a time when controls are allowed:
 * names, ids and titles are mostly ASCII. */
static inline int text_utf8_scan(const void *str, size_t n, unsigned flags, int *cps, int *utf16)
{
    const unsigned char *s = (const unsigned char *)str;
    size_t i = 0;
    int nc = 0, nu = 0;
    while (i < n) {
        if (!(flags & TEXT_UTF8_NO_CONTROLS)) {
            uint64_t w;
            while (i + 8 <= n && (memcpy(&w, s + i, 8), (w & 0x8080808080808080ull) == 0)) { i += 8; nc += 8; nu += 8; }
            if (i >= n) break;
        }
        uint32_t cp;
        const int k = text_utf8_next(s + i, n - i, &cp);
        if (k < 0) return -1;
        if ((flags & TEXT_UTF8_NO_CONTROLS) && (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0))) return -1;
        i += (size_t)k;
        nc++;
        nu += cp >= 0x10000 ? 2 : 1;
    }
    if (cps) *cps = nc;
    if (utf16) *utf16 = nu;
    return 0;
}

#endif
