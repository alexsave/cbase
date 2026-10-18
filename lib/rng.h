/* rng.h - the one non-cryptographic generator, kept once: splitmix64.
 *
 * Every seeded stream in the repo's C draws from it: a bot's playouts and
 * sampled worlds, a test's fuzz, a backoff's jitter, a dice throw from a seed.
 * Header only and freestanding (stdint.h, no libc), so a wasm kernel links it
 * with nothing added to a build list.
 *
 * ONE ALGORITHM, ON PURPOSE. The repo used to carry five (splitmix64,
 * xorshift32, xorshift64, xorshift64*, a 32-bit LCG), and none of the four
 * extra ones bought anything where security does not matter: splitmix64 passes
 * BigCrush, every 64-bit state is a good one (no zero state to guard), it is a
 * counter underneath (draw k is rng_splitmix64_at(seed + k * RNG_GOLDEN)), and
 * it costs two multiplies a draw. New code calls rng_splitmix64 (or rng_u32,
 * rng_unit, rng_seed64 over it) and nothing else.
 *
 * THE STATE IS THE CALLER'S. The generator advances a plain uint64_t the
 * caller owns and passes by pointer: a local, a field of its bot or game, or a
 * _Thread_local at the one call site that wants one. Nothing here holds state,
 * so two streams can never share one by accident.
 *
 * DETERMINISM IS THE POINT, NOT AN ACCIDENT. The kernels take randomness as an
 * injected seed, and some sequences are re-derived from data already in
 * people's hands: a die's throw from the game's seed, a pen's strokes from the
 * seed a message or a replay link carries. splitmix64 is exactly its published
 * algorithm, pinned by its published vectors in rng_test.c; change a constant
 * or a shift and every such sequence and every golden moves with it. (When the
 * other four generators were folded into this one, the owner accepted that
 * sequences re-derived from old messages and links come out differently: no
 * generator is kept for compatibility.)
 *
 * WHAT IS NOT HERE, ON PURPOSE:
 * - deal_rng.h, the ChaCha20 stream a deal is drawn from: a player sees some
 *   of its outputs (their own cards), and splitmix64 can be run backwards from
 *   one output. A deal never draws from this file.
 * - hashes: a function of a key, not a stream (a transposition-table
 *   fingerprint, a book's bucket, a texture's noise at a coordinate). They
 *   own their own mixing and their own formats.
 * - how a draw becomes a number in [0, n): each caller's reduction (a modulo,
 *   Lemire's multiply) is part of its sequence and stays with it. The
 *   conversions here are rng_u32 (the high half) and rng_unit (to [0, 1)).
 *
 * Source: Steele, Lea and Flood, "Fast splittable pseudorandom number
 * generators" (OOPSLA 2014), as in Vigna's splitmix64.c, with Stafford's
 * variant-13 finaliser. */
#ifndef SHARED_RNG_H
#define SHARED_RNG_H

#include <stdint.h>

/* 2^64 / phi, odd: splitmix64's increment, and the usual way to spread a
 * small integer over 64 bits. */
#define RNG_GOLDEN UINT64_C(0x9e3779b97f4a7c15)

/* The output a splitmix64 step from `state` gives, without a state to keep:
 * the state moved by RNG_GOLDEN, then the finaliser. Draw k of a stream is
 * rng_splitmix64_at(seed + k * RNG_GOLDEN), so it doubles as a counter-based
 * draw and as a mixer for a small key. */
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

/* One step's high 32 bits, for a caller that reduces a 32-bit draw. */
static inline uint32_t rng_u32(uint64_t *state)
{
    return (uint32_t)(rng_splitmix64(state) >> 32);
}

/* A stream state from a key: one splitmix64 output.
 *
 * SPLITMIX64 IS A COUNTER, so a state that is another state plus k *
 * RNG_GOLDEN is the same stream k draws along: `s = base + i * RNG_GOLDEN` as
 * playout i's state hands every playout its neighbour's numbers, one draw
 * late, and `s = (*outer += RNG_GOLDEN)` hands a playout the outer stream's
 * own next draws. Derive a sub-stream through this instead (s =
 * rng_seed64(base + i * RNG_GOLDEN)): its start is a draw, not a step. A key
 * that is a hash, or any 32-bit value, is already a fine state. */
static inline uint64_t rng_seed64(uint64_t seed)
{
    return rng_splitmix64_at(seed);
}

/* The top 53 bits of a 64-bit draw as a double in [0, 1): every value a
 * multiple of 2^-53, exactly, with no rounding up to 1. */
static inline double rng_unit(uint64_t draw)
{
    return (double)(draw >> 11) * (1.0 / 9007199254740992.0);
}

#endif
