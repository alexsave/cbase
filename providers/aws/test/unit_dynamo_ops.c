// unit_dynamo_ops.c - every store_dynamo.c operation unit_dynamo.c leaves to
// DynamoDB Local, over the same kind of scripted transport: the request each
// sends (its target and key), how it reads DynamoDB's answer into rows, and
// how each failure maps (a lost condition, a cancelled transaction, a
// ValidationException). The shapes are the DynamoDB API reference's, as in
// unit_dynamo.c; `make check-dynamo` runs the same store contract against
// DynamoDB Local itself. The platform's own rows are here (accounts, sessions,
// identities, the console's collections); a product's are its own tests.
#include "t.h"

#include "aws_json.h"
#include "store/store_priv.h"
#include "util/clock.h"
#include "util/json.h"
#include "util/log.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define U1 "aaaaaaaa-0000-4000-8000-000000000001"
#define U2 "aaaaaaaa-0000-4000-8000-000000000002"

typedef struct {
    const char *op;          // the target after "DynamoDB_20120810."
    const char *contains;    // the body must contain this (NULL: anything)
    int         status;
    const char *answer;
} step;

typedef struct {
    const step *steps;
    int         n, at;
    char        bodies[24][4096];
} script;

static const char VALIDATION[] =
    "{\"__type\":\"com.amazon.coral.validate#ValidationException\",\"message\":\"One or more parameter values were invalid\"}";
static const char COND_NO_ITEM[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#ConditionalCheckFailedException\",\"message\":\"The conditional request failed\"}";
static const char CANCEL_0[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
    "\"CancellationReasons\":[{\"Code\":\"ConditionalCheckFailed\"},{\"Code\":\"None\"},{\"Code\":\"None\"}]}";
static const char NOITEM[] = "{}";

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
    if (strcmp(dot ? dot + 1 : target, st->op) != 0) fprintf(stderr, "     step %d: %s, want %s\n", s->at, target, st->op);
    T_EQ_STR(dot ? dot + 1 : target, st->op);
    T_CHECK(cb_json_valid((const char *)b, len));
    int slot = s->at < 24 ? s->at : 23;
    snprintf(s->bodies[slot], sizeof s->bodies[0], "%.*s", (int)len, (const char *)b);
    if (st->contains && !strstr(s->bodies[slot], st->contains)) {
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

#define USER_ITEM(id, email) \
    "{\"pk\":{\"S\":\"USER#" id "\"},\"sk\":{\"S\":\"PROFILE\"},\"id\":{\"S\":\"" id "\"},\"email\":{\"S\":\"" email "\"}," \
    "\"password_hash\":{\"S\":\"$2b$10$x\"},\"username\":{\"S\":\"ANN\"},\"created_at\":{\"N\":\"5\"}}"

// ---- users -------------------------------------------------------------------------------------

static void users_by_email_password_touch(void) {
    static const step steps[] = {
        { "GetItem", "\"EMAIL#ann@x.io\"", 200, "{\"Item\":{\"pk\":{\"S\":\"EMAIL#ann@x.io\"},\"user_id\":{\"S\":\"" U1 "\"}}}" },
        { "GetItem", "\"USER#" U1 "\"", 200, "{\"Item\":" USER_ITEM(U1, "ann@x.io") "}" },
        { "GetItem", "EMAIL#nobody@x.io", 200, NOITEM },
        { "GetItem", "EMAIL#bad@x.io", 200, "{\"Item\":{\"pk\":{\"S\":\"EMAIL#bad@x.io\"}}}" },   // no user_id
        { "GetItem", NULL, 400, VALIDATION },
        { "UpdateItem", "password_hash", 200, "{}" },
        { "UpdateItem", "password_hash", 400, COND_NO_ITEM },
        { "UpdateItem", "last_sign_in_at", 200, "{}" },
        { "UpdateItem", "last_sign_in_at", 400, COND_NO_ITEM },
        { "UpdateItem", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    cb_user_row u;
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "ann@x.io", &u), CB_OK);
    T_EQ_STR(u.id, U1);
    T_EQ_STR(u.username, "ANN");
    T_EQ_INT(u.created_at, 5);
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "nobody@x.io", &u), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "bad@x.io", &u), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "err@x.io", &u), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, user_set_password, U1, "$2b$10$y", ""), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_set_password, U2, "$2b$10$y", ""), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, user_touch_sign_in, U1, 77), CB_OK);
    T_CHECK(strstr(sc_.bodies[7], "\"N\":\"77\"") != NULL);
    T_EQ_INT(CB_STORE_CALL(s, user_touch_sign_in, U2, 77), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, user_touch_sign_in, U2, 77), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

typedef struct { int n; char ids[8][40]; int stop_after; } users_seen;
static bool see_user(const cb_user_row *u, void *ud) {
    users_seen *w = ud;
    if (w->n < 8) snprintf(w->ids[w->n], 40, "%s", u->id);
    w->n++;
    return w->stop_after <= 0 || w->n < w->stop_after;
}

