// hpack.c - HPACK (RFC 7541): see hpack.h.
#include "net/hpack.h"

#include "util/err.h"

#include <stdlib.h>
#include <string.h>

// ---- the static table (RFC 7541 Appendix A) ---------------------------------
typedef struct {
    const char *name, *value;
    uint8_t     name_len, value_len;
} hp_static;

#define HP_STATIC_N 61
static const hp_static hp_static_table[HP_STATIC_N] = {
    {":authority",                   "",               10,  0},
    {":method",                      "GET",             7,  3},
    {":method",                      "POST",            7,  4},
    {":path",                        "/",               5,  1},
    {":path",                        "/index.html",     5, 11},
    {":scheme",                      "http",            7,  4},
    {":scheme",                      "https",           7,  5},
    {":status",                      "200",             7,  3},
    {":status",                      "204",             7,  3},
    {":status",                      "206",             7,  3},
    {":status",                      "304",             7,  3},
    {":status",                      "400",             7,  3},
    {":status",                      "404",             7,  3},
    {":status",                      "500",             7,  3},
    {"accept-charset",               "",               14,  0},
    {"accept-encoding",              "gzip, deflate",  15, 13},
    {"accept-language",              "",               15,  0},
    {"accept-ranges",                "",               13,  0},
    {"accept",                       "",                6,  0},
    {"access-control-allow-origin",  "",               27,  0},
    {"age",                          "",                3,  0},
    {"allow",                        "",                5,  0},
    {"authorization",                "",               13,  0},
    {"cache-control",                "",               13,  0},
    {"content-disposition",          "",               19,  0},
    {"content-encoding",             "",               16,  0},
    {"content-language",             "",               16,  0},
    {"content-length",               "",               14,  0},
    {"content-location",             "",               16,  0},
    {"content-range",                "",               13,  0},
    {"content-type",                 "",               12,  0},
    {"cookie",                       "",                6,  0},
    {"date",                         "",                4,  0},
    {"etag",                         "",                4,  0},
    {"expect",                       "",                6,  0},
    {"expires",                      "",                7,  0},
    {"from",                         "",                4,  0},
    {"host",                         "",                4,  0},
    {"if-match",                     "",                8,  0},
    {"if-modified-since",            "",               17,  0},
    {"if-none-match",                "",               13,  0},
    {"if-range",                     "",                8,  0},
    {"if-unmodified-since",          "",               19,  0},
    {"last-modified",                "",               13,  0},
    {"link",                         "",                4,  0},
    {"location",                     "",                8,  0},
    {"max-forwards",                 "",               12,  0},
    {"proxy-authenticate",           "",               18,  0},
    {"proxy-authorization",          "",               19,  0},
    {"range",                        "",                5,  0},
    {"referer",                      "",                7,  0},
    {"refresh",                      "",                7,  0},
    {"retry-after",                  "",               11,  0},
    {"server",                       "",                6,  0},
    {"set-cookie",                   "",               10,  0},
    {"strict-transport-security",    "",               25,  0},
    {"transfer-encoding",            "",               17,  0},
    {"user-agent",                   "",               10,  0},
    {"vary",                         "",                4,  0},
    {"via",                          "",                3,  0},
    {"www-authenticate",             "",               16,  0},
};

// ---- the Huffman code (RFC 7541 Appendix B) ---------------------------------
// Symbol 0..255 is a byte, 256 is EOS. Each code is its low `len` bits.
typedef struct {
    uint32_t code;
    uint8_t  len;
} hp_code_t;

