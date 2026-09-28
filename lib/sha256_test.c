/* sha256_test.c - SHA-256 (sha256.h): the FIPS 180-4 / NIST known answers,
 * and every way of feeding the same bytes (one call, random chunks, empty
 * updates) giving the same digest. Deterministic and bounded; the one
 * million "a" vector is the long one, and it takes milliseconds.
 *
 *   cc -std=c11 -Wall -Wextra -Werror sha256_test.c sha256.c -o sha256_test && ./sha256_test
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include "test/check.h"
#include "sha256.h"

static uint64_t rng = 1234567;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

static void hex(const uint8_t *d, char *o)
{
    static const char x[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { o[2 * i] = x[d[i] >> 4]; o[2 * i + 1] = x[d[i] & 15]; }
    o[64] = 0;
}

static void kat(const char *msg, long rep, const char *want)
{
    Sha256 c;
    sha256_init(&c);
    size_t n = strlen(msg);
    for (long i = 0; i < rep; i++) sha256_update(&c, msg, n);
    uint8_t d[32];
    char h[65];
    sha256_final(&c, d);
    hex(d, h);
    CHECK(!strcmp(h, want), "\"%.16s...\" x%ld = %s", msg, rep, h);
}

int main(void)
{
    TEST("sha256: NIST vectors");
    kat("", 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    kat("abc", 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    kat("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    kat("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 1,
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    kat("a", 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    kat("aaaaaaaaaa", 100000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    TEST("sha256: one call equals any chunking");
    static uint8_t buf[1100];
    for (int it = 0; it < 3000; it++) {
        int n = it < 1100 ? it : (int)(next() % 1100);
        for (int i = 0; i < n; i++) buf[i] = (uint8_t)next();
        uint8_t a[32], b[32];
        sha256(buf, (size_t)n, a);
        Sha256 c;
        sha256_init(&c);
        int at = 0;
        while (at < n) {
            int k = (int)(next() % (uint32_t)(n - at + 1));
            sha256_update(&c, buf + at, (size_t)k);
            at += k;
            if (next() % 4 == 0) sha256_update(&c, buf, 0);
        }
        sha256_final(&c, b);
        CHECK(!memcmp(a, b, 32), "length %d chunked differs", n);
    }
    return report("sha256_test");
}
