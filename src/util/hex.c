#include "hex.h"

static const char DIGITS[] = "0123456789abcdef";

int cb_hex_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    if (n > (SIZE_MAX - 1) / 2 || cap < 2 * n + 1) return -1;
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = DIGITS[in[i] >> 4];
        out[2 * i + 1] = DIGITS[in[i] & 15];
    }
    out[2 * n] = 0;
    return (int)(2 * n);
}

int cb_bytea_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    if (cap < 3) return -1;
    out[0] = '\\';
    out[1] = 'x';
    int k = cb_hex_encode(in, n, out + 2, cap - 2);
    return k < 0 ? -1 : k + 2;
}

static int nib(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int cb_hex_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
    if (n >= 2 && in[0] == '\\' && in[1] == 'x') { in += 2; n -= 2; }
    if (n % 2 != 0 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = nib((unsigned char)in[2 * i]), lo = nib((unsigned char)in[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(n / 2);
}
