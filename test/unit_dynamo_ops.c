// unit_dynamo_ops.c - every store_dynamo.c operation unit_dynamo.c leaves to
// DynamoDB Local, over the same kind of scripted transport: the request each
// sends (its target and key), how it reads DynamoDB's answer into rows, and
// how each failure maps (a lost condition, a cancelled transaction, a
// ValidationException). The shapes are the DynamoDB API reference's, as in
// unit_dynamo.c; `make check-dynamo` runs the same store contract against
// DynamoDB Local itself.
#include "t.h"

#include "json_surface.h"
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
    T_EQ_INT(CB_STORE_CALL(s, user_set_password, U1, "$2b$10$y"), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_set_password, U2, "$2b$10$y"), CB_E_NOT_FOUND);
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

static void refresh_put_and_revoke_session(void) {
    static const step steps[] = {
        { "PutItem", "REFRESH#h1", 200, "{}" },
        { "PutItem", "REFRESH#h1", 400, COND_NO_ITEM },   // the hash is taken
        { "Query", "SESSION#s1", 200, "{\"Items\":[{\"pk\":{\"S\":\"REFRESH#h1\"},\"sk\":{\"S\":\"TOKEN\"}},"
                                      "{\"pk\":{\"S\":\"REFRESH#h2\"},\"sk\":{\"S\":\"TOKEN\"}}]}" },
        { "UpdateItem", "REFRESH#h1", 200, "{}" },
        { "UpdateItem", "REFRESH#h2", 400, COND_NO_ITEM },   // deleted meanwhile: nothing to revoke
        { "Query", "SESSION#s2", 400, VALIDATION },
    };
    RUN(steps, s);
    cb_refresh_row r = { .token_hash = "h1", .user_id = U1, .session_id = "s1", .created_at = 9 };
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &r), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &r), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, refresh_revoke_session, "s1"), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_revoke_session, "s2"), CB_E_DB);
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
    T_EQ_INT(CB_STORE_CALL(s, game_create, &g, U1), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, game_create, &g, U1), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, game_create, &g, U1), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, game_create, &g, U1), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, game_delete, "ab12cd", 4), CB_OK);
    T_CHECK(strstr(sc_.bodies[5], "BOT#" B1) != NULL);
    T_CHECK(strstr(sc_.bodies[5], "\"N\":\"4\"") != NULL);   // the fence
    T_EQ_INT(CB_STORE_CALL(s, game_delete, "ab12cd", 4), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, game_delete, "ab12cd", 4), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, game_delete, "ab12cd", 4), CB_E_DB);
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
    T_EQ_INT(CB_STORE_CALL(s, games_of_user, U1, 0, see_member, &w), 2);
    T_EQ_STR(w.ids[0], "aaaaaa");
    T_EQ_STR(w.ids[1], "bbbbbb");
    memset(&w, 0, sizeof w);
    T_EQ_INT(CB_STORE_CALL(s, games_of_user, U1, 1, see_member, &w), 1);
    T_EQ_INT(CB_STORE_CALL(s, games_of_user, U1, 0, see_member, &w), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "aaaaaa", U1), 1);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "aaaaaa", U2), 0);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "aaaaaa", U2), CB_E_DB);
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
    T_EQ_INT(CB_STORE_CALL(s, bots_idle, "ab12cd", 7), CB_OK);
    T_CHECK(strstr(sc_.bodies[0], "\"N\":\"7\"") != NULL);
    T_EQ_INT(CB_STORE_CALL(s, bots_idle, "ab12cd", 7), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, bots_idle, "ab12cd", 7), CB_E_DB);
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
    T_EQ_INT(CB_STORE_CALL(s, bots_list, see_bot, &w), 2);
    T_EQ_STR(w.names[0], "%Ann");
    T_EQ_STR(w.names[1], "%Zed");
    memset(&w, 0, sizeof w);
    w.stop_after = 1;
    T_EQ_INT(CB_STORE_CALL(s, bots_list, see_bot, &w), 1);
    T_EQ_INT(CB_STORE_CALL(s, bots_list, see_bot, &w), CB_E_DB);
    // Every rating row, highest first.
    memset(&w, 0, sizeof w);
    T_EQ_INT(CB_STORE_CALL(s, ratings_list, see_rating, &w), 2);
    T_EQ_STR(w.names[0], U1);
    T_EQ_INT(w.elo[0], 1200);
    T_EQ_INT(CB_STORE_CALL(s, ratings_list, see_rating, &w), CB_E_DB);
    // The top: one bounded Query; a row never played is not on the board.
    memset(&w, 0, sizeof w);
    T_EQ_INT(CB_STORE_CALL(s, ratings_top, 2, see_rating, &w), 1);
    T_EQ_STR(w.names[0], U1);
    T_EQ_INT(CB_STORE_CALL(s, ratings_top, 0, see_rating, &w), 0);   // asks nothing
    T_EQ_INT(CB_STORE_CALL(s, ratings_top, 2, see_rating, &w), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, rating_set_username, U1, "NEW"), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, rating_set_username, U1, NULL), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, rating_set_username, U2, "X"), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, rating_set_username, U2, "X"), CB_E_DB);
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

