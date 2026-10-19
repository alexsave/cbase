// unit_dynamo_ops.c - every store_dynamo.c operation unit_dynamo.c leaves to
// DynamoDB Local, over the same kind of scripted transport: the request each
// sends (its target and key), how it reads DynamoDB's answer into rows, and
// how each failure maps (a lost condition, a cancelled transaction, a
// ValidationException). The shapes are the DynamoDB API reference's, as in
// unit_dynamo.c; `make check-dynamo` runs the same store contract against
// DynamoDB Local itself.
#include "t.h"

#include "aws_json.h"
#include "store/store_import.h"
#include "store/store_priv.h"
#include "util/clock.h"
#include "util/json.h"
#include "util/log.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define U1 "aaaaaaaa-0000-4000-8000-000000000001"
#define U2 "aaaaaaaa-0000-4000-8000-000000000002"
#define B1 "bbbbbbbb-0000-4000-8000-000000000001"

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
static const char CANCEL_0_ITEM[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
    "\"CancellationReasons\":[{\"Code\":\"ConditionalCheckFailed\",\"Item\":{\"pk\":{\"S\":\"GAME#ab12cd\"}}},{\"Code\":\"None\"}]}";
static const char CANCEL_1[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
    "\"CancellationReasons\":[{\"Code\":\"None\"},{\"Code\":\"ConditionalCheckFailed\"},{\"Code\":\"None\"}]}";
static const char CANCEL_OTHER[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
    "\"CancellationReasons\":[{\"Code\":\"None\"},{\"Code\":\"None\"},{\"Code\":\"ValidationError\"}]}";
static const char EMPTY[] = "{\"Count\":0,\"Items\":[]}";
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

// The cascade by hand: the account, then its seats, chat and tokens through
// GSI1, deleted 25 to a batch (sent again while some are left undone), then
// the account's own items in one transaction.
static void user_delete_cascade(void) {
    char seats[8192];
    int o = snprintf(seats, sizeof seats, "{\"Items\":[");
    for (int i = 0; i < 30; i++)
        o += snprintf(seats + o, sizeof seats - (size_t)o, "%s{\"pk\":{\"S\":\"GAME#g%05d\"},\"sk\":{\"S\":\"MEMBER#" U1 "\"}}", i ? "," : "", i);
    snprintf(seats + o, sizeof seats - (size_t)o, "]}");
    static char seats_s[8192];
    memcpy(seats_s, seats, sizeof seats_s);
    static const char UNPROCESSED[] =
        "{\"UnprocessedItems\":{\"cbase\":[{\"DeleteRequest\":{\"Key\":{\"pk\":{\"S\":\"GAME#g00000\"},\"sk\":{\"S\":\"MEMBER#x\"}}}}]}}";
    const step steps[] = {
        { "GetItem", "\"USER#" U1 "\"", 200, "{\"Item\":" USER_ITEM(U1, "ann@x.io") "}" },
        { "Query", "USERGAMES#" U1, 200, seats_s },
        { "Query", "USERCHAT#" U1, 200, "{\"Items\":[{\"pk\":{\"S\":\"CHAT#g00000\"},\"sk\":{\"S\":\"1#c\"}}]}" },
        { "Query", "USERREFRESH#" U1, 200, "{\"Items\":[{\"pk\":{\"S\":\"REFRESH#aa\"},\"sk\":{\"S\":\"TOKEN\"}}]}" },
        { "BatchWriteItem", "GAME#g00024", 200, UNPROCESSED },   // the first 25, partly undone
        { "BatchWriteItem", "GAME#g00024", 200, "{}" },          // sent again whole
        { "BatchWriteItem", "REFRESH#aa", 200, "{}" },           // the other 7
        { "TransactWriteItems", "EMAIL#ann@x.io", 200, "{}" },
        // A user who is not there: nothing else asked.
        { "GetItem", "\"USER#" U2 "\"", 200, NOITEM },
        // A query that fails stops it before any delete.
        { "GetItem", "\"USER#" U1 "\"", 200, "{\"Item\":" USER_ITEM(U1, "ann@x.io") "}" },
        { "Query", "USERGAMES#", 400, VALIDATION },
        // The account's transaction failing is a failure.
        { "GetItem", "\"USER#" U1 "\"", 200, "{\"Item\":" USER_ITEM(U1, "ann@x.io") "}" },
        { "Query", "USERGAMES#", 200, EMPTY },
        { "Query", "USERCHAT#", 200, EMPTY },
        { "Query", "USERREFRESH#", 200, EMPTY },
        { "TransactWriteItems", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    T_EQ_INT(CB_STORE_CALL(s, user_delete, U1), CB_OK);
    T_EQ_INT(sc_.at, 8);
    T_CHECK(strstr(sc_.bodies[6], "GAME#g00029") != NULL);   // the second batch holds what the first did not
    T_CHECK(strstr(sc_.bodies[6], "GAME#g00024") == NULL);
    T_EQ_INT(CB_STORE_CALL(s, user_delete, U2), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, user_delete, U1), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, user_delete, U1), CB_E_DB);
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

// ---- games ---------------------------------------------------------------------------------------

static void game_create_delete(void) {
    static const char SEATS[] = "{\"Items\":[{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"MEMBER#" U1 "\"}},"
                                "{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"BOT#" B1 "\"}}]}";
    static const step steps[] = {
        { "TransactWriteItems", "\"GAME#ab12cd\"", 200, "{}" },
        { "TransactWriteItems", NULL, 400, CANCEL_0 },       // the id is taken
        { "TransactWriteItems", NULL, 400, CANCEL_1 },       // something else refused
        { "TransactWriteItems", NULL, 400, VALIDATION },
        // delete: the seats, the row fenced on its version with them, then chat and replays
        { "Query", "\"GAME#ab12cd\"", 200, SEATS },
        { "TransactWriteItems", "MEMBER#" U1, 200, "{}" },
        { "Query", "CHAT#ab12cd", 200, "{\"Items\":[{\"pk\":{\"S\":\"CHAT#ab12cd\"},\"sk\":{\"S\":\"1#c\"}}]}" },
        { "BatchWriteItem", "CHAT#ab12cd", 200, "{}" },
        { "Query", "GAMESNAPS#ab12cd", 200, "{\"Items\":[{\"pk\":{\"S\":\"SNAP#s1\"},\"sk\":{\"S\":\"SNAP\"}}]}" },
        { "UpdateItem", "SNAP#s1", 400, VALIDATION },          // logged, the delete still stands
        { "Query", "LOG#ab12cd", 200, EMPTY },                 // its session log items
        // moved on, gone, failed
        { "Query", NULL, 200, EMPTY },
        { "TransactWriteItems", NULL, 400, CANCEL_0_ITEM },
        { "Query", NULL, 200, EMPTY },
        { "TransactWriteItems", NULL, 400, CANCEL_0 },
        { "Query", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    uint8_t st[2] = { 1, 2 };
    cb_game_row g = { .id = "ab12cd", .status = 0, .state = st, .state_len = 2, .roster = st, .roster_len = 2, .created_at = 1, .updated_at = 1 };
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, U1), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, U1), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, U1), CB_E_DB);
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, U1), CB_E_DB);
    T_EQ_INT(CB_ROWS_CALL(s, game_delete, "ab12cd", 4), CB_OK);
    T_CHECK(strstr(sc_.bodies[5], "BOT#" B1) != NULL);
    T_CHECK(strstr(sc_.bodies[5], "\"N\":\"4\"") != NULL);   // the fence
    T_EQ_INT(CB_ROWS_CALL(s, game_delete, "ab12cd", 4), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, game_delete, "ab12cd", 4), CB_E_NOT_FOUND);
    T_EQ_INT(CB_ROWS_CALL(s, game_delete, "ab12cd", 4), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

typedef struct { int n; char ids[8][CB_GAME_ID_MAX + 1]; int stop_after; } games_seen;
static bool see_member(const cb_member_row *m, void *ud) {
    games_seen *w = ud;
    if (w->n < 8) snprintf(w->ids[w->n], sizeof w->ids[0], "%s", m->game_id);
    w->n++;
    return w->stop_after <= 0 || w->n < w->stop_after;
}

static void games_of_user_and_membership(void) {
    static const char P[] = "{\"Items\":[{\"game_id\":{\"S\":\"aaaaaa\"},\"updated_at\":{\"N\":\"9\"}},{\"nogame\":{\"S\":\"x\"}},"
                            "{\"game_id\":{\"S\":\"bbbbbb\"}}]}";
    static const step steps[] = {
        { "Query", "USERGAMES#" U1, 200, P },
        { "Query", "\"Limit\":1", 200, P },
        { "Query", NULL, 400, VALIDATION },
        { "GetItem", "MEMBER#" U1, 200, "{\"Item\":{\"pk\":{\"S\":\"GAME#aaaaaa\"}}}" },
        { "GetItem", "MEMBER#" U2, 200, NOITEM },
        { "GetItem", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    games_seen w = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, games_of_user, U1, 0, see_member, &w), 2);
    T_EQ_STR(w.ids[0], "aaaaaa");
    T_EQ_STR(w.ids[1], "bbbbbb");
    memset(&w, 0, sizeof w);
    T_EQ_INT(CB_ROWS_CALL(s, games_of_user, U1, 1, see_member, &w), 1);
    T_EQ_INT(CB_ROWS_CALL(s, games_of_user, U1, 0, see_member, &w), CB_E_DB);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "aaaaaa", U1), 1);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "aaaaaa", U2), 0);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "aaaaaa", U2), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

