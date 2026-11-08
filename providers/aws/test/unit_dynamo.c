// unit_dynamo.c - the DynamoDB backend without a network.
//
// Three layers, each against bodies written down here:
//   * the wire's requests, byte for byte (the CAS, the lease, a signup, the
//     query shapes), and every builder's output is valid JSON;
//   * the wire's readers over DynamoDB's answers (items with Binary, Number
//     and BOOL attributes, the error shapes, cancellation reasons, pages);
//   * store_dynamo.c's decisions over a scripted transport: which request
//     follows which answer (a lost CAS, a rating that moved under a commit, a
//     refused lease, a throttle, a paged scan).
// The answers follow the DynamoDB API reference's documented shapes; no body
// here was captured from a live table. A product's own rows over this backend
// (its commits, leases, scans and batched reads) are the product's tests.
#include "t.h"
#include "lib_tree.h"

#include "aws_json.h"
#include "store/store_priv.h"
#include "util/clock.h"
#include "util/json.h"

#include <stdlib.h>

#define TOK "11111111-2222-4333-8444-555555555555"
#define U1 "aaaaaaaa-0000-4000-8000-000000000001"
#define U2 "aaaaaaaa-0000-4000-8000-000000000002"
#define B1 "bbbbbbbb-0000-4000-8000-000000000001"
#define U3 "aaaaaaaa-0000-4000-8000-000000000003"


// ---- requests, byte for byte -----------------------------------------------------------------

// ---- readers ---------------------------------------------------------------------------------

