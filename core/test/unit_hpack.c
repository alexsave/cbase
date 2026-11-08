// unit_hpack.c - HPACK (net/hpack.h) against RFC 7541's own examples
// (Appendix C), its edges (integers, Huffman padding, table size updates,
// eviction), and the encoder against the decoder.
#include "t.h"

#include "net/hpack.h"
#include "rng.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// THE SEED of the round-trip draws: change this one line to vary them.
#define HPACK_TEST_SEED 0x5eed4ac4u

// Hex to bytes ("8286 84be": spaces skipped). Answers the length.
static size_t unhex(const char *h, uint8_t *out, size_t cap) {
    size_t n = 0;
    int hi = -1;
    for (; *h; h++) {
        int v;
        if (*h >= '0' && *h <= '9') v = *h - '0';
        else if (*h >= 'a' && *h <= 'f') v = *h - 'a' + 10;
        else continue;
        if (hi < 0) hi = v;
        else {
            if (n < cap) out[n] = (uint8_t)(hi << 4 | v);
            n++;
            hi = -1;
        }
    }
    return n;
}

// The fields a block decoded to, as "name: value\n" lines (never: "name:! value").
typedef struct {
    char text[4096];
    size_t len;
    int n;
} fields;

static int collect(void *ctx, const char *name, size_t nl, const char *value, size_t vl, bool never) {
    fields *f = ctx;
    int k = snprintf(f->text + f->len, sizeof f->text - f->len, "%.*s:%s %.*s\n", (int)nl, name, never ? "!" : "",
                     (int)vl, value);
    if (k > 0) f->len += (size_t)k;
    f->n++;
    return CB_OK;
}

static int decode_hex(cb_hpack_dec *d, const char *hex, fields *f) {
    uint8_t b[1024];
    size_t n = unhex(hex, b, sizeof b);
    memset(f, 0, sizeof *f);
    return cb_hpack_decode(d, b, n, collect, f);
}

static void table_is(const cb_hpack_dec *d, uint32_t size, const char *const *entries, int n) {
    T_EQ_INT(d->size, size);
    T_EQ_INT((int)d->count, n);
    for (int i = 0; i < n; i++) {
        cb_span name, value;
        T_CHECK(cb_hpack_dec_entry(d, (uint32_t)i + 1, &name, &value));
        char got[512];
        snprintf(got, sizeof got, "%.*s: %.*s", (int)name.n, name.p, (int)value.n, value.p);
        T_EQ_STR(got, entries[i]);
    }
}

// C.1: integers.
static void integers(void) {
    uint32_t v;
    uint8_t ten[] = { 0x0a };
    T_EQ_INT(cb_hpack_int_decode(ten, 1, 5, &v), 1);
    T_EQ_INT(v, 10);
    uint8_t big[] = { 0x1f, 0x9a, 0x0a };
    T_EQ_INT(cb_hpack_int_decode(big, 3, 5, &v), 3);
    T_EQ_INT(v, 1337);
    uint8_t fortytwo[] = { 0x2a };
    T_EQ_INT(cb_hpack_int_decode(fortytwo, 1, 8, &v), 1);
    T_EQ_INT(v, 42);
    // The encoder writes the same bytes.
    cb_buf b;
    cb_buf_init(&b);
    cb_hpack_int_encode(&b, 0x00, 5, 1337);
    T_EQ_MEM(b.p, b.len, big, sizeof big);
    // Cut short, and past 2^31 - 1.
    T_EQ_INT(cb_hpack_int_decode(big, 2, 5, &v), CB_E_INVALID);
    uint8_t huge[] = { 0x1f, 0xff, 0xff, 0xff, 0xff, 0x0f };
    T_EQ_INT(cb_hpack_int_decode(huge, sizeof huge, 5, &v), CB_E_INVALID);
    uint8_t endless[] = { 0x1f, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 };
    T_EQ_INT(cb_hpack_int_decode(endless, sizeof endless, 5, &v), CB_E_INVALID);
    // 2^31 - 1 itself fits.
    cb_buf_reset(&b);
    cb_hpack_int_encode(&b, 0x00, 5, 0x7fffffffu);
    T_EQ_INT(cb_hpack_int_decode(b.p, b.len, 5, &v), (int)b.len);
    T_EQ_INT(v, 0x7fffffff);
    cb_buf_free(&b);
}