// A game's human members (a chat's audience): one consistent Query of the
// game's seat items, the bots left out; fn may stop it; a failed read is the
// store's error.
typedef struct { int n; char ids[4][40]; int stop_after; } ids_seen;
static bool see_id(const char *id, void *ud) {
    ids_seen *w = ud;
    if (w->n < 4) snprintf(w->ids[w->n], sizeof w->ids[0], "%s", id);
    w->n++;
    return w->stop_after <= 0 || w->n < w->stop_after;
}

static void game_members_are_the_human_seats(void) {
    static const char SEATS[] =
        "{\"Items\":[{\"pk\":{\"S\":\"GAME#aaaaaa\"},\"sk\":{\"S\":\"BOT#b0000000-0000-4000-8000-000000000001\"}},"
        "{\"pk\":{\"S\":\"GAME#aaaaaa\"},\"sk\":{\"S\":\"MEMBER#" U1 "\"}},"
        "{\"pk\":{\"S\":\"GAME#aaaaaa\"},\"sk\":{\"S\":\"MEMBER#" U2 "\"}}],\"Count\":3}";
    static const step steps[] = {
        { "Query", "\"GAME#aaaaaa\"", 200, SEATS },
        { "Query", NULL, 200, SEATS },
        { "Query", NULL, 200, "{\"Items\":[],\"Count\":0}" },
        { "Query", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    ids_seen w = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, game_members, "aaaaaa", see_id, &w), 2);
    T_EQ_STR(w.ids[0], U1);
    T_EQ_STR(w.ids[1], U2);
    T_CHECK(strstr(sc_.bodies[0], "\"ConsistentRead\":true") != NULL);
    ids_seen one = { .stop_after = 1 };
    T_EQ_INT(CB_ROWS_CALL(s, game_members, "aaaaaa", see_id, &one), 1);
    ids_seen none = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, game_members, "aaaaaa", see_id, &none), 0);
    T_EQ_INT(CB_ROWS_CALL(s, game_members, "aaaaaa", see_id, &none), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

// A drive that found nothing takes the game out of the scan: the SCAN marker
// deleted, on condition it still describes the version the drive read.
static void bots_idle_deletes_the_marker(void) {
    static const step steps[] = {
        { "DeleteItem", "\"GAME#ab12cd\"", 200, "{}" },
        { "DeleteItem", NULL, 400, COND_NO_ITEM },   // a commit moved it on: the scan keeps it
        { "DeleteItem", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    T_EQ_INT(CB_ROWS_CALL(s, bots_idle, "ab12cd", 7), CB_OK);
    T_CHECK(strstr(sc_.bodies[0], "\"N\":\"7\"") != NULL);
    T_EQ_INT(CB_ROWS_CALL(s, bots_idle, "ab12cd", 7), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, bots_idle, "ab12cd", 7), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

// Whether a drive holds the game's lease: its SCAN marker's lease_until not
// yet before now; no marker, no lease; a failed read is the store's error.
static void lease_held_reads_the_marker(void) {
    static const step steps[] = {
        { "GetItem", "\"GAME#ab12cd\"", 200, "{\"Item\":{\"pk\":{\"S\":\"SCAN\"},\"lease_until\":{\"N\":\"5000\"}}}" },
        { "GetItem", NULL, 200, "{\"Item\":{\"pk\":{\"S\":\"SCAN\"},\"lease_until\":{\"N\":\"5000\"}}}" },
        { "GetItem", NULL, 200, "{\"Item\":{\"pk\":{\"S\":\"SCAN\"}}}" },   // never leased
        { "GetItem", NULL, 200, NOITEM },
        { "GetItem", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, "ab12cd", 5000), 1);   // the last millisecond still holds
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, "ab12cd", 5001), 0);
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, "ab12cd", 1), 0);
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, "ab12cd", 1), 0);
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, "ab12cd", 1), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

