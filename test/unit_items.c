// unit_items.c - the item layer (store/items.h) on both backends, and the
// key-capture proof's shapes (json_surface.h dw_shape): just enough to hold
// the coverage floors through stage 5 (docs/PLATFORM.md section 9 lists the
// full suite owed by stage 8).
#include "t.h"

#include "json_surface.h"
#include "lib_proc.h"
#include "lib_store_backend.h"
#include "store/items.h"
#include "store/store.h"
#include "store/store_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_dir[256];

typedef struct {
    int  n;
    char sk[8][32];
} sks;

static bool collect(const cb_item *it, void *ud) {
    sks *k = ud;
    if (k->n < 8) cb_item_s(it, "sk", k->sk[k->n++], sizeof k->sk[0]);
    return true;
}

// A transaction applies whole or not at all, and a query lists by an index.
static void sqlite_txn_and_query(void) {
    char path[320];
    snprintf(path, sizeof path, "%s/items.sqlite", g_dir);
    cb_store *s = tsb_open_fresh(path);
    T_CHECK(s != NULL);
    if (!s) return;
    const cb_av a1[] = { CB_AVS("gsi1pk", "L#x"), CB_AVS("gsi1sk", "1"), CB_AVN("n", 1) };
    const cb_av a2[] = { CB_AVS("gsi1pk", "L#x"), CB_AVS("gsi1sk", "2"), CB_AVN("n", 2) };
    cb_iwrite w[2] = {
        { .op = CB_IW_PUT, .key = { "P#1", "A" }, .attrs = a1, .n_attrs = 3, .cond = CB_IF_ABSENT },
        { .op = CB_IW_PUT, .key = { "P#1", "B" }, .attrs = a2, .n_attrs = 3, .cond = CB_IF_ABSENT },
    };
    int failed = 9;
    T_EQ_INT(CB_STORE_CALL(s, item_txn, w, 2, &failed), CB_OK);
    T_EQ_INT(failed, -1);
    // Again: the first put's condition fails, and nothing is written.
    cb_iwrite again[2] = { { .op = CB_IW_DELETE, .key = { "P#1", "B" } }, w[0] };
    T_EQ_INT(CB_STORE_CALL(s, item_txn, again, 2, &failed), CB_E_CONFLICT);
    T_EQ_INT(failed, 1);
    sks k = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, item_query, 1, "L#x", NULL, false, 0, collect, &k), CB_OK);
    T_EQ_INT(k.n, 2);
    T_EQ_STR(k.sk[0], "B");   // still there: the transaction rolled back
    memset(&k, 0, sizeof k);
    T_EQ_INT(CB_STORE_CALL(s, item_query, 0, "P#1", "A", true, 0, collect, &k), CB_OK);
    T_EQ_INT(k.n, 1);
    cb_iset inc = { CB_AVN("n", 5), CB_SET_INCR };
    cb_iwrite up = { .op = CB_IW_UPDATE, .key = { "P#1", "A" }, .sets = &inc, .n_sets = 1, .cond = CB_IF_EXISTS };
    cb_item old;
    cb_item_init(&old);
    T_EQ_INT(CB_STORE_CALL(s, item_write, &up, &old), CB_OK);
    T_EQ_INT(cb_item_n_or(&old, "n", 0), 1);
    T_EQ_INT(CB_STORE_CALL(s, item_get, (cb_ikey){ "P#1", "A" }, true, &old), CB_OK);
    T_EQ_INT(cb_item_n_or(&old, "n", 0), 6);
    cb_item_free(&old);
    cb_store_close(s);
}

// The DynamoDB spelling of a transaction and a query, over a recorded answer.
static const char *g_answer;
static char g_target[64];
static cb_buf g_body;

static int answer(void *ud, const char *target, const uint8_t *body, size_t len, int *status, cb_buf *resp) {
    snprintf(g_target, sizeof g_target, "%s", target);
    cb_buf_reset(&g_body);
    cb_buf_append(&g_body, body, len);
    *status = 200;
    cb_buf_puts(resp, g_answer);
    return CB_OK;
}

