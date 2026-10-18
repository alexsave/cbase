#include "bytea.h"

#include "../../../shared/c/hex/hex.h"

int cb_bytea_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    if (cap < 3) return -1;
    out[0] = '\\';
    out[1] = 'x';
    int k = hex_encode(in, n, out + 2, cap - 2);
    return k < 0 ? -1 : k + 2;
}

int cb_bytea_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
    if (n >= 2 && in[0] == '\\' && in[1] == 'x') { in += 2; n -= 2; }
    return hex_decode(in, n, out, cap);
}