// ---- bots and ratings ------------------------------------------------------------------------------

typedef struct { int n; char names[8][80]; int32_t elo[8]; int stop_after; } names_seen;
static bool see_bot(const cb_bot_row *b, void *ud) {
    names_seen *w = ud;
    if (w->n < 8) { snprintf(w->names[w->n], sizeof w->names[0], "%s", b->nickname); w->elo[w->n] = b->elo_rating; }
    w->n++;
    return w->stop_after <= 0 || w->n < w->stop_after;
}
static bool see_rating(const cb_rating_row *r, void *ud) {
    names_seen *w = ud;
    if (w->n < 8) { snprintf(w->names[w->n], sizeof w->names[0], "%s", r->user_id); w->elo[w->n] = r->elo_rating; }
    w->n++;
    return w->stop_after <= 0 || w->n < w->stop_after;
}

#define BOT_ITEM(id, nick, key, elo) \
    "{\"pk\":{\"S\":\"BOT#" id "\"},\"sk\":{\"S\":\"BOT\"},\"id\":{\"S\":\"" id "\"},\"nickname\":{\"S\":\"" nick "\"}," \
    "\"strategy_key\":{\"S\":\"" key "\"},\"elo_rating\":{\"N\":\"" elo "\"},\"games_played\":{\"N\":\"3\"}}"
#define RATING_ITEM(id, elo, games) \
    "{\"pk\":{\"S\":\"USER#" id "\"},\"sk\":{\"S\":\"RATING\"},\"user_id\":{\"S\":\"" id "\"},\"username\":{\"S\":\"N\"}," \
    "\"elo_rating\":{\"N\":\"" elo "\"},\"games_played\":{\"N\":\"" games "\"}}"