// What a Put writes, a read gives back: every row type through its item.
// A DynamoDB number attribute is digits: strtoll took " 5", "+5" and "5 ".
static void numbers_are_digits(void) {
    static const struct { const char *item; bool ok; int64_t v; } cases[] = {
        { "{\"v\":{\"N\":\"5\"}}", true, 5 },
        { "{\"v\":{\"N\":\"-9223372036854775808\"}}", true, INT64_MIN },
        { "{\"v\":{\"N\":\" 5\"}}", false, 0 },
        { "{\"v\":{\"N\":\"+5\"}}", false, 0 },
        { "{\"v\":{\"N\":\"5 \"}}", false, 0 },
        { "{\"v\":{\"N\":\"9223372036854775808\"}}", false, 0 },
        { "{\"v\":{\"N\":\"\"}}", false, 0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int64_t v = 0;
        T_EQ_INT(dw_get_n(cb_span_of(cases[i].item, strlen(cases[i].item)), "v", &v), cases[i].ok);
        if (cases[i].ok) T_EQ_INT(v, cases[i].v);
    }
}

static const char VALIDATION[] =
    "{\"__type\":\"com.amazon.coral.validate#ValidationException\",\"message\":\"One or more parameter values were invalid\"}";

// ---- store_dynamo.c over a scripted transport ---------------------------------------------------

typedef struct {
    const char *op;          // the target after "DynamoDB_20120810."
    const char *contains;    // the body must contain this (NULL: anything)
    int         status;
    const char *answer;
} step;

typedef struct {
    const step *steps;
    int         n, at;
    char        bodies[16][4096];
} script;

static int fake_send(void *ud, const char *target, const uint8_t *b, size_t len, int *status, cb_buf *resp) {
    script *s = ud;
    if (s->at >= s->n) {
        T_CHECK(!"a request past the end of the script");
        fprintf(stderr, "     unexpected %s %.*s\n", target, (int)(len > 300 ? 300 : len), (const char *)b);
        *status = 400;
        cb_buf_puts(resp, VALIDATION);
        return CB_OK;
    }
    const step *st = &s->steps[s->at];
    const char *dot = strchr(target, '.');
    T_EQ_STR(dot ? dot + 1 : target, st->op);
    T_CHECK(cb_json_valid((const char *)b, len));
    if (s->at < 16) snprintf(s->bodies[s->at], sizeof s->bodies[0], "%.*s", (int)len, (const char *)b);
    if (st->contains && !strstr(s->bodies[s->at < 16 ? s->at : 15], st->contains)) {
        T_CHECK(!"the request lacks what the script expects");
        fprintf(stderr, "     step %d wants %s in %.*s\n", s->at, st->contains, (int)(len > 400 ? 400 : len), (const char *)b);
    }
    *status = st->status;
    cb_buf_puts(resp, st->answer);
    s->at++;
    return CB_OK;
}

#define RUN(script_steps, store_var)                                                                     \
    script sc_ = { .steps = (script_steps), .n = (int)(sizeof(script_steps) / sizeof((script_steps)[0])) }; \
    cb_store *store_var = NULL;                                                                          \
    T_EQ_INT(cb_store_open_dynamo_with("cbase", fake_send, &sc_, &store_var), CB_OK)



static const char TXN_THROTTLED[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
    "\"CancellationReasons\":[{\"Code\":\"ThrottlingError\",\"Message\":\"Throughput exceeds the current "
    "capacity of your table or index. DynamoDB is automatically scaling your table or index so please try again "
    "shortly.\"},{\"Code\":\"None\"}],\"Message\":\"Transaction cancelled, please refer cancellation reasons for "
    "specific reasons [ThrottlingError, None]\"}";
static const char TXN_THROTTLED_PT[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
    "\"CancellationReasons\":[{\"Code\":\"None\"},{\"Code\":\"ProvisionedThroughputExceeded\",\"Message\":"
    "\"The level of configured provisioned throughput for the table was exceeded.\"}],"
    "\"Message\":\"Transaction cancelled [None, ProvisionedThroughputExceeded]\"}";

static uint64_t throttled_since(const cb_store_throttles *t0) {
    cb_store_throttles t;
    cb_store_throttles_get(&t);
    return t.throttled - t0->throttled;
}

// A transaction cancelled for throttling (either code) is sent again, not
// read as a refused condition: a sign-up goes through on the third send.
static void flow_transaction_throttled(void) {
    static const step steps[] = {
        { "TransactWriteItems", NULL, 400, TXN_THROTTLED },
        { "TransactWriteItems", NULL, 400, TXN_THROTTLED_PT },
        { "TransactWriteItems", NULL, 200, "{}" },
    };
    RUN(steps, s);
    cb_store_throttles t0;
    cb_store_throttles_get(&t0);
    cb_user_row u = { .id = U1, .email = "a@x.io", .username = "A", .created_at = 1 };
    T_EQ_INT(CB_STORE_CALL(s, user_create, &u), CB_OK);
    T_EQ_INT(sc_.at, 3);
    T_EQ_INT(throttled_since(&t0), 2);
    cb_store_close(s);
}

#define GB_N 130
typedef struct {
    int  calls, keys[8];
    char asked[8][GB_N][8];
} games_srv;


// Failed sign-ins (store.h signin_fails) over the scripted transport: the
// AUTHFAIL item read consistently, replaced with its TTL in seconds, deleted;
// an item past its time (TTL deletion lags) reads as none.
static void flow_signin_fails(void) {
    static const char LIVE[] =
        "{\"Item\":{\"pk\":{\"S\":\"AUTHFAIL#a@x.io\"},\"sk\":{\"S\":\"FAILS\"},\"n\":{\"N\":\"6\"},"
        "\"last_at\":{\"N\":\"900\"},\"locked_until\":{\"N\":\"30900\"},\"expires_ms\":{\"N\":\"3600900\"}}}";
    static const step steps[] = {
        { "GetItem", "\"AUTHFAIL#a@x.io\"", 200, LIVE },
        { "GetItem", "\"ConsistentRead\":true", 200, LIVE },          // read again past its time
        { "GetItem", NULL, 200, "{}" },                                 // none
        { "PutItem", "\"expires_at\":{\"N\":\"3601\"}", 200, "{}" },
        { "PutItem", NULL, 400, VALIDATION },
        { "DeleteItem", "\"AUTHFAIL#a@x.io\"", 200, "{}" },
    };
    RUN(steps, s);
    cb_signin_fails f;
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_get, "a@x.io", 1000, &f), CB_OK);
    T_CHECK(f.n == 6 && f.last_at == 900 && f.locked_until == 30900);
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_get, "a@x.io", 3600900, &f), CB_E_NOT_FOUND);
    T_EQ_INT(f.n, 0);
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_get, "a@x.io", 1000, &f), CB_E_NOT_FOUND);
    cb_signin_fails w = { .n = 1, .last_at = 1000, .locked_until = 0 };
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_put, "a@x.io", &w, 1000), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_put, "a@x.io", &w, 1000), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_clear, "a@x.io"), CB_OK);
    T_EQ_INT(sc_.at, 6);
    cb_store_close(s);
}

