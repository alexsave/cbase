// unit_store_copy.c - the DynamoDB-to-SQLite copy (core/src/store/store_copy.c, the body of cbase-export)
// without a table.
//
// Representative items (an account, a game row with both index keys and a TTL, a rating with the sparse
// rating-ordered key, a row of every attribute kind) go through DynamoDB's own encoder (PutItem's body,
// dw_iwrite) and decoder (dw_item_read), then cb_copy_put into a SQLite file. A second file is written
// by the local store's own item_write from the same attributes. The two files' `items` rows must be
// identical, column by column and byte by byte (the attribute record). Then the checks: cb_copy_same
// holds for every item, every index key round-trips, and a damaged index key or attribute is caught.
#include "t.h"

#include "aws_json.h"
#include "lib_proc.h"
#include "store/store.h"
#include "store/store_copy.h"
#include "store/store_priv.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_dir[256];

typedef struct {
    const char *pk, *sk;
    cb_av       attrs[12];
    int         n;
    int64_t     ttl_ms;
} fixture;

static const uint8_t BLOB[5] = { 0x00, 0xff, 0x10, 0x80, 0x7f };
static const char *const LIST[3] = { "a", "b,c", "\xc3\xa9" };

static void fixtures(fixture *f) {
    f[0] = (fixture){ .pk = "USER#u1", .sk = "PROFILE", .n = 5, .attrs = {
        CB_AVS("nickname", "alex \"q\" \\ \xc3\xa9"), CB_AVS("email", "a@example.com"), CB_AVN("created_ms", 1700000000000LL),
        CB_AVBOOL("admin", false), CB_AVB("hash", BLOB, sizeof BLOB) } };
    f[1] = (fixture){ .pk = "GAME#g1", .sk = "STATE", .ttl_ms = 4102444800000LL, .n = 7, .attrs = {
        CB_AVS("gsi1pk", "GAMES#open"), CB_AVS("gsi1sk", "000001700000000000#g1"), CB_AVS("gsi2pk", "PLAYER#u1"),
        CB_AVS("gsi2sk", "000001700000000000#g1"), CB_AVN("version", 42), CB_AVSL("seats", LIST, 3), CB_AVN("big", INT64_MIN) } };
    f[2] = (fixture){ .pk = "RATING#u1", .sk = "ELO", .n = 4, .attrs = {
        CB_AVN("rating", 1523), CB_AVN("games", 17), CB_AVS("gsi2pk", "RATINGS"), CB_AVS("gsi2sk", "E#0000000000#u1") } };
    f[3] = (fixture){ .pk = "SCAN#marker", .sk = "X", .n = 1, .attrs = { CB_AVBOOL("flag", true) } };
}
#define NFIX 4

// The item exactly as DynamoDB's PutItem would carry it, decoded the way a Scan page is.
static bool through_dynamo(const fixture *f, cb_item *out) {
    cb_iwrite w;
    memset(&w, 0, sizeof w);
    w.op = CB_IW_PUT;
    w.key = (cb_ikey){ f->pk, f->sk };
    w.attrs = f->attrs;
    w.n_attrs = f->n;
    w.ttl_ms = f->ttl_ms;
    dw_req r;
    dw_req_init(&r);
    cb_buf ans;
    cb_buf_init(&ans);
    cb_span item;
    cb_item_init(out);
    bool ok = dw_iwrite(&r, "t", &w, false) && dw_get_answer_of_put(&r, &ans) && dw_item(cb_buf_cstr(&ans), ans.len, &item) &&
              dw_item_read(item, out);
    cb_buf_free(&ans);
    dw_req_free(&r);
    return ok;
}

static void write_direct(cb_store *s, const fixture *f) {
    cb_iwrite w;
    memset(&w, 0, sizeof w);
    w.op = CB_IW_PUT;
    w.key = (cb_ikey){ f->pk, f->sk };
    w.attrs = f->attrs;
    w.n_attrs = f->n;
    w.ttl_ms = f->ttl_ms;
    T_EQ_INT(s->ops->item_write(s, &w, NULL), CB_OK);
}

