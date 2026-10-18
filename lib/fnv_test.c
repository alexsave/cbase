/* fnv_test.c - FNV-1a (fnv.h) against the published vectors (Fowler, Noll and
 * Vo's test suite, draft-eastlake-fnv), the byte and string forms against each
 * other, and the wide step against byte steps where they must agree.
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include "test/check.h"
#include "fnv.h"

int main(void)
{
    TEST("fnv: the published vectors");
    static const char *const s[] = { "", "a", "foobar" };
    static const uint32_t h32[] = { 0x811c9dc5u, 0xe40c292cu, 0xbf9cf968u };
    static const uint64_t h64[] = { 0xcbf29ce484222325ull, 0xaf63dc4c8601ec8cull, 0x85944171f73967e8ull };
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(s[i]);
        CHECK(fnv1a32(FNV32_OFFSET, s[i], n) == h32[i], "fnv1a32 \"%s\"", s[i]);
        CHECK(fnv1a64(FNV64_OFFSET, s[i], n) == h64[i], "fnv1a64 \"%s\"", s[i]);
        CHECK(fnv1a32_str(FNV32_OFFSET, s[i]) == h32[i], "fnv1a32_str \"%s\"", s[i]);
        CHECK(fnv1a64_str(FNV64_OFFSET, s[i]) == h64[i], "fnv1a64_str \"%s\"", s[i]);
    }

    TEST("fnv: a fold continues from any basis");
    CHECK(fnv1a32(fnv1a32(FNV32_OFFSET, "foo", 3), "bar", 3) == h32[2], "32 in two halves");
    CHECK(fnv1a64(fnv1a64(FNV64_OFFSET, "foo", 3), "bar", 3) == h64[2], "64 in two halves");

    TEST("fnv: the wide step is the byte step for a byte, and keeps every bit of a wider value");
    CHECK(fnv1a64_step(FNV64_OFFSET, 'a') == h64[1], "a byte as a value");
    CHECK(fnv1a64_step(1, 0x100) != fnv1a64_step(1, 0), "a bit above the low byte counts");
    CHECK(fnv1a32_step(1, 0x100) != fnv1a32_step(1, 0), "a bit above the low byte counts (32)");

    return report("fnv");
}