// Sessions (store.h cb_session_row) over the scripted transport: a put with
// its generation and TTL, a consistent read, a read past the session's end
// (TTL deletion lags), none, a revoke that keeps what it finds, a failure.
static void flow_sessions(void) {
    static const char LIVE[] =
        "{\"Item\":{\"pk\":{\"S\":\"SESSION#s1\"},\"sk\":{\"S\":\"SESSION\"},\"id\":{\"S\":\"s1\"},"
        "\"user_id\":{\"S\":\"" U1 "\"},\"created_at\":{\"N\":\"10\"},\"gen\":{\"N\":\"2\"},"
        "\"revoked\":{\"BOOL\":true},\"expires_ms\":{\"N\":\"5000\"}}}";
    static const step steps[] = {
        { "PutItem", "\"expires_at\":{\"N\":\"5\"}", 200, "{}" },
        { "GetItem", "\"ConsistentRead\":true", 200, LIVE },
        { "GetItem", NULL, 200, LIVE },
        { "GetItem", NULL, 200, "{}" },
        { "UpdateItem", "if_not_exists(#expires_ms, :expires_ms)", 200, "{}" },
        { "PutItem", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    cb_session_row r = { .id = "s1", .user_id = U1, .created_at = 10, .expires_at = 5000, .gen = 2 }, got;
    T_EQ_INT(CB_STORE_CALL(s, session_put, &r), CB_OK);
    T_CHECK(strstr(sc_.bodies[0], "\"gen\":{\"N\":\"2\"}") != NULL);
    T_EQ_INT(CB_STORE_CALL(s, session_get, "s1", 1000, &got), CB_OK);
    T_CHECK(got.revoked && got.gen == 2 && got.created_at == 10 && got.expires_at == 5000);
    T_EQ_STR(got.user_id, U1);
    T_EQ_INT(CB_STORE_CALL(s, session_get, "s1", 5000, &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, session_get, "s9", 1000, &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, session_revoke, "s1", U1, 9000), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, session_put, &r), CB_E_DB);
    T_EQ_INT(sc_.at, 6);
    cb_store_close(s);
}

typedef struct { int n; char ids[8][16]; } ids_t;

typedef struct { int n; char msg[8][16]; } msgs_t;

static void flow_admin_query_guard(void) {
    static const step steps[] = {
        { "ExecuteStatement", "SELECT", 200, "{\"Items\":[{\"pk\":{\"S\":\"BOT#x\"}}]}" },
        // CB_ADMIN_GAMES is the GAME family on this backend.
        { "Scan", "{\"S\":\"GAME#ab\"}", 200, "{\"Items\":[{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"},\"id\":{\"S\":\"ab12cd\"}}]}" },
    };
    RUN(steps, s);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "DELETE FROM \"cbase\" WHERE pk='x'", false, NULL, NULL), CB_E_FORBIDDEN);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELECT * FROM \"cbase\"", false, NULL, NULL), 1);
    char next[64];
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, CB_ADMIN_GAMES, "ab", NULL, 10, NULL, NULL, next, sizeof next), 1);
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "nosuch", NULL, NULL, 10, NULL, NULL, next, sizeof next), CB_E_INVALID);
    T_EQ_INT(sc_.at, 2);
    cb_store_close(s);
}

// providers/aws/ops/cbase.yaml creates the table this wire writes to: the same index names,
// all four index keys String (gsi1sk is "<ms>#<id>" text, not a Number),
// both projecting ALL, and the TTL on the attribute the wire writes seconds to.
static void the_template_matches_the_wire(void) {
    char yaml[TT_PATH_MAX];
    FILE *f = fopen(tt_path(yaml, sizeof yaml, "providers/aws/ops/cbase.yaml"), "r");
    T_CHECK(f != NULL);
    if (!f) return;
    static char y[64 * 1024];
    size_t n = fread(y, 1, sizeof y - 1, f);
    fclose(f);
    y[n] = 0;
    const char *want[] = {
        "- IndexName: " DW_GSI1 "\n",
        "- IndexName: " DW_GSI2 "\n",
        "{ AttributeName: pk, AttributeType: S }",
        "{ AttributeName: sk, AttributeType: S }",
        "{ AttributeName: gsi1pk, AttributeType: S }",
        "{ AttributeName: gsi1sk, AttributeType: S }",
        "{ AttributeName: gsi2pk, AttributeType: S }",
        "{ AttributeName: gsi2sk, AttributeType: S }",
        "AttributeName: " DW_TTL_ATTR "\n        Enabled: true",
        "Input: '\"scan\"'",
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        if (!strstr(y, want[i])) printf("  template lacks: %s\n", want[i]);
        T_CHECK(strstr(y, want[i]) != NULL);
    }
    int all = 0;
    for (const char *q = y; (q = strstr(q, "ProjectionType: ALL")) != NULL; q++) all++;
    T_EQ_INT(all, 2);
}



