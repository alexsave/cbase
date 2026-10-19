// conformance/store.c - the store conformance suite (conformance/store.h).
#include "conformance/store.h"

#include "t.h"

#include "store/items.h"
#include "util/err.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static cb_store *S;
static char RUN[40];   // this run's partition prefix: every key the suite writes begins with it

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void pause_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// "<RUN>#<name>", in one of a few rotating buffers.
static const char *P(const char *name) {
    static char buf[8][128];
    static int at;
    char *b = buf[at++ % 8];
    snprintf(b, sizeof buf[0], "%s#%s", RUN, name);
    return b;
}

// ---- the calls -------------------------------------------------------------------------

// A write expected to go through: a refusal may be spurious (contention), so
// it is sent again a few times before its answer counts.
static int write_ok(const cb_iwrite *w, cb_item *old) {
    int rc = CB_E_CONFLICT;
    for (int i = 0; i < 3 && rc == CB_E_CONFLICT; i++) rc = CB_STORE_CALL(S, item_write, w, old);
    return rc;
}

static int write_once(const cb_iwrite *w, cb_item *old) { return CB_STORE_CALL(S, item_write, w, old); }

static int txn_ok(const cb_iwrite *w, int n) {
    int rc = CB_E_CONFLICT;
    for (int i = 0; i < 3 && rc == CB_E_CONFLICT; i++) rc = CB_STORE_CALL(S, item_txn, w, n, NULL, NULL);
    return rc;
}

static int get(const char *pk, const char *sk, cb_item *it) {
    return CB_STORE_CALL(S, item_get, (cb_ikey){ pk, sk }, true, it);
}

static int put(const char *pk, const char *sk, const cb_av *a, int n) {
    cb_iwrite w = { .op = CB_IW_PUT, .key = { pk, sk }, .attrs = a, .n_attrs = n };
    return write_ok(&w, NULL);
}

static int update(const char *pk, const char *sk, const cb_iset *st, int n, const cb_iclause *c, int nc) {
    cb_iwrite w = { .op = CB_IW_UPDATE, .key = { pk, sk }, .sets = st, .n_sets = n, .n_cond = nc };
    for (int i = 0; i < nc; i++) w.cond[i] = c[i];
    return nc ? write_once(&w, NULL) : write_ok(&w, NULL);
}

static bool present(const char *pk, const char *sk) {
    cb_item it;
    cb_item_init(&it);
    bool ok = get(pk, sk, &it) == CB_OK;
    cb_item_free(&it);
    return ok;
}

// The attribute `name` of the item at pk/sk holds v (another kind or an
// absent attribute does not).
static bool holds(const char *pk, const char *sk, const cb_av *v) {
    cb_item it;
    cb_item_init(&it);
    cb_av have;
    bool ok = get(pk, sk, &it) == CB_OK && cb_item_get(&it, v->name, &have) && cb_av_same(&have, v);
    cb_item_free(&it);
    return ok;
}

static bool lacks(const char *pk, const char *sk, const char *name) {
    cb_item it;
    cb_item_init(&it);
    cb_av have;
    bool ok = get(pk, sk, &it) == CB_OK && !cb_item_get(&it, name, &have);
    cb_item_free(&it);
    return ok;
}

static int64_t num(const char *pk, const char *sk, const char *name) {
    cb_item it;
    cb_item_init(&it);
    int64_t v = get(pk, sk, &it) == CB_OK ? cb_item_n_or(&it, name, INT64_MIN) : INT64_MIN;
    cb_item_free(&it);
    return v;
}

// ---- a walk's answers: the sort keys handed out, joined by ',' -----------------------------

typedef struct {
    char text[2048];
    int  calls;
    int  stop_after;   // > 0: answer false at this call
    bool del;          // delete each item handed out
    bool keys_only;    // each item holds pk and sk and nothing else
    bool table_sk;     // the table's sk (else: the queried index's sort key)
    int  index;
} walk;

static bool on_item(const cb_item *it, void *ud) {
    walk *w = ud;
    char sk[CB_ITEM_KEY_MAX + 1] = "", pk[CB_ITEM_KEY_MAX + 1] = "";
    const char *skn = w->index == 1 && !w->table_sk ? "gsi1sk" : w->index == 2 && !w->table_sk ? "gsi2sk" : "sk";
    cb_item_s(it, skn, sk, sizeof sk);
    cb_item_s(it, "pk", pk, sizeof pk);
    size_t at = strlen(w->text);
    snprintf(w->text + at, sizeof w->text - at, "%s%s", w->calls ? "," : "", sk);
    w->calls++;
    if (w->keys_only) {
        cb_av v;
        T_EQ_INT(it->n, 2);
        T_CHECK(cb_item_get(it, "pk", &v) && cb_item_get(it, "sk", &v));
    }
    if (w->del) {
        char tsk[CB_ITEM_KEY_MAX + 1] = "";
        cb_item_s(it, "sk", tsk, sizeof tsk);
        cb_iwrite d = { .op = CB_IW_DELETE, .key = { pk, tsk } };
        T_EQ_INT(CB_STORE_CALL(S, item_write, &d, NULL), CB_OK);   // the walk's callback writes the store
    }
    return !(w->stop_after > 0 && w->calls >= w->stop_after);
}

// The query's answer as joined sort keys. An index lags its table: its query
// is polled until it answers `want` (or CS_INDEX_WAIT_MS passes, and the last
// answer is the one judged).
static int query(const cb_iquery *q, walk *w, const char *want) {
    int64_t until = now_ms() + CS_INDEX_WAIT_MS;
    walk base = *w;
    for (;;) {
        *w = base;
        w->index = q->index;
        int rc = CB_STORE_CALL(S, item_query, q, on_item, w);
        if (rc != CB_OK || q->index == 0 || !want || strcmp(w->text, want) == 0 || now_ms() > until) return rc;
        pause_ms(50);
    }
}

// The joined answer of a plain query.
static const char *q_text(int index, const char *pk, const char *prefix, const char *below, bool forward, const char *want) {
    static walk w;
    memset(&w, 0, sizeof w);
    cb_iquery q = { .index = index, .pk = pk, .sk_prefix = prefix, .sk_below = below, .forward = forward };
    if (query(&q, &w, want) != CB_OK) return "(error)";
    return w.text;
}

// ---- the cases ----------------------------------------------------------------------------

