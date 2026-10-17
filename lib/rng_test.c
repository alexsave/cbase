/* rng_test.c - every generator in rng.h against its published vectors.
 *
 * A vector from the literature, not one this file computed, is what tells a
 * wrong shift or constant from a right one: the generators' users re-derive
 * deals, dice and bot moves from stored seeds, so a generator that is merely
 * self-consistent is not enough.
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

    TEST("seed64: splitmix64's first output, and never 0");
    {
        CHECK(rng_seed64(0) == UINT64_C(0xe220a8397b1dcdaf), "seed 0");
        CHECK(rng_seed64(1234567) == UINT64_C(6457827717110365317), "seed 1234567");
        /* The one seed that mixes to 0: the finaliser is a bijection that
         * keeps 0 at 0, so it is the seed the increment carries to 0. */
        uint64_t zero_seed = (uint64_t)0 - RNG_GOLDEN;
        CHECK(rng_splitmix64_at(zero_seed) == 0, "0 - GOLDEN mixes to 0");
        CHECK(rng_seed64(zero_seed) == RNG_XORSHIFT64STAR_MUL, "that seed still gives a live state");
    }

    TEST("xorshift32: Marsaglia's (13,17,5) from 2463534242");
    {
        /* Marsaglia 2003's own example seed, the first three outputs */
        static const uint32_t want[3] = { 723471715u, 2497366906u, 2064144800u };
        uint32_t s = 2463534242u;
        for (int i = 0; i < 3; i++) {
            uint32_t got = rng_xorshift32(&s);
            CHECK(got == want[i] && s == got, "draw %d: %u", i, got);
        }
    }

    TEST("xorshift64: Marsaglia's (13,7,17) from 88172645463325252");
    {
        /* Marsaglia 2003's own example seed, the first three outputs */
        static const uint64_t want[3] = {
            UINT64_C(8748534153485358512), UINT64_C(3040900993826735515),
            UINT64_C(3453997556048239312),
        };
        uint64_t s = UINT64_C(88172645463325252);
        for (int i = 0; i < 3; i++) {
            uint64_t got = rng_xorshift64(&s);
            CHECK(got == want[i] && s == got, "draw %d: %llu", i, (unsigned long long)got);
        }
    }

    TEST("xorshift64*: Vigna's (12,25,27) from 1234567, the high 32 bits");
    {
        /* Rosetta Code's "Pseudo-random numbers/Xorshift star" */
        static const uint32_t want[5] = { 3540625527u, 2750739987u, 4037983143u, 1993361440u, 3809424708u };
        uint64_t s = 1234567;
        for (int i = 0; i < 5; i++) {
            uint32_t got = (uint32_t)(rng_xorshift64star(&s) >> 32);
            CHECK(got == want[i], "draw %d: %u", i, got);
        }
        CHECK(RNG_XORSHIFT64STAR_MUL == UINT64_C(2685821657736338717), "the multiplier in decimal");
    }

    TEST("unit: the top 53 bits, in [0, 1)");
    {
        CHECK(rng_unit(0) == 0.0, "0");
        CHECK(rng_unit(UINT64_C(1) << 63) == 0.5, "the top bit alone is a half");
        CHECK(rng_unit(UINT64_C(0x7ff)) == 0.0, "the low 11 bits are dropped");
        CHECK(rng_unit(UINT64_C(0x800)) == 1.0 / 9007199254740992.0, "bit 11 is 2^-53");
        CHECK(rng_unit(~UINT64_C(0)) == 1.0 - 1.0 / 9007199254740992.0, "all ones is the largest double below 1");
    }

    TEST("lcg32: Numerical Recipes' ranqd1 from 0");
    {
        static const uint32_t want[4] = { 1013904223u, 1196435762u, 3519870697u, 2868466484u };
        uint32_t s = 0;
        for (int i = 0; i < 4; i++) {
            uint32_t got = rng_lcg32(&s);
            CHECK(got == want[i] && s == got, "draw %d: %u", i, got);
        }
    }

    return report("rng");
}