#define SNAP_ITEM(id, game) \
    "{\"pk\":{\"S\":\"PLAYERSNAPS#" U1 "\"},\"id\":{\"S\":\"" id "\"},\"game_id\":{\"S\":\"" game "\"}," \
    "\"player_ids\":{\"L\":[{\"S\":\"" U1 "\"},{\"S\":\"" U2 "\"}]},\"moves\":{\"B\":\"AQI=\"},\"created_at\":{\"N\":\"3\"}}"

static void snapshots_put_and_query(void) {
    static const char SNAPS[] = "{\"Items\":[" SNAP_ITEM("s1", "aaaaaa") "," SNAP_ITEM("s2", "bbbbbb")
                                ",{\"pk\":{\"S\":\"x\"}}]}";
    static const step steps[] = {
        { "TransactWriteItems", "PLAYER#" U2, 200, "{}" },
        { "TransactWriteItems", NULL, 400, CANCEL_0 },
        { "TransactWriteItems", NULL, 400, VALIDATION },
        { "Query", "PLAYER#" U1, 200, SNAPS },
        { "Query", NULL, 200, SNAPS },
        { "Query", "\"ScanIndexForward\":false", 200, SNAPS },
        { "Query", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    uint8_t mv[2] = { 1, 2 };
    cb_snapshot_row r = { .id = "s1", .game_id = "aaaaaa", .n_players = 2, .moves = mv, .moves_len = 2, .created_at = 3 };
    snprintf(r.player_ids[0], sizeof r.player_ids[0], U1);
    snprintf(r.player_ids[1], sizeof r.player_ids[1], U2);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_put, &r), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_put, &r), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_put, &r), CB_E_DB);
    r.n_players = 9;
    T_EQ_INT(CB_STORE_CALL(s, snapshot_put, &r), CB_E_INVALID);   // asked nothing
    snaps_seen w = { 0 };
    cb_snapshot_query q = { .viewer_id = U1 };
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, see_snap, &w), 2);
    T_EQ_INT(w.players, 2);
    memset(&w, 0, sizeof w);
    q.game_id = "bbbbbb";
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, see_snap, &w), 1);   // one game's
    T_EQ_STR(w.ids[0], "s2");
    memset(&w, 0, sizeof w);
    cb_snapshot_query q2 = { .viewer_id = U1, .id = "s1", .limit = 1, .newest_first = true };
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q2, see_snap, &w), 1);
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q2, see_snap, &w), CB_E_DB);
    cb_snapshot_query none = { .viewer_id = "" };
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &none, see_snap, &w), CB_E_INVALID);
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
    T_EQ_INT(CB_STORE_CALL(s, chat_put, &c), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, chat_put, &c), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, chat_put, &c), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, chat_put, &c), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, chat_put, &c), CB_E_DB);
    T_EQ_INT(sc_.at, sc_.n);
    cb_store_close(s);
}

// ---- realtime connections -------------------------------------------------------------------------

static char g_conn_live[1024], g_conn_dead[1024];