// Every kind of value comes back as it was written; an absent item is
// CB_E_NOT_FOUND with the item handed in left empty.
static void every_kind_round_trips(void) {
    const char *pk = P("kinds");
    static const uint8_t bytes[] = { 0, 1, 0x7f, 0x80, 0xff };
    const char *list[] = { "a", "", "c\"d\\e" };
    const cb_av a[] = {
        CB_AVS("s", "plain"), CB_AVS("s_odd", "q\"b\\s/\n\t\x01 \xc3\xa9"), CB_AVS("s_empty", ""),
        CB_AVN("n0", 0), CB_AVN("n_neg", -1), CB_AVN("n_max", INT64_MAX), CB_AVN("n_min", INT64_MIN),
        CB_AVBOOL("t", true), CB_AVBOOL("f", false),
        CB_AVB("b", bytes, sizeof bytes), CB_AVB("b_empty", NULL, 0),
        CB_AVSL("l", list, 3), CB_AVSL("l_empty", list, 0),
    };
    int n = (int)(sizeof a / sizeof a[0]);
    T_EQ_INT(put(pk, "A", a, n), CB_OK);
    cb_item it;
    cb_item_init(&it);
    T_EQ_INT(get(pk, "A", &it), CB_OK);
    T_EQ_INT(it.n, n + 2);   // and pk and sk
    for (int i = 0; i < n; i++) {
        cb_av have;
        bool got = cb_item_get(&it, a[i].name, &have);
        if (!t_report(got && cb_av_same(&have, &a[i]), __FILE__, __LINE__, "the value read is the value written"))
            fprintf(stderr, "     attribute %s\n", a[i].name);
    }
    char pkv[128];
    T_CHECK(cb_item_s(&it, "pk", pkv, sizeof pkv) && strcmp(pkv, pk) == 0);
    T_CHECK(cb_item_s(&it, "sk", pkv, sizeof pkv) && strcmp(pkv, "A") == 0);
    // An absent item: NOT_FOUND, and the item read into before is emptied.
    T_EQ_INT(get(pk, "nothing", &it), CB_E_NOT_FOUND);
    T_EQ_INT(it.n, 0);
    T_EQ_INT(get(P("no-partition"), "A", &it), CB_E_NOT_FOUND);
    cb_item_free(&it);
}

// A put replaces the whole item; a delete of an absent item is fine.
static void put_replaces_and_delete_is_idempotent(void) {
    const char *pk = P("put");
    const cb_av a1[] = { CB_AVS("x", "1"), CB_AVS("y", "2") }, a2[] = { CB_AVS("z", "3") };
    T_EQ_INT(put(pk, "A", a1, 2), CB_OK);
    T_EQ_INT(put(pk, "A", a2, 1), CB_OK);
    T_CHECK(lacks(pk, "A", "x"));
    T_CHECK(lacks(pk, "A", "y"));
    T_CHECK(holds(pk, "A", &a2[0]));
    cb_iwrite d = { .op = CB_IW_DELETE, .key = { pk, "A" } };
    T_EQ_INT(write_ok(&d, NULL), CB_OK);
    T_CHECK(!present(pk, "A"));
    T_EQ_INT(write_ok(&d, NULL), CB_OK);
}

// An update makes an absent item, keeps what it does not set, and each set kind does its own thing.
static void update_and_each_set_kind(void) {
    const char *pk = P("update");
    const cb_iset make[] = { { CB_AVS("a", "1"), CB_SET_TO }, { CB_AVN("n", 7), CB_SET_TO }, { CB_AVS("gone", "x"), CB_SET_TO } };
    T_EQ_INT(update(pk, "A", make, 3, NULL, 0), CB_OK);   // no condition: an upsert
    cb_av a1 = CB_AVS("a", "1"), n7 = CB_AVN("n", 7);
    T_CHECK(holds(pk, "A", &a1));
    T_CHECK(holds(pk, "A", &n7));
    // TO replaces, IF_ABSENT keeps a present one and sets an absent one,
    // INCR adds to a present number and starts a missing one at 0, REMOVE
    // drops (an absent one too), and what is not named is kept.
    const cb_iset st[] = {
        { CB_AVS("a", "2"), CB_SET_TO },
        { CB_AVN("n", 100), CB_SET_IF_ABSENT },
        { CB_AVS("fresh", "f"), CB_SET_IF_ABSENT },
        { CB_AVN("c", 5), CB_SET_INCR },
        { CB_AVS("gone", ""), CB_SET_REMOVE },
        { CB_AVS("never", ""), CB_SET_REMOVE },
    };
    T_EQ_INT(update(pk, "A", st, 6, NULL, 0), CB_OK);
    cb_av a2 = CB_AVS("a", "2"), ff = CB_AVS("fresh", "f"), c5 = CB_AVN("c", 5);
    T_CHECK(holds(pk, "A", &a2));
    T_CHECK(holds(pk, "A", &n7));
    T_CHECK(holds(pk, "A", &ff));
    T_CHECK(holds(pk, "A", &c5));
    T_CHECK(lacks(pk, "A", "gone"));
    T_CHECK(lacks(pk, "A", "never"));
    const cb_iset inc[] = { { CB_AVN("c", -7), CB_SET_INCR }, { CB_AVN("n", 3), CB_SET_INCR } };
    T_EQ_INT(update(pk, "A", inc, 2, NULL, 0), CB_OK);
    T_EQ_INT(num(pk, "A", "c"), -2);
    T_EQ_INT(num(pk, "A", "n"), 10);
    // INCR onto a string is refused (how is the backend's), and nothing changes.
    const cb_iset bad[] = { { CB_AVS("a", "z"), CB_SET_TO }, { CB_AVN("fresh", 1), CB_SET_INCR } };
    T_CHECK(update(pk, "A", bad, 2, NULL, 0) != CB_OK);
    T_CHECK(holds(pk, "A", &a2));
    T_CHECK(holds(pk, "A", &ff));
}

// One clause at a time, against an item holding n = 5 and s = "a": each
// check holds where it should and refuses where it should, and a refused
// write changes nothing.
typedef struct {
    cb_iclause c;
    bool       holds;
    const char *what;
} clause_case;