static void bots_and_ratings(void) {
    static const char BOTS[] = "{\"Items\":[" BOT_ITEM("b2", "%Zed", "random", "900") "," BOT_ITEM("b1", "%Ann", "random", "1100")
                               ",{\"pk\":{\"S\":\"BOT#broken\"}}]}";
    static const char RATINGS[] = "{\"Items\":[" RATING_ITEM(U2, "900", "2") "," RATING_ITEM(U1, "1200", "5") "]}";
    static const char TOP[] = "{\"Items\":[" RATING_ITEM(U1, "1200", "5") "," RATING_ITEM(U2, "900", "0") "]}";
    static const step steps[] = {
        { "Query", "BOT", 200, BOTS },
        { "Query", NULL, 200, BOTS },
        { "Query", NULL, 400, VALIDATION },
        { "Query", "RATING", 200, RATINGS },
        { "Query", NULL, 400, VALIDATION },
        { "Query", "\"Limit\":2", 200, TOP },
        { "Query", NULL, 400, VALIDATION },
        { "UpdateItem", "USER#" U1, 200, "{}" },
        { "UpdateItem", "REMOVE", 200, "{}" },
        { "UpdateItem", NULL, 400, COND_NO_ITEM },   // no row: nothing to rename
        { "UpdateItem", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    names_seen w = { 0 };
    // Ladder order: same tier, so by nickname.
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, see_bot, &w), 2);
    T_EQ_STR(w.names[0], "%Ann");
    T_EQ_STR(w.names[1], "%Zed");
    memset(&w, 0, sizeof w);
    w.stop_after = 1;
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, see_bot, &w), 1);
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, see_bot, &w), CB_E_DB);
    // Every rating row, highest first.
    memset(&w, 0, sizeof w);
    T_EQ_INT(CB_ROWS_CALL(s, ratings_list, see_rating, &w), 2);
    T_EQ_STR(w.names[0], U1);
    T_EQ_INT(w.elo[0], 1200);
    T_EQ_INT(CB_ROWS_CALL(s, ratings_list, see_rating, &w), CB_E_DB);
    // The top: one bounded Query; a row never played is not on the board.
    memset(&w, 0, sizeof w);
    T_EQ_INT(CB_ROWS_CALL(s, ratings_top, 2, see_rating, &w), 1);
    T_EQ_STR(w.names[0], U1);
    T_EQ_INT(CB_ROWS_CALL(s, ratings_top, 0, see_rating, &w), 0);   // asks nothing
    T_EQ_INT(CB_ROWS_CALL(s, ratings_top, 2, see_rating, &w), CB_E_DB);
    T_EQ_INT(CB_ROWS_CALL(s, rating_set_username, U1, "NEW"), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, rating_set_username, U1, NULL), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, rating_set_username, U2, "X"), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, rating_set_username, U2, "X"), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

// ---- snapshots and chat --------------------------------------------------------------------------

typedef struct { int n; char ids[8][40]; int players; } snaps_seen;
static bool see_snap(const cb_snapshot_row *r, void *ud) {
    snaps_seen *w = ud;
    if (w->n < 8) snprintf(w->ids[w->n], 40, "%s", r->id);
    w->players = r->n_players;
    w->n++;
    return true;
}

// A player's pointer (SNAP#<id> PLAYER#<user>: the id, the game, the date)
// and the replay it points at (SNAP#<id> SNAP: the only item with the moves).
#define SNAP_POINTER(id, game) \
    "{\"pk\":{\"S\":\"SNAP#" id "\"},\"sk\":{\"S\":\"PLAYER#" U1 "\"},\"id\":{\"S\":\"" id "\"}," \
    "\"game_id\":{\"S\":\"" game "\"},\"created_at\":{\"N\":\"3\"}}"
#define SNAP_MAIN(id, game) \
    "{\"pk\":{\"S\":\"SNAP#" id "\"},\"sk\":{\"S\":\"SNAP\"},\"id\":{\"S\":\"" id "\"},\"game_id\":{\"S\":\"" game "\"}," \
    "\"player_ids\":{\"L\":[{\"S\":\"" U1 "\"},{\"S\":\"" U2 "\"}]},\"moves\":{\"B\":\"AQI=\"},\"created_at\":{\"N\":\"3\"}}"
#define SNAP_BATCH(...) "{\"Responses\":{\"cbase\":[" __VA_ARGS__ "]},\"UnprocessedKeys\":{}}"

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p++) n++;
    return n;
}