// The console's list: every account, newest first, the search on email or
// name, an offset, a limit, and a row that does not read as an account skipped.
static void users_list_pages(void) {
    static const char P1[] = "{\"Items\":[" USER_ITEM(U1, "ann@x.io") ",{\"pk\":{\"S\":\"USER#broken\"}}],"
                             "\"LastEvaluatedKey\":{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"PROFILE\"}}}";
    static const char P2[] = "{\"Items\":[" USER_ITEM(U2, "bob@y.io") "]}";
    static const step steps[] = {
        { "Query", "\"ScanIndexForward\":false", 200, P1 },
        { "Query", "ExclusiveStartKey", 200, P2 },
        { "Query", NULL, 200, P1 },
        { "Query", NULL, 200, P2 },
        { "Query", NULL, 200, P1 },   // offset 1: U1 skipped, the page after it read
        { "Query", NULL, 200, P2 },
        { "Query", NULL, 200, P1 },   // a callback that stops: no second page
        { "Query", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    users_seen w = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, users_list, "", 0, 0, see_user, &w), 2);
    T_EQ_STR(w.ids[0], U1);
    T_EQ_STR(w.ids[1], U2);
    memset(&w, 0, sizeof w);
    T_EQ_INT(CB_STORE_CALL(s, users_list, "BOB", 0, 10, see_user, &w), 1);   // the search, case-blind
    T_EQ_STR(w.ids[0], U2);
    memset(&w, 0, sizeof w);
    T_EQ_INT(CB_STORE_CALL(s, users_list, "", 1, 1, see_user, &w), 1);       // offset 1, limit 1
    T_EQ_STR(w.ids[0], U2);
    memset(&w, 0, sizeof w);
    w.stop_after = 1;
    T_EQ_INT(CB_STORE_CALL(s, users_list, "", 0, 0, see_user, &w), 1);
    T_EQ_INT(CB_STORE_CALL(s, users_list, "", 0, 0, see_user, &w), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

// ---- refresh tokens -----------------------------------------------------------------------------

// A token row carries its TTL (store.h cb_refresh_row, finding M2), and a
// session ends in one write to its own item (M3): nothing is listed.
static void refresh_put_and_revoke_session(void) {
    static const step steps[] = {
        { "PutItem", "\"expires_at\":{\"N\":\"5\"}", 200, "{}" },
        { "PutItem", "REFRESH#h1", 400, COND_NO_ITEM },   // the hash is taken
        { "UpdateItem", "SESSION#s1", 200, "{}" },
        { "UpdateItem", "SESSION#s2", 400, VALIDATION },
    };
    RUN(steps, s);
    cb_refresh_row r = { .token_hash = "h1", .user_id = U1, .session_id = "s1", .created_at = 9, .expires_at = 5000 };
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &r), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &r), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, session_revoke, "s1", U1, 9000), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, session_revoke, "s2", U1, 9000), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

// ---- identities and one-time claims (store.h cb_identity_row, once_claim) ------------------------

#define IDENT_ITEM(sub, user) \
    "{\"pk\":{\"S\":\"IDENT#1#" sub "\"},\"sk\":{\"S\":\"IDENT\"},\"idp\":{\"N\":\"1\"},\"subject\":{\"S\":\"" sub "\"}," \
    "\"user_id\":{\"S\":\"" user "\"},\"client_id\":{\"S\":\"com.example.web\"},\"email\":{\"S\":\"r@privaterelay.appleid.com\"}," \
    "\"email_verified\":{\"BOOL\":true},\"private_email\":{\"BOOL\":true},\"grant\":{\"S\":\"00ff\"}," \
    "\"created_at\":{\"N\":\"5\"},\"updated_at\":{\"N\":\"6\"}}"

static bool count_ident(const cb_identity_row *r, void *ud) {
    T_EQ_INT(r->idp, 1);
    (*(int *)ud)++;
    return true;
}