static const hp_code_t hp_code[257] = {
    {0x00001ff8, 13}, {0x007fffd8, 23}, {0x0fffffe2, 28}, {0x0fffffe3, 28},
    {0x0fffffe4, 28}, {0x0fffffe5, 28}, {0x0fffffe6, 28}, {0x0fffffe7, 28},
    {0x0fffffe8, 28}, {0x00ffffea, 24}, {0x3ffffffc, 30}, {0x0fffffe9, 28},
    {0x0fffffea, 28}, {0x3ffffffd, 30}, {0x0fffffeb, 28}, {0x0fffffec, 28},
    {0x0fffffed, 28}, {0x0fffffee, 28}, {0x0fffffef, 28}, {0x0ffffff0, 28},
    {0x0ffffff1, 28}, {0x0ffffff2, 28}, {0x3ffffffe, 30}, {0x0ffffff3, 28},
    {0x0ffffff4, 28}, {0x0ffffff5, 28}, {0x0ffffff6, 28}, {0x0ffffff7, 28},
    {0x0ffffff8, 28}, {0x0ffffff9, 28}, {0x0ffffffa, 28}, {0x0ffffffb, 28},
    {0x00000014,  6}, {0x000003f8, 10}, {0x000003f9, 10}, {0x00000ffa, 12},
    {0x00001ff9, 13}, {0x00000015,  6}, {0x000000f8,  8}, {0x000007fa, 11},
    {0x000003fa, 10}, {0x000003fb, 10}, {0x000000f9,  8}, {0x000007fb, 11},
    {0x000000fa,  8}, {0x00000016,  6}, {0x00000017,  6}, {0x00000018,  6},
    {0x00000000,  5}, {0x00000001,  5}, {0x00000002,  5}, {0x00000019,  6},
    {0x0000001a,  6}, {0x0000001b,  6}, {0x0000001c,  6}, {0x0000001d,  6},
    {0x0000001e,  6}, {0x0000001f,  6}, {0x0000005c,  7}, {0x000000fb,  8},
    {0x00007ffc, 15}, {0x00000020,  6}, {0x00000ffb, 12}, {0x000003fc, 10},
    {0x00001ffa, 13}, {0x00000021,  6}, {0x0000005d,  7}, {0x0000005e,  7},
    {0x0000005f,  7}, {0x00000060,  7}, {0x00000061,  7}, {0x00000062,  7},
    {0x00000063,  7}, {0x00000064,  7}, {0x00000065,  7}, {0x00000066,  7},
    {0x00000067,  7}, {0x00000068,  7}, {0x00000069,  7}, {0x0000006a,  7},
    {0x0000006b,  7}, {0x0000006c,  7}, {0x0000006d,  7}, {0x0000006e,  7},
    {0x0000006f,  7}, {0x00000070,  7}, {0x00000071,  7}, {0x00000072,  7},
    {0x000000fc,  8}, {0x00000073,  7}, {0x000000fd,  8}, {0x00001ffb, 13},
    {0x0007fff0, 19}, {0x00001ffc, 13}, {0x00003ffc, 14}, {0x00000022,  6},
    {0x00007ffd, 15}, {0x00000003,  5}, {0x00000023,  6}, {0x00000004,  5},
    {0x00000024,  6}, {0x00000005,  5}, {0x00000025,  6}, {0x00000026,  6},
    {0x00000027,  6}, {0x00000006,  5}, {0x00000074,  7}, {0x00000075,  7},
    {0x00000028,  6}, {0x00000029,  6}, {0x0000002a,  6}, {0x00000007,  5},
    {0x0000002b,  6}, {0x00000076,  7}, {0x0000002c,  6}, {0x00000008,  5},
    {0x00000009,  5}, {0x0000002d,  6}, {0x00000077,  7}, {0x00000078,  7},
    {0x00000079,  7}, {0x0000007a,  7}, {0x0000007b,  7}, {0x00007ffe, 15},
    {0x000007fc, 11}, {0x00003ffd, 14}, {0x00001ffd, 13}, {0x0ffffffc, 28},
    {0x000fffe6, 20}, {0x003fffd2, 22}, {0x000fffe7, 20}, {0x000fffe8, 20},
    {0x003fffd3, 22}, {0x003fffd4, 22}, {0x003fffd5, 22}, {0x007fffd9, 23},
    {0x003fffd6, 22}, {0x007fffda, 23}, {0x007fffdb, 23}, {0x007fffdc, 23},
    {0x007fffdd, 23}, {0x007fffde, 23}, {0x00ffffeb, 24}, {0x007fffdf, 23},
    {0x00ffffec, 24}, {0x00ffffed, 24}, {0x003fffd7, 22}, {0x007fffe0, 23},
    {0x00ffffee, 24}, {0x007fffe1, 23}, {0x007fffe2, 23}, {0x007fffe3, 23},
    {0x007fffe4, 23}, {0x001fffdc, 21}, {0x003fffd8, 22}, {0x007fffe5, 23},
    {0x003fffd9, 22}, {0x007fffe6, 23}, {0x007fffe7, 23}, {0x00ffffef, 24},
    {0x003fffda, 22}, {0x001fffdd, 21}, {0x000fffe9, 20}, {0x003fffdb, 22},
    {0x003fffdc, 22}, {0x007fffe8, 23}, {0x007fffe9, 23}, {0x001fffde, 21},
    {0x007fffea, 23}, {0x003fffdd, 22}, {0x003fffde, 22}, {0x00fffff0, 24},
    {0x001fffdf, 21}, {0x003fffdf, 22}, {0x007fffeb, 23}, {0x007fffec, 23},
    {0x001fffe0, 21}, {0x001fffe1, 21}, {0x003fffe0, 22}, {0x001fffe2, 21},
    {0x007fffed, 23}, {0x003fffe1, 22}, {0x007fffee, 23}, {0x007fffef, 23},
    {0x000fffea, 20}, {0x003fffe2, 22}, {0x003fffe3, 22}, {0x003fffe4, 22},
    {0x007ffff0, 23}, {0x003fffe5, 22}, {0x003fffe6, 22}, {0x007ffff1, 23},
    {0x03ffffe0, 26}, {0x03ffffe1, 26}, {0x000fffeb, 20}, {0x0007fff1, 19},
    {0x003fffe7, 22}, {0x007ffff2, 23}, {0x003fffe8, 22}, {0x01ffffec, 25},
    {0x03ffffe2, 26}, {0x03ffffe3, 26}, {0x03ffffe4, 26}, {0x07ffffde, 27},
    {0x07ffffdf, 27}, {0x03ffffe5, 26}, {0x00fffff1, 24}, {0x01ffffed, 25},
    {0x0007fff2, 19}, {0x001fffe3, 21}, {0x03ffffe6, 26}, {0x07ffffe0, 27},
    {0x07ffffe1, 27}, {0x03ffffe7, 26}, {0x07ffffe2, 27}, {0x00fffff2, 24},
    {0x001fffe4, 21}, {0x001fffe5, 21}, {0x03ffffe8, 26}, {0x03ffffe9, 26},
    {0x0ffffffd, 28}, {0x07ffffe3, 27}, {0x07ffffe4, 27}, {0x07ffffe5, 27},
    {0x000fffec, 20}, {0x00fffff3, 24}, {0x000fffed, 20}, {0x001fffe6, 21},
    {0x003fffe9, 22}, {0x001fffe7, 21}, {0x001fffe8, 21}, {0x007ffff3, 23},
    {0x003fffea, 22}, {0x003fffeb, 22}, {0x01ffffee, 25}, {0x01ffffef, 25},
    {0x00fffff4, 24}, {0x00fffff5, 24}, {0x03ffffea, 26}, {0x007ffff4, 23},
    {0x03ffffeb, 26}, {0x07ffffe6, 27}, {0x03ffffec, 26}, {0x03ffffed, 26},
    {0x07ffffe7, 27}, {0x07ffffe8, 27}, {0x07ffffe9, 27}, {0x07ffffea, 27},
    {0x07ffffeb, 27}, {0x0ffffffe, 28}, {0x07ffffec, 27}, {0x07ffffed, 27},
    {0x07ffffee, 27}, {0x07ffffef, 27}, {0x07fffff0, 27}, {0x03ffffee, 26},
    {0x3fffffff, 30},
};

