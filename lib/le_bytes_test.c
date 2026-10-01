/* le_bytes_test.c - little-endian integers (le_bytes.h): the byte order against
 * literal bytes, round trips, the u48 boundary at 2^48 - 1 and the
 * out-of-range rule (a put keeps the low bytes). Deterministic and bounded.
 *
 *   cc -std=c11 -Wall -Wextra -Werror le_bytes_test.c -o le_bytes_test && ./le_bytes_test
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include <string.h>
#include "test/check.h"
#include "le_bytes.h"

static uint64_t rng = 88172645463325252ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main(void)
{
    TEST("le_bytes: the low byte comes first");
    unsigned char b[8];
    static const unsigned char u16[] = { 0x34, 0x12 };
    static const unsigned char u32[] = { 0x78, 0x56, 0x34, 0x12 };
    static const unsigned char u48[] = { 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12 };
    memset(b, 0xee, sizeof b);
    le_put_u16(b, 0x1234);
    CHECK(!memcmp(b, u16, 2) && b[2] == 0xee, "u16 0x1234 wrote %02x %02x %02x", b[0], b[1], b[2]);
    memset(b, 0xee, sizeof b);
    le_put_u32(b, 0x12345678u);
    CHECK(!memcmp(b, u32, 4) && b[4] == 0xee, "u32 0x12345678 wrote %02x %02x %02x %02x", b[0], b[1], b[2], b[3]);
    memset(b, 0xee, sizeof b);
    le_put_u48(b, 0x123456789abcull);
    CHECK(!memcmp(b, u48, 6) && b[6] == 0xee, "u48 0x123456789abc wrote %02x %02x %02x %02x %02x %02x",
          b[0], b[1], b[2], b[3], b[4], b[5]);
    CHECK(le_get_u16(u16) == 0x1234, "get u16 = %x", le_get_u16(u16));
    CHECK(le_get_u32(u32) == 0x12345678u, "get u32 = %x", (unsigned)le_get_u32(u32));
    CHECK(le_get_u48(u48) == 0x123456789abcull, "get u48 = %llx", (unsigned long long)le_get_u48(u48));

    TEST("le_bytes: the u48 boundary");
    static const unsigned char ones[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    le_put_u48(b, LE_U48_MAX);
    CHECK(!memcmp(b, ones, 6), "2^48 - 1 is six 0xff bytes");
    CHECK(le_get_u48(ones) == LE_U48_MAX, "six 0xff bytes read %llx", (unsigned long long)le_get_u48(ones));
    CHECK(LE_U48_MAX == (1ull << 48) - 1, "LE_U48_MAX is 2^48 - 1");
    static const unsigned char high[6] = { 0, 0, 0, 0, 0, 0x80 };
    CHECK(le_get_u48(high) == 1ull << 47, "the top bit reads as 2^47, never sign-extended");

    TEST("le_bytes: a put keeps the low bytes");
    static const unsigned char zero[6] = { 0 };
    le_put_u48(b, 1ull << 48);
    CHECK(!memcmp(b, zero, 6), "2^48 writes as 0");
    le_put_u48(b, (1ull << 48) + 5);
    CHECK(le_get_u48(b) == 5, "2^48 + 5 reads back as %llu", (unsigned long long)le_get_u48(b));
    le_put_u48(b, (uint64_t)(int64_t)-1);
    CHECK(!memcmp(b, ones, 6), "a negative cast to u64 keeps its low 48 bits");
    le_put_u16(b, (uint16_t)0x12345);
    CHECK(le_get_u16(b) == 0x2345, "u16 keeps the low 16 bits");

    TEST("le_bytes: round trips");
    for (int i = 0; i < 10000; i++) {
        const uint64_t v = next();
        le_put_u16(b, (uint16_t)v);
        CHECK(le_get_u16(b) == (uint16_t)v, "u16 %x", (unsigned)(uint16_t)v);
        le_put_u32(b, (uint32_t)v);
        CHECK(le_get_u32(b) == (uint32_t)v, "u32 %x", (unsigned)(uint32_t)v);
        le_put_u48(b, v);
        CHECK(le_get_u48(b) == (v & LE_U48_MAX), "u48 %llx", (unsigned long long)v);
    }

    return report("le_bytes_test");
}