static void snapshots_put_and_query(void) {
    static const char POINTERS[] = "{\"Items\":[" SNAP_POINTER("s1", "aaaaaa") "," SNAP_POINTER("s2", "bbbbbb")
                                   ",{\"pk\":{\"S\":\"x\"}}]}";
    static const step steps[] = {
        { "TransactWriteItems", "PLAYER#" U2, 200, "{}" },
        { "TransactWriteItems", NULL, 400, CANCEL_0 },
        { "TransactWriteItems", NULL, 400, VALIDATION },
        { "Query", "PLAYER#" U1, 200, POINTERS },
        { "BatchGetItem", "\"S\":\"SNAP\"", 200, SNAP_BATCH(SNAP_MAIN("s2", "bbbbbb") "," SNAP_MAIN("s1", "aaaaaa")) },
        { "Query", NULL, 200, POINTERS },
        { "BatchGetItem", "SNAP#s2", 200, SNAP_BATCH(SNAP_MAIN("s2", "bbbbbb")) },
        { "Query", "\"ScanIndexForward\":false", 200, POINTERS },
        { "BatchGetItem", "SNAP#s1", 200, SNAP_BATCH(SNAP_MAIN("s1", "aaaaaa")) },
        { "Query", NULL, 400, VALIDATION },
        // The pointer still names the game; its replay does not (the game was deleted).
        { "Query", NULL, 200, POINTERS },
        { "BatchGetItem", "SNAP#s1", 200, SNAP_BATCH(SNAP_MAIN("s1", "")) },
    };
    RUN(steps, s);
    uint8_t mv[2] = { 1, 2 };
    cb_snapshot_row r = { .id = "s1", .game_id = "aaaaaa", .n_players = 2, .moves = mv, .moves_len = 2, .created_at = 3 };
    snprintf(r.player_ids[0], sizeof r.player_ids[0], U1);
    snprintf(r.player_ids[1], sizeof r.player_ids[1], U2);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &r), CB_OK);
    // The replay and two pointers: only the replay holds the moves and the players.
    T_EQ_INT(count_of(sc_.bodies[0], "\"Put\""), 3);
    T_EQ_INT(count_of(sc_.bodies[0], "\"moves\""), 1);
    T_EQ_INT(count_of(sc_.bodies[0], "\"player_ids\""), 1);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &r), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &r), CB_E_DB);
    r.n_players = 9;
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &r), CB_E_INVALID);   // asked nothing
    snaps_seen w = { 0 };
    cb_snapshot_query q = { .viewer_id = U1 };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, see_snap, &w), 2);
    T_EQ_STR(w.ids[0], "s1");   // the pointers' order, not the batch's
    T_EQ_STR(w.ids[1], "s2");
    T_EQ_INT(w.players, 2);
    memset(&w, 0, sizeof w);
    q.game_id = "bbbbbb";
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, see_snap, &w), 1);   // one game's
    T_EQ_STR(w.ids[0], "s2");
    memset(&w, 0, sizeof w);
    cb_snapshot_query q2 = { .viewer_id = U1, .id = "s1", .limit = 1, .newest_first = true };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q2, see_snap, &w), 1);
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q2, see_snap, &w), CB_E_DB);
    memset(&w, 0, sizeof w);
    q.game_id = "aaaaaa";
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, see_snap, &w), 0);
    cb_snapshot_query none = { .viewer_id = "" };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &none, see_snap, &w), CB_E_INVALID);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

// ---- the history summary on the pointers -------------------------------------------------------------

typedef struct {
    int n, seat[4], rank[4], n_players[4], fool[4], dur[4], has[4], moves_null, ids_ok;
    size_t extras_len[4];
} sum_seen;
static bool see_sum(const cb_snapshot_row *r, void *ud) {
    sum_seen *w = ud;
    if (w->n < 4) {
        w->seat[w->n] = r->seat;
        w->rank[w->n] = r->rank;
        w->n_players[w->n] = r->n_players;
        w->fool[w->n] = r->sum.fool;
        w->dur[w->n] = r->sum.duration_s;
        w->has[w->n] = r->sum.has;
        w->extras_len[w->n] = r->extras_len;
        if (r->moves != NULL) w->moves_null++;   // counts the rows that DO hold moves
    }
    w->n++;
    return true;
}