// Every row of the items table, one line each, the attribute record in hex.
static char *dump(const char *path) {
    sqlite3 *db;
    char *out = calloc(1, 1);
    size_t len = 0;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) return out;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT pk, sk, ifnull(gsi1pk,'-'), ifnull(gsi1sk,'-'), ifnull(gsi2pk,'-'), ifnull(gsi2sk,'-'), "
            "ifnull(expires_ms,-1), hex(attrs) FROM items WHERE pk NOT LIKE 'BOT#%' ORDER BY pk, sk", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            char line[2048];
            int n = snprintf(line, sizeof line, "%s|%s|%s|%s|%s|%s|%lld|%s\n", sqlite3_column_text(st, 0),
                             sqlite3_column_text(st, 1), sqlite3_column_text(st, 2), sqlite3_column_text(st, 3),
                             sqlite3_column_text(st, 4), sqlite3_column_text(st, 5), (long long)sqlite3_column_int64(st, 6),
                             sqlite3_column_text(st, 7));
            out = realloc(out, len + (size_t)n + 1);
            memcpy(out + len, line, (size_t)n + 1);
            len += (size_t)n;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

static void the_copy_writes_what_the_store_writes(void) {
    fixture f[NFIX];
    fixtures(f);
    char pa[300], pb[300];
    snprintf(pa, sizeof pa, "%s/copied.sqlite", g_dir);
    snprintf(pb, sizeof pb, "%s/direct.sqlite", g_dir);
    cb_store *a = NULL, *b = NULL;
    T_EQ_INT(cb_store_open_sqlite(pa, &a), CB_OK);
    T_EQ_INT(cb_store_open_sqlite(pb, &b), CB_OK);
    for (int i = 0; i < NFIX; i++) {
        cb_item it;
        T_CHECK(through_dynamo(&f[i], &it));
        T_EQ_INT(cb_copy_put(a, (cb_ikey){ f[i].pk, f[i].sk }, &it), CB_OK);
        T_CHECK(cb_copy_same(a, (cb_ikey){ f[i].pk, f[i].sk }, &it));
        cb_item_free(&it);
        write_direct(b, &f[i]);
    }
    cb_store_close(a);
    cb_store_close(b);
    char *da = dump(pa), *db = dump(pb);
    T_CHECK(strlen(da) > 0);
    T_EQ_STR(da, db);
    free(da);
    free(db);
}

static void every_index_key_round_trips_and_a_broken_one_is_caught(void) {
    fixture f[NFIX];
    fixtures(f);
    char pa[300];
    snprintf(pa, sizeof pa, "%s/index.sqlite", g_dir);
    cb_store *a = NULL;
    T_EQ_INT(cb_store_open_sqlite(pa, &a), CB_OK);
    for (int i = 0; i < NFIX; i++) {
        cb_item it;
        T_CHECK(through_dynamo(&f[i], &it));
        T_EQ_INT(cb_copy_put(a, (cb_ikey){ f[i].pk, f[i].sk }, &it), CB_OK);
        cb_item_free(&it);
    }
    int total = 0;
    for (int i = 0; i < NFIX; i++) {
        int n = 0;
        T_EQ_INT(cb_copy_index_roundtrip(a, (cb_ikey){ f[i].pk, f[i].sk }, &n), 0);
        total += n;
    }
    T_EQ_INT(total, 3);   // the game on gsi1 and gsi2, the rating on gsi2; the account and the marker are in none
    // The copy of an item whose index key was lost (the GSI column NULL while the attribute remains) does not round-trip.
    sqlite3 *db;
    char *err = NULL;
    cb_store_close(a);
    T_EQ_INT(sqlite3_open(pa, &db), SQLITE_OK);
    T_EQ_INT(sqlite3_exec(db, "UPDATE items SET gsi2pk = NULL WHERE pk = 'GAME#g1'", NULL, NULL, &err), SQLITE_OK);
    sqlite3_close(db);
    T_EQ_INT(cb_store_open_sqlite(pa, &a), CB_OK);
    int n = 0;
    T_EQ_INT(cb_copy_index_roundtrip(a, (cb_ikey){ "GAME#g1", "STATE" }, &n), 1);
    T_EQ_INT(n, 2);
    // An attribute changed in the file is not the same item.
    cb_item it;
    T_CHECK(through_dynamo(&f[0], &it));
    cb_av other = CB_AVS("nickname", "someone else");
    cb_iset set = { .v = other, .how = CB_SET_TO };
    cb_iwrite w;
    memset(&w, 0, sizeof w);
    w.op = CB_IW_UPDATE;
    w.key = (cb_ikey){ f[0].pk, f[0].sk };
    w.sets = &set;
    w.n_sets = 1;
    T_EQ_INT(a->ops->item_write(a, &w, NULL), CB_OK);
    T_CHECK(!cb_copy_same(a, (cb_ikey){ f[0].pk, f[0].sk }, &it));
    cb_item_free(&it);
    cb_store_close(a);
}