static void each_check_alone(void) {
    const char *pk = P("cond");
    const cb_av base[] = { CB_AVN("n", 5), CB_AVS("s", "a"), CB_AVN("mark", 0) };
    T_EQ_INT(put(pk, "A", base, 3), CB_OK);
    const clause_case cases[] = {
        { CB_IF_EXISTS, true, "EXISTS on a present item" },
        { CB_IF_ABSENT, false, "ABSENT on a present item" },
        { { CB_C_EQ, CB_AVS("s", "a") }, true, "EQ equal" },
        { { CB_C_EQ, CB_AVS("s", "b") }, false, "EQ another value" },
        { { CB_C_EQ, CB_AVN("s", 5) }, false, "EQ another kind" },
        { { CB_C_EQ, CB_AVS("missing", "a") }, false, "EQ an absent attribute" },
        { { CB_C_NE, CB_AVS("s", "b") }, true, "NE another value" },
        { { CB_C_NE, CB_AVS("s", "a") }, false, "NE equal" },
        { { CB_C_NE, CB_AVS("missing", "a") }, true, "NE an absent attribute (DynamoDB's <>)" },
        { { CB_C_LT, CB_AVN("n", 6) }, true, "LT below" },
        { { CB_C_LT, CB_AVN("n", 5) }, false, "LT equal" },
        { { CB_C_LT, CB_AVN("missing", 9) }, false, "LT an absent attribute" },
        { { CB_C_ATTR_EXISTS, CB_AVS("s", "") }, true, "ATTR_EXISTS present" },
        { { CB_C_ATTR_EXISTS, CB_AVS("missing", "") }, false, "ATTR_EXISTS absent" },
        { { CB_C_ABSENT_OR_EQ, CB_AVS("missing", "q") }, true, "ABSENT_OR_EQ absent" },
        { { CB_C_ABSENT_OR_EQ, CB_AVS("s", "a") }, true, "ABSENT_OR_EQ equal" },
        { { CB_C_ABSENT_OR_EQ, CB_AVS("s", "b") }, false, "ABSENT_OR_EQ another value" },
        { { CB_C_ABSENT_OR_LT, CB_AVN("missing", 0) }, true, "ABSENT_OR_LT absent" },
        { { CB_C_ABSENT_OR_LT, CB_AVN("n", 6) }, true, "ABSENT_OR_LT below" },
        { { CB_C_ABSENT_OR_LT, CB_AVN("n", 5) }, false, "ABSENT_OR_LT not below" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int64_t before = num(pk, "A", "mark");
        cb_iset st = { CB_AVN("mark", (int64_t)i + 1), CB_SET_TO };
        cb_iwrite w = { .op = CB_IW_UPDATE, .key = { pk, "A" }, .sets = &st, .n_sets = 1, .cond = { cases[i].c }, .n_cond = 1 };
        int rc = cases[i].holds ? write_ok(&w, NULL) : write_once(&w, NULL);
        bool ok = rc == (cases[i].holds ? CB_OK : CB_E_CONFLICT) && num(pk, "A", "mark") == (cases[i].holds ? (int64_t)i + 1 : before);
        if (!t_report(ok, __FILE__, __LINE__, "the clause holds or refuses, and a refusal writes nothing"))
            fprintf(stderr, "     %s: rc %d\n", cases[i].what, rc);
    }
    // On an absent item: EXISTS refuses (an update makes nothing), ABSENT holds.
    cb_iset st = { CB_AVN("mark", 1), CB_SET_TO };
    cb_iwrite w = { .op = CB_IW_UPDATE, .key = { pk, "B" }, .sets = &st, .n_sets = 1, .cond = { CB_IF_EXISTS }, .n_cond = 1 };
    T_EQ_INT(write_once(&w, NULL), CB_E_CONFLICT);
    T_CHECK(!present(pk, "B"));
    w.cond[0] = (cb_iclause){ CB_C_NE, CB_AVS("s", "a") };   // a comparison on an absent item: the attribute is absent
    T_EQ_INT(write_ok(&w, NULL), CB_OK);
    T_CHECK(present(pk, "B"));
    cb_iwrite p = { .op = CB_IW_PUT, .key = { pk, "C" }, .cond = { CB_IF_ABSENT }, .n_cond = 1 };
    T_EQ_INT(write_ok(&p, NULL), CB_OK);
    T_EQ_INT(write_once(&p, NULL), CB_E_CONFLICT);
    cb_iwrite d = { .op = CB_IW_DELETE, .key = { pk, "C" }, .cond = { { CB_C_EQ, CB_AVN("n", 1) } }, .n_cond = 1 };
    T_EQ_INT(write_once(&d, NULL), CB_E_CONFLICT);
    T_CHECK(present(pk, "C"));
}

// Four clauses, all of which must hold: each one failing alone refuses the write.
static void four_clauses_all_must_hold(void) {
    const char *pk = P("cond4");
    const cb_av base[] = { CB_AVN("n", 5), CB_AVS("s", "a"), CB_AVN("mark", 0) };
    T_EQ_INT(put(pk, "A", base, 3), CB_OK);
    const cb_iclause good[CB_ICOND_MAX] = {
        CB_IF_EXISTS, { CB_C_EQ, CB_AVS("s", "a") }, { CB_C_NE, CB_AVS("absent", "x") }, { CB_C_LT, CB_AVN("n", 9) },
    };
    const cb_iclause bad[CB_ICOND_MAX] = {
        CB_IF_ABSENT, { CB_C_EQ, CB_AVS("s", "b") }, { CB_C_NE, CB_AVS("s", "a") }, { CB_C_LT, CB_AVN("n", 5) },
    };
    cb_iset st = { CB_AVN("mark", 1), CB_SET_TO };
    cb_iwrite w = { .op = CB_IW_UPDATE, .key = { pk, "A" }, .sets = &st, .n_sets = 1, .n_cond = CB_ICOND_MAX };
    memcpy(w.cond, good, sizeof good);
    T_EQ_INT(write_ok(&w, NULL), CB_OK);
    T_EQ_INT(num(pk, "A", "mark"), 1);
    for (int i = 0; i < CB_ICOND_MAX; i++) {
        memcpy(w.cond, good, sizeof good);
        w.cond[i] = bad[i];
        st.v.n = 10 + i;
        int rc = write_once(&w, NULL);
        if (!t_report(rc == CB_E_CONFLICT && num(pk, "A", "mark") == 1, __FILE__, __LINE__, "one failing clause of four refuses"))
            fprintf(stderr, "     clause %d: rc %d\n", i, rc);
    }
    // More clauses than the layer holds: refused before anything is sent.
    w.n_cond = CB_ICOND_MAX + 1;
    T_EQ_INT(write_once(&w, NULL), CB_E_INVALID);
}

// The item as it was comes back from a write handed `old`: after an update
// and a delete, and on a refusal (empty when there was no item).
static void the_old_item_comes_back(void) {
    const char *pk = P("old");
    const cb_av base[] = { CB_AVN("n", 1), CB_AVS("s", "a") };
    T_EQ_INT(put(pk, "A", base, 2), CB_OK);
    cb_item old;
    cb_item_init(&old);
    cb_iset st = { CB_AVN("n", 2), CB_SET_TO };
    cb_iwrite w = { .op = CB_IW_UPDATE, .key = { pk, "A" }, .sets = &st, .n_sets = 1, .cond = { CB_IF_EXISTS }, .n_cond = 1 };
    T_EQ_INT(write_ok(&w, &old), CB_OK);
    T_EQ_INT(cb_item_n_or(&old, "n", 0), 1);   // as it was, not as written
    T_EQ_INT(num(pk, "A", "n"), 2);
    // Refused: the item, as it is.
    w.cond[0] = (cb_iclause){ CB_C_EQ, CB_AVN("n", 9) };
    T_EQ_INT(write_once(&w, &old), CB_E_CONFLICT);
    T_EQ_INT(cb_item_n_or(&old, "n", 0), 2);
    char s[8];
    T_CHECK(cb_item_s(&old, "s", s, sizeof s) && strcmp(s, "a") == 0);
    // Refused on an absent item: empty.
    cb_iwrite x = w;
    x.key.sk = "absent";
    x.cond[0] = CB_IF_EXISTS;
    T_EQ_INT(write_once(&x, &old), CB_E_CONFLICT);
    T_EQ_INT(old.n, 0);
    // An update that makes the item: empty.
    x.n_cond = 0;
    T_EQ_INT(write_ok(&x, &old), CB_OK);
    T_EQ_INT(old.n, 0);
    // A delete: the item it removed.
    cb_iwrite d = { .op = CB_IW_DELETE, .key = { pk, "A" } };
    T_EQ_INT(write_ok(&d, &old), CB_OK);
    T_EQ_INT(cb_item_n_or(&old, "n", 0), 2);
    T_CHECK(!present(pk, "A"));
    cb_item_free(&old);
}

// TTL: a write's ttl_ms is expires_ms (ms) and expires_at (seconds); an
// update sets it as ttl_how says. An expired item may still be stored (TTL
// deletion lags), so a read either misses it or gets it with its time past:
// the layer above filters.
static void ttl_and_expired_items(void) {
    const char *pk = P("ttl");
    int64_t t = now_ms() + 3600 * 1000;
    const cb_av a[] = { CB_AVS("x", "1") };
    cb_iwrite w = { .op = CB_IW_PUT, .key = { pk, "A" }, .attrs = a, .n_attrs = 1, .ttl_ms = t };
    T_EQ_INT(write_ok(&w, NULL), CB_OK);
    T_EQ_INT(num(pk, "A", CB_ITEM_TTL_MS), t);
    T_EQ_INT(num(pk, "A", CB_ITEM_TTL_S), t / 1000);
    cb_iwrite u = { .op = CB_IW_UPDATE, .key = { pk, "A" }, .ttl_ms = t + 5000, .ttl_how = CB_SET_IF_ABSENT };
    T_EQ_INT(write_ok(&u, NULL), CB_OK);
    T_EQ_INT(num(pk, "A", CB_ITEM_TTL_MS), t);   // kept
    u.ttl_how = CB_SET_TO;
    T_EQ_INT(write_ok(&u, NULL), CB_OK);
    T_EQ_INT(num(pk, "A", CB_ITEM_TTL_MS), t + 5000);
    T_EQ_INT(num(pk, "A", CB_ITEM_TTL_S), (t + 5000) / 1000);
    u.key.sk = "B";   // IF_ABSENT on an absent item sets it
    u.ttl_how = CB_SET_IF_ABSENT;
    T_EQ_INT(write_ok(&u, NULL), CB_OK);
    T_EQ_INT(num(pk, "B", CB_ITEM_TTL_MS), t + 5000);
    // Expired (a minute ago).
    int64_t past = now_ms() - 60 * 1000;
    w.key.sk = "C";
    w.ttl_ms = past;
    T_EQ_INT(write_ok(&w, NULL), CB_OK);
    cb_item it;
    cb_item_init(&it);
    int rc = get(pk, "C", &it);
    T_CHECK(rc == CB_E_NOT_FOUND || (rc == CB_OK && cb_item_n_or(&it, CB_ITEM_TTL_MS, INT64_MAX) <= now_ms()));
    cb_item_free(&it);
}

// A transaction applies whole or not at all; a refusal names the first write
// that failed, and, asked, whether its item existed.
static void txn_whole_or_nothing(void) {
    const char *pk = P("txn");
    const cb_av a[] = { CB_AVN("n", 1) };
    T_EQ_INT(put(pk, "held", a, 1), CB_OK);
    T_EQ_INT(put(pk, "doomed", a, 1), CB_OK);
    T_EQ_INT(put(pk, "witness", a, 1), CB_OK);
    cb_iset inc = { CB_AVN("n", 1), CB_SET_INCR };
    cb_iwrite w[4] = {
        { .op = CB_IW_PUT, .key = { pk, "new" }, .attrs = a, .n_attrs = 1, .cond = { CB_IF_ABSENT }, .n_cond = 1 },
        { .op = CB_IW_DELETE, .key = { pk, "doomed" } },
        { .op = CB_IW_UPDATE, .key = { pk, "held" }, .sets = &inc, .n_sets = 1, .cond = { { CB_C_EQ, CB_AVN("n", 9) } }, .n_cond = 1,
          .old_on_fail = true },
        { .op = CB_IW_CHECK, .key = { pk, "witness" }, .cond = { CB_IF_EXISTS }, .n_cond = 1 },
    };
    int failed = 7;
    bool had = false;
    T_EQ_INT(CB_STORE_CALL(S, item_txn, w, 4, &failed, &had), CB_E_CONFLICT);
    T_EQ_INT(failed, 2);
    T_CHECK(had);   // the refused write's item exists
    T_CHECK(!present(pk, "new"));
    T_CHECK(present(pk, "doomed"));
    T_EQ_INT(num(pk, "held", "n"), 1);
    // Refused where the item is absent: had is false.
    w[2].key.sk = "nobody";
    w[2].cond[0] = CB_IF_EXISTS;
    had = true;
    T_EQ_INT(CB_STORE_CALL(S, item_txn, w, 4, &failed, &had), CB_E_CONFLICT);
    T_EQ_INT(failed, 2);
    T_CHECK(!had);
    // A CHECK refusing, alone in its clause.
    w[2].key.sk = "held";
    w[2].cond[0] = (cb_iclause){ CB_C_EQ, CB_AVN("n", 1) };
    w[3].cond[0] = CB_IF_ABSENT;
    T_EQ_INT(CB_STORE_CALL(S, item_txn, w, 4, &failed, NULL), CB_E_CONFLICT);
    T_EQ_INT(failed, 3);
    T_CHECK(!present(pk, "new"));
    // All holding: every write lands.
    w[3].cond[0] = CB_IF_EXISTS;
    failed = 7;
    int rc = CB_E_CONFLICT;
    for (int i = 0; i < 3 && rc == CB_E_CONFLICT; i++) rc = CB_STORE_CALL(S, item_txn, w, 4, &failed, &had);
    T_EQ_INT(rc, CB_OK);
    T_EQ_INT(failed, -1);
    T_CHECK(present(pk, "new"));
    T_CHECK(!present(pk, "doomed"));
    T_EQ_INT(num(pk, "held", "n"), 2);
    // A CHECK is a transaction's only: alone it is refused, and so is one with no condition.
    T_EQ_INT(write_once(&w[3], NULL), CB_E_INVALID);
    cb_iwrite bare = { .op = CB_IW_CHECK, .key = { pk, "held" } };
    T_EQ_INT(CB_STORE_CALL(S, item_txn, &bare, 1, NULL, NULL), CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(S, item_txn, w, 0, NULL, NULL), CB_E_INVALID);
}

// CB_ITEM_TXN_MAX writes go in one transaction; one more is refused whole;
// so is one that touches an item twice (DynamoDB refuses it, and local is
// the tightest).
static void txn_limits(void) {
    const char *pk = P("txnmax");
    cb_iwrite *w = calloc(CB_ITEM_TXN_MAX + 1, sizeof *w);
    char (*sk)[8] = calloc(CB_ITEM_TXN_MAX + 1, sizeof *sk);
    if (!w || !sk) { T_CHECK(false); free(w); free(sk); return; }
    for (int i = 0; i <= CB_ITEM_TXN_MAX; i++) {
        snprintf(sk[i], sizeof sk[0], "%03d", i);
        w[i] = (cb_iwrite){ .op = CB_IW_PUT, .key = { pk, sk[i] } };
    }
    T_EQ_INT(CB_STORE_CALL(S, item_txn, w, CB_ITEM_TXN_MAX + 1, NULL, NULL), CB_E_INVALID);
    T_CHECK(!present(pk, "000"));
    T_EQ_INT(txn_ok(w, CB_ITEM_TXN_MAX), CB_OK);
    T_CHECK(present(pk, "000"));
    T_CHECK(present(pk, "099"));
    cb_iwrite twice[2] = { { .op = CB_IW_PUT, .key = { pk, "dup" } }, { .op = CB_IW_DELETE, .key = { pk, "dup" } } };
    T_EQ_INT(CB_STORE_CALL(S, item_txn, twice, 2, NULL, NULL), CB_E_INVALID);
    T_CHECK(!present(pk, "dup"));
    free(w);
    free(sk);
}

// A query of the table: one partition in sort key order, either way, with a
// prefix or an upper bound, keys only.
static void query_the_table(void) {
    const char *pk = P("qt");
    static const char *const sks[] = { "b2", "a1", "a3", "b1", "a2" };
    const cb_av a[] = { CB_AVS("x", "1"), CB_AVS("gsi1pk", "nowhere") };
    for (int i = 0; i < 5; i++) T_EQ_INT(put(pk, sks[i], a, 2), CB_OK);
    T_EQ_STR(q_text(0, pk, NULL, NULL, true, NULL), "a1,a2,a3,b1,b2");
    T_EQ_STR(q_text(0, pk, NULL, NULL, false, NULL), "b2,b1,a3,a2,a1");
    T_EQ_STR(q_text(0, pk, "a", NULL, true, NULL), "a1,a2,a3");
    T_EQ_STR(q_text(0, pk, "b", NULL, false, NULL), "b2,b1");
    T_EQ_STR(q_text(0, pk, "a2", NULL, true, NULL), "a2");
    T_EQ_STR(q_text(0, pk, "c", NULL, true, NULL), "");
    T_EQ_STR(q_text(0, pk, NULL, "b", true, NULL), "a1,a2,a3");
    T_EQ_STR(q_text(0, pk, NULL, "b1", false, NULL), "a3,a2,a1");
    T_EQ_STR(q_text(0, pk, NULL, "a1", true, NULL), "");
    T_EQ_STR(q_text(0, P("qt-empty"), NULL, NULL, true, NULL), "");
    walk w = { .keys_only = true };
    cb_iquery q = { .index = 0, .pk = pk, .forward = true, .keys_only = true };
    T_EQ_INT(query(&q, &w, NULL), CB_OK);
    T_EQ_INT(w.calls, 5);
}

// Pages (a small page size gives every item, across pages), a callback that
// stops, and one that writes the store (a cascade deletes what it lists).
static void query_pages_stops_and_writes(void) {
    const char *pk = P("qp");
    for (int i = 0; i < 7; i++) {
        char sk[8];
        snprintf(sk, sizeof sk, "%d", i);
        T_EQ_INT(put(pk, sk, NULL, 0), CB_OK);
    }
    walk w = { 0 };
    cb_iquery q = { .index = 0, .pk = pk, .forward = true, .page = 2 };
    T_EQ_INT(query(&q, &w, NULL), CB_OK);
    T_EQ_STR(w.text, "0,1,2,3,4,5,6");
    memset(&w, 0, sizeof w);
    w.stop_after = 3;
    T_EQ_INT(query(&q, &w, NULL), CB_OK);
    T_EQ_INT(w.calls, 3);
    T_EQ_STR(w.text, "0,1,2");
    memset(&w, 0, sizeof w);
    w.stop_after = 1;
    q.page = 0;
    T_EQ_INT(query(&q, &w, NULL), CB_OK);
    T_EQ_INT(w.calls, 1);
    memset(&w, 0, sizeof w);
    w.del = true;
    q.page = 3;
    T_EQ_INT(query(&q, &w, NULL), CB_OK);
    T_EQ_INT(w.calls, 7);
    T_EQ_STR(q_text(0, pk, NULL, NULL, true, NULL), "");
}

// The two overloaded indexes: an item is listed under the keys it carries
// (sparse: one with none is in no index), in index sort key order, and a
// moved or removed index key moves or removes it, once the index catches up.
static void query_the_indexes(void) {
    const char *pk = P("qi"), *g1 = P("g1"), *g2 = P("g2");
    struct { const char *sk, *g1sk, *g2sk; } it[] = { { "A", "3", "y" }, { "B", "1", NULL }, { "C", "2", "x" }, { "D", NULL, NULL } };
    for (int i = 0; i < 4; i++) {
        cb_av a[4];
        int n = 0;
        if (it[i].g1sk) { a[n++] = CB_AVS("gsi1pk", g1); a[n++] = CB_AVS("gsi1sk", it[i].g1sk); }
        if (it[i].g2sk) { a[n++] = CB_AVS("gsi2pk", g2); a[n++] = CB_AVS("gsi2sk", it[i].g2sk); }
        T_EQ_INT(put(pk, it[i].sk, a, n), CB_OK);
    }
    T_EQ_STR(q_text(1, g1, NULL, NULL, true, "1,2,3"), "1,2,3");
    T_EQ_STR(q_text(1, g1, NULL, NULL, false, "3,2,1"), "3,2,1");
    T_EQ_STR(q_text(1, g1, "2", NULL, true, "2"), "2");
    T_EQ_STR(q_text(1, g1, NULL, "3", false, "2,1"), "2,1");
    T_EQ_STR(q_text(2, g2, NULL, NULL, true, "x,y"), "x,y");
    T_EQ_STR(q_text(2, g2, "y", NULL, true, "y"), "y");
    T_EQ_STR(q_text(1, g2, NULL, NULL, true, ""), "");   // another index's key is not this one's
    // The table's sort key of what the index lists.
    walk w = { .table_sk = true };
    cb_iquery q = { .index = 1, .pk = g1, .forward = true };
    T_EQ_INT(query(&q, &w, "B,C,A"), CB_OK);
    T_EQ_STR(w.text, "B,C,A");
    memset(&w, 0, sizeof w);
    w.keys_only = true;
    w.table_sk = true;
    q.keys_only = true;
    T_EQ_INT(query(&q, &w, "B,C,A"), CB_OK);
    T_EQ_INT(w.calls, 3);
    // Moved, then removed.
    const cb_iset mv[] = { { CB_AVS("gsi1sk", "0"), CB_SET_TO } };
    T_EQ_INT(update(pk, "A", mv, 1, NULL, 0), CB_OK);
    T_EQ_STR(q_text(1, g1, NULL, NULL, true, "0,1,2"), "0,1,2");
    const cb_iset rm[] = { { CB_AVS("gsi1pk", ""), CB_SET_REMOVE }, { CB_AVS("gsi1sk", ""), CB_SET_REMOVE } };
    T_EQ_INT(update(pk, "B", rm, 2, NULL, 0), CB_OK);
    T_EQ_STR(q_text(1, g1, NULL, NULL, true, "0,2"), "0,2");
    cb_iwrite d = { .op = CB_IW_DELETE, .key = { pk, "C" } };
    T_EQ_INT(write_ok(&d, NULL), CB_OK);
    T_EQ_STR(q_text(1, g1, NULL, NULL, true, "0"), "0");
    T_EQ_STR(q_text(2, g2, NULL, NULL, true, "y"), "y");
}

// Batch get: each present key's item handed out once, with the index of its
// first place in keys; an absent one not at all; past 100 keys; projected.
typedef struct {
    int  calls, stop_after;
    int  seen[256];
    bool only_projected;
} batch_walk;

static bool on_batch(int i, const cb_item *it, void *ud) {
    batch_walk *b = ud;
    b->calls++;
    if (i >= 0 && i < 256) b->seen[i]++;
    if (b->only_projected) {
        cb_av v;
        T_CHECK(cb_item_get(it, "pk", &v) && cb_item_get(it, "sk", &v) && cb_item_get(it, "a", &v));
        T_CHECK(!cb_item_get(it, "b", &v));
        T_EQ_INT(it->n, 3);
    }
    return !(b->stop_after > 0 && b->calls >= b->stop_after);
}

static void batch_get(void) {
    const char *pk = P("bg");
    enum { N = 150 };
    char (*sk)[8] = calloc(N + 4, sizeof *sk);
    cb_ikey *keys = calloc(N + 4, sizeof *keys);
    cb_iwrite *w = calloc(N, sizeof *w);
    if (!sk || !keys || !w) { T_CHECK(false); free(sk); free(keys); free(w); return; }
    const cb_av a[] = { CB_AVS("a", "1"), CB_AVS("b", "2") };
    for (int i = 0; i < N; i++) {
        snprintf(sk[i], sizeof sk[0], "%03d", i);
        keys[i] = (cb_ikey){ pk, sk[i] };
        w[i] = (cb_iwrite){ .op = CB_IW_PUT, .key = keys[i], .attrs = a, .n_attrs = 2 };
    }
    T_EQ_INT(txn_ok(w, 100), CB_OK);
    T_EQ_INT(txn_ok(w + 100, N - 100), CB_OK);
    // Past 100 keys, and two absent ones at the end.
    snprintf(sk[N], sizeof sk[0], "x1");
    snprintf(sk[N + 1], sizeof sk[0], "x2");
    keys[N] = (cb_ikey){ pk, sk[N] };
    keys[N + 1] = (cb_ikey){ pk, sk[N + 1] };
    batch_walk b = { 0 };
    T_EQ_INT(CB_STORE_CALL(S, item_batch_get, keys, N + 2, NULL, 0, on_batch, &b), CB_OK);
    T_EQ_INT(b.calls, N);
    int once = 0;
    for (int i = 0; i < N; i++) once += b.seen[i] == 1;
    T_EQ_INT(once, N);
    T_EQ_INT(b.seen[N] + b.seen[N + 1], 0);
    // A repeated key: read once, handed out under its first index.
    cb_ikey rep[4] = { keys[5], keys[7], keys[5], keys[N] };
    memset(&b, 0, sizeof b);
    T_EQ_INT(CB_STORE_CALL(S, item_batch_get, rep, 4, NULL, 0, on_batch, &b), CB_OK);
    T_EQ_INT(b.calls, 2);
    T_EQ_INT(b.seen[0], 1);
    T_EQ_INT(b.seen[1], 1);
    T_EQ_INT(b.seen[2], 0);
    // Projected: pk and sk always, and what was asked.
    const char *const proj[] = { "a" };
    memset(&b, 0, sizeof b);
    b.only_projected = true;
    T_EQ_INT(CB_STORE_CALL(S, item_batch_get, keys, 3, proj, 1, on_batch, &b), CB_OK);
    T_EQ_INT(b.calls, 3);
    // Stopped by the callback, and nothing asked.
    memset(&b, 0, sizeof b);
    b.stop_after = 1;
    T_EQ_INT(CB_STORE_CALL(S, item_batch_get, keys, N, NULL, 0, on_batch, &b), CB_OK);
    T_EQ_INT(b.calls, 1);
    memset(&b, 0, sizeof b);
    T_EQ_INT(CB_STORE_CALL(S, item_batch_get, keys, 0, NULL, 0, on_batch, &b), CB_OK);
    T_EQ_INT(b.calls, 0);
    free(sk);
    free(keys);
    free(w);
}

// Batch delete: past DynamoDB's 25 a request, absent keys among them.
static void batch_delete(void) {
    const char *pk = P("bd");
    enum { N = 60 };
    char sk[N + 2][12];   // (an int's worst case, so gcc's truncation check has nothing to say)
    cb_ikey keys[N + 2];
    cb_iwrite w[N];
    for (int i = 0; i < N + 2; i++) {
        snprintf(sk[i], sizeof sk[0], "%02d", i);
        keys[i] = (cb_ikey){ pk, sk[i] };
        if (i < N) w[i] = (cb_iwrite){ .op = CB_IW_PUT, .key = keys[i] };
    }
    T_EQ_INT(txn_ok(w, N), CB_OK);
    T_EQ_INT(put(pk, "keep", NULL, 0), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, item_batch_delete, keys, N + 2), CB_OK);
    T_EQ_STR(q_text(0, pk, NULL, NULL, true, NULL), "keep");
    T_EQ_INT(CB_STORE_CALL(S, item_batch_delete, keys, 0), CB_OK);
}