// A pointer written with a summary carries the numbers (never a name, never the moves); a
// pointer written without one carries none; the history query is ONE Query of the pointers and
// ONE BatchGetItem of the replays projected to the extras (the moves and the player ids only
// when some pointer holds no summary), and it reads rank, seat, fool and duration off the pointer.
static void history_pointers_carry_the_summary(void) {
#define SUM_POINTER(id, seat, rank) \
    "{\"pk\":{\"S\":\"SNAP#" id "\"},\"sk\":{\"S\":\"PLAYER#" U1 "\"},\"id\":{\"S\":\"" id "\"}," \
    "\"game_id\":{\"S\":\"aaaaaa\"},\"created_at\":{\"N\":\"3\"},\"n_players\":{\"N\":\"3\"}," \
    "\"seat\":{\"N\":\"" seat "\"},\"rank\":{\"N\":\"" rank "\"},\"fool\":{\"N\":\"2\"},\"dur\":{\"N\":\"95\"}}"
#define EXTRAS_MAIN(id) \
    "{\"pk\":{\"S\":\"SNAP#" id "\"},\"sk\":{\"S\":\"SNAP\"},\"id\":{\"S\":\"" id "\"},\"extras\":{\"B\":\"AQI=\"}}"
    static const char WITH[] = "{\"Items\":[" SUM_POINTER("s2", "1", "1") "," SUM_POINTER("s1", "2", "3") "]}";
    static const char OLD[] = "{\"Items\":[" SNAP_POINTER("s1", "aaaaaa") "," SUM_POINTER("s2", "1", "1") "]}";
    static const step steps[] = {
        { "TransactWriteItems", "PLAYER#" U1, 200, "{}" },
        { "TransactWriteItems", "PLAYER#" U1, 200, "{}" },
        { "Query", "PLAYER#" U1, 200, WITH },
        { "BatchGetItem", "SNAP#s2", 200, SNAP_BATCH(EXTRAS_MAIN("s2") "," EXTRAS_MAIN("s1")) },
        { "Query", "PLAYER#" U1, 200, OLD },
        { "BatchGetItem", "SNAP#s1", 200, SNAP_BATCH(SNAP_MAIN("s1", "aaaaaa") "," EXTRAS_MAIN("s2")) },
    };
    RUN(steps, s);
    uint8_t mv[2] = { 1, 2 };
    cb_snapshot_row r = { .id = "s1", .game_id = "aaaaaa", .n_players = 3, .moves = mv, .moves_len = 2, .created_at = 3 };
    snprintf(r.player_ids[0], sizeof r.player_ids[0], U1);
    snprintf(r.player_ids[1], sizeof r.player_ids[1], U2);
    snprintf(r.player_ids[2], sizeof r.player_ids[2], "%s", "55555555-5555-4555-8555-555555555555");
    r.sum = (cb_snap_summary){ .has = true, .fool = 2, .n_elim = 2, .elim = { 1, 0 }, .duration_s = 95 };
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &r), CB_OK);
    // Three pointers: each its own seat and rank (seat 1 went out first, seat 0 second, seat 2 is the fool).
    T_EQ_INT(count_of(sc_.bodies[0], "\"n_players\""), 3);
    T_EQ_INT(count_of(sc_.bodies[0], "\"seat\""), 3);
    T_EQ_INT(count_of(sc_.bodies[0], "\"dur\""), 3);
    T_EQ_INT(count_of(sc_.bodies[0], "\"fool\""), 3);
    T_EQ_INT(count_of(sc_.bodies[0], "\"seat\":{\"N\":\"0\"},\"rank\":{\"N\":\"2\"}"), 1);
    T_EQ_INT(count_of(sc_.bodies[0], "\"seat\":{\"N\":\"1\"},\"rank\":{\"N\":\"1\"}"), 1);
    T_EQ_INT(count_of(sc_.bodies[0], "\"seat\":{\"N\":\"2\"},\"rank\":{\"N\":\"3\"}"), 1);   // the fool
    T_EQ_INT(count_of(sc_.bodies[0], "\"moves\""), 1);                // only the replay holds the moves
    // Without a summary the pointer holds none of it.
    r.sum.has = false;
    cb_uuid_v4(r.id);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &r), CB_OK);
    T_EQ_INT(count_of(sc_.bodies[1], "\"rank\""), 0);
    T_EQ_INT(count_of(sc_.bodies[1], "\"dur\""), 0);
    T_EQ_INT(count_of(sc_.bodies[1], "\"seat\""), 0);

    sum_seen w = { 0 };
    cb_snapshot_query q = { .viewer_id = U1, .summary_only = true };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, see_sum, &w), 2);
    T_EQ_INT(w.moves_null, 0);                       // no row holds the moves
    T_EQ_INT(w.rank[0], 1);
    T_EQ_INT(w.seat[0], 1);
    T_EQ_INT(w.rank[1], 3);                          // rank n = the fool
    T_EQ_INT(w.seat[1], 2);
    T_EQ_INT(w.n_players[0], 3);
    T_EQ_INT(w.fool[0], 2);
    T_EQ_INT(w.dur[0], 95);
    T_CHECK(w.has[0] && w.has[1]);
    T_EQ_INT((int)w.extras_len[0], 2);               // the names ride in the extras
    // ONE BatchGetItem, projected to the extras: not the moves, not the player ids.
    T_EQ_INT(count_of(sc_.bodies[3], "ProjectionExpression"), 1);
    T_EQ_INT(count_of(sc_.bodies[3], "\"moves\""), 0);
    T_EQ_INT(count_of(sc_.bodies[3], "\"player_ids\""), 0);
    T_CHECK(strstr(sc_.bodies[3], "\"extras\"") != NULL);
    // A pointer from before the summary: no rank, and the replay is read whole enough to cut one.
    memset(&w, 0, sizeof w);
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, see_sum, &w), 2);
    T_CHECK(!w.has[0]);
    T_EQ_INT(w.rank[0], 0);
    T_CHECK(w.has[1]);
    T_CHECK(strstr(sc_.bodies[5], "\"moves\"") != NULL && strstr(sc_.bodies[5], "\"player_ids\"") != NULL);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
#undef SUM_POINTER
#undef EXTRAS_MAIN
}

// The query's `ref` is the game id OR the replay's own id, and nothing else.
static void snapshot_ref_matches_the_replay_or_its_game(void) {
    static const char POINTERS[] = "{\"Items\":[" SNAP_POINTER("s1", "aaaaaa") "," SNAP_POINTER("s2", "bbbbbb") "]}";
    static const step steps[] = {
        { "Query", "PLAYER#" U1, 200, POINTERS }, { "BatchGetItem", "SNAP#s1", 200, SNAP_BATCH(SNAP_MAIN("s1", "aaaaaa")) },
        { "Query", "PLAYER#" U1, 200, POINTERS }, { "BatchGetItem", "SNAP#s2", 200, SNAP_BATCH(SNAP_MAIN("s2", "bbbbbb")) },
        { "Query", "PLAYER#" U1, 200, POINTERS },
    };
    RUN(steps, s);
    snaps_seen w = { 0 };
    cb_snapshot_query q = { .viewer_id = U1, .ref = "aaaaaa" };   // a game id
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, see_snap, &w), 1);
    T_EQ_STR(w.ids[0], "s1");
    memset(&w, 0, sizeof w);
    q.ref = "s2";                                                  // a replay id, as the history row gives it
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, see_snap, &w), 1);
    T_EQ_STR(w.ids[0], "s2");
    memset(&w, 0, sizeof w);
    q.ref = "s";                                                   // a prefix is neither
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, see_snap, &w), 0);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