static void identities_and_claims(void) {
    static const step steps[] = {
        // get: the item, then none, then an item that is not an identity
        { "GetItem", "\"IDENT#1#s1\"", 200, "{\"Item\":" IDENT_ITEM("s1", U1) "}" },
        { "GetItem", "\"IDENT#1#s2\"", 200, NOITEM },
        { "GetItem", "\"IDENT#1#s3\"", 200, "{\"Item\":{\"pk\":{\"S\":\"IDENT#1#s3\"}}}" },
        // put: fresh (absent, or refused), and a replace with no condition
        { "PutItem", "\"gsi1pk\":{\"S\":\"USERIDENT#" U1 "\"}", 200, "{}" },
        { "PutItem", "attribute_not_exists", 400, COND_NO_ITEM },
        { "PutItem", "\"grant\":{\"S\":\"\"}", 200, "{}" },
        // of: GSI1 by the user
        { "Query", "USERIDENT#" U1, 200, "{\"Items\":[" IDENT_ITEM("s1", U1) "," IDENT_ITEM("s2", U1) ",{\"pk\":{\"S\":\"x\"}}]}" },
        // the account and its identity in one transaction; each refusal of the three is taken
        { "TransactWriteItems", "IDENT#1#s4", 200, "{}" },
        { "TransactWriteItems", NULL, 400, CANCEL_0 },
        { "TransactWriteItems", NULL, 400, "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
                                             "\"CancellationReasons\":[{\"Code\":\"None\"},{\"Code\":\"None\"},{\"Code\":\"ConditionalCheckFailed\"}]}" },
        // a claim: taken, then refused while it stands
        { "PutItem", "ONCE#" "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 200, "{}" },
        { "PutItem", "attribute_not_exists", 400, COND_NO_ITEM },
    };
    RUN(steps, s);
    cb_identity_row r;
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "s1", &r), CB_OK);
    T_EQ_STR(r.user_id, U1);
    T_EQ_STR(r.client_id, "com.example.web");
    T_CHECK(r.email_verified && r.private_email);
    T_EQ_STR(r.grant, "00ff");
    T_EQ_INT(r.updated_at, 6);
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "s2", &r), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "s3", &r), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "", &r), CB_E_NOT_FOUND);   // asks nothing
    cb_identity_row w = { .idp = 1, .subject = "s1", .user_id = U1, .client_id = "com.example.web", .grant = "00ff" };
    T_EQ_INT(CB_STORE_CALL(s, identity_put, &w, true), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, identity_put, &w, true), CB_E_CONFLICT);
    w.grant[0] = 0;
    T_EQ_INT(CB_STORE_CALL(s, identity_put, &w, false), CB_OK);
    T_CHECK(strstr(sc_.bodies[5], "attribute_not_exists") == NULL);   // a replace holds no condition
    cb_identity_row bad = { .idp = 1, .subject = "", .user_id = U1 };
    T_EQ_INT(CB_STORE_CALL(s, identity_put, &bad, true), CB_E_INVALID);   // asks nothing
    int n = 0;
    T_EQ_INT(CB_STORE_CALL(s, identities_of, U1, count_ident, &n), 2);
    T_EQ_INT(n, 2);
    T_EQ_INT(CB_STORE_CALL(s, identities_of, "x", count_ident, &n), CB_E_INVALID);
    cb_user_row u = { .id = U2, .email = "d@x.io", .username = "D", .created_at = 1 };
    cb_identity_row i4 = { .idp = 1, .subject = "s4", .user_id = U2 };
    T_EQ_INT(CB_STORE_CALL(s, user_create_identity, &u, &i4), CB_OK);
    int conds = 0;   // each of the three writes must be absent: the email, the profile, the identity
    for (const char *p = sc_.bodies[7]; (p = strstr(p, "attribute_not_exists")) != NULL; p++) conds++;
    T_EQ_INT(conds, 3);
    T_EQ_INT(CB_STORE_CALL(s, user_create_identity, &u, &i4), CB_E_CONFLICT);   // the email
    T_EQ_INT(CB_STORE_CALL(s, user_create_identity, &u, &i4), CB_E_CONFLICT);   // the identity
    i4.user_id[0] = 0;
    T_EQ_INT(CB_STORE_CALL(s, user_create_identity, &u, &i4), CB_E_INVALID);    // asks nothing
    const char *k = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    T_EQ_INT(CB_STORE_CALL(s, once_claim, k, 9000, 1000), CB_OK);
    T_CHECK(strstr(sc_.bodies[10], "expires_ms") != NULL);   // the condition reads the claim's end
    T_EQ_INT(CB_STORE_CALL(s, once_claim, k, 9000, 1000), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, once_claim, "short", 9000, 1000), CB_E_INVALID);   // asks nothing
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

// ---- games ---------------------------------------------------------------------------------------


// A game's human members (a chat's audience): one consistent Query of the
// game's seat items, the bots left out; fn may stop it; a failed read is the
// store's error.
typedef struct { int n; char ids[4][40]; int stop_after; } ids_seen;

typedef struct { int n; char names[8][80]; } names_seen;

// ---- admin and the import -------------------------------------------------------------------------

static bool see_name(const char *name, void *ud) {
    names_seen *w = ud;
    if (w->n < 8) snprintf(w->names[w->n], sizeof w->names[0], "%s", name);
    w->n++;
    return w->n < 3;
}

static void admin_collections_are_the_families(void) {
    static const step none[] = { { "GetItem", NULL, 200, NOITEM } };
    script sc_ = { .steps = none, .n = 0 };
    cb_store *s = NULL;
    T_EQ_INT(cb_store_open_dynamo_with("cbase", fake_send, &sc_, &s), CB_OK);
    names_seen w = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, admin_collections, see_name, &w), 3);   // stopped after three
    T_EQ_STR(w.names[0], "USER");
    T_EQ_STR(w.names[1], "EMAIL");
    T_EQ_STR(w.names[2], "REFRESH");
    T_CHECK(dw_collection(-1) == NULL);
    T_CHECK(dw_collection(8) == NULL);
    T_EQ_STR(dw_collection(7), "SCAN");
    T_EQ_INT(sc_.at, 0);
    cb_store_close(s);
}

int main(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    T_RUN(users_by_email_password_touch);
    T_RUN(users_list_pages);
    T_RUN(refresh_put_and_revoke_session);
    T_RUN(identities_and_claims);
    T_RUN(admin_collections_are_the_families);
    return T_DONE();
}