// C.2: one field of each representation, each on a fresh table.
static void single_fields(void) {
    cb_hpack_dec d;
    fields f;
    T_EQ_INT(cb_hpack_dec_init(&d, 4096), CB_OK);
    T_EQ_INT(decode_hex(&d, "400a 6375 7374 6f6d 2d6b 6579 0d63 7573 746f 6d2d 6865 6164 6572", &f), CB_OK);
    T_EQ_STR(f.text, "custom-key: custom-header\n");
    const char *c21[] = { "custom-key: custom-header" };
    table_is(&d, 55, c21, 1);
    cb_hpack_dec_free(&d);

    T_EQ_INT(cb_hpack_dec_init(&d, 4096), CB_OK);
    T_EQ_INT(decode_hex(&d, "040c 2f73 616d 706c 652f 7061 7468", &f), CB_OK);
    T_EQ_STR(f.text, ":path: /sample/path\n");
    table_is(&d, 0, NULL, 0);
    T_EQ_INT(decode_hex(&d, "1008 7061 7373 776f 7264 0673 6563 7265 74", &f), CB_OK);
    T_EQ_STR(f.text, "password:! secret\n");
    table_is(&d, 0, NULL, 0);
    T_EQ_INT(decode_hex(&d, "82", &f), CB_OK);
    T_EQ_STR(f.text, ":method: GET\n");
    cb_hpack_dec_free(&d);
}

// C.3 and C.4: three requests on one connection, plain then Huffman: the same
// fields and the same tables.
static void requests(void) {
    static const char *const plain[3] = {
        "8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d",
        "8286 84be 5808 6e6f 2d63 6163 6865",
        "8287 85bf 400a 6375 7374 6f6d 2d6b 6579 0c63 7573 746f 6d2d 7661 6c75 65",
    };
    static const char *const huff[3] = {
        "8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff",
        "8286 84be 5886 a8eb 1064 9cbf",
        "8287 85bf 4088 25a8 49e9 5ba9 7d7f 8925 a849 e95b b8e8 b4bf",
    };
    static const char *const want[3] = {
        ":method: GET\n:scheme: http\n:path: /\n:authority: www.example.com\n",
        ":method: GET\n:scheme: http\n:path: /\n:authority: www.example.com\ncache-control: no-cache\n",
        ":method: GET\n:scheme: https\n:path: /index.html\n:authority: www.example.com\ncustom-key: custom-value\n",
    };
    const char *t1[] = { ":authority: www.example.com" };
    const char *t2[] = { "cache-control: no-cache", ":authority: www.example.com" };
    const char *t3[] = { "custom-key: custom-value", "cache-control: no-cache", ":authority: www.example.com" };
    for (int h = 0; h < 2; h++) {
        cb_hpack_dec d;
        fields f;
        T_EQ_INT(cb_hpack_dec_init(&d, 4096), CB_OK);
        const char *const *in = h ? huff : plain;
        T_EQ_INT(decode_hex(&d, in[0], &f), CB_OK);
        T_EQ_STR(f.text, want[0]);
        table_is(&d, 57, t1, 1);
        T_EQ_INT(decode_hex(&d, in[1], &f), CB_OK);
        T_EQ_STR(f.text, want[1]);
        table_is(&d, 110, t2, 2);
        T_EQ_INT(decode_hex(&d, in[2], &f), CB_OK);
        T_EQ_STR(f.text, want[2]);
        table_is(&d, 164, t3, 3);
        cb_hpack_dec_free(&d);
    }
}