// DescribeTable as the live table answered it (aws dynamodb describe-table on
// cbase, 2026-10-14, cut to the fields read): 15/15 on the table, 5/5 on each
// GSI: reads (of the table only) go at 15 a second, writes (copied into each GSI) at 5.
static const char DESCRIBE_LIVE[] =
    "{\"Table\":{\"TableName\":\"cbase\",\"TableStatus\":\"ACTIVE\",\"ProvisionedThroughput\":{\"NumberOfDecreasesToday\":0,"
    "\"ReadCapacityUnits\":15,\"WriteCapacityUnits\":15},\"GlobalSecondaryIndexes\":[{\"IndexName\":\"gsi1\","
    "\"IndexStatus\":\"ACTIVE\",\"ProvisionedThroughput\":{\"NumberOfDecreasesToday\":0,\"ReadCapacityUnits\":5,"
    "\"WriteCapacityUnits\":5}},{\"IndexName\":\"gsi2\",\"IndexStatus\":\"ACTIVE\",\"ProvisionedThroughput\":"
    "{\"NumberOfDecreasesToday\":0,\"ReadCapacityUnits\":5,\"WriteCapacityUnits\":5}}]}}";
static const char DESCRIBE_ON_DEMAND[] =
    "{\"Table\":{\"TableName\":\"cbase\",\"ProvisionedThroughput\":{\"NumberOfDecreasesToday\":0,"
    "\"ReadCapacityUnits\":0,\"WriteCapacityUnits\":0},\"BillingModeSummary\":{\"BillingMode\":\"PAY_PER_REQUEST\"}}}";

static void provisioned_read(void) {
    int64_t r, w;
    T_CHECK(dw_provisioned(DESCRIBE_LIVE, strlen(DESCRIBE_LIVE), &r, &w));
    T_EQ_INT(r, 15);
    T_EQ_INT(w, 5);
    T_CHECK(dw_provisioned(DESCRIBE_ON_DEMAND, strlen(DESCRIBE_ON_DEMAND), &r, &w));
    T_EQ_INT(r, 0);
    T_EQ_INT(w, 0);
    T_CHECK(!dw_provisioned("{}", 2, &r, &w));
}

// ConsumedCapacity comes back as an object (one table) or, for the batch and
// transaction calls, a one-entry array; capacity is fractional for an
// eventually consistent read.
static void consumed_capacity_read(void) {
    static const char OBJ[] = "{\"Item\":{},\"ConsumedCapacity\":{\"TableName\":\"cbase\",\"CapacityUnits\":0.5}}";
    static const char ARR[] = "{\"ConsumedCapacity\":[{\"TableName\":\"cbase\",\"CapacityUnits\":18.0}]}";
    static const char NONE[] = "{\"Item\":{}}";
    T_CHECK(dw_consumed(OBJ, strlen(OBJ)) == 0.5);
    T_CHECK(dw_consumed(ARR, strlen(ARR)) == 18.0);
    T_CHECK(dw_consumed(NONE, strlen(NONE)) == 0);
}

// ---- the one-pass decode --------------------------------------------------------------------
//
// core_json.c or aws_json.c crosses a string with memchr, reads an item through a
// one-pass index and a response through its top level. Each case is one the
// old byte-at-a-time walk answered, and fz_dynamo.c checks the rest against it.

static cb_span lit_span(const char *s) { return cb_span_of(s, strlen(s)); }

// A quote closes a string unless an odd run of backslashes ends right before it.
static void strings_skipped_by_parity(void) {
    // "a\"b" (one backslash: escaped), "c\\" (two: the second escapes nothing,
    // the quote closes), "d\\\"e" (three: escaped), then the next key.
    const char *obj = "{\"k1\":\"a\\\"b\",\"k2\":\"c\\\\\",\"k3\":\"d\\\\\\\"e\",\"k4\":1}";
    cb_span o = lit_span(obj), k, v;
    size_t pos = 0;
    const char *want_v[] = { "\"a\\\"b\"", "\"c\\\\\"", "\"d\\\\\\\"e\"", "1" };
    for (int i = 0; i < 4; i++) {
        T_CHECK(dw_object_next(o, &pos, &k, &v));
        T_EQ_INT(v.n, strlen(want_v[i]));
        T_CHECK(v.n == strlen(want_v[i]) && memcmp(v.p, want_v[i], v.n) == 0);
    }
    T_CHECK(!dw_object_next(o, &pos, &k, &v));
    // An unclosed string, and one whose only quote is escaped, end the walk.
    pos = 0;
    T_CHECK(!dw_object_next(lit_span("{\"k\":\"abc"), &pos, &k, &v));
    pos = 0;
    T_CHECK(!dw_object_next(lit_span("{\"k\":\"abc\\\"}"), &pos, &k, &v));
    // A backslash run that starts the string counts from the string's first byte.
    pos = 0;
    T_CHECK(dw_object_next(lit_span("{\"k\":\"\\\\\"}"), &pos, &k, &v));
    T_EQ_INT(v.n, 4);
}