// The key walk: every item's keys (this run's among the store's own), a
// callback that stops, and one that calls the store.
typedef struct {
    int  calls, mine, stop_after, got, wanted;
    bool call_store;
    const char *want_pk;   // counted in wanted when handed out
} keys_walk;

static bool on_key(const char *pk, const char *sk, void *ud) {
    keys_walk *k = ud;
    k->calls++;
    if (k->want_pk && strcmp(pk, k->want_pk) == 0) k->wanted++;
    if (strncmp(pk, P("kw"), strlen(P("kw"))) == 0) k->mine++;
    if (k->call_store) {
        cb_item it;
        cb_item_init(&it);
        k->got += get(pk, sk, &it) == CB_OK;
        cb_item_free(&it);
    }
    return !(k->stop_after > 0 && k->calls >= k->stop_after);
}

static void item_keys_walk(void) {
    const char *pk = P("kw");
    for (int i = 0; i < 3; i++) {
        char sk[4];
        snprintf(sk, sizeof sk, "%d", i);
        T_EQ_INT(put(pk, sk, NULL, 0), CB_OK);
    }
    keys_walk k = { 0 };
    T_EQ_INT(CB_STORE_CALL(S, item_keys, on_key, &k), CB_OK);
    T_EQ_INT(k.mine, 3);
    T_CHECK(k.calls >= 3);
    int all = k.calls;
    memset(&k, 0, sizeof k);
    k.stop_after = 1;
    T_EQ_INT(CB_STORE_CALL(S, item_keys, on_key, &k), CB_OK);
    T_EQ_INT(k.calls, 1);
    memset(&k, 0, sizeof k);
    k.call_store = true;
    T_EQ_INT(CB_STORE_CALL(S, item_keys, on_key, &k), CB_OK);
    T_EQ_INT(k.got, k.calls);
    T_CHECK(k.calls == all);
}