// C.5 and C.6: three responses on a 256-byte table, so the later ones evict.
static void responses(void) {
    static const char *const plain[3] = {
        ("4803 3330 3258 0770 7269 7661 7465 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a 3133 3a32 3120 474d"
        " 546e 1768 7474 7073 3a2f 2f77 7777 2e65 7861 6d70 6c65 2e63 6f6d"),
        "4803 3330 37c1 c0bf",
        ("88c1 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a 3133 3a32 3220 474d 54c0 5a04 677a 6970 7738 666f"
        " 6f3d 4153 444a 4b48 514b 425a 584f 5157 454f 5049 5541 5851 5745 4f49 553b 206d 6178 2d61 6765 3d33 3630 303b"
        " 2076 6572 7369 6f6e 3d31"),
    };
    static const char *const huff[3] = {
        ("4882 6402 5885 aec3 771a 4b61 96d0 7abe 9410 54d4 44a8 2005 9504 0b81 66e0 82a6 2d1b ff6e 919d 29ad 1718 63c7"
        " 8f0b 97c8 e9ae 82ae 43d3"),
        "4883 640e ffc1 c0bf",
        ("88c1 6196 d07a be94 1054 d444 a820 0595 040b 8166 e084 a62d 1bff c05a 839b d9ab 77ad 94e7 821d d7f2 e6c7 b335"
        " dfdf cd5b 3960 d5af 2708 7f36 72c1 ab27 0fb5 291f 9587 3160 65c0 03ed 4ee5 b106 3d50 07"),
    };
    static const char *const want[3] = {
        ":status: 302\ncache-control: private\ndate: Mon, 21 Oct 2013 20:13:21 GMT\nlocation: https://www.example.com\n",
        ":status: 307\ncache-control: private\ndate: Mon, 21 Oct 2013 20:13:21 GMT\nlocation: https://www.example.com\n",
        (":status: 200\ncache-control: private\ndate: Mon, 21 Oct 2013 20:13:22 GMT\nlocation: https://www.example.com\n"
        "content-encoding: gzip\nset-cookie: foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1\n"),
    };
    const char *t1[] = { "location: https://www.example.com", "date: Mon, 21 Oct 2013 20:13:21 GMT",
                         "cache-control: private", ":status: 302" };
    const char *t2[] = { ":status: 307", "location: https://www.example.com", "date: Mon, 21 Oct 2013 20:13:21 GMT",
                         "cache-control: private" };
    const char *t3[] = { "set-cookie: foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1",
                         "content-encoding: gzip", "date: Mon, 21 Oct 2013 20:13:22 GMT" };
    for (int h = 0; h < 2; h++) {
        cb_hpack_dec d;
        fields f;
        T_EQ_INT(cb_hpack_dec_init(&d, 256), CB_OK);
        const char *const *in = h ? huff : plain;
        T_EQ_INT(decode_hex(&d, in[0], &f), CB_OK);
        T_EQ_STR(f.text, want[0]);
        table_is(&d, 222, t1, 4);
        T_EQ_INT(decode_hex(&d, in[1], &f), CB_OK);
        T_EQ_STR(f.text, want[1]);
        table_is(&d, 222, t2, 4);
        T_EQ_INT(decode_hex(&d, in[2], &f), CB_OK);
        T_EQ_STR(f.text, want[2]);
        table_is(&d, 215, t3, 3);
        cb_hpack_dec_free(&d);
    }
}

// Huffman: the tables agree with the code, every byte round-trips, and the
// padding rules hold.
static void huffman(void) {
    T_CHECK(cb_hpack_huff_tables_agree());
    uint8_t all[256];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    cb_buf enc, dec;
    cb_buf_init(&enc);
    cb_buf_init(&dec);
    cb_hpack_huff_encode(&enc, all, sizeof all);
    T_EQ_INT((int)enc.len, (int)cb_hpack_huff_len(all, sizeof all));
    T_EQ_INT(cb_hpack_huff_decode(enc.p, enc.len, &dec), CB_OK);
    T_EQ_MEM(dec.p, dec.len, all, sizeof all);
    // "www.example.com" (C.4.1).
    cb_buf_reset(&enc);
    cb_hpack_huff_encode(&enc, (const uint8_t *)"www.example.com", 15);
    uint8_t want[16];
    size_t wn = unhex("f1e3 c2e5 f23a 6ba0 ab90 f4ff", want, sizeof want);
    T_EQ_MEM(enc.p, enc.len, want, wn);
    // Padding of 8 ones is too long; padding with a zero is not EOS's prefix;
    // EOS itself (30 ones) in the data is refused.
    uint8_t pad8[] = { 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff, 0xff };
    cb_buf_reset(&dec);
    T_EQ_INT(cb_hpack_huff_decode(pad8, sizeof pad8, &dec), CB_E_INVALID);
    uint8_t padzero[] = { 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xfe };
    cb_buf_reset(&dec);
    T_EQ_INT(cb_hpack_huff_decode(padzero, sizeof padzero, &dec), CB_E_INVALID);
    // "00000000" is eight 5-bit codes, 40 bits exactly, so 0xff after it is 8 bits of padding.
    uint8_t zeros8[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0xff };
    cb_buf_reset(&dec);
    T_EQ_INT(cb_hpack_huff_decode(zeros8, 5, &dec), CB_OK);
    T_EQ_MEM(dec.p, dec.len, "00000000", 8);
    cb_buf_reset(&dec);
    T_EQ_INT(cb_hpack_huff_decode(zeros8, sizeof zeros8, &dec), CB_E_INVALID);
    // EOS (30 ones), then 2 ones of valid padding: refused for the EOS alone.
    uint8_t eos[] = { 0xff, 0xff, 0xff, 0xff };
    cb_buf_reset(&dec);
    T_EQ_INT(cb_hpack_huff_decode(eos, sizeof eos, &dec), CB_E_INVALID);
    // The encoder Huffman-codes a value when that is shorter (C.4.1's :authority,
    // as a literal without indexing on the static name), and leaves it raw when not.
    cb_buf_reset(&enc);
    cb_hpack_encode(&enc, ":authority", 10, "www.example.com", 15, false);
    wn = unhex("01 8c f1e3 c2e5 f23a 6ba0 ab90 f4ff", want, sizeof want);
    T_EQ_MEM(enc.p, enc.len, want, wn);
    cb_buf_reset(&enc);
    cb_hpack_encode(&enc, ":path", 5, "\x7f\x7f", 2, false);   // 28-bit codes: raw is shorter
    T_EQ_MEM(enc.p, enc.len, "\x04\x02\x7f\x7f", 4);
    cb_buf_free(&enc);
    cb_buf_free(&dec);
}