static void conn_items(void) {
    int64_t now = cb_now_ms();
    snprintf(g_conn_live, sizeof g_conn_live,
             "{\"Item\":{\"pk\":{\"S\":\"CONN#c1\"},\"sk\":{\"S\":\"CONN\"},\"conn_id\":{\"S\":\"c1\"},\"user_id\":{\"S\":\"" U1 "\"},"
             "\"vsn\":{\"N\":\"3\"},\"expires_ms\":{\"N\":\"%lld\"},\"ver\":{\"N\":\"2\"},"
             "\"topics\":{\"L\":[{\"M\":{\"t\":{\"S\":\"pv-" U1 "\"},\"r\":{\"S\":\"1\"}}}]}}}",
             (long long)(now + 3600000));
    snprintf(g_conn_dead, sizeof g_conn_dead,
             "{\"Item\":{\"pk\":{\"S\":\"CONN#c1\"},\"sk\":{\"S\":\"CONN\"},\"conn_id\":{\"S\":\"c1\"},\"expires_ms\":{\"N\":\"%lld\"},"
             "\"ver\":{\"N\":\"7\"}}}",
             (long long)(now - 1000));
}

typedef struct { int n; char users[8][40]; } conns_seen;
static bool see_conn(const cb_conn_row *c, void *ud) {
    conns_seen *w = ud;
    if (w->n < 8) snprintf(w->users[w->n], 40, "%s", c->user_id);
    w->n++;
    return true;
}

static void conns_put_get_delete_by_topic(void) {
    conn_items();
    static char topic[2048];
    int64_t now = cb_now_ms();
    snprintf(topic, sizeof topic,
             "{\"Items\":[{\"conn_id\":{\"S\":\"c1\"},\"user_id\":{\"S\":\"" U1 "\"},\"expires_ms\":{\"N\":\"%lld\"}},"
             "{\"conn_id\":{\"S\":\"c2\"},\"user_id\":{\"S\":\"" U2 "\"},\"expires_ms\":{\"N\":\"%lld\"}},{\"broken\":{\"S\":\"x\"}}]}",
             (long long)(now + 60000), (long long)(now - 60000));
    const step steps[] = {
        // put at version 2 over a live row at 2: the old topics go, the new ones come
        { "GetItem", "CONN#c1", 200, g_conn_live },
        { "TransactWriteItems", "TOPIC#", 200, "{}" },
        // a stale version: turned away without a write
        { "GetItem", "CONN#c1", 200, g_conn_live },
        // an expired row counts as none (version 0)
        { "GetItem", "CONN#c1", 200, g_conn_dead },
        { "TransactWriteItems", NULL, 400, CANCEL_0 },       // lost the race
        // no row at all
        { "GetItem", "CONN#c1", 200, NOITEM },
        { "TransactWriteItems", NULL, 400, VALIDATION },
        { "GetItem", NULL, 400, VALIDATION },
        // get: live, expired (TTL deletes lazily), absent
        { "GetItem", "CONN#c1", 200, g_conn_live },
        { "GetItem", "CONN#c1", 200, g_conn_dead },
        { "GetItem", "CONN#c1", 200, NOITEM },
        // delete: the row and its topic items in one batch; absent is done; a failed read fails
        { "GetItem", "CONN#c1", 200, g_conn_live },
        { "BatchWriteItem", "TOPIC#pv-" U1, 200, "{}" },
        { "GetItem", "CONN#c1", 200, NOITEM },
        { "GetItem", "CONN#c1", 400, VALIDATION },
        { "GetItem", "CONN#c1", 200, g_conn_live },
        { "BatchWriteItem", NULL, 400, VALIDATION },
        // by topic: the live ones only
        { "Query", "TOPIC#game-ab12cd", 200, topic },
        { "Query", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    cb_conn_row c;
    memset(&c, 0, sizeof c);
    snprintf(c.conn_id, sizeof c.conn_id, "c1");
    snprintf(c.user_id, sizeof c.user_id, U1);
    c.vsn = 3;
    c.version = 2;
    c.n_topics = 1;
    snprintf(c.topics[0], sizeof c.topics[0], "game-ab12cd");
    T_EQ_INT(CB_STORE_CALL(s, conn_put, &c), CB_OK);
    T_CHECK(strstr(sc_.bodies[1], "TOPIC#pv-" U1) != NULL);       // the old topic's item deleted
    T_CHECK(strstr(sc_.bodies[1], "TOPIC#game-ab12cd") != NULL);  // the new one written
    c.version = 1;
    T_EQ_INT(CB_STORE_CALL(s, conn_put, &c), CB_E_CONFLICT);
    c.version = 0;
    T_EQ_INT(CB_STORE_CALL(s, conn_put, &c), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, conn_put, &c), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, conn_put, &c), CB_E_DB);
    c.n_topics = -1;
    T_EQ_INT(CB_STORE_CALL(s, conn_put, &c), CB_E_INVALID);
    cb_conn_row got;
    T_EQ_INT(CB_STORE_CALL(s, conn_get, "c1", &got), CB_OK);
    T_EQ_STR(got.user_id, U1);
    T_EQ_INT(got.n_topics, 1);
    T_EQ_STR(got.topics[0], "pv-" U1);
    T_EQ_STR(got.join_refs[0], "1");
    T_EQ_INT(got.version, 2);
    T_EQ_INT(CB_STORE_CALL(s, conn_get, "c1", &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, conn_get, "c1", &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, conn_delete, "c1"), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, conn_delete, "c1"), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, conn_delete, "c1"), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, conn_delete, "c1"), CB_E_DB);
    conns_seen w = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, conns_by_topic, "game-ab12cd", see_conn, &w), 1);
    T_EQ_STR(w.users[0], U1);
    T_EQ_INT(CB_STORE_CALL(s, conns_by_topic, "game-ab12cd", see_conn, &w), CB_E_DB);
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
    T_CHECK(dw_collection(10) == NULL);
    T_EQ_STR(dw_collection(9), "SCAN");
    T_EQ_INT(sc_.at, 0);
    cb_store_close(s);
}