// THE CANONICAL DECODE TABLES, derived from the code table above (the code is canonical:
// codes of one length are consecutive, in symbol order). unit_hpack derives them again
// from hp_code and compares, so the two cannot drift.
static const uint16_t hp_sorted[257] = {
     48,  49,  50,  97,  99, 101, 105, 111, 115, 116,  32,  37,  45,  46,  47,  51,
     52,  53,  54,  55,  56,  57,  61,  65,  95,  98, 100, 102, 103, 104, 108, 109,
    110, 112, 114, 117,  58,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,
     77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  89, 106, 107, 113, 118,
    119, 120, 121, 122,  38,  42,  44,  59,  88,  90,  33,  34,  40,  41,  63,  39,
     43, 124,  35,  62,   0,  36,  64,  91,  93, 126,  94, 125,  60,  96, 123,  92,
    195, 208, 128, 130, 131, 162, 184, 194, 224, 226, 153, 161, 167, 172, 176, 177,
    179, 209, 216, 217, 227, 229, 230, 129, 132, 133, 134, 136, 146, 154, 156, 160,
    163, 164, 169, 170, 173, 178, 181, 185, 186, 187, 189, 190, 196, 198, 228, 232,
    233,   1, 135, 137, 138, 139, 140, 141, 143, 147, 149, 150, 151, 152, 155, 157,
    158, 165, 166, 168, 174, 175, 180, 182, 183, 188, 191, 197, 231, 239,   9, 142,
    144, 145, 148, 159, 171, 206, 215, 225, 236, 237, 199, 207, 234, 235, 192, 193,
    200, 201, 202, 205, 210, 213, 218, 219, 238, 240, 242, 243, 255, 203, 204, 211,
    212, 214, 221, 222, 223, 241, 244, 245, 246, 247, 248, 250, 251, 252, 253, 254,
      2,   3,   4,   5,   6,   7,   8,  11,  12,  14,  15,  16,  17,  18,  19,  20,
     21,  23,  24,  25,  26,  27,  28,  29,  30,  31, 127, 220, 249,  10,  13,  22,
    256,
};
static const uint32_t hp_first[31] = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x14, 0x5c, 0xf8, 0x1fc, 0x3f8, 0x7fa, 0xffa, 0x1ff8, 0x3ffc, 0x7ffc, 0xfffe, 0x1fffc, 0x3fff8, 0x7fff0, 0xfffe6, 0x1fffdc, 0x3fffd2, 0x7fffd8, 0xffffea, 0x1ffffec, 0x3ffffe0, 0x7ffffde, 0xfffffe2, 0x1ffffffe, 0x3ffffffc};
static const uint16_t hp_count[31] = {0, 0, 0, 0, 0, 10, 26, 32, 6, 0, 5, 3, 2, 6, 2, 3, 0, 0, 0, 3, 8, 13, 26, 29, 12, 4, 15, 19, 29, 0, 4};
static const uint16_t hp_offset[31] = {0, 0, 0, 0, 0, 0, 10, 36, 68, 74, 74, 79, 82, 84, 90, 92, 95, 95, 95, 95, 98, 106, 119, 145, 174, 186, 190, 205, 224, 253, 253};