// Malformed blocks: index 0, an index past the table, a size update above the
// advertised max or after a field, a string longer than the block.
static void malformed(void) {
    static const char *const bad[] = {
        "80",          // indexed 0
        "be",          // 62: no dynamic entries yet
        "3fe2 1f",     // a size update to 4097, above the 4096 advertised
        "82 20",       // a size update after a field
        "400a 6375",   // a name cut short
        "40",          // a literal with nothing after it
        "4000",        // an empty name, then nothing for the value
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cb_hpack_dec d;
        fields f;
        T_EQ_INT(cb_hpack_dec_init(&d, 4096), CB_OK);
        T_EQ_INT(decode_hex(&d, bad[i], &f), CB_E_INVALID);
        cb_hpack_dec_free(&d);
    }
    // A size update at the start empties the table; to 0 and back is allowed.
    cb_hpack_dec d;
    fields f;
    T_EQ_INT(cb_hpack_dec_init(&d, 4096), CB_OK);
    T_EQ_INT(decode_hex(&d, "400a 6375 7374 6f6d 2d6b 6579 0d63 7573 746f 6d2d 6865 6164 6572", &f), CB_OK);
    T_EQ_INT((int)d.count, 1);
    T_EQ_INT(decode_hex(&d, "20 3fe1 1f 82", &f), CB_OK);   // to 0, then to 4096
    T_EQ_INT((int)d.count, 0);
    T_EQ_STR(f.text, ":method: GET\n");
    // An entry larger than the table empties it and is not added (4.4).
    T_EQ_INT(decode_hex(&d, "3f0f", &f), CB_OK);   // limit 31 + 15 = 46
    T_EQ_INT(decode_hex(&d, "400a 6375 7374 6f6d 2d6b 6579 0d63 7573 746f 6d2d 6865 6164 6572", &f), CB_OK);
    T_EQ_INT((int)d.count, 0);
    T_EQ_INT(d.size, 0);
    cb_hpack_dec_free(&d);
    T_EQ_INT(cb_hpack_dec_init(&d, CB_HPACK_TABLE_MAX + 1), CB_E_INVALID);
}

// The encoder against the decoder: static pairs, static names, new names,
// never-indexed, empty and long values, drawn from one seeded stream.
static void round_trip(void) {
    uint64_t seed = rng_seed64(HPACK_TEST_SEED);
    cb_hpack_dec d;
    T_EQ_INT(cb_hpack_dec_init(&d, 4096), CB_OK);
    static const char *const names[] = { ":method", ":path", ":scheme", ":authority", "content-type", "te",
                                         "grpc-timeout", "x-goog-api-key", "user-agent", "a" };
    for (int round = 0; round < 200; round++) {
        cb_buf block;
        cb_buf_init(&block);
        char want[4096];
        size_t wl = 0;
        int nf = 1 + (int)(rng_u32(&seed) % 6);
        for (int i = 0; i < nf; i++) {
            const char *name = names[rng_u32(&seed) % (sizeof names / sizeof names[0])];
            char value[300];
            int vl = (int)(rng_u32(&seed) % 120);
            for (int k = 0; k < vl; k++) value[k] = (char)(32 + rng_u32(&seed) % 95);
            if (rng_u32(&seed) % 4 == 0) { strcpy(value, "POST"); vl = 4; }
            bool never = rng_u32(&seed) % 5 == 0;
            cb_hpack_encode(&block, name, strlen(name), value, (size_t)vl, never);
            wl += (size_t)snprintf(want + wl, sizeof want - wl, "%s:%s %.*s\n", name, never ? "!" : "", vl, value);
        }
        fields f;
        memset(&f, 0, sizeof f);
        T_EQ_INT(cb_hpack_decode(&d, block.p, block.len, collect, &f), CB_OK);
        T_EQ_STR(f.text, want);
        T_EQ_INT((int)d.count, 0);   // the encoder never indexes
        cb_buf_free(&block);
    }
    cb_hpack_dec_free(&d);
}

int main(void) {
    T_RUN(integers);
    T_RUN(single_fields);
    T_RUN(requests);
    T_RUN(responses);
    T_RUN(huffman);
    T_RUN(malformed);
    T_RUN(round_trip);
    return T_DONE();
}