static void dynamo_txn_and_query(void) {
    cb_buf_init(&g_body);
    cb_store *s = NULL;
    T_EQ_INT(cb_store_open_dynamo_with("cbase", answer, NULL, &s), CB_OK);
    if (!s) return;
    const cb_av a[] = { CB_AVS("gsi1pk", "L#x"), CB_AVBOOL("b", true), CB_AVB("bytes", (const uint8_t *)"\1\2", 2) };
    cb_iwrite w[2] = {
        { .op = CB_IW_PUT, .key = { "P#1", "A" }, .attrs = a, .n_attrs = 3, .ttl_ms = 5000, .cond = CB_IF_ABSENT },
        { .op = CB_IW_CHECK, .key = { "P#2", "B" }, .cond = CB_IF_EXISTS },
    };
    g_answer = "{}";
    T_EQ_INT(CB_STORE_CALL(s, item_txn, w, 2, NULL), CB_OK);
    T_EQ_STR(g_target, "DynamoDB_20120810.TransactWriteItems");
    T_CHECK(strstr(cb_buf_cstr(&g_body), "\"ConditionCheck\"") != NULL);
    // And its shape: one line an item, the keys normalized.
    cb_buf shape;
    cb_buf_init(&shape);
    dw_shape(g_target, cb_span_of((const char *)g_body.p, g_body.len), &shape);
    T_CHECK(strstr(cb_buf_cstr(&shape), "Txn.Put item=b:BOOL,bytes:B,expires_at:N,expires_ms:N,gsi1pk:S=?,pk:S=?,sk:S=?\n") != NULL);
    T_CHECK(strstr(cb_buf_cstr(&shape), "Txn.ConditionCheck key=pk:S=?,sk:S=?\n") != NULL);
    cb_buf_free(&shape);
    g_answer = "{\"Items\":[{\"pk\":{\"S\":\"P#1\"},\"sk\":{\"S\":\"A\"},\"n\":{\"N\":\"-3\"}}],\"Count\":1}";
    sks k = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, item_query, 1, "L#x", "A", false, 10, collect, &k), CB_OK);
    T_EQ_INT(k.n, 1);
    T_EQ_STR(k.sk[0], "A");
    T_CHECK(strstr(cb_buf_cstr(&g_body), "\"IndexName\":\"gsi1\"") != NULL);
    T_CHECK(strstr(cb_buf_cstr(&g_body), "begins_with(#gsi1sk, :sk)") != NULL);
    cb_buf_init(&shape);
    dw_shape(g_target, cb_span_of((const char *)g_body.p, g_body.len), &shape);
    T_EQ_STR(cb_buf_cstr(&shape), "Query index=gsi1 names=gsi1pk,gsi1sk vals=S:?,S:{}#{}(prefix)\n");
    cb_buf_free(&shape);
    // A batch's shapes, one line an item, with an email and a hash normalized.
    cb_buf_init(&shape);
    dw_shape("DynamoDB_20120810.BatchWriteItem",
             CB_SPAN_LIT("{\"RequestItems\":{\"cbase\":[{\"DeleteRequest\":{\"Key\":{\"pk\":{\"S\":\"AUTHFAIL#a@x.io\"},"
                         "\"sk\":{\"S\":\"FAILS\"}}}},{\"DeleteRequest\":{\"Key\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"}}}}]}}"),
             &shape);
    T_EQ_STR(cb_buf_cstr(&shape), "Batch.DeleteRequest key=pk:S=AUTHFAIL#{},sk:S=FAILS\n"
                                  "Batch.DeleteRequest key=pk:S=GAME#{},sk:S=ROW\n");
    cb_buf_free(&shape);
    cb_buf_init(&shape);
    dw_shape("DynamoDB_20120810.BatchGetItem",
             CB_SPAN_LIT("{\"RequestItems\":{\"cbase\":{\"Keys\":[{\"pk\":{\"S\":\"USER#aaaaaaaa-0000-4000-8000-000000000001\"},"
                         "\"sk\":{\"S\":\"PROFILE\"}}]}}}"),
             &shape);
    T_EQ_STR(cb_buf_cstr(&shape), "BatchGet key=pk:S=USER#{},sk:S=PROFILE\n");
    cb_buf_free(&shape);
    cb_store_close(s);
    cb_buf_free(&g_body);
}

