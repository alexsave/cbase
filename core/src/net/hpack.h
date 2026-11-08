// hpack.h - HPACK (RFC 7541), the header compression of HTTP/2: a decoder with
// its dynamic table and Huffman, and an encoder that needs no dynamic table.
//
// THE DECODER is pure: bytes in, fields out through a callback. It holds the
// dynamic table one connection's peer fills, so every header block the peer
// sends on the connection must pass through it, in order, a cancelled
// stream's included, or the table falls out of step with the peer's.
//
// Any malformed block answers CB_E_INVALID, which is a COMPRESSION_ERROR: the
// table can no longer be trusted, and the connection must end (RFC 7541 2.1,
// RFC 9113 4.3).
//
// THE ENCODER writes each field as a static-table index when the pair is
// there, else as a literal never added to a table (with the static name's
// index when the name is there), Huffman-coded when that is shorter. It never
// indexes, so it never sends a table size update, and the peer's
// SETTINGS_HEADER_TABLE_SIZE costs nothing.
//
// Pure: no allocation but the decoder's table and its scratch, no I/O, no
// locks. One decoder belongs to one connection's reader.
#ifndef CB_HPACK_H
#define CB_HPACK_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The default and the most a decoder accepts for its dynamic table (the value
// the reader advertises as SETTINGS_HEADER_TABLE_SIZE).
#define CB_HPACK_TABLE_DEFAULT 4096u
#define CB_HPACK_TABLE_MAX     (1u << 16)
// The longest name or value a decoder takes, Huffman-decoded.
#define CB_HPACK_STRING_MAX    (1u << 20)

typedef struct {
    uint8_t *bytes;      // name then value, one allocation
    uint32_t name_len, value_len;
} cb_hpack_entry;

typedef struct {
    uint32_t        max;      // the most the peer may make it (what we advertised)
    uint32_t        limit;    // the size the peer's last table size update set (<= max)
    uint32_t        size;     // RFC 7541 4.1: the sum of name_len + value_len + 32
    cb_hpack_entry *e;        // newest first: dynamic index 1 is e[0]
    uint32_t        count, cap;   // cap = max / 32 + 1, the most entries the size allows
    cb_buf          name, value;  // scratch for a literal's Huffman-decoded strings
} cb_hpack_dec;

// max: the dynamic table size this side advertised, at most CB_HPACK_TABLE_MAX.
// CB_OK, CB_E_INVALID (max too big) or CB_E_NOMEM.
int  cb_hpack_dec_init(cb_hpack_dec *d, uint32_t max);
void cb_hpack_dec_free(cb_hpack_dec *d);

// One decoded field. name and value are valid only during the call. never:
// the peer sent it as never-indexed (a credential): an intermediary must not
// index it either. Answers CB_OK to go on; anything else stops the decode
// with that code, and the decoder is then out of step (end the connection).
typedef int (*cb_hpack_emit)(void *ctx, const char *name, size_t name_len, const char *value, size_t value_len,
                             bool never);

// Decodes one complete header block (the HEADERS payload and its
// CONTINUATIONs, joined). CB_OK, CB_E_INVALID for a malformed block, or what
// emit answered.
int cb_hpack_decode(cb_hpack_dec *d, const uint8_t *p, size_t n, cb_hpack_emit emit, void *ctx);

// The decoder's dynamic table, for the tests: entry i (1 is the newest), or
// false when there is none.
bool cb_hpack_dec_entry(const cb_hpack_dec *d, uint32_t i, cb_span *name, cb_span *value);

// ---- the encoder ------------------------------------------------------------
// Appends one field to a header block. never: write it as never-indexed (an
// API key, a token). Names must already be lowercase (HTTP/2 refuses others).
void cb_hpack_encode(cb_buf *out, const char *name, size_t name_len, const char *value, size_t value_len,
                     bool never);

// ---- the pieces, exposed for the tests and the fuzzers ----------------------
// An integer with an n-bit prefix (RFC 7541 5.1). decode: bytes used (> 0),
// or CB_E_INVALID when it is cut short or over 2^31 - 1.
int  cb_hpack_int_decode(const uint8_t *p, size_t n, int prefix_bits, uint32_t *out);
void cb_hpack_int_encode(cb_buf *out, uint8_t first, int prefix_bits, uint32_t v);
// Huffman (RFC 7541 5.2, Appendix B). decode appends to out: CB_OK, or
// CB_E_INVALID for EOS in the data, padding over 7 bits or not all ones.
int    cb_hpack_huff_decode(const uint8_t *p, size_t n, cb_buf *out);
size_t cb_hpack_huff_len(const uint8_t *p, size_t n);   // the encoded length in bytes
void   cb_hpack_huff_encode(cb_buf *out, const uint8_t *p, size_t n);
// The code table's own derivation check (unit_hpack): the canonical decode
// tables in hpack.c equal the ones derived from the code table. true when they do.
bool cb_hpack_huff_tables_agree(void);

#endif