// ---- integers (RFC 7541 5.1) ------------------------------------------------
int cb_hpack_int_decode(const uint8_t *p, size_t n, int prefix_bits, uint32_t *out) {
    if (n < 1) return CB_E_INVALID;
    uint32_t mask = (1u << prefix_bits) - 1;
    uint32_t v = p[0] & mask;
    if (v < mask) { *out = v; return 1; }
    // At most 2^31 - 1: five continuation bytes carry 35 bits, so the fifth may
    // add at most what still fits.
    uint64_t acc = v;
    for (size_t i = 1, shift = 0; i < n && i <= 5; i++, shift += 7) {
        acc += (uint64_t)(p[i] & 0x7f) << shift;
        if (acc > 0x7fffffffu) return CB_E_INVALID;
        if (!(p[i] & 0x80)) { *out = (uint32_t)acc; return (int)i + 1; }
    }
    return CB_E_INVALID;   // cut short, or more continuation bytes than 2^31 needs
}

void cb_hpack_int_encode(cb_buf *out, uint8_t first, int prefix_bits, uint32_t v) {
    uint32_t mask = (1u << prefix_bits) - 1;
    if (v < mask) { cb_buf_putc(out, (char)(first | v)); return; }
    cb_buf_putc(out, (char)(first | mask));
    v -= mask;
    while (v >= 0x80) { cb_buf_putc(out, (char)(0x80 | (v & 0x7f))); v >>= 7; }
    cb_buf_putc(out, (char)v);
}

