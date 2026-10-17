/* rng.h - the small, fast, non-cryptographic generators, kept once.
 *
 * Every hand-rolled generator in the repo is one of these, called by name:
 * a bot's playouts, a test's fuzz, a backoff's jitter, a dice throw from a
 * seed. Header only and freestanding (stdint.h, no libc), so a wasm kernel
 * links it with nothing added to a build list.
 *
 * THE STATE IS THE CALLER'S. Each generator advances a plain integer the
 * caller owns and passes by pointer: a local, a field of its bot or game, or
 * a _Thread_local at the one call site that wants one. Nothing here holds
 * state, so two streams can never share one by accident, and a state can sit
 * in a struct a host or a generated binding already reads.
 *
 * DETERMINISM IS THE POINT, NOT AN ACCIDENT. The kernels take randomness as an
 * injected seed, and some of these sequences are re-derived from data already
 * in people's hands: the legacy deal and the strategy stream come from
 * rng_lcg32 over a stored 32-bit seed, a die's throw from rng_splitmix64 over
 * the game's seed. Each algorithm below is exactly its published one, pinned
 * by its published vectors in rng_test.c; change a constant or a shift and
 * every replay, every in-flight Messages game and every golden moves with it.
 * A new algorithm is a new function, never an edit to one of these.
 *
 * WHAT IS NOT HERE, ON PURPOSE:
 * - deal_rng.h, the ChaCha20 stream a deal is drawn from: a player sees some
 *   of its outputs (their own cards), and every generator below can be run
 *   backwards from a few outputs. A deal never draws from this file.
 * - hashes: a function of a key, not a stream (a transposition-table
 *   fingerprint, a book's bucket, a texture's noise at a coordinate). They
 *   own their own mixing and their own formats.
 * - how a draw becomes a number in [0, n): each caller's reduction (a modulo,
 *   a mask, Lemire's multiply) is part of its sequence and stays with it.
 *
 * Sources: splitmix64 is Steele, Lea and Flood, "Fast splittable pseudorandom
 * number generators" (OOPSLA 2014), as in Vigna's splitmix64.c; xorshift32
 * and xorshift64 are Marsaglia, "Xorshift RNGs" (J. Stat. Softw. 8(14), 2003),
 * shift triples (13,17,5) and (13,7,17); xorshift64* is Vigna, "An
 * experimental exploration of Marsaglia's xorshift generators, scrambled"
 * (ACM TOMS 42(4), 2016), triple (12,25,27); the 32-bit LCG is Numerical
 * Recipes' "ranqd1" (Press et al., 2nd ed., section 7.1). */
#ifndef SHARED_RNG_H
#define SHARED_RNG_H

#include <stdint.h>

/* 2^64 / phi, odd: splitmix64's increment, and the usual way to spread a
 * small integer over 64 bits. */
#define RNG_GOLDEN UINT64_C(0x9e3779b97f4a7c15)

/* xorshift64*'s output multiplier (2685821657736338717). */
#define RNG_XORSHIFT64STAR_MUL UINT64_C(0x2545f4914f6cdd1d)

/* Numerical Recipes' LCG, state = state * MUL + INC (mod 2^32). */
#define RNG_LCG32_MUL 1664525u
#define RNG_LCG32_INC 1013904223u

/* ---- splitmix64: seeding, and streams where any 64-bit state is fine ----- */

/* The output a splitmix64 step from `state` gives, without a state to keep:
 * the state moved by RNG_GOLDEN, then Stafford's variant-13 finaliser. Draw k
 * of a stream is rng_splitmix64_at(seed + k * RNG_GOLDEN), so it doubles as
 * a counter-based draw and as a mixer for a small key. */
static inline uint64_t rng_splitmix64_at(uint64_t state)
{
    uint64_t z = state + RNG_GOLDEN;
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

/* One splitmix64 step: advance *state and return the mixed output. Every
 * state, 0 included, is a good one. */
static inline uint64_t rng_splitmix64(uint64_t *state)
{
    uint64_t z = rng_splitmix64_at(*state);
    *state += RNG_GOLDEN;
    return z;
}

/* A state for the xorshift64 generators from any seed: one splitmix64 output,
 * so seeds 1, 2, 3 start unrelated streams, and never 0, the one state an
 * xorshift cannot leave (the single seed that mixes to 0 gets the
 * xorshift64* multiplier instead). */
static inline uint64_t rng_seed64(uint64_t seed)
{
    uint64_t z = rng_splitmix64_at(seed);
    return z ? z : RNG_XORSHIFT64STAR_MUL;
}

/* ---- xorshift: the fast streams; a state of 0 stays 0 forever ----------- */

/* xorshift64, Marsaglia's (13,7,17): returns the new state. */
static inline uint64_t rng_xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *state = x;
}

/* xorshift64*, Vigna's (12,25,27) and multiplier: advances *state and returns
 * the state times the multiplier, whose high bits are the good ones. */
static inline uint64_t rng_xorshift64star(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * RNG_XORSHIFT64STAR_MUL;
}

/* xorshift32, Marsaglia's (13,17,5): returns the new state. */
static inline uint32_t rng_xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

/* ---- the 32-bit LCG ------------------------------------------------------ */

/* Numerical Recipes' LCG: returns the new state. Its low bits are weak (bit k
 * has period 2^(k+1)), so read it from the top. Every state is a good one. */
static inline uint32_t rng_lcg32(uint32_t *state)
{
    return *state = *state * RNG_LCG32_MUL + RNG_LCG32_INC;
}

#endif