// THE KEY LIMITS: CB_ITEM_KEY_MAX bytes for pk, sk and every index key,
// at the limit through every read, and past it (or empty) refused, CB_E_INVALID, nothing written.
static void key_limits(void) {
    char *pk = malloc(CB_ITEM_KEY_MAX + 2), *sk = malloc(CB_ITEM_KEY_MAX + 2);
    if (!pk || !sk) { T_CHECK(false); free(pk); free(sk); return; }
    int at = snprintf(pk, CB_ITEM_KEY_MAX + 2, "%s#long#", RUN);
    memset(pk + at, 'p', (size_t)(CB_ITEM_KEY_MAX - at));
    pk[CB_ITEM_KEY_MAX] = 0;
    memset(sk, 's', CB_ITEM_KEY_MAX);
    sk[CB_ITEM_KEY_MAX] = 0;
    const cb_av a[] = { CB_AVS("gsi1pk", P("longidx")), CB_AVS("gsi1sk", sk) };
    T_EQ_INT(put(pk, sk, a, 2), CB_OK);
    T_CHECK(present(pk, sk));
    char want[CB_ITEM_KEY_MAX + 1];
    snprintf(want, sizeof want, "%s", sk);
    T_EQ_STR(q_text(0, pk, NULL, NULL, true, NULL), want);
    T_EQ_STR(q_text(1, P("longidx"), NULL, NULL, true, want), want);
    batch_walk b = { 0 };
    cb_ikey key = { pk, sk };
    T_EQ_INT(CB_STORE_CALL(S, item_batch_get, &key, 1, NULL, 0, on_batch, &b), CB_OK);
    T_EQ_INT(b.calls, 1);
    keys_walk k = { .call_store = true, .want_pk = pk };
    T_EQ_INT(CB_STORE_CALL(S, item_keys, on_key, &k), CB_OK);
    T_EQ_INT(k.wanted, 1);
    T_EQ_INT(k.got, k.calls);   // the long key among them, read back by what the walk handed out
    T_EQ_INT(CB_STORE_CALL(S, item_batch_delete, &key, 1), CB_OK);
    T_CHECK(!present(pk, sk));
    // One byte past, in each key: refused, nothing written.
    strcat(sk, "s");
    T_EQ_INT(put(P("short"), sk, NULL, 0), CB_E_INVALID);
    cb_item it;
    cb_item_init(&it);
    T_EQ_INT(CB_STORE_CALL(S, item_get, (cb_ikey){ P("short"), sk }, true, &it), CB_E_INVALID);
    cb_item_free(&it);
    sk[CB_ITEM_KEY_MAX] = 0;
    strcat(pk, "p");
    T_EQ_INT(put(pk, "A", NULL, 0), CB_E_INVALID);
    cb_iwrite u = { .op = CB_IW_UPDATE, .key = { pk, "A" } };
    T_EQ_INT(write_once(&u, NULL), CB_E_INVALID);
    cb_iwrite t[2] = { { .op = CB_IW_PUT, .key = { P("short"), "ok" } }, { .op = CB_IW_DELETE, .key = { pk, "A" } } };
    T_EQ_INT(CB_STORE_CALL(S, item_txn, t, 2, NULL, NULL), CB_E_INVALID);
    T_CHECK(!present(P("short"), "ok"));
    walk qw = { 0 };
    cb_iquery lq = { .index = 0, .pk = pk, .forward = true };
    T_EQ_INT(query(&lq, &qw, NULL), CB_E_INVALID);
    lq.pk = P("short");
    lq.sk_prefix = pk;
    T_EQ_INT(query(&lq, &qw, NULL), CB_E_INVALID);
    lq.sk_prefix = NULL;
    lq.sk_below = pk;
    T_EQ_INT(query(&lq, &qw, NULL), CB_E_INVALID);
    lq.sk_below = NULL;
    lq.index = 3;
    T_EQ_INT(query(&lq, &qw, NULL), CB_E_INVALID);
    cb_ikey bad = { pk, "A" };
    T_EQ_INT(CB_STORE_CALL(S, item_batch_delete, &bad, 1), CB_E_INVALID);
    b.calls = 0;
    T_EQ_INT(CB_STORE_CALL(S, item_batch_get, &bad, 1, NULL, 0, on_batch, &b), CB_E_INVALID);
    const cb_av longidx[] = { CB_AVS("gsi2pk", pk), CB_AVS("gsi2sk", "x") };
    T_EQ_INT(put(P("short"), "idx", longidx, 2), CB_E_INVALID);
    T_CHECK(!present(P("short"), "idx"));
    const cb_iset longset[] = { { CB_AVS("gsi1sk", pk), CB_SET_TO } };
    T_EQ_INT(update(P("short"), "idx", longset, 1, NULL, 0), CB_E_INVALID);
    T_CHECK(!present(P("short"), "idx"));
    // Empty keys: DynamoDB refuses an empty key string.
    T_EQ_INT(put(P("short"), "", NULL, 0), CB_E_INVALID);
    T_EQ_INT(put("", "A", NULL, 0), CB_E_INVALID);
    const cb_av emptyidx[] = { CB_AVS("gsi1pk", ""), CB_AVS("gsi1sk", "x") };
    T_EQ_INT(put(P("short"), "eidx", emptyidx, 2), CB_E_INVALID);
    free(pk);
    free(sk);
}