// The response readers find a top-level member whatever comes first, with a
// key spelled with an escape too, and never a member of a member.
static void response_top_level(void) {
    cb_span it;
    const char *after = "{\"ConsumedCapacity\":{\"CapacityUnits\":1.5},\"Item\":{\"a\":{\"S\":\"b\"}}}";
    T_CHECK(dw_item(after, strlen(after), &it));
    T_CHECK(it.n == 15 && memcmp(it.p, "{\"a\":{\"S\":\"b\"}}", 15) == 0);
    T_CHECK(dw_consumed(after, strlen(after)) == 1.5);
    const char *escaped = "{\"\\u0049tem\":{\"a\":{\"S\":\"b\"}}}";
    T_CHECK(dw_item(escaped, strlen(escaped), &it));
    const char *nested = "{\"Other\":{\"Item\":{}}}";
    T_CHECK(!dw_item(nested, strlen(nested), &it));
    const char *notobj = "{\"Item\":[1]}";
    T_CHECK(!dw_item(notobj, strlen(notobj), &it));
    // ConsumedCapacity: an object, a one-entry array, an object keyed "0" (the
    // second path cb_json_find was asked), a units value that is not a number.
    const char *zero = "{\"ConsumedCapacity\":{\"0\":{\"CapacityUnits\":3}}}";
    T_CHECK(dw_consumed(zero, strlen(zero)) == 3.0);
    const char *str = "{\"ConsumedCapacity\":{\"CapacityUnits\":\"4\"}}";
    T_CHECK(dw_consumed(str, strlen(str)) == 0);
    const char *str0 = "{\"ConsumedCapacity\":{\"CapacityUnits\":\"4\",\"0\":{\"CapacityUnits\":3}}}";
    T_CHECK(dw_consumed(str0, strlen(str0)) == 3.0);
    const char *arr0 = "{\"ConsumedCapacity\":[]}";
    T_CHECK(dw_consumed(arr0, strlen(arr0)) == 0);
    const char *page = "{\"Items\":[],\"LastEvaluatedKey\":{\"pk\":{\"S\":\"x\"}}}";
    cb_span last = dw_last_key(page, strlen(page));
    T_CHECK(last.n == 16 && memcmp(last.p, "{\"pk\":{\"S\":\"x\"}}", 16) == 0);
    cb_span none = dw_items("{\"Items\":{}}", 12);   // not an array: the empty one
    T_CHECK(none.n == 2 && memcmp(none.p, "[]", 2) == 0);
    T_CHECK(dw_unprocessed("{\"UnprocessedKeys\":{\"t\":{}}}", 28));
    // One scan serves every reader; past DW_TOP_MAX members the body is walked.
    dw_top t;
    const char *both = "{\"Items\":[1],\"LastEvaluatedKey\":{\"k\":1},\"ConsumedCapacity\":{\"CapacityUnits\":2}}";
    dw_top_scan(both, strlen(both), &t);
    T_EQ_INT(t.n, 3);
    T_EQ_INT(dw_top_items(&t).n, 3);
    T_EQ_INT(dw_top_last_key(&t).n, 7);
    T_CHECK(dw_top_consumed(&t) == 2.0);
    T_CHECK(!dw_top_item(&t, &it));
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_puts(&b, "{");
    for (int i = 0; i < DW_TOP_MAX + 4; i++) cb_buf_printf(&b, "\"m%d\":%d,", i, i);
    cb_buf_puts(&b, "\"Item\":{\"a\":{\"N\":\"1\"}},\"ConsumedCapacity\":{\"CapacityUnits\":0.5}}");
    dw_top_scan((const char *)b.p, b.len, &t);
    T_CHECK(t.full);
    T_CHECK(dw_top_item(&t, &it) && it.n == 15);
    T_CHECK(dw_top_consumed(&t) == 0.5);
    cb_buf_free(&b);
    T_CHECK(!dw_unprocessed("{\"UnprocessedItems\":{}}", 23));
}

int main(void) {
    T_RUN(the_template_matches_the_wire);
    T_RUN(numbers_are_digits);
    T_RUN(flow_signin_fails);
    T_RUN(flow_sessions);
    T_RUN(flow_transaction_throttled);
    T_RUN(flow_admin_query_guard);
    T_RUN(provisioned_read);
    T_RUN(consumed_capacity_read);
    T_RUN(strings_skipped_by_parity);
    T_RUN(response_top_level);
    return T_DONE();
}