// The record SQLite keeps, every kind, and a cut one refused; DynamoDB's
// item read, and a value of a type the layer has no kind for refused.
static void records_and_reads(void) {
    cb_item it, back;
    cb_item_init(&it);
    cb_item_init(&back);
    const cb_av v[] = { CB_AVS("s", "text"), CB_AVN("n", -7), CB_AVBOOL("b", true), CB_AVB("y", (const uint8_t *)"\0\1", 2) };
    for (int i = 0; i < 4; i++) T_CHECK(cb_item_add(&it, &v[i]));
    cb_buf rec;
    cb_buf_init(&rec);
    T_CHECK(cb_item_record_encode(&it, &rec));
    T_CHECK(cb_item_record_decode(rec.p, rec.len, &back));
    T_EQ_INT(back.n, 4);
    char small[3];
    T_CHECK(!cb_item_s(&back, "s", small, sizeof small));
    T_CHECK(!cb_item_s(&back, "n", small, sizeof small));
    bool b = false;
    T_CHECK(cb_item_bool(&back, "b", &b) && b);
    T_EQ_INT(cb_item_n_or(&back, "missing", 42), 42);
    for (size_t cut = 1; cut < rec.len; cut++) {
        cb_item_reset(&back);
        // Cut inside an attribute: refused; at a boundary: the attributes before it.
        T_CHECK(!cb_item_record_decode(rec.p, cut, &back) || back.n < 4);
    }
    cb_buf_free(&rec);
    cb_item_reset(&back);
    T_CHECK(dw_item_read(CB_SPAN_LIT("{\"a\":{\"S\":\"x\"},\"b\":{\"BOOL\":false},\"c\":{\"B\":\"AAE=\"},\"d\":{\"N\":\"12\"}}"), &back));
    T_EQ_INT(back.n, 4);
    cb_item_reset(&back);
    T_CHECK(!dw_item_read(CB_SPAN_LIT("{\"a\":{\"L\":[]}}"), &back));
    T_CHECK(!dw_item_read(CB_SPAN_LIT("{\"a\":{\"N\":\"1.5\"}}"), &back));
    T_CHECK(!dw_item_read(CB_SPAN_LIT("{\"a\":{\"B\":\"!!\"}}"), &back));
    cb_item_free(&it);
    cb_item_free(&back);
}

// THE KEY-CAPTURE PROOF'S CLASSES ARE POSITIONAL (data safety: the proof
// holds the production keys). A game id is a short code that may be all
// digits of any length, all letters, or a word the layout also uses; the
// shape of a request must not change with it.
static void shape_of_game(const char *gid, cb_buf *out) {
    char pk[64], gsk[64], chat[64];
    snprintf(pk, sizeof pk, "GAME#%s", gid);
    snprintf(gsk, sizeof gsk, "001760000000000#%s", gid);
    snprintf(chat, sizeof chat, "CHAT#%s", gid);
    cb_buf_reset(out);
    const cb_iset st[] = { { CB_AVS("game_id", gid), CB_SET_TO }, { CB_AVS("gsi1pk", "USERGAMES#aaaaaaaa-0000-4000-8000-000000000001"), CB_SET_TO },
                           { CB_AVS("gsi1sk", gsk), CB_SET_TO } };
    cb_iwrite w = { .op = CB_IW_UPDATE, .key = { pk, "MEMBER#aaaaaaaa-0000-4000-8000-000000000001" }, .sets = st, .n_sets = 3 };
    dw_req r;
    dw_req_init(&r);
    T_CHECK(dw_iwrite(&r, "cbase", &w, false));
    dw_shape(r.target, cb_span_of((const char *)r.body.p, r.body.len), out);
    const cb_av a[] = { CB_AVS("game_id", gid) };
    cb_iwrite p = { .op = CB_IW_PUT, .key = { chat, gsk }, .attrs = a, .n_attrs = 1 };
    T_CHECK(dw_iwrite(&r, "cbase", &p, false));
    dw_shape(r.target, cb_span_of((const char *)r.body.p, r.body.len), out);
    dw_iquery(&r, "cbase", 0, pk, "MEMBER#", true, 0, cb_span_of("", 0));
    dw_shape(r.target, cb_span_of((const char *)r.body.p, r.body.len), out);
    dw_req_free(&r);
}

static void shapes_ignore_what_an_id_looks_like(void) {
    static const char *const IDS[] = { "1234567890", "123456789012345", "0000000000", "000000000000000", "7", "123456", "123456789012", "ABC123", "ROW", "SCAN",
                                       "a1b2c3d4e5f6", "aaaaaaaa-0000-4000-8000-000000000001" };
    cb_buf first, other;
    cb_buf_init(&first);
    cb_buf_init(&other);
    shape_of_game("ab12cd", &first);
    T_CHECK(strstr(cb_buf_cstr(&first), "pk:S=GAME#{}") != NULL);
    T_CHECK(strstr(cb_buf_cstr(&first), "sk:S={}#{}") != NULL);
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        shape_of_game(IDS[i], &other);
        T_EQ_STR(cb_buf_cstr(&other), cb_buf_cstr(&first));
    }
    cb_buf_free(&first);
    cb_buf_free(&other);
}

int main(void) {
    if (tp_tempdir(g_dir, sizeof g_dir) != 0) return 1;
    T_RUN(sqlite_txn_and_query);
    T_RUN(dynamo_txn_and_query);
    T_RUN(records_and_reads);
    T_RUN(shapes_ignore_what_an_id_looks_like);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) return 1;
    return T_DONE();
}
