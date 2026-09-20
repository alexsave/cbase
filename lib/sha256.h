// SHA-256 (FIPS 180-4). Added for the iMessage envelope (msg_wire.h): the
// `parent8` chain link and Rule P's digest tiebreak both need a hash that is
// identical on every device, and no other hash in this tree is a cryptographic
// digest (the FNV mixers in state_fnv/octogen are seeds, not commitments).
//
// Freestanding: no allocation, no libc beyond memcpy/memset, so it compiles
// unchanged into the wasm modules and libfoolish.a.
#ifndef CNITRO_SHA256_H
#define CNITRO_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_LEN 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    int      buflen;
} Sha256;

void sha256_init(Sha256 *c);
void sha256_update(Sha256 *c, const void *data, size_t len);
void sha256_final(Sha256 *c, uint8_t out[SHA256_DIGEST_LEN]);

// One-shot convenience — the only form msg_wire.c needs.
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

#endif