// ---- Huffman ----------------------------------------------------------------
int cb_hpack_huff_decode(const uint8_t *p, size_t n, cb_buf *out) {
    uint32_t code = 0;
    int len = 0;
    for (size_t i = 0; i < n; i++) {
        for (int b = 7; b >= 0; b--) {
            code = (code << 1) | ((p[i] >> b) & 1);
            len++;
            if (len > 30) return CB_E_INVALID;   // no code is longer: cannot happen with these tables
            if (hp_count[len] && code - hp_first[len] < hp_count[len]) {
                unsigned sym = hp_sorted[hp_offset[len] + (code - hp_first[len])];
                if (sym == 256) return CB_E_INVALID;   // EOS in the data (RFC 7541 5.2)
                cb_buf_putc(out, (char)sym);
                code = 0;
                len = 0;
            }
        }
    }
    // The padding: fewer than 8 bits, every one set (a prefix of EOS).
    if (len > 7) return CB_E_INVALID;
    if (code != (1u << len) - 1) return CB_E_INVALID;
    return CB_OK;
}

size_t cb_hpack_huff_len(const uint8_t *p, size_t n) {
    uint64_t bits = 0;
    for (size_t i = 0; i < n; i++) bits += hp_code[p[i]].len;
    return (size_t)((bits + 7) / 8);
}

void cb_hpack_huff_encode(cb_buf *out, const uint8_t *p, size_t n) {
    uint64_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        const hp_code_t *c = &hp_code[p[i]];
        acc = (acc << c->len) | c->code;
        bits += c->len;
        while (bits >= 8) { bits -= 8; cb_buf_putc(out, (char)(acc >> bits)); }
        acc &= (bits ? ((1ull << bits) - 1) : 0);
    }
    if (bits) cb_buf_putc(out, (char)((acc << (8 - bits)) | ((1u << (8 - bits)) - 1)));
}

bool cb_hpack_huff_tables_agree(void) {
    // Derive the canonical tables from hp_code and compare with the stored ones.
    uint16_t count[31] = { 0 };
    for (int s = 0; s < 257; s++) {
        if (hp_code[s].len < 1 || hp_code[s].len > 30) return false;
        count[hp_code[s].len]++;
    }
    uint32_t code = 0;
    uint16_t idx = 0;
    for (int l = 1; l <= 30; l++) {
        code <<= 1;
        if (hp_first[l] != code || hp_count[l] != count[l] || hp_offset[l] != idx) return false;
        code += count[l];
        idx += count[l];
    }
    // Every symbol's code is the canonical one, at its place in hp_sorted.
    for (int s = 0; s < 257; s++) {
        int l = hp_code[s].len;
        uint32_t c = hp_code[s].code;
        if (c - hp_first[l] >= hp_count[l]) return false;
        if (hp_sorted[hp_offset[l] + (c - hp_first[l])] != s) return false;
    }
    return true;
}

// ---- the dynamic table (RFC 7541 4) -----------------------------------------
#define HP_ENTRY_OVERHEAD 32u

int cb_hpack_dec_init(cb_hpack_dec *d, uint32_t max) {
    memset(d, 0, sizeof *d);
    if (max > CB_HPACK_TABLE_MAX) return CB_E_INVALID;
    d->max = max;
    d->limit = max;
    d->cap = max / HP_ENTRY_OVERHEAD + 1;
    d->e = calloc(d->cap, sizeof *d->e);
    if (!d->e) return CB_E_NOMEM;
    cb_buf_init(&d->name);
    cb_buf_init(&d->value);
    return CB_OK;
}