static void a_pk_family_is_the_text_before_the_first_hash(void) {
    char fam[8];
    cb_copy_family("USER#u1", fam, sizeof fam);
    T_EQ_STR(fam, "USER");
    cb_copy_family("SOLO", fam, sizeof fam);
    T_EQ_STR(fam, "SOLO");
    cb_copy_family("LONGFAMILY#x", fam, sizeof fam);
    T_EQ_STR(fam, "LONGFAM");
}

// What a failed decode says: the attribute and its type, never the value (the log must not hold user data).
static void an_item_that_does_not_decode_says_which_attribute_and_never_its_value(void) {
    const char *bad[] = {
        "{\"pk\":{\"S\":\"X#1\"},\"sk\":{\"S\":\"A\"},\"score\":{\"N\":\"1523.75\"}}",   // a fraction
        "{\"pk\":{\"S\":\"X#1\"},\"sk\":{\"S\":\"A\"},\"score\":{\"M\":{\"secret\":{\"S\":\"hunter2\"}}}}",   // a map
    };
    const char *want_type[] = { "type N", "type M" };
    for (int i = 0; i < 2; i++) {
        cb_item it;
        cb_item_init(&it);
        char why[160] = "";
        T_CHECK(!dw_item_read(cb_span_of(bad[i], strlen(bad[i])), &it));
        cb_item_reset(&it);
        T_CHECK(!dw_item_read_why(cb_span_of(bad[i], strlen(bad[i])), &it, why, sizeof why));
        T_CHECK(strstr(why, "\"score\"") != NULL && strstr(why, want_type[i]) != NULL);
        T_CHECK(strstr(why, "1523") == NULL && strstr(why, "hunter2") == NULL);
        cb_item_free(&it);
    }
    cb_item ok;
    cb_item_init(&ok);
    const char *good = "{\"pk\":{\"S\":\"X#1\"},\"n\":{\"N\":\"7\"}}";
    T_CHECK(dw_item_read_why(cb_span_of(good, strlen(good)), &ok, NULL, 0));
    cb_item_free(&ok);
}

// ---- credentials that run out ---------------------------------------------------------------

static int g_sends, g_expired_sent;

// A DynamoDB that answers GetItem only to the session token T2: the first token the test sets is stale.
static int fake_send(void *ud, const char *target, const uint8_t *body, size_t len, int *status, cb_buf *resp) {
    g_sends++;
    const char *tok = getenv("AWS_SESSION_TOKEN");
    if (!tok || strcmp(tok, "T2") != 0) {
        g_expired_sent++;
        *status = 400;
        cb_buf_puts(resp, "{\"__type\":\"com.amazon.coral.service#ExpiredTokenException\",\"message\":\"The security token included in the request is expired\"}");
        return CB_OK;
    }
    *status = 200;
    cb_buf_puts(resp, "{\"Item\":{\"pk\":{\"S\":\"X#1\"},\"sk\":{\"S\":\"A\"},\"n\":{\"N\":\"7\"}}}");
    return CB_OK;
}