// THE ITEM SIZE LIMIT: CB_ITEM_SIZE_MAX bytes as DynamoDB counts them (each
// attribute's name and value), at it accepted, one past refused; an update
// that would grow an item past it changes nothing; a transaction past
// CB_ITEM_TXN_SIZE_MAX in all is refused whole.
static void item_size_limits(void) {
    const char *pk = P("size");
    size_t fixed = 2 + strlen(pk) + 2 + 1 + 1;   // "pk", its value, "sk", "A", the attribute's name "b"
    size_t n = CB_ITEM_SIZE_MAX - fixed;
    uint8_t *big = calloc(1, CB_ITEM_SIZE_MAX + 1);
    if (!big) { T_CHECK(false); return; }
    for (size_t i = 0; i < CB_ITEM_SIZE_MAX; i++) big[i] = (uint8_t)(i * 31);
    cb_av at = CB_AVB("b", big, n), past = CB_AVB("b", big, n + 1);
    T_EQ_INT(put(pk, "A", &at, 1), CB_OK);
    T_CHECK(holds(pk, "A", &at));
    T_EQ_INT(put(pk, "B", &past, 1), CB_E_INVALID);
    T_CHECK(!present(pk, "B"));
    cb_av s_past = CB_AVS("b", NULL);
    char *text = malloc(n + 2);
    if (text) {
        memset(text, 'x', n + 1);
        text[n + 1] = 0;
        s_past.s = text;
        T_EQ_INT(put(pk, "B", &s_past, 1), CB_E_INVALID);
        T_CHECK(!present(pk, "B"));
    }
    // Growth past it by an update.
    cb_av half = CB_AVB("b", big, CB_ITEM_SIZE_MAX / 2);
    T_EQ_INT(put(pk, "C", &half, 1), CB_OK);
    const cb_iset grow[] = { { CB_AVB("c", big, CB_ITEM_SIZE_MAX / 2 + 16), CB_SET_TO } };
    T_CHECK(update(pk, "C", grow, 1, NULL, 0) != CB_OK);
    T_CHECK(lacks(pk, "C", "c"));
    // A transaction past its size in all, each write under the item limit.
    enum { K = CB_ITEM_TXN_SIZE_MAX / (CB_ITEM_SIZE_MAX - 1024) + 1 };
    cb_iwrite w[K];
    char sk[K][8];
    cb_av each = CB_AVB("b", big, CB_ITEM_SIZE_MAX - 1024);
    for (int i = 0; i < K; i++) {
        snprintf(sk[i], sizeof sk[0], "T%d", i);
        w[i] = (cb_iwrite){ .op = CB_IW_PUT, .key = { pk, sk[i] }, .attrs = &each, .n_attrs = 1 };
    }
    T_EQ_INT(CB_STORE_CALL(S, item_txn, w, K, NULL, NULL), CB_E_INVALID);
    T_CHECK(!present(pk, "T0"));
    free(text);
    free(big);
}

