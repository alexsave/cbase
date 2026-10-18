#include "hex.h"

int hex_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    if (n > (SIZE_MAX - 1) / 2 || cap < 2 * n + 1) return -1;
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hex_digit(in[i] >> 4);
        out[2 * i + 1] = hex_digit(in[i]);
    }
    out[2 * n] = 0;
    return (int)(2 * n);
}

int hex_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
    if (n % 2 != 0 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble((unsigned char)in[2 * i]), lo = hex_nibble((unsigned char)in[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(n / 2);
}