void cb_hpack_dec_free(cb_hpack_dec *d) {
    if (d->e)
        for (uint32_t i = 0; i < d->count; i++) free(d->e[i].bytes);
    free(d->e);
    cb_buf_free(&d->name);
    cb_buf_free(&d->value);
    memset(d, 0, sizeof *d);
}

static uint32_t entry_size(uint32_t name_len, uint32_t value_len) {
    return name_len + value_len + HP_ENTRY_OVERHEAD;
}

// Evicts the oldest entries until the table is at most `to`.
static void evict_to(cb_hpack_dec *d, uint32_t to) {
    while (d->size > to && d->count) {
        cb_hpack_entry *last = &d->e[--d->count];
        d->size -= entry_size(last->name_len, last->value_len);
        free(last->bytes);
        last->bytes = NULL;
    }
}

// RFC 7541 4.4: an entry larger than the table empties it and is not added.
static int table_add(cb_hpack_dec *d, const char *name, size_t nl, const char *value, size_t vl) {
    uint64_t sz = (uint64_t)nl + vl + HP_ENTRY_OVERHEAD;
    if (sz > d->limit) { evict_to(d, 0); return CB_OK; }
    evict_to(d, d->limit - (uint32_t)sz);
    uint8_t *b = malloc(nl + vl + 1);
    if (!b) return CB_E_NOMEM;
    memcpy(b, name, nl);
    memcpy(b + nl, value, vl);
    // The size bounds the count: each entry is at least 32, so count < cap.
    memmove(&d->e[1], &d->e[0], d->count * sizeof *d->e);
    d->e[0] = (cb_hpack_entry){ b, (uint32_t)nl, (uint32_t)vl };
    d->count++;
    d->size += (uint32_t)sz;
    return CB_OK;
}

bool cb_hpack_dec_entry(const cb_hpack_dec *d, uint32_t i, cb_span *name, cb_span *value) {
    if (i < 1 || i > d->count) return false;
    const cb_hpack_entry *e = &d->e[i - 1];
    *name = cb_span_of((const char *)e->bytes, e->name_len);
    *value = cb_span_of((const char *)e->bytes + e->name_len, e->value_len);
    return true;
}

// Index i of the joined table (1..61 static, then dynamic).
static bool lookup(const cb_hpack_dec *d, uint32_t i, cb_span *name, cb_span *value) {
    if (i == 0) return false;
    if (i <= HP_STATIC_N) {
        const hp_static *s = &hp_static_table[i - 1];
        *name = cb_span_of(s->name, s->name_len);
        *value = cb_span_of(s->value, s->value_len);
        return true;
    }
    return cb_hpack_dec_entry(d, i - HP_STATIC_N, name, value);
}

// A string literal (RFC 7541 5.2) at p: bytes used (> 0), with the string in
// *s (pointing into p, or into scratch when Huffman-coded), or CB_E_INVALID.
static int read_string(const uint8_t *p, size_t n, cb_buf *scratch, cb_span *s) {
    if (n < 1) return CB_E_INVALID;
    bool huff = p[0] & 0x80;
    uint32_t len;
    int k = cb_hpack_int_decode(p, n, 7, &len);
    if (k < 0) return k;
    if (len > n - (size_t)k) return CB_E_INVALID;
    const uint8_t *q = p + k;
    if (!huff) {
        if (len > CB_HPACK_STRING_MAX) return CB_E_INVALID;
        *s = cb_span_of((const char *)q, len);
        return k + (int)len;
    }
    // The decoded string is at most len * 8 / 5 bytes (the shortest code is 5 bits).
    if ((uint64_t)len * 8 / 5 > CB_HPACK_STRING_MAX) return CB_E_INVALID;
    cb_buf_reset(scratch);
    int rc = cb_hpack_huff_decode(q, len, scratch);
    if (rc < 0) return rc;
    // A scratch that could not grow holds less than the string (or nothing): no field, the decode
    // ends. Its out-of-memory mark stays, so the decoder answers so from here on.
    if (scratch->oom) return CB_E_NOMEM;
    *s = cb_span_of(scratch->p ? (const char *)scratch->p : "", scratch->len);
    return k + (int)len;
}