// THE ATTRIBUTE LIMITS: a name of CB_ITEM_NAME_MAX bytes and no more, at
// most CB_ITEM_ATTRS attributes an item, an update naming at most
// CB_ITEM_UPDATE_NAMES, no name twice in a write, and the keys never set as
// attributes. Each past refused, CB_E_INVALID, nothing written.
static void attribute_limits(void) {
    const char *pk = P("attrs");
    char name[CB_ITEM_NAME_MAX + 2];
    memset(name, 'n', CB_ITEM_NAME_MAX);
    name[CB_ITEM_NAME_MAX] = 0;
    cb_av v = CB_AVS(name, "ok");
    T_EQ_INT(put(pk, "A", &v, 1), CB_OK);
    T_CHECK(holds(pk, "A", &v));
    cb_iset st = { CB_AVN(name, 2), CB_SET_TO };
    cb_iwrite cw = { .op = CB_IW_UPDATE, .key = { pk, "A" }, .sets = &st, .n_sets = 1, .cond = { { CB_C_EQ, CB_AVS(name, "ok") } }, .n_cond = 1 };
    T_EQ_INT(write_ok(&cw, NULL), CB_OK);
    strcat(name, "n");
    T_EQ_INT(put(pk, "B", &v, 1), CB_E_INVALID);
    T_EQ_INT(update(pk, "B", &st, 1, NULL, 0), CB_E_INVALID);
    T_EQ_INT(write_once(&cw, NULL), CB_E_INVALID);
    T_CHECK(!present(pk, "B"));
    cb_av empty = CB_AVS("", "x");
    T_EQ_INT(put(pk, "B", &empty, 1), CB_E_INVALID);
    // As many attributes as an item holds (pk and sk among them), and one more.
    cb_av many[CB_ITEM_ATTRS];
    char names[CB_ITEM_ATTRS][8];
    for (int i = 0; i < CB_ITEM_ATTRS; i++) {
        snprintf(names[i], sizeof names[0], "a%d", i);
        many[i] = CB_AVN(names[i], i);
    }
    T_EQ_INT(put(pk, "C", many, CB_ITEM_ATTRS - 2), CB_OK);
    cb_item it;
    cb_item_init(&it);
    T_EQ_INT(get(pk, "C", &it), CB_OK);
    T_EQ_INT(it.n, CB_ITEM_ATTRS);
    cb_item_free(&it);
    T_EQ_INT(put(pk, "D", many, CB_ITEM_ATTRS - 1), CB_E_INVALID);
    T_CHECK(!present(pk, "D"));
    cb_iwrite ttl = { .op = CB_IW_PUT, .key = { pk, "D" }, .attrs = many, .n_attrs = CB_ITEM_ATTRS - 3, .ttl_ms = now_ms() + 60000 };
    T_EQ_INT(write_once(&ttl, NULL), CB_E_INVALID);   // the TTL's two attributes count
    // An update naming as many attributes as one may, and one more.
    cb_iset sets[CB_ITEM_UPDATE_NAMES + 1];
    for (int i = 0; i <= CB_ITEM_UPDATE_NAMES; i++) sets[i] = (cb_iset){ CB_AVN(names[i], -i), CB_SET_TO };
    T_EQ_INT(update(pk, "E", sets, CB_ITEM_UPDATE_NAMES, NULL, 0), CB_OK);
    T_EQ_INT(update(pk, "F", sets, CB_ITEM_UPDATE_NAMES + 1, NULL, 0), CB_E_INVALID);
    T_CHECK(!present(pk, "F"));
    cb_iwrite wn = { .op = CB_IW_UPDATE, .key = { pk, "F" }, .sets = sets, .n_sets = CB_ITEM_UPDATE_NAMES - 1,
                     .cond = { { CB_C_ATTR_EXISTS, CB_AVS("zz", "") }, { CB_C_EQ, CB_AVN("yy", 1) } }, .n_cond = 2 };
    T_EQ_INT(write_once(&wn, NULL), CB_E_INVALID);   // a condition's names count
    // A name twice in one write, and the keys as attributes.
    const cb_av twice[] = { CB_AVS("x", "1"), CB_AVS("x", "2") };
    T_EQ_INT(put(pk, "G", twice, 2), CB_E_INVALID);
    const cb_iset twice_set[] = { { CB_AVS("x", "1"), CB_SET_TO }, { CB_AVN("x", 1), CB_SET_INCR } };
    T_EQ_INT(update(pk, "G", twice_set, 2, NULL, 0), CB_E_INVALID);
    const cb_iset ttl_twice[] = { { CB_AVN(CB_ITEM_TTL_MS, 1), CB_SET_TO } };
    cb_iwrite tt = { .op = CB_IW_UPDATE, .key = { pk, "G" }, .sets = ttl_twice, .n_sets = 1, .ttl_ms = now_ms() + 1000 };
    T_EQ_INT(write_once(&tt, NULL), CB_E_INVALID);
    const cb_av key_attr[] = { CB_AVS("sk", "other") };
    T_EQ_INT(put(pk, "G", key_attr, 1), CB_E_INVALID);
    const cb_iset key_set[] = { { CB_AVS("pk", "other"), CB_SET_TO } };
    T_EQ_INT(update(pk, "G", key_set, 1, NULL, 0), CB_E_INVALID);
    T_CHECK(!present(pk, "G"));
}

