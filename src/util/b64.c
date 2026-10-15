#include "b64.h"

static const char STD[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t cb_b64_len(size_t n) { return (n + 2) / 3 * 4; }
size_t cb_b64url_len(size_t n) { return n / 3 * 4 + (n % 3 ? n % 3 + 1 : 0); }

static int encode(const uint8_t *in, size_t n, char *out, size_t cap, const char *tbl, int pad) {
    size_t need = pad ? cb_b64_len(n) : cb_b64url_len(n);
    if (cap < need + 1) return -1;
    size_t o = 0, i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = tbl[v >> 18 & 63];
        out[o++] = tbl[v >> 12 & 63];
        out[o++] = tbl[v >> 6 & 63];
        out[o++] = tbl[v & 63];
    }
    size_t rem = n - i;
    if (rem) {
        uint32_t v = (uint32_t)in[i] << 16 | (rem > 1 ? (uint32_t)in[i + 1] << 8 : 0);
        out[o++] = tbl[v >> 18 & 63];
        out[o++] = tbl[v >> 12 & 63];
        if (rem > 1) out[o++] = tbl[v >> 6 & 63];
        else if (pad) out[o++] = '=';
        if (pad) out[o++] = '=';
    }
    out[o] = 0;
    return (int)o;
}

int cb_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) { return encode(in, n, out, cap, STD, 1); }
int cb_b64url_encode(const uint8_t *in, size_t n, char *out, size_t cap) { return encode(in, n, out, cap, URL, 0); }

static int val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

int cb_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
    // Trailing padding (at most two) is read as absent.
    size_t pads = 0;
    while (n > 0 && in[n - 1] == '=' && pads < 2) { n--; pads++; }
    if (pads && (n + pads) % 4 != 0) return -1;
    if (n % 4 == 1) return -1;
    size_t outn = n / 4 * 3 + (n % 4 ? n % 4 - 1 : 0);
    if (outn > cap) return -1;
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = val((unsigned char)in[i]);
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1;
        }
    }
    // The leftover bits of a short final group must be zero (a canonical encoding).
    if (acc != 0) return -1;
    return (int)o;
}