static void chat_put_refusals(void) {
    static const step steps[] = {
        { "TransactWriteItems", "CHAT#ab12cd", 200, "{}" },
        { "TransactWriteItems", NULL, 400, CANCEL_0 },       // the id is taken
        { "TransactWriteItems", NULL, 400, CANCEL_1 },       // the game or the member is gone
        { "TransactWriteItems", NULL, 400, CANCEL_OTHER },
        { "TransactWriteItems", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    cb_chat_row c = { .id = "c1", .game_id = "ab12cd", .user_id = U1, .message = "hi", .created_at = 4 };
    T_EQ_INT(CB_ROWS_CALL(s, chat_put, &c), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, chat_put, &c), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, chat_put, &c), CB_E_NOT_FOUND);
    T_EQ_INT(CB_ROWS_CALL(s, chat_put, &c), CB_E_DB);
    T_EQ_INT(CB_ROWS_CALL(s, chat_put, &c), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

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

// The import's writes: an account (and its email and rating), a bot set, a
// game (with its seats, no fence), a reindexed rating.
static void import_writes(void) {
    static const char HAVE_BOTS[] = "{\"Items\":[{\"pk\":{\"S\":\"BOT#old\"},\"sk\":{\"S\":\"BOT\"},\"id\":{\"S\":\"old\"},"
                                    "\"nickname\":{\"S\":\"%Old\"},\"strategy_key\":{\"S\":\"random\"}}]}";
    static const step steps[] = {
        // user_import: the stored profile read first (its session_gen is never lowered), then the account's transaction
        { "GetItem", "\"USER#" U1 "\"", 200, NOITEM },
        { "TransactWriteItems", "EMAIL#ann@x.io", 200, "{}" },
        { "GetItem", "\"USER#" U1 "\"", 200, "{\"Item\":" USER_ITEM(U1, "ann@x.io") "}" },
        { "TransactWriteItems", NULL, 400, CANCEL_0 },
        { "GetItem", "\"USER#" U1 "\"", 200, NOITEM },
        { "TransactWriteItems", NULL, 400, VALIDATION },
        // bots_replace: the current set read, then one transaction deletes it and writes the new
        { "Query", "BOT", 200, HAVE_BOTS },
        { "TransactWriteItems", "BOT#old", 200, "{}" },
        { "Query", "BOT", 200, HAVE_BOTS },
        { "TransactWriteItems", NULL, 400, VALIDATION },
        { "Query", "BOT", 400, VALIDATION },
        // game_import: seats read, the new seat's account looked up, the row and its seats written whole
        { "Query", "GAME#ab12cd", 200, EMPTY },
        { "BatchGetItem", "USER#" U1, 200, "{\"Responses\":{\"cbase\":[{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"PROFILE\"}}]}}" },
        { "TransactWriteItems", "MEMBER#" U1, 200, "{}" },
        // rating_reindex: the row given the sparse key, or found already done
        { "UpdateItem", "gsi2pk", 200, "{}" },
        { "UpdateItem", "gsi2pk", 400, COND_NO_ITEM },
        { "UpdateItem", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    cb_user_row u = { .id = U1, .email = "ann@x.io", .password_hash = "$2b$10$x", .username = "ANN" };
    cb_rating_row ra = { .user_id = U1, .username = "ANN", .elo_rating = 1000, .games_played = 1 };
    T_EQ_INT(CB_STORE_CALL(s, user_import, &u), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_import, &u), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, user_import, &u), CB_E_DB);
    cb_bot_row b = { .id = B1, .nickname = "%Bee", .strategy_key = "random", .elo_rating = 1000 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_replace, &b, 1), CB_OK);
    T_CHECK(strstr(sc_.bodies[7], "BOT#" B1) != NULL);   // the new set in the same transaction
    T_EQ_INT(CB_ROWS_CALL(s, bots_replace, &b, 1), CB_E_DB);
    T_EQ_INT(CB_ROWS_CALL(s, bots_replace, &b, 1), CB_E_DB);
    uint8_t st[1] = { 1 };
    cb_game_row g = { .id = "ab12cd", .status = 1, .state = st, .state_len = 1, .roster = st, .roster_len = 1, .version = 3 };
    cb_uuid humans[1];
    snprintf(humans[0], sizeof humans[0], U1);
    T_EQ_INT(CB_ROWS_CALL(s, game_import, &g, humans, 1, NULL, 0), CB_OK);
    T_CHECK(strstr(sc_.bodies[13], "MEMBER#" U1) != NULL);
    T_EQ_INT(CB_ROWS_CALL(s, game_import, &g, humans, 9, NULL, 0), CB_E_INVALID);
    T_EQ_INT(CB_ROWS_CALL(s, rating_reindex, &ra), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, rating_reindex, &ra), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, rating_reindex, &ra), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

// cbase-import --seed-bots and --reindex-ratings on DynamoDB: seeded only into
// an empty bot set; every rating row reindexed, a done one not counted, an
// error stopping it.
static void import_seed_and_reindex(void) {
    static const char RATINGS[] = "{\"Items\":[" RATING_ITEM(U1, "1200", "5") "," RATING_ITEM(U2, "900", "2") "]}";
    static const step steps[] = {
        { "Query", "BOT", 200, EMPTY },                // no bots: the schema's go in
        { "Query", "BOT", 200, EMPTY },                // bots_replace reads the (empty) set
        { "TransactWriteItems", "strategy_key", 200, "{}" },
        { "Query", "BOT", 200, "{\"Items\":[" BOT_ITEM("b1", "%Ann", "random", "1000") "]}" },   // some: none seeded
        { "Query", "BOT", 400, VALIDATION },
        { "Query", "RATING", 200, RATINGS },
        { "UpdateItem", "USER#" U1, 200, "{}" },
        { "UpdateItem", "USER#" U2, 400, COND_NO_ITEM },
        { "Query", "RATING", 200, RATINGS },
        { "UpdateItem", NULL, 400, VALIDATION },
        { "Query", "RATING", 400, VALIDATION },
    };
    RUN(steps, s);
    int seeded = cb_import_seed_bots(s);
    T_CHECK(seeded > 10);
    T_EQ_INT(cb_import_seed_bots(s), 0);
    T_EQ_INT(cb_import_seed_bots(s), CB_E_DB);
    T_EQ_INT(cb_import_reindex_ratings(s), 1);
    T_EQ_INT(cb_import_reindex_ratings(s), CB_E_DB);
    T_EQ_INT(cb_import_reindex_ratings(s), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

// cbase-import's check on DynamoDB: one keys-only Scan, every item counted
// against the export's ids. A user the export has is "stored"; a game it does
// not have is an extra, listed and not a failure.
static void import_counts_by_scan(void) {
    char dir[256];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/cbase-dyops-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    mkdir(dir, 0700);
    static const struct { const char *name, *text; } files[] = {
        { "users.csv", "id,email,encrypted_password,username,created_at,last_sign_in_at\n"
                       "\"" U1 "\",\"ann@x.io\",\"$2a$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234\",\"Ann\",\"1000\",\"5000\"\n" },
        { "bots.csv", "id,nickname,strategy_key,elo_rating,previous_elo,games_played,created_at,updated_at\n" },
        { "user_elo_ratings.csv", "user_id,username,elo_rating,previous_elo,games_played,updated_at\n" },
        { "games.csv", "id,status,state,roster,needs_bots,game_seed,logs_packed,version,round_epoch,created_at,updated_at,last_commit_at\n" },
        { "player_hands.csv", "game_id,player_id,joined_at\n" },
        { "bot_hands.csv", "game_id,bot_id\n" },
        { "game_snapshots.csv", "id,game_id,player_ids,moves,extras,created_at\n" },
        { "chat_messages.csv", "id,game_id,user_id,message,is_system,created_at\n" },
        { "counts.txt", "users 1\n" },
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        char path[400];
        snprintf(path, sizeof path, "%s/%s", dir, files[i].name);
        FILE *f = fopen(path, "w");
        T_CHECK(f != NULL);
        if (f) { fputs(files[i].text, f); fclose(f); }
    }
    static const char PAGE1[] = "{\"Items\":[{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"PROFILE\"}}],"
                                "\"LastEvaluatedKey\":{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"PROFILE\"}}}";
    static const char PAGE2[] = "{\"Items\":[{\"pk\":{\"S\":\"GAME#zz99zz\"},\"sk\":{\"S\":\"ROW\"}}]}";
    static const step steps[] = {
        { "GetItem", "\"USER#" U1 "\"", 200, NOITEM },
        { "TransactWriteItems", "EMAIL#ann@x.io", 200, "{}" },
        { "Scan", "\"Limit\":100", 200, PAGE1 },
        { "Scan", "ExclusiveStartKey", 200, PAGE2 },
    };
    RUN(steps, s);
    FILE *out = tmpfile();
    T_EQ_INT(cb_import_dir(s, dir, out), 0);
    char report[4096];
    rewind(out);
    size_t n = fread(report, 1, sizeof report - 1, out);
    report[n] = 0;
    fclose(out);
    T_CHECK(strstr(report, "extra (not in the export): games zz99zz") != NULL);
    T_CHECK(strstr(report, "cbase-import: ok") != NULL);
    if (!strstr(report, "cbase-import: ok")) fprintf(stderr, "%s", report);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    T_EQ_INT(system(cmd), 0);
}

int main(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    T_RUN(users_by_email_password_touch);
    T_RUN(user_delete_cascade);
    T_RUN(users_list_pages);
    T_RUN(refresh_put_and_revoke_session);
    T_RUN(game_create_delete);
    T_RUN(games_of_user_and_membership);
    T_RUN(bots_idle_deletes_the_marker);
    T_RUN(lease_held_reads_the_marker);
    T_RUN(game_members_are_the_human_seats);
    T_RUN(bots_and_ratings);
    T_RUN(snapshots_put_and_query);
    T_RUN(history_pointers_carry_the_summary);
    T_RUN(snapshot_ref_matches_the_replay_or_its_game);
    T_RUN(chat_put_refusals);
    T_RUN(admin_collections_are_the_families);
    T_RUN(import_writes);
    T_RUN(import_seed_and_reindex);
    T_RUN(import_counts_by_scan);
    return T_DONE();
}
