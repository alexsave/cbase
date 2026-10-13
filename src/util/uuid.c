#include "uuid.h"

#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>

void cb_random_bytes(void *out, size_t n) {
    // RAND_bytes takes an int; draw in chunks so a large request cannot wrap it.
    unsigned char *p = out;
    while (n > 0) {
        int k = n > (1u << 20) ? (1 << 20) : (int)n;
        if (RAND_bytes(p, k) != 1) {
            fprintf(stderr, "cbase: RAND_bytes failed: no randomness, refusing to continue\n");
            abort();
        }
        p += k;
        n -= (size_t)k;
    }
}

void cb_uuid_v4(char out[CB_UUID_LEN + 1]) {
    static const char D[] = "0123456789abcdef";
    unsigned char b[16];
    cb_random_bytes(b, sizeof b);
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = D[b[i] >> 4];
        out[o++] = D[b[i] & 15];
    }
    out[o] = 0;
}

bool cb_uuid_valid(const char *s, size_t n) {
    if (n != CB_UUID_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; continue; }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}