int cb_hpack_decode(cb_hpack_dec *d, const uint8_t *p, size_t n, cb_hpack_emit emit, void *ctx) {
    size_t at = 0;
    bool any_field = false;
    while (at < n) {
        uint8_t b = p[at];
        uint32_t idx;
        int k;
        cb_span name, value;
        if (b & 0x80) {   // 6.1 indexed
            if ((k = cb_hpack_int_decode(p + at, n - at, 7, &idx)) < 0) return k;
            at += (size_t)k;
            if (!lookup(d, idx, &name, &value)) return CB_E_INVALID;
            any_field = true;
            int rc = emit(ctx, name.p, name.n, value.p, value.n, false);
            if (rc != CB_OK) return rc;
            continue;
        }
        if ((b & 0xe0) == 0x20) {   // 6.3 dynamic table size update
            if (any_field) return CB_E_INVALID;   // only at the start of a block (4.2)
            if ((k = cb_hpack_int_decode(p + at, n - at, 5, &idx)) < 0) return k;
            at += (size_t)k;
            if (idx > d->max) return CB_E_INVALID;
            d->limit = idx;
            evict_to(d, idx);
            continue;
        }
        // 6.2 literals: 01 with incremental indexing (6-bit index), 0000 without,
        // 0001 never indexed (4-bit index each).
        bool add = (b & 0xc0) == 0x40;
        bool never = (b & 0xf0) == 0x10;
        int prefix = add ? 6 : 4;
        if ((k = cb_hpack_int_decode(p + at, n - at, prefix, &idx)) < 0) return k;
        at += (size_t)k;
        cb_span vv;
        if (idx) {
            if (!lookup(d, idx, &name, &vv)) return CB_E_INVALID;
            // The name may live in the table, which adding this entry can evict
            // from: keep a copy.
            cb_buf_reset(&d->name);
            cb_buf_append(&d->name, name.p, name.n);
            name = cb_span_of(d->name.p ? (const char *)d->name.p : "", d->name.len);
        } else {
            if ((k = read_string(p + at, n - at, &d->name, &name)) < 0) return k;
            at += (size_t)k;
        }
        if ((k = read_string(p + at, n - at, &d->value, &value)) < 0) return k;
        at += (size_t)k;
        any_field = true;
        if (add) {
            int rc = table_add(d, name.p, name.n, value.p, value.n);
            if (rc != CB_OK) return rc;
        }
        int rc = emit(ctx, name.p, name.n, value.p, value.n, never);
        if (rc != CB_OK) return rc;
    }
    return CB_OK;
}

// ---- the encoder ------------------------------------------------------------
static void write_string(cb_buf *out, const char *s, size_t n) {
    size_t h = cb_hpack_huff_len((const uint8_t *)s, n);
    if (h < n) {
        cb_hpack_int_encode(out, 0x80, 7, (uint32_t)h);
        cb_hpack_huff_encode(out, (const uint8_t *)s, n);
    } else {
        cb_hpack_int_encode(out, 0x00, 7, (uint32_t)n);
        cb_buf_append(out, s, n);
    }
}

void cb_hpack_encode(cb_buf *out, const char *name, size_t name_len, const char *value, size_t value_len,
                     bool never) {
    uint32_t name_idx = 0;
    for (uint32_t i = 0; i < HP_STATIC_N; i++) {
        const hp_static *s = &hp_static_table[i];
        if (s->name_len != name_len || memcmp(s->name, name, name_len) != 0) continue;
        if (!never && s->value_len == value_len && memcmp(s->value, value, value_len) == 0) {
            cb_hpack_int_encode(out, 0x80, 7, i + 1);   // the whole pair
            return;
        }
        if (!name_idx) name_idx = i + 1;
    }
    cb_hpack_int_encode(out, never ? 0x10 : 0x00, 4, name_idx);
    if (!name_idx) write_string(out, name, name_len);
    write_string(out, value, value_len);
}
