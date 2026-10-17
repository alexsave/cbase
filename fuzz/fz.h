// fz.h - what every fuzz target (fuzz/fz_*.c) shares.
//
// Each fz_<name>.c defines LLVMFuzzerTestOneInput for one untrusted-input
// boundary, and is built two ways (c_base/Makefile):
//   build/fuzz/<name>         libFuzzer + ASan + UBSan (clang: `make fuzz`)
//   build/asan/fuzz/<name>    the same input function with fuzz/replay_main.c
//                             as main, under the ordinary ASan build (gcc or
//                             clang): every file of fuzz/corpus/<name> run once
//                             (`make fuzz-replay`, part of the asan CI lane).
//
// FZ_CHECK is an invariant: a failure aborts, which libFuzzer reports as a
// crash with the input saved, exactly as a sanitizer report is.
#ifndef CB_FZ_H
#define CB_FZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv);

#define FZ_CHECK(cond)                                                                      \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FZ_CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);   \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

// A reader over the fuzz input for the structure-aware targets: each take
// answers from the bytes left, and zeros once they run out (never fails).
typedef struct {
    const uint8_t *p;
    size_t         n, at;
} fz_in;

static inline fz_in fz_in_of(const uint8_t *p, size_t n) { fz_in r = { p, n, 0 }; return r; }
static inline size_t fz_left(const fz_in *r) { return r->n - r->at; }
static inline uint8_t fz_u8(fz_in *r) { return r->at < r->n ? r->p[r->at++] : 0; }
static inline uint16_t fz_u16(fz_in *r) { uint16_t a = fz_u8(r); return (uint16_t)(a | (uint16_t)fz_u8(r) << 8); }
static inline uint32_t fz_u32(fz_in *r) { uint32_t a = fz_u16(r); return a | (uint32_t)fz_u16(r) << 16; }
// Up to `want` bytes (fewer at the end): a pointer into the input and *got.
static inline const uint8_t *fz_bytes(fz_in *r, size_t want, size_t *got) {
    size_t k = fz_left(r) < want ? fz_left(r) : want;
    const uint8_t *p = r->p + r->at;
    r->at += k;
    *got = k;
    return p;
}

// A malloc'd exact-size copy, so a read one byte past the end is an ASan
// report (the fuzzer's own buffer is exact too; a copy is for a NUL-ended one).
static inline char *fz_cstr(const uint8_t *p, size_t n) {
    char *s = malloc(n + 1);
    if (!s) abort();
    if (n) memcpy(s, p, n);
    s[n] = 0;
    return s;
}
static inline uint8_t *fz_dup(const uint8_t *p, size_t n) {
    // calloc: an empty input's one byte is defined too (gcc warns otherwise).
    uint8_t *s = calloc(n ? n : 1, 1);
    if (!s) abort();
    if (n) memcpy(s, p, n);
    return s;
}

#endif
