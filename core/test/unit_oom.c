// unit_oom.c - allocation failure on cbase's own paths, through oom.h's failing allocator: the k-th
// allocation of an operation fails, for k = 0, 1, 2, ... until a run fails none, and each run must
// answer its error (never a success with less in it) and leave nothing allocated.
//
// Not under ASan (oom.h OOM_AVAILABLE): each sweep then runs once, unfailed, and checks the answer.
#include "t.h"
#include "oom.h"

#include "net/hpack.h"
#include "net/router.h"
#include "util/err.h"
#include "util/str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A sweep's runs at most: an operation here makes far fewer allocations.
#define SWEEP_MAX 64

// The seam itself reaches cbase: an allocation util/str.c makes fails when armed, and a block it
// keeps is counted live until freed.
static void the_seam_reaches_cbase(void) {
    if (!OOM_AVAILABLE) { T_CHECK(true); return; }
    cb_buf b;
    cb_buf_init(&b);
    oom_fail_after(1);   // the first growth is made, the second fails
    T_CHECK(cb_buf_reserve(&b, 64));
    T_CHECK(!cb_buf_reserve(&b, 1000));
    long live = oom_live();
    size_t n = 1;
    uint8_t *taken = cb_buf_take(&b, &n);   // a buffer that ran out is no answer, and is freed
    T_EQ_INT(oom_live(), live - 1);
    oom_off();
    T_CHECK(oom_failed());
    T_EQ_INT(live, 1);
    T_CHECK(taken == NULL);
    T_EQ_INT(n, 0);
    free(taken);

    cb_buf_init(&b);
    oom_fail_after(SWEEP_MAX);
    T_CHECK(cb_buf_reserve(&b, 64));
    T_EQ_INT(oom_live(), 1);
    cb_buf_free(&b);
    T_EQ_INT(oom_live(), 0);
    oom_off();
    T_CHECK(!oom_failed());
}

// ---- HPACK: RFC 7541 C.4's three requests, Huffman-coded, into one decoder ----------------------
// Its allocations: the table (init), each entry the blocks add, and the scratch a Huffman string is
// decoded into.

typedef struct {
    char text[1024];
    size_t n;
} fields;

static int collect(void *ctx, const char *name, size_t nl, const char *value, size_t vl, bool never) {
    fields *f = ctx;
    int w = snprintf(f->text + f->n, sizeof f->text - f->n, "%.*s: %.*s\n", (int)nl, name, (int)vl, value);
    if (w > 0 && (size_t)w < sizeof f->text - f->n) f->n += (size_t)w;
    return CB_OK;
}

static size_t unhex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    int hi = -1;
    for (; *s; s++) {
        int v = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : -1;
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else if (n < cap) out[n++] = (uint8_t)(hi << 4 | v), hi = -1;
    }
    return n;
}

static const char *const c4[3] = {
    "8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff",
    "8286 84be 5886 a8eb 1064 9cbf",
    "8287 85bf 4088 25a8 49e9 5ba9 7d7f 8925 a849 e95b b8e8 b4bf",
};
static const char c4_fields[] =
    ":method: GET\n:scheme: http\n:path: /\n:authority: www.example.com\n"
    ":method: GET\n:scheme: http\n:path: /\n:authority: www.example.com\ncache-control: no-cache\n"
    ":method: GET\n:scheme: https\n:path: /index.html\n:authority: www.example.com\ncustom-key: custom-value\n";

// The three blocks: CB_OK and every field, or the first error.
static int hpack_requests(fields *f) {
    uint8_t blk[3][64];
    size_t len[3];
    for (int i = 0; i < 3; i++) len[i] = unhex(c4[i], blk[i], sizeof blk[i]);   // before the window
    memset(f, 0, sizeof *f);
    cb_hpack_dec d;
    int rc = cb_hpack_dec_init(&d, 4096);
    for (int i = 0; i < 3 && rc == CB_OK; i++) rc = cb_hpack_decode(&d, blk[i], len[i], collect, f);
    cb_hpack_dec_free(&d);
    return rc;
}

static void hpack_decode_fails_whole(void) {
    int runs = 0, failed_runs = 0;
    for (long k = 0; k < SWEEP_MAX; k++, runs++) {
        fields f;
        oom_fail_after(k);
        int rc = hpack_requests(&f);
        long live = oom_live();
        oom_off();
        T_EQ_INT(live, 0);
        if (!oom_failed()) {
            T_EQ_INT(rc, CB_OK);
            T_EQ_STR(f.text, c4_fields);
            break;
        }
        failed_runs++;
        if (rc != CB_E_NOMEM) fprintf(stderr, "     allocation %ld failed: rc %d, fields:\n%s", k, rc, f.text);
        T_EQ_INT(rc, CB_E_NOMEM);
    }
    T_CHECK(runs < SWEEP_MAX);
    if (OOM_AVAILABLE) T_CHECK(failed_runs >= 3);   // the table, an entry and a Huffman string at least
}

// ---- the router: 20 routes, past the table's first 16 ------------------------------------------

static void nothing(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {}

static const char *const patterns[20] = {
    "/a", "/b", "/c", "/d", "/e", "/f", "/g", "/h", "/i", "/j",
    "/k/:id", "/l/:id", "/m/:id", "/n/:id", "/o/:id", "/p/*", "/q/*", "/r/*", "/s/*", "/t/*",
};

static int routes(void) {
    cb_router *r = cb_router_new();
    if (!r) return CB_E_NOMEM;
    int rc = CB_OK;
    for (int i = 0; i < 20 && rc == CB_OK; i++) rc = cb_router_add(r, "GET", patterns[i], 0, nothing);
    bool known = false;
    if (rc == CB_OK && !cb_router_match(r, CB_M_GET, "/t/x", &known)) rc = CB_E_NOT_FOUND;
    cb_router_free(r);
    return rc;
}

static void router_fails_whole(void) {
    int runs = 0, failed_runs = 0;
    for (long k = 0; k < SWEEP_MAX; k++, runs++) {
        oom_fail_after(k);
        int rc = routes();
        long live = oom_live();
        oom_off();
        T_EQ_INT(live, 0);
        if (!oom_failed()) {
            T_EQ_INT(rc, CB_OK);
            break;
        }
        failed_runs++;
        T_EQ_INT(rc, CB_E_NOMEM);
    }
    T_CHECK(runs < SWEEP_MAX);
    if (OOM_AVAILABLE) T_CHECK(failed_runs >= 3);   // the router, its first table, the table grown
}

int main(void) {
    T_RUN(the_seam_reaches_cbase);
    T_RUN(hpack_decode_fails_whole);
    T_RUN(router_fails_whole);
    return T_DONE();
}
