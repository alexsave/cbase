#include "uuid.h"

#include "../../../../shared/c/hex/hex.h"


void cb_uuid_v4(char out[CB_UUID_LEN + 1]) {
    unsigned char b[16];
    cb_random_bytes(b, sizeof b);
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hex_digit(b[i] >> 4);
        out[o++] = hex_digit(b[i]);
    }
    out[o] = 0;
}

bool cb_uuid_valid(const char *s, size_t n) {
    if (n != CB_UUID_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; continue; }
        if (hex_nibble((unsigned char)c) < 0) return false;
    }
    return true;
}