// The import's writes: an account (and its email and rating), a bot set, a
// game (with its seats, no fence), a reindexed rating.
static void import_writes(void) {
    static const char HAVE_BOTS[] = "{\"Items\":[{\"pk\":{\"S\":\"BOT#old\"},\"sk\":{\"S\":\"BOT\"},\"id\":{\"S\":\"old\"},"
                                    "\"nickname\":{\"S\":\"%Old\"},\"strategy_key\":{\"S\":\"random\"}}]}";
    static const step steps[] = {
        { "TransactWriteItems", "EMAIL#ann@x.io", 200, "{}" },
        { "TransactWriteItems", NULL, 400, CANCEL_0 },
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
    T_EQ_INT(CB_STORE_CALL(s, bots_replace, &b, 1), CB_OK);
    T_CHECK(strstr(sc_.bodies[4], "BOT#" B1) != NULL);   // the new set in the same transaction
    T_EQ_INT(CB_STORE_CALL(s, bots_replace, &b, 1), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, bots_replace, &b, 1), CB_E_DB);
    uint8_t st[1] = { 1 };
    cb_game_row g = { .id = "ab12cd", .status = 1, .state = st, .state_len = 1, .roster = st, .roster_len = 1, .version = 3 };
    cb_uuid humans[1];
    snprintf(humans[0], sizeof humans[0], U1);
    T_EQ_INT(CB_STORE_CALL(s, game_import, &g, humans, 1, NULL, 0), CB_OK);
    T_CHECK(strstr(sc_.bodies[10], "MEMBER#" U1) != NULL);
    T_EQ_INT(CB_STORE_CALL(s, game_import, &g, humans, 9, NULL, 0), CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(s, rating_reindex, &ra), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, rating_reindex, &ra), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, rating_reindex, &ra), CB_E_DB);
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
    T_RUN(bots_and_ratings);
    T_RUN(snapshots_put_and_query);
    T_RUN(chat_put_refusals);
    T_RUN(conns_put_get_delete_by_topic);
    T_RUN(admin_collections_are_the_families);
    T_RUN(import_writes);
    T_RUN(import_seed_and_reindex);
    T_RUN(import_counts_by_scan);
    return T_DONE();
}
