// structgen fixture: what --snapshot copies out of wasm memory.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "kinds.h"

typedef struct { KCard a, b; } SPair;
typedef struct { uint8_t len; char text[6]; int16_t score; } SItem;

typedef struct {
    int8_t   n_pairs;      // count of pairs
    uint8_t  n_items;      // count of items
    uint16_t n_text;       // count of text (UTF-8 bytes)
    bool     flag;
    int16_t  w;
    uint32_t u;
    int64_t  big;
    double   d;
    unsigned bits : 3;
    int      sbits : 5;
    SPair    pairs[4];
    SItem    items[3];
    char     text[8];      // counted: n_text bytes
    char     cstr[6];      // NUL-terminated
    int16_t  nums[3];      // not counted: all three
    KCard    card;
} Snap;

// Pointers a snapshot follows, each by its --count; not writable (--writer refuses it).
typedef struct {
    const int16_t *vals;   uint8_t  n_vals;
    const SItem   *items;  int32_t  n_items;
    const char    *name;   uint16_t name_len;   // counted: name_len bytes of UTF-8
    const SPair   *none;   uint8_t  n_none;     // NULL with a count of 0
    int16_t        tail;
} SPtr;

// Not snapshot-able: two array dimensions, and a union.
typedef struct { int8_t m[2][2]; } SMatrix;
typedef struct { union { int32_t i; float f; } u; } SWithUnion;
