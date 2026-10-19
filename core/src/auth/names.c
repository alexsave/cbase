// names.c - a username's canonical form (names.h).
#include "names.h"

#include "auth.h"
#include "util/err.h"
#include "../../../../shared/c/text_util/utf8.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// One code point from p (at most `left` bytes): strict, shared/c/text_util/utf8.h.
static int utf8_next(const unsigned char *p, size_t left, uint32_t *cp) {
    return text_utf8_next(p, left, cp);
}

static int utf8_put(uint32_t cp, char *out, size_t cap, size_t at) {
    unsigned char b[4];
    int n;
    if (cp < 0x80) { b[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (unsigned char)(0xC0 | (cp >> 6)); b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12)); b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        b[0] = (unsigned char)(0xF0 | (cp >> 18)); b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (at + (size_t)n >= cap) return -1;
    memcpy(out + at, b, (size_t)n);
    return n;
}

// The upper case of cp where toUpperCase gives one code point for it, in the
// scripts names.h lists; cp itself otherwise (and for a letter whose upper
// case is several, ß and ŉ among them: a client sends those already expanded).
static uint32_t upper(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp == 0xB5) return 0x39C;                                     // µ -> Greek capital mu
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 32;       // à..þ, not ÷
    if (cp == 0xFF) return 0x178;                                     // ÿ -> Ÿ
    if ((cp >= 0x100 && cp <= 0x12F) || (cp >= 0x132 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177))
        return cp & 1 ? cp - 1 : cp;                                  // even upper, odd lower
    if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E))
        return cp & 1 ? cp : cp - 1;                                  // odd upper, even lower
    if (cp == 0x131) return 'I';                                      // dotless ı
    if (cp == 0x17F) return 'S';                                      // long ſ
    if (cp >= 0x3B1 && cp <= 0x3CB && cp != 0x3C2) return cp - 32;    // α..ϋ, final ς below
    if (cp == 0x3C2) return 0x3A3;
    if (cp == 0x3AC) return 0x386;
    if (cp >= 0x3AD && cp <= 0x3AF) return cp - 37;                   // έ ή ί
    if (cp == 0x3CC) return 0x38C;
    if (cp == 0x3CD || cp == 0x3CE) return cp - 63;                   // ύ ώ
    if (cp >= 0x430 && cp <= 0x44F) return cp - 32;                   // а..я
    if (cp >= 0x450 && cp <= 0x45F) return cp - 80;                   // ѐ..џ
    return cp;
}

int cb_name_case(const char *in, size_t n, char *out, size_t cap) {
    if (!in || !out || cap == 0) return -1;
    const unsigned char *p = (const unsigned char *)in;
    size_t at = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int k = utf8_next(p + i, n - i, &cp);
        if (k < 0) return -1;
        int w = utf8_put(upper(cp), out, cap, at);
        if (w < 0) return -1;
        at += (size_t)w;
        i += (size_t)k;
    }
    out[at] = 0;
    return (int)at;
}

// Characters nobody can see, or that change how others are shown.
static bool invisible(uint32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0xA0 || cp == 0xAD || cp == 0x34F || cp == 0x61C ||
           cp == 0x115F || cp == 0x1160 || cp == 0x17B4 || cp == 0x17B5 || (cp >= 0x180B && cp <= 0x180F) ||
           (cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202F) || (cp >= 0x205F && cp <= 0x206F) ||
           cp == 0x2800 || cp == 0x3000 || cp == 0x3164 || cp == 0xFEFF || cp == 0xFFA0 ||
           (cp >= 0xFFF0 && cp <= 0xFFFB) || (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
           (cp >= 0x1D173 && cp <= 0x1D17A) || (cp >= 0xE0000 && cp <= 0xE0FFF) || cp >= 0xF0000;
}

static bool variation_selector(uint32_t cp) { return (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF); }

// A symbol a variation selector may follow (it picks emoji or text style).
static bool emoji_base(uint32_t cp) {
    return cp == 0xA9 || cp == 0xAE || (cp >= 0x2190 && cp <= 0x2BFF) || (cp >= 0x3030 && cp <= 0x303D) ||
           cp == 0x3297 || cp == 0x3299 || (cp >= 0x1F000 && cp <= 0x1FAFF);
}

enum { SCRIPT_NONE = 0, SCRIPT_LATIN = 1, SCRIPT_GREEK = 2, SCRIPT_CYRILLIC = 4 };

static int script_of(uint32_t cp) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return SCRIPT_LATIN;
    if (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7) return SCRIPT_LATIN;
    if (cp >= 0x1E00 && cp <= 0x1EFF) return SCRIPT_LATIN;
    if (cp >= 0x370 && cp <= 0x3FF) return SCRIPT_GREEK;
    if (cp >= 0x1F00 && cp <= 0x1FFF) return SCRIPT_GREEK;
    if (cp >= 0x400 && cp <= 0x52F) return SCRIPT_CYRILLIC;
    return SCRIPT_NONE;
}

int cb_name_check(const char *name, size_t n) {
    if (!name || n == 0 || n > CB_USERNAME_MAX) return CB_AUTH_E_BAD_USERNAME;
    const unsigned char *p = (const unsigned char *)name;
    int chars = 0, scripts = 0;
    uint32_t prev = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int k = utf8_next(p + i, n - i, &cp);
        if (k < 0 || cp == '%' || invisible(cp)) return CB_AUTH_E_BAD_USERNAME;
        if (variation_selector(cp) && !emoji_base(prev)) return CB_AUTH_E_BAD_USERNAME;
        if (cp == ' ' && (i == 0 || prev == ' ' || i + 1 == n)) return CB_AUTH_E_BAD_USERNAME;
        scripts |= script_of(cp);
        if (++chars > CB_NAME_MAX_CHARS) return CB_AUTH_E_BAD_USERNAME;
        prev = cp;
        i += (size_t)k;
    }
    // More than one of the three confusable scripts: the look-alike spoof.
    if (scripts & (scripts - 1)) return CB_AUTH_E_BAD_USERNAME;
    return CB_OK;
}

int cb_name_key(const char *name, size_t n, char *out, size_t cap) {
    int k = cb_name_case(name, n, out, cap);
    if (k >= 0 || !name || !out || n + 1 > cap) return k;
    memcpy(out, name, n);
    out[n] = 0;
    return (int)n;
}
