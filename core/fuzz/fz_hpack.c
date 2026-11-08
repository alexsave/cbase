// fz_hpack.c - the HPACK decoder (net/hpack.h) as a connection's reader runs
// it: one decoder, its dynamic table carried across several header blocks cut
// from the input, so a block can name entries earlier ones added and resize
// the table under them. The input's first byte picks the table size this side
// advertised; then each block is a 2-byte length and that many bytes.
//
// Invariants: never a crash or a read past a block; the table's size stays
// within its limit and the limit within the advertised max; a malformed block
// is the end (the decoder is out of step, as a connection would be); and every
// block that decodes, its fields re-encoded with cb_hpack_encode and decoded by
// a fresh decoder (the encoder never indexes, so none needs a table), comes
// back the same fields, never-indexed flags and all.
#include "fz.h"

#include "net/hpack.h"
#include "util/err.h"

// Fields as <u32 name_len><name><u32 value_len><value><u8 never>, in order.
typedef struct {
    cb_buf b;
    int    n;
} fields;

static void put_u32(cb_buf *b, uint32_t v) { cb_buf_append(b, &v, sizeof v); }

static int collect(void *ctx, const char *name, size_t nl, const char *value, size_t vl, bool never) {
    fields *f = ctx;
    FZ_CHECK(nl <= CB_HPACK_STRING_MAX && vl <= CB_HPACK_STRING_MAX);
    put_u32(&f->b, (uint32_t)nl);
    cb_buf_append(&f->b, name, nl);
    put_u32(&f->b, (uint32_t)vl);
    cb_buf_append(&f->b, value, vl);
    cb_buf_putc(&f->b, never ? 1 : 0);
    f->n++;
    return CB_OK;
}

static uint32_t get_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

// Every field of f, encoded into one block.
static void encode_all(const fields *f, cb_buf *out) {
    size_t at = 0;
    for (int i = 0; i < f->n; i++) {
        uint32_t nl = get_u32(f->b.p + at);
        const char *n = (const char *)f->b.p + at + 4;
        at += 4 + nl;
        uint32_t vl = get_u32(f->b.p + at);
        const char *v = (const char *)f->b.p + at + 4;
        at += 4 + vl;
        bool never = f->b.p[at++] != 0;
        cb_hpack_encode(out, n, nl, v, vl, never);
    }
    FZ_CHECK(at == f->b.len);
}

static void check_table(const cb_hpack_dec *d) {
    FZ_CHECK(d->limit <= d->max);
    FZ_CHECK(d->size <= d->limit);
    uint32_t sum = 0;
    for (uint32_t i = 1; i <= d->count; i++) {
        cb_span n, v;
        FZ_CHECK(cb_hpack_dec_entry(d, i, &n, &v));
        sum += (uint32_t)(n.n + v.n + 32);
    }
    FZ_CHECK(sum == d->size);
    cb_span n, v;
    FZ_CHECK(!cb_hpack_dec_entry(d, d->count + 1, &n, &v));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fz_in in = fz_in_of(data, size);
    // 0: the default (4096); n: (n - 1) * 256, so 1 is no dynamic table and 2 is C.5's 256.
    uint8_t sel = fz_u8(&in);
    uint32_t max = sel == 0 ? CB_HPACK_TABLE_DEFAULT : (uint32_t)(sel - 1) * 256u;
    if (max > CB_HPACK_TABLE_MAX) max = CB_HPACK_TABLE_MAX;
    cb_hpack_dec d;
    FZ_CHECK(cb_hpack_dec_init(&d, max) == CB_OK);
    for (int blocks = 0; blocks < 64 && fz_left(&in); blocks++) {
        size_t n;
        const uint8_t *p = fz_bytes(&in, fz_u16(&in), &n);
        uint8_t *block = fz_dup(p, n);   // exact size: a read past it is an ASan report
        fields f = { .n = 0 };
        cb_buf_init(&f.b);
        int rc = cb_hpack_decode(&d, block, n, collect, &f);
        free(block);
        FZ_CHECK(rc == CB_OK || rc == CB_E_INVALID || rc == CB_E_NOMEM);
        check_table(&d);
        if (rc != CB_OK) {   // out of step: a connection would end here
            cb_buf_free(&f.b);
            break;
        }
        // The round trip through the encoder.
        cb_buf enc;
        cb_buf_init(&enc);
        encode_all(&f, &enc);
        cb_hpack_dec fresh;
        FZ_CHECK(cb_hpack_dec_init(&fresh, CB_HPACK_TABLE_DEFAULT) == CB_OK);
        fields g = { .n = 0 };
        cb_buf_init(&g.b);
        FZ_CHECK(cb_hpack_decode(&fresh, enc.p ? enc.p : (const uint8_t *)"", enc.len, collect, &g) == CB_OK);
        FZ_CHECK(fresh.count == 0);   // nothing was indexed
        FZ_CHECK(g.n == f.n && g.b.len == f.b.len && (!f.b.len || memcmp(g.b.p, f.b.p, f.b.len) == 0));
        cb_hpack_dec_free(&fresh);
        cb_buf_free(&g.b);
        cb_buf_free(&enc);
        cb_buf_free(&f.b);
    }
    cb_hpack_dec_free(&d);
    return 0;
}
