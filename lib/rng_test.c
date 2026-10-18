/* rng_test.c - the generator in rng.h against its published vectors.
 *
 * A vector from the literature, not one this file computed, is what tells a
 * wrong shift or constant from a right one: dice and a pen's strokes are
 * re-derived from seeds already on people's phones, so a generator that is
 * merely self-consistent is not enough.
 *
 *   cc -std=c11 -Wall -Wextra -Werror rng_test.c -o rng_test && ./rng_test
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include "test/check.h"
#include "rng.h"

int main(void)
{
    TEST("splitmix64: Vigna's splitmix64.c from 0, and from 1234567");
    {
        /* The first from state 0 is the one every port quotes; the five from
         * 1234567 are Rosetta Code's "Pseudo-random numbers/Splitmix64". */
        uint64_t s = 0;
        CHECK(rng_splitmix64(&s) == UINT64_C(0xe220a8397b1dcdaf), "first draw from 0");
        CHECK(s == RNG_GOLDEN, "the state moved by the golden increment");
        static const uint64_t want[5] = {
            UINT64_C(6457827717110365317), UINT64_C(3203168211198807973),
            UINT64_C(9817491932198370423), UINT64_C(4593380528125082431),
            UINT64_C(16408922859458223821),
        };
        s = 1234567;
        for (int i = 0; i < 5; i++) {
            uint64_t got = rng_splitmix64(&s);
            CHECK(got == want[i], "draw %d from 1234567: %llu", i, (unsigned long long)got);
        }
    }

    TEST("splitmix64_at: draw k is the stream's draw k, with no state");
    {
        uint64_t s = 1234567;
        for (uint64_t k = 0; k < 64; k++) {
            uint64_t at = rng_splitmix64_at(UINT64_C(1234567) + k * RNG_GOLDEN);
            CHECK(at == rng_splitmix64(&s), "draw %llu", (unsigned long long)k);
        }
    }

    TEST("u32: the high half of a splitmix64 draw");
    {
        uint64_t a = 1234567, b = 1234567;
        for (int i = 0; i < 16; i++)
            CHECK(rng_u32(&a) == (uint32_t)(rng_splitmix64(&b) >> 32) && a == b, "draw %d", i);
    }

    TEST("seed64: splitmix64's first output");
    {
        CHECK(rng_seed64(0) == UINT64_C(0xe220a8397b1dcdaf), "seed 0");
        CHECK(rng_seed64(1234567) == UINT64_C(6457827717110365317), "seed 1234567");
    }

    TEST("unit: the top 53 bits, in [0, 1)");
    {
        CHECK(rng_unit(0) == 0.0, "0");
        CHECK(rng_unit(UINT64_C(1) << 63) == 0.5, "the top bit alone is a half");
        CHECK(rng_unit(UINT64_C(0x7ff)) == 0.0, "the low 11 bits are dropped");
        CHECK(rng_unit(UINT64_C(0x800)) == 1.0 / 9007199254740992.0, "bit 11 is 2^-53");
        CHECK(rng_unit(~UINT64_C(0)) == 1.0 - 1.0 / 9007199254740992.0, "all ones is the largest double below 1");
    }

    return report("rng");
}
