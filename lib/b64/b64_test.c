/* b64_test.c - base64 (b64.h): the RFC 4648 section 10 vectors in both
 * alphabets, round trips of every length class, and each refusal the decoder
 * owes a JWT, a refresh token or a stored blob. Deterministic and bounded.
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include "../test/check.h"
#include "b64.h"

int main(void)
{
    TEST("b64: RFC 4648 section 10 vectors, both alphabets");
    static const char *in[]  = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char *std[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
    static const char *url[] = { "", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy" };
    for (int i = 0; i < 7; i++) {
        char out[32];
        uint8_t back[32];
        size_t n = strlen(in[i]);
        CHECK(b64_encode((const uint8_t *)in[i], n, out, sizeof out) == (int)strlen(std[i]) && !strcmp(out, std[i]),
              "encode \"%s\" gave \"%s\"", in[i], out);
        CHECK(b64url_encode((const uint8_t *)in[i], n, out, sizeof out) == (int)strlen(url[i]) && !strcmp(out, url[i]),
              "url-encode \"%s\" gave \"%s\"", in[i], out);
        CHECK(b64_decode(std[i], strlen(std[i]), back, sizeof back) == (int)n && !memcmp(back, in[i], n),
              "decode \"%s\"", std[i]);
        CHECK(b64_decode(url[i], strlen(url[i]), back, sizeof back) == (int)n && !memcmp(back, in[i], n),
              "decode \"%s\"", url[i]);
    }

    TEST("b64: every length class round-trips, at the length b64_len says");
    uint8_t raw[256], back[256];
    char enc[400];
    for (int i = 0; i < 256; i++) raw[i] = (uint8_t)(255 - i);
    for (size_t n = 0; n <= 256; n++) {
        int k = b64_encode(raw, n, enc, sizeof enc);
        CHECK(k == (int)b64_len(n), "b64_len(%zu)", n);
        CHECK(b64_encode(raw, n, enc, (size_t)k) == -1, "no room for the NUL at %zu", n);
        CHECK(b64_decode(enc, (size_t)k, back, sizeof back) == (int)n && !memcmp(back, raw, n), "std %zu", n);
        k = b64url_encode(raw, n, enc, sizeof enc);
        CHECK(k == (int)b64url_len(n), "b64url_len(%zu)", n);
        CHECK(!strchr(enc, '+') && !strchr(enc, '/') && !strchr(enc, '='), "url alphabet at %zu", n);
        CHECK(b64_decode(enc, (size_t)k, back, sizeof back) == (int)n && !memcmp(back, raw, n), "url %zu", n);
        CHECK(n == 0 || b64_decode(enc, (size_t)k, back, n - 1) == -1, "a cap one short is refused at %zu", n);
    }

    TEST("b64: refusals");
    uint8_t out[16];
    char small[4];
    CHECK(b64_decode("Zm9v!", 5, out, sizeof out) == -1, "a character in neither alphabet");
    CHECK(b64_decode("Z", 1, out, sizeof out) == -1, "no encoding is 1 mod 4");
    CHECK(b64_decode("Zg=a", 4, out, sizeof out) == -1, "padding in the middle");
    CHECK(b64_decode("Zm9v=", 5, out, sizeof out) == -1, "padding after a whole group");
    CHECK(b64_decode("Zh==", 4, out, sizeof out) == -1, "non-zero leftover bits");
    CHECK(b64_decode("Zm9vYmFy", 8, out, 5) == -1, "does not fit");
    CHECK(b64_encode((const uint8_t *)"foo", 3, small, sizeof small) == -1, "no room for the NUL");

    return report("b64");
}
