/* hex_test.c - hex text (hex.h): both cases in, lowercase out, every byte,
 * the digit helpers, and the refusals. Deterministic and bounded.
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include "../test/check.h"
#include "hex.h"

int main(void)
{
    TEST("hex: a known run, both cases in, lowercase out");
    const uint8_t raw[] = { 0x00, 0x01, 0xab, 0xff, 0x10 };
    char out[32];
    uint8_t back[8];
    CHECK(hex_encode(raw, 5, out, sizeof out) == 10 && !strcmp(out, "0001abff10"), "encode gave \"%s\"", out);
    CHECK(hex_decode("0001ABFF10", 10, back, sizeof back) == 5 && !memcmp(back, raw, 5), "upper case decodes");
    CHECK(hex_decode("0001abff10", 10, back, sizeof back) == 5 && !memcmp(back, raw, 5), "lower case decodes");

    TEST("hex: every byte round-trips, and the digit helpers agree");
    for (int b = 0; b < 256; b++) {
        uint8_t one = (uint8_t)b, got = 0;
        char t[3];
        CHECK(hex_encode(&one, 1, t, sizeof t) == 2 && t[0] == hex_digit((unsigned)b >> 4) && t[1] == hex_digit((unsigned)b),
              "byte %d", b);
        CHECK(hex_decode(t, 2, &got, 1) == 1 && got == one, "byte %d back", b);
        CHECK(hex_nibble((unsigned char)hex_digit_upper((unsigned)b)) == (b & 15), "upper digit %d", b);
    }
    int digits = 0;
    for (int c = 0; c < 256; c++) digits += hex_nibble((unsigned char)c) >= 0;
    CHECK(digits == 22, "exactly 0-9, a-f and A-F are digits (%d)", digits);

    TEST("hex: refusals");
    CHECK(hex_decode("abc", 3, back, sizeof back) == -1, "an odd length");
    CHECK(hex_decode("zz", 2, back, sizeof back) == -1, "a non-hex digit");
    CHECK(hex_decode("\\x00", 4, back, sizeof back) == -1, "no Postgres prefix here (c_base's bytea strips it)");
    CHECK(hex_decode("0001", 4, back, 1) == -1, "a cap too small");
    CHECK(hex_encode(raw, 5, out, 10) == -1, "no room for the NUL");

    return report("hex");
}
