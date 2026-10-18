// The process's one source of random bytes (uuid.h cb_random_bytes), in a file
// of its own so a fuzz target can replace exactly it (fuzz/fuzz.mk
// FUZZ_EXCLUDE_sequence) and keep every id built from it.
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
