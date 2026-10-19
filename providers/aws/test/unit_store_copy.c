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

int main(void) {
    if (tp_tempdir(g_dir, sizeof g_dir) != 0) return 1;
    T_RUN(the_copy_writes_what_the_store_writes);
    T_RUN(every_index_key_round_trips_and_a_broken_one_is_caught);
    T_RUN(a_pk_family_is_the_text_before_the_first_hash);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) return 1;
    return T_DONE();
}