// A clause comparing an attribute with the value the same update sets it to
// shares that value (DynamoDB's spelling: one ":<attribute>"); a clause with
// the same name and ANOTHER value keeps its own.
static void a_clause_on_the_set_attribute(void) {
    const char *pk = P("shared");
    const cb_av a[] = { CB_AVS("gsi2sk", "A") };
    T_EQ_INT(put(pk, "R", a, 1), CB_OK);
    const cb_iset st[] = { { CB_AVS("gsi2sk", "A"), CB_SET_TO }, { CB_AVN("mark", 1), CB_SET_TO } };
    // Same value: "<> A" on an item holding A refuses.
    cb_iwrite w = { .op = CB_IW_UPDATE, .key = { pk, "R" }, .sets = st, .n_sets = 2, .cond = { { CB_C_NE, CB_AVS("gsi2sk", "A") } }, .n_cond = 1 };
    T_EQ_INT(write_once(&w, NULL), CB_E_CONFLICT);
    T_CHECK(lacks(pk, "R", "mark"));
    // Another value: "<> C" on an item holding A holds.
    w.cond[0] = (cb_iclause){ CB_C_NE, CB_AVS("gsi2sk", "C") };
    T_EQ_INT(write_ok(&w, NULL), CB_OK);
    T_EQ_INT(num(pk, "R", "mark"), 1);
    // EQ with another value than the set one: "= A" holds on A while setting B.
    const cb_iset to_b[] = { { CB_AVS("gsi2sk", "B"), CB_SET_TO } };
    cb_iwrite e = { .op = CB_IW_UPDATE, .key = { pk, "R" }, .sets = to_b, .n_sets = 1, .cond = { { CB_C_EQ, CB_AVS("gsi2sk", "A") } }, .n_cond = 1 };
    T_EQ_INT(write_ok(&e, NULL), CB_OK);
    cb_av b = CB_AVS("gsi2sk", "B");
    T_CHECK(holds(pk, "R", &b));
    T_EQ_INT(write_once(&e, NULL), CB_E_CONFLICT);   // now B, not A
}

int cs_store_run(const cs_backend *b, int *checks) {
    static int runs;
    snprintf(RUN, sizeof RUN, "CS%dx%d", (int)getpid(), ++runs);
    S = b->open ? b->open(b->ud) : NULL;
    T_CHECK(S != NULL);
    if (S) {
        fprintf(stderr, "conformance/store: %s, partition prefix %s\n", b->name, RUN);
        T_RUN(every_kind_round_trips);
        T_RUN(put_replaces_and_delete_is_idempotent);
        T_RUN(update_and_each_set_kind);
        T_RUN(each_check_alone);
        T_RUN(four_clauses_all_must_hold);
        T_RUN(the_old_item_comes_back);
        T_RUN(ttl_and_expired_items);
        T_RUN(txn_whole_or_nothing);
        T_RUN(txn_limits);
        T_RUN(query_the_table);
        T_RUN(query_pages_stops_and_writes);
        T_RUN(query_the_indexes);
        T_RUN(batch_get);
        T_RUN(batch_delete);
        T_RUN(item_keys_walk);
        T_RUN(key_limits);
        T_RUN(item_size_limits);
        T_RUN(attribute_limits);
        T_RUN(a_clause_on_the_set_attribute);
        cb_store_close(S);
        S = NULL;
    }
    if (checks) *checks = t_checks;
    fprintf(stderr, "conformance/store: %s, %d cases, %d checks, %d failed\n", b->name, t_tests, t_checks, t_failed);
    return t_failed;
}