static void expired_credentials_are_fetched_again_and_the_request_goes_on(void) {
    setenv("AWS_ACCESS_KEY_ID", "AK1", 1);
    setenv("AWS_SECRET_ACCESS_KEY", "SK1", 1);
    setenv("AWS_SESSION_TOKEN", "T1", 1);
    // Without a credential command the expiry is a failure.
    cb_store *s = NULL;
    T_EQ_INT(cb_store_open_dynamo_with("t", fake_send, NULL, &s), CB_OK);
    cb_item it;
    cb_item_init(&it);
    g_sends = g_expired_sent = 0;
    T_EQ_INT(s->ops->item_get(s, (cb_ikey){ "X#1", "A" }, true, &it), CB_E_DB);
    T_EQ_INT(g_sends, 1);
    // With one: the command runs once, the environment holds its keys, the request is sent again and succeeds.
    T_EQ_INT(cb_store_dynamo_creds_process(s, "printf '{\"Version\":1,\"AccessKeyId\":\"AK2\",\"SecretAccessKey\":\"SK2\",\"SessionToken\":\"T2\"}'"), CB_OK);
    g_sends = g_expired_sent = 0;
    T_EQ_INT(s->ops->item_get(s, (cb_ikey){ "X#1", "A" }, true, &it), CB_OK);
    T_EQ_INT(g_sends, 2);
    T_EQ_INT(g_expired_sent, 1);
    T_EQ_STR(getenv("AWS_ACCESS_KEY_ID"), "AK2");
    T_EQ_INT(it.n, 3);
    // A command that fails does not loop: the expiry is the answer.
    setenv("AWS_SESSION_TOKEN", "T1", 1);
    T_EQ_INT(cb_store_dynamo_creds_process(s, "false"), CB_OK);
    g_sends = g_expired_sent = 0;
    T_EQ_INT(s->ops->item_get(s, (cb_ikey){ "X#1", "A" }, true, &it), CB_E_DB);
    T_CHECK(g_sends <= 4);
    cb_item_free(&it);
    cb_store_close(s);
    // The classifier and the parser.
    const char *exp = "{\"__type\":\"com.amazon.coral.service#ExpiredTokenException\"}";
    T_CHECK(dw_creds_expired(400, exp, strlen(exp)));
    T_CHECK(!dw_creds_expired(200, exp, strlen(exp)));
    const char *thr = "{\"__type\":\"com.amazonaws.dynamodb.v20120810#ThrottlingException\"}";
    T_CHECK(!dw_creds_expired(400, thr, strlen(thr)));
    char k[32], sec[32], tok[32];
    const char *pj = "{\"AccessKeyId\":\"A\",\"SecretAccessKey\":\"B\"}";
    T_CHECK(dw_creds_process_parse(pj, strlen(pj), k, sizeof k, sec, sizeof sec, tok, sizeof tok) && tok[0] == 0);
    T_CHECK(!dw_creds_process_parse("{}", 2, k, sizeof k, sec, sizeof sec, tok, sizeof tok));
}

int main(void) {
    if (tp_tempdir(g_dir, sizeof g_dir) != 0) return 1;
    T_RUN(the_copy_writes_what_the_store_writes);
    T_RUN(every_index_key_round_trips_and_a_broken_one_is_caught);
    T_RUN(an_item_that_does_not_decode_says_which_attribute_and_never_its_value);
    T_RUN(expired_credentials_are_fetched_again_and_the_request_goes_on);
    T_RUN(a_pk_family_is_the_text_before_the_first_hash);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) return 1;
    return T_DONE();
}
