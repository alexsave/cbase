/* sha256_openssl_test.c - shared/c's SHA-256 against OpenSSL's SHA256 over
 * random lengths (0..4096, with every edge around the 55/56/63/64/119/120-byte
 * padding boundaries) and random content, one call and random chunking.
 * A native-only test (OpenSSL is not in the wasm kernel).
 *
 *   cc -std=c11 -Wall -Wextra -Werror -I$OPENSSL/include sha256_openssl_test.c sha256.c \
 *      -L$OPENSSL/lib -lcrypto -o sha256_openssl_test && ./sha256_openssl_test
 */
#include <stdint.h>
#include <string.h>
#include <openssl/sha.h>
#include "test/check.h"
#include "sha256.h"
#include "rng.h"

static uint64_t rng = 0x5eed5eed;
static uint32_t next(void) { return rng_u32(&rng); }

static void one(const uint8_t *msg, size_t n)
{
    uint8_t want[32], got[32], chunked[32];
    SHA256(msg, n, want);
    sha256(msg, n, got);
    CHECK(!memcmp(want, got, 32), "one call, %zu bytes", n);
    Sha256 c;
    sha256_init(&c);
    for (size_t at = 0; at < n;) {
        size_t k = next() % 130;
        if (k > n - at) k = n - at;
        sha256_update(&c, msg + at, k);
        at += k;
    }
    sha256_final(&c, chunked);
    CHECK(!memcmp(want, chunked, 32), "random chunks, %zu bytes", n);
}

int main(void)
{
    static uint8_t buf[4200];
    TEST("sha256 vs OpenSSL: every length around the padding edges");
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)next();
    for (size_t n = 0; n <= 260; n++) one(buf, n);
    TEST("sha256 vs OpenSSL: random lengths and offsets");
    for (int t = 0; t < 2000; t++) {
        size_t off = next() % 64, n = next() % 4096;
        one(buf + off, n);
    }
    return report("sha256_openssl_test");
}
