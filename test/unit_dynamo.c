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
// here was captured from a live table (there is none yet).
#include "t.h"

#include "json_surface.h"
#include "store/store_priv.h"
#include "table.h"
#include "util/clock.h"
#include "util/json.h"

#include <stdlib.h>

#define TOK "11111111-2222-4333-8444-555555555555"
#define U1 "aaaaaaaa-0000-4000-8000-000000000001"
#define U2 "aaaaaaaa-0000-4000-8000-000000000002"
#define B1 "bbbbbbbb-0000-4000-8000-000000000001"
#define U3 "aaaaaaaa-0000-4000-8000-000000000003"

static bool valid(const dw_req *r) { return cb_json_valid((const char *)r->body.p, r->body.len); }
static cb_span body(const dw_req *r) { return cb_span_of((const char *)r->body.p, r->body.len); }

// ---- requests, byte for byte -----------------------------------------------------------------

static void get_item_shape(void) {
    dw_req r;
    dw_req_init(&r);
    dw_get_game(&r, "cbase", "ab12cd");
    T_EQ_STR(r.target, "DynamoDB_20120810.GetItem");
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"}},\"ConsistentRead\":true}");
    dw_req_free(&r);
}

// try_acquire_bot_lease, on the game's SCAN marker (a small item, so each
// lease write is 1 WCU, and the one write also moves the scan's staleness
// clock): free when never taken, released or run out; the failure brings the
// old item back, so a game with no marker (gone, or no bot work) is told
// apart from a lease held.
static void lease_requests(void) {
    dw_req r;
    dw_req_init(&r);
    dw_lease_acquire(&r, "cbase", "ab12cd", TOK, 1000000, 25000);
    T_EQ_STR(r.target, "DynamoDB_20120810.UpdateItem");
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#ab12cd\"}},"
             "\"UpdateExpression\":\"SET #lease_token = :lease_token, #lease_until = :lease_until, #updated_at = :updated_at\","
             "\"ConditionExpression\":\"attribute_exists(pk) AND (attribute_not_exists(#lease_until) OR #lease_until < :updated_at)\","
             "\"ExpressionAttributeNames\":{\"#lease_token\":\"lease_token\",\"#lease_until\":\"lease_until\",\"#updated_at\":\"updated_at\"},"
             "\"ExpressionAttributeValues\":{\":lease_token\":{\"S\":\"" TOK "\"},\":lease_until\":{\"N\":\"1025000\"},"
             "\":updated_at\":{\"N\":\"1000000\"}},\"ReturnValuesOnConditionCheckFailure\":\"ALL_OLD\"}");
    dw_lease_renew(&r, "cbase", "ab12cd", TOK, 1010000, 25000);
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#ab12cd\"}},"
             "\"UpdateExpression\":\"SET #lease_until = :lease_until, #updated_at = :updated_at\","
             "\"ConditionExpression\":\"#lease_token = :token\","
             "\"ExpressionAttributeNames\":{\"#lease_until\":\"lease_until\",\"#updated_at\":\"updated_at\",\"#lease_token\":\"lease_token\"},"
             "\"ExpressionAttributeValues\":{\":lease_until\":{\"N\":\"1035000\"},\":updated_at\":{\"N\":\"1010000\"},"
             "\":token\":{\"S\":\"" TOK "\"}}}");
    dw_lease_release(&r, "cbase", "ab12cd", TOK);
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#ab12cd\"}},"
             "\"UpdateExpression\":\"REMOVE #lease_until\",\"ConditionExpression\":\"#lease_token = :token\","
             "\"ExpressionAttributeNames\":{\"#lease_until\":\"lease_until\",\"#lease_token\":\"lease_token\"},"
             "\"ExpressionAttributeValues\":{\":token\":{\"S\":\"" TOK "\"}}}");
    // A kick's look at the lease: the marker, read consistently.
    dw_get_scan(&r, "cbase", "ab12cd");
    T_EQ_STR(r.target, "DynamoDB_20120810.GetItem");
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#ab12cd\"}},"
             "\"ConsistentRead\":true}");
    cb_span lit = cb_span_of("{\"pk\":{\"S\":\"SCAN\"},\"lease_until\":{\"N\":\"1025000\"}}", 49);
    T_EQ_INT(dw_item_lease_until(lit), 1025000);
    cb_span nolease = cb_span_of("{\"pk\":{\"S\":\"SCAN\"}}", 19);
    T_CHECK(dw_item_lease_until(nolease) == INT64_MIN);
    // A drive that found no bot can act takes the marker (and its lease)
    // away, fenced on the row version the marker carries, or on none (a
    // marker no commit has written since markers carried it).
    dw_bots_idle(&r, "cbase", "ab12cd", 41);
    T_EQ_STR(r.target, "DynamoDB_20120810.DeleteItem");
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#ab12cd\"}},"
             "\"ConditionExpression\":\"attribute_not_exists(#row_version) OR #row_version = :version\","
             "\"ExpressionAttributeNames\":{\"#row_version\":\"row_version\"},"
             "\"ExpressionAttributeValues\":{\":version\":{\"N\":\"41\"}}}");
    dw_req_free(&r);
}

// THE CAS: item 0 the version-fenced row, then a new member, a member gone,
// the ratings fenced on what was read (clamped arithmetic done here), the
// SCAN marker, updated in place so a drive's lease on it survives the commit.
// The bot already seated is not written again.
static void commit_transaction(void) {
    uint8_t st[3] = { 1, 2, 3 }, ro[2] = { 9, 9 };
    cb_game_row g = { .id = "ab12cd", .status = 1, .state = st, .state_len = 3, .roster = ro, .roster_len = 2,
                      .needs_bots = true, .version = 8, .round_epoch = 5, .created_at = 100, .updated_at = 2000 };
    cb_game_commit c = { .row = &g, .expected_version = 7, .n_humans = 1, .n_bots = 1, .n_rated = 2 };
    snprintf(c.humans[0], sizeof(cb_uuid), U1);
    snprintf(c.bots[0], sizeof(cb_uuid), B1);
    snprintf(c.rated_ids[0], sizeof(cb_uuid), U1);
    c.rating_delta[0] = 12;
    snprintf(c.rated_ids[1], sizeof(cb_uuid), B1);
    c.rated_is_bot[1] = true;
    c.rating_delta[1] = -1000;
    dw_seats s = { .n_humans = 1, .n_bots = 1 };
    snprintf(s.humans[0], sizeof(cb_uuid), U2);
    snprintf(s.bots[0], sizeof(cb_uuid), B1);
    dw_rating_read rd[2] = { { .exists = true, .elo = 1000 }, { .exists = true, .elo = 990 } };
    dw_req r;
    dw_req_init(&r);
    T_CHECK(dw_game_commit(&r, "cbase", &c, &s, rd, false));
    T_EQ_STR(r.target, "DynamoDB_20120810.TransactWriteItems");
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TransactItems\":["
             "{\"Update\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"}},"
             "\"UpdateExpression\":\"SET #status = :status, #state = :state, #roster = :roster, #needs_bots = :needs_bots, "
             "#version = :version, #round_epoch = :round_epoch, #updated_at = :updated_at, #last_commit_at = :last_commit_at, "
             "#seats_exact = :seats_exact "
             "REMOVE #logs_packed, #game_seed\","
             "\"ConditionExpression\":\"#version = :expected\","
             "\"ExpressionAttributeNames\":{\"#status\":\"status\",\"#state\":\"state\",\"#roster\":\"roster\","
             "\"#logs_packed\":\"logs_packed\",\"#game_seed\":\"game_seed\",\"#needs_bots\":\"needs_bots\",\"#version\":\"version\","
             "\"#round_epoch\":\"round_epoch\",\"#updated_at\":\"updated_at\",\"#last_commit_at\":\"last_commit_at\","
             "\"#seats_exact\":\"seats_exact\"},"
             "\"ExpressionAttributeValues\":{\":status\":{\"N\":\"1\"},\":state\":{\"B\":\"AQID\"},\":roster\":{\"B\":\"CQk=\"},"
             "\":needs_bots\":{\"BOOL\":true},\":version\":{\"N\":\"8\"},\":round_epoch\":{\"N\":\"5\"},\":updated_at\":{\"N\":\"2000\"},"
             "\":last_commit_at\":{\"N\":\"2000\"},\":seats_exact\":{\"BOOL\":true},\":expected\":{\"N\":\"7\"}},"
             "\"ReturnValuesOnConditionCheckFailure\":\"ALL_OLD\"}},"
             "{\"Update\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"MEMBER#" U1 "\"}},"
             "\"UpdateExpression\":\"SET #game_id = :game_id, #user_id = :user_id, #updated_at = :updated_at, #gsi1pk = :gsi1pk, "
             "#gsi1sk = :gsi1sk, #joined_at = if_not_exists(#joined_at, :updated_at)\","
             "\"ExpressionAttributeNames\":{\"#game_id\":\"game_id\",\"#user_id\":\"user_id\",\"#updated_at\":\"updated_at\","
             "\"#gsi1pk\":\"gsi1pk\",\"#gsi1sk\":\"gsi1sk\",\"#joined_at\":\"joined_at\"},"
             "\"ExpressionAttributeValues\":{\":game_id\":{\"S\":\"ab12cd\"},\":user_id\":{\"S\":\"" U1 "\"},"
             "\":updated_at\":{\"N\":\"2000\"},\":gsi1pk\":{\"S\":\"USERGAMES#" U1 "\"},\":gsi1sk\":{\"S\":\"000000000002000#ab12cd\"}}}},"
             "{\"Delete\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"MEMBER#" U2 "\"}}}},"
             "{\"Update\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"RATING\"}},"
             "\"UpdateExpression\":\"SET #elo_rating = :elo_rating, #gsi2pk = :gsi2pk, #gsi2sk = :gsi2sk, "
             "#previous_elo = :previous_elo, #updated_at = :updated_at, "
             "#games_played = #games_played + :one\",\"ConditionExpression\":\"#elo_rating = :read\","
             "\"ExpressionAttributeNames\":{\"#elo_rating\":\"elo_rating\",\"#gsi2pk\":\"gsi2pk\",\"#gsi2sk\":\"gsi2sk\","
             "\"#previous_elo\":\"previous_elo\",\"#updated_at\":\"updated_at\",\"#games_played\":\"games_played\"},"
             "\"ExpressionAttributeValues\":{\":elo_rating\":{\"N\":\"1012\"},\":gsi2pk\":{\"S\":\"RATINGS\"},"
             "\":gsi2sk\":{\"S\":\"E#2147482635#" U1 "\"},\":previous_elo\":{\"N\":\"1000\"},"
             "\":updated_at\":{\"N\":\"2000\"},\":one\":{\"N\":\"1\"},\":read\":{\"N\":\"1000\"}}}},"
             "{\"Update\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"BOT#" B1 "\"},\"sk\":{\"S\":\"BOT\"}},"
             "\"UpdateExpression\":\"SET #elo_rating = :elo_rating, #previous_elo = :previous_elo, #updated_at = :updated_at, "
             "#games_played = #games_played + :one\",\"ConditionExpression\":\"#elo_rating = :read\","
             "\"ExpressionAttributeNames\":{\"#elo_rating\":\"elo_rating\",\"#previous_elo\":\"previous_elo\","
             "\"#updated_at\":\"updated_at\",\"#games_played\":\"games_played\"},"
             "\"ExpressionAttributeValues\":{\":elo_rating\":{\"N\":\"0\"},\":previous_elo\":{\"N\":\"990\"},"
             "\":updated_at\":{\"N\":\"2000\"},\":one\":{\"N\":\"1\"},\":read\":{\"N\":\"990\"}}}},"
             "{\"Update\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#ab12cd\"}},"
             "\"UpdateExpression\":\"SET #game_id = :game_id, #updated_at = :updated_at, #last_commit_at = :last_commit_at, "
             "#row_version = :row_version\","
             "\"ExpressionAttributeNames\":{\"#game_id\":\"game_id\",\"#updated_at\":\"updated_at\",\"#last_commit_at\":\"last_commit_at\","
             "\"#row_version\":\"row_version\"},"
             "\"ExpressionAttributeValues\":{\":game_id\":{\"S\":\"ab12cd\"},\":updated_at\":{\"N\":\"2000\"},"
             "\":last_commit_at\":{\"N\":\"2000\"},\":row_version\":{\"N\":\"8\"}}}}"
             "]}");

    // A human with no rating row but an account gets one from the start
    // rating, guarded by the account still existing; no bots left: no marker.
    rd[0] = (dw_rating_read){ .exists = false, .account = true };
    c.n_rated = 1;
    c.rating_delta[0] = -3;
    snprintf(c.rated_names[0], sizeof c.rated_names[0], "Ann");
    g.needs_bots = false;
    T_CHECK(dw_game_commit(&r, "cbase", &c, &s, rd, false));
    T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body),
                   "{\"Put\":{\"TableName\":\"cbase\",\"Item\":{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"RATING\"},"
                   "\"gsi2pk\":{\"S\":\"RATINGS\"},\"gsi2sk\":{\"S\":\"E#2147482650#" U1 "\"},\"user_id\":{\"S\":\"" U1 "\"},"
                   "\"username\":{\"S\":\"Ann\"},\"elo_rating\":{\"N\":\"997\"},\"previous_elo\":{\"N\":\"1000\"},"
                   "\"games_played\":{\"N\":\"1\"},\"updated_at\":{\"N\":\"2000\"}},"
                   "\"ConditionExpression\":\"attribute_not_exists(pk)\"}},"
                   "{\"ConditionCheck\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"USER#" U1 "\"},"
                   "\"sk\":{\"S\":\"PROFILE\"}},\"ConditionExpression\":\"attribute_exists(pk)\"}}") != NULL);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "{\"Delete\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"SCAN\"},"
                                         "\"sk\":{\"S\":\"GAME#ab12cd\"}}}}]}") != NULL);
    // A deleted account's seat is not rated at all.
    rd[0] = (dw_rating_read){ .exists = false, .account = false };
    T_CHECK(dw_game_commit(&r, "cbase", &c, &s, rd, false));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "RATING") == NULL);
    // Seats already stored are not written again: a human's seat costs its
    // write (and its GSI1 entry) once, when taken, not on every commit.
    snprintf(s.humans[0], sizeof(cb_uuid), U1);
    c.n_rated = 0;
    T_CHECK(dw_game_commit(&r, "cbase", &c, &s, NULL, false));
    T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "MEMBER#") == NULL);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "BOT#") == NULL);
    // The import writes every seat (nothing is stored to compare with).
    T_CHECK(dw_game_commit(&r, "cbase", &c, NULL, NULL, true));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "MEMBER#" U1) != NULL);
    dw_req_free(&r);
}

// A commit that changes the row alone (a move between humans: the seats
// stay, nothing is rated, no bot work before or after) is item 0 by itself,
// one UpdateItem with the transaction's own fence and refusal shape. Every
// other item there could be sends the transaction as before.
static void commit_lone(void) {
    uint8_t st[3] = { 1, 2, 3 }, ro[2] = { 9, 9 };
    cb_game_row g = { .id = "ab12cd", .status = 1, .state = st, .state_len = 3, .roster = ro, .roster_len = 2,
                      .version = 8, .round_epoch = 5, .created_at = 100, .updated_at = 2000 };
    cb_game_commit c = { .row = &g, .expected_version = 7, .n_humans = 2, .n_bots = 1, .prev_known = true };
    snprintf(c.humans[0], sizeof(cb_uuid), U1);
    snprintf(c.humans[1], sizeof(cb_uuid), U2);
    snprintf(c.bots[0], sizeof(cb_uuid), B1);
    dw_seats s = { .n_humans = 2, .n_bots = 1 };
    snprintf(s.humans[0], sizeof(cb_uuid), U2);   // the same seats, in another order
    snprintf(s.humans[1], sizeof(cb_uuid), U1);
    snprintf(s.bots[0], sizeof(cb_uuid), B1);
    dw_req r;
    dw_req_init(&r);
    T_CHECK(dw_commit_is_lone(&c, &s, NULL, false));
    T_CHECK(dw_game_commit(&r, "cbase", &c, &s, NULL, false));
    T_EQ_STR(r.target, "DynamoDB_20120810.UpdateItem");
    T_CHECK(!dw_req_is_txn(&r));
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"}},"
             "\"UpdateExpression\":\"SET #status = :status, #state = :state, #roster = :roster, #needs_bots = :needs_bots, "
             "#version = :version, #round_epoch = :round_epoch, #updated_at = :updated_at, #last_commit_at = :last_commit_at, "
             "#seats_exact = :seats_exact "
             "REMOVE #logs_packed, #game_seed\","
             "\"ConditionExpression\":\"#version = :expected\","
             "\"ExpressionAttributeNames\":{\"#status\":\"status\",\"#state\":\"state\",\"#roster\":\"roster\","
             "\"#logs_packed\":\"logs_packed\",\"#game_seed\":\"game_seed\",\"#needs_bots\":\"needs_bots\",\"#version\":\"version\","
             "\"#round_epoch\":\"round_epoch\",\"#updated_at\":\"updated_at\",\"#last_commit_at\":\"last_commit_at\","
             "\"#seats_exact\":\"seats_exact\"},"
             "\"ExpressionAttributeValues\":{\":status\":{\"N\":\"1\"},\":state\":{\"B\":\"AQID\"},\":roster\":{\"B\":\"CQk=\"},"
             "\":needs_bots\":{\"BOOL\":false},\":version\":{\"N\":\"8\"},\":round_epoch\":{\"N\":\"5\"},\":updated_at\":{\"N\":\"2000\"},"
             "\":last_commit_at\":{\"N\":\"2000\"},\":seats_exact\":{\"BOOL\":true},\":expected\":{\"N\":\"7\"}},"
             "\"ReturnValuesOnConditionCheckFailure\":\"ALL_OLD\"}");

    // Each thing that needs a second item sends the transaction instead, its
    // item 0 the same update.
#define TXN_AGAIN(why)                                                                                 \
    do {                                                                                               \
        T_CHECK(!dw_commit_is_lone(&c, &s, rd_, imp_));                                               \
        T_CHECK(dw_game_commit(&r, "cbase", &c, imp_ ? NULL : &s, rd_, imp_));                       \
        T_CHECK(dw_req_is_txn(&r));                                                                    \
        T_EQ_STR(r.target, "DynamoDB_20120810.TransactWriteItems");                                     \
        T_CHECK(valid(&r));                                                                            \
        if (!imp_) T_CHECK(strstr(cb_buf_cstr(&r.body), "\"ConditionExpression\":\"#version = :expected\"") != NULL); \
        (void)(why);                                                                                   \
    } while (0)
    const dw_rating_read *rd_ = NULL;
    bool imp_ = false;
    // The game had bot work at version 7: its SCAN marker may be there, and goes.
    c.prev_needs_bots = true;
    TXN_AGAIN("the marker is deleted");
    T_CHECK(strstr(cb_buf_cstr(&r.body), "{\"Delete\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"SCAN\"}") != NULL);
    c.prev_needs_bots = false;
    // The row says nothing of version 7 (not vouched for): the marker is deleted all the same.
    c.prev_known = false;
    TXN_AGAIN("not vouched for");
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\"pk\":{\"S\":\"SCAN\"}") != NULL);
    c.prev_known = true;
    // Bot work after: the marker is written.
    g.needs_bots = true;
    TXN_AGAIN("the marker is put");
    T_CHECK(strstr(cb_buf_cstr(&r.body), "{\"Update\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"SCAN\"}") != NULL);
    g.needs_bots = false;
    // A human seated, a human gone, a bot seated, a bot gone.
    snprintf(c.humans[1], sizeof(cb_uuid), U3);
    TXN_AGAIN("a seat taken and one left");
    T_CHECK(strstr(cb_buf_cstr(&r.body), "MEMBER#" U3) != NULL);
    snprintf(c.humans[1], sizeof(cb_uuid), U2);
    c.n_humans = 1;
    TXN_AGAIN("a human left");
    c.n_humans = 2;
    c.n_bots = 0;
    TXN_AGAIN("a bot left");
    c.n_bots = 1;
    snprintf(s.bots[0], sizeof(cb_uuid), U3);
    TXN_AGAIN("a bot seated in another's place");
    snprintf(s.bots[0], sizeof(cb_uuid), B1);
    // A rated commit (the end of a game).
    dw_rating_read rd[1] = { { .exists = true, .elo = 1000 } };
    c.n_rated = 1;
    snprintf(c.rated_ids[0], sizeof(cb_uuid), U1);
    rd_ = rd;
    TXN_AGAIN("rated");
    c.n_rated = 0;
    rd_ = NULL;
    // The import: a Put, never alone.
    imp_ = true;
    TXN_AGAIN("the import");
    imp_ = false;
#undef TXN_AGAIN
    // And back where it started: alone again.
    T_CHECK(dw_game_commit(&r, "cbase", &c, &s, NULL, false));
    T_CHECK(!dw_req_is_txn(&r));
    dw_req_free(&r);
}

static void query_shapes(void) {
    dw_req r;
    dw_req_init(&r);
    // The dashboard: GSI1 (no ConsistentRead: a GSI refuses it), newest first.
    dw_query(&r, "cbase", DWQ_USER_GAMES, U1, true, 20, CB_SPAN_LIT("{\"pk\":{\"S\":\"x\"}}"));
    T_EQ_STR(r.target, "DynamoDB_20120810.Query");
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"IndexName\":\"gsi1\",\"KeyConditionExpression\":\"#gsi1pk = :pk\","
             "\"ExpressionAttributeNames\":{\"#gsi1pk\":\"gsi1pk\"},"
             "\"ExpressionAttributeValues\":{\":pk\":{\"S\":\"USERGAMES#" U1 "\"}},"
             "\"ScanIndexForward\":false,\"Limit\":20,\"ExclusiveStartKey\":{\"pk\":{\"S\":\"x\"}}}");
    // The leaderboard: GSI2's rating-ordered keys only ("E#"), forward, bounded.
    dw_query(&r, "cbase", DWQ_RATINGS_TOP, NULL, true, 100, cb_span_of("", 0));
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"IndexName\":\"gsi2\","
             "\"KeyConditionExpression\":\"#gsi2pk = :pk AND begins_with(#gsi2sk, :sk)\","
             "\"ExpressionAttributeNames\":{\"#gsi2pk\":\"gsi2pk\",\"#gsi2sk\":\"gsi2sk\"},"
             "\"ExpressionAttributeValues\":{\":pk\":{\"S\":\"RATINGS\"},\":sk\":{\"S\":\"E#\"}},\"Limit\":100}");
    // The reindex: only the keys, only while the values are the ones read.
    cb_rating_row rt = { .elo_rating = 1210, .games_played = 4, .previous_elo = -1 };
    snprintf(rt.user_id, sizeof rt.user_id, "%s", U1);
    dw_rating_reindex(&r, "cbase", &rt);
    T_EQ_STR(r.target, "DynamoDB_20120810.UpdateItem");
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"RATING\"}},"
             "\"UpdateExpression\":\"SET #gsi2pk = :gsi2pk, #gsi2sk = :gsi2sk\","
             "\"ConditionExpression\":\"#elo_rating = :elo AND #games_played = :games AND #gsi2sk <> :gsi2sk\","
             "\"ExpressionAttributeNames\":{\"#gsi2pk\":\"gsi2pk\",\"#gsi2sk\":\"gsi2sk\",\"#elo_rating\":\"elo_rating\","
             "\"#games_played\":\"games_played\"},"
             "\"ExpressionAttributeValues\":{\":gsi2pk\":{\"S\":\"RATINGS\"},\":gsi2sk\":{\"S\":\"E#2147482437#" U1 "\"},"
             "\":elo\":{\"N\":\"1210\"},\":games\":{\"N\":\"4\"}}}");
    rt.games_played = 0;   // never played: out of the index
    dw_rating_reindex(&r, "cbase", &rt);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\"UpdateExpression\":\"REMOVE #gsi2pk, #gsi2sk\"") != NULL);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "AND attribute_exists(#gsi2pk)\"") != NULL);
    // A game's seat keys: a key range (a Query cannot filter on a key).
    dw_query(&r, "cbase", DWQ_GAME_SEATS, "ab12cd", true, 0, cb_span_of("", 0));
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"KeyConditionExpression\":\"#pk = :pk AND #sk < :sk\",\"ProjectionExpression\":\"#pk, #sk\","
             "\"ExpressionAttributeNames\":{\"#pk\":\"pk\",\"#sk\":\"sk\"},"
             "\"ExpressionAttributeValues\":{\":pk\":{\"S\":\"GAME#ab12cd\"},\":sk\":{\"S\":\"ROW\"}},\"ConsistentRead\":true}");
    // A seat is told by its key: humans and bots from the one query.
    cb_uuid id;
    bool bot = true;
    T_CHECK(dw_seat_key(CB_SPAN_LIT("{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"MEMBER#" U1 "\"}}"), id, &bot));
    T_EQ_STR(id, U1);
    T_CHECK(!bot);
    T_CHECK(dw_seat_key(CB_SPAN_LIT("{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"BOT#" B1 "\"}}"), id, &bot));
    T_EQ_STR(id, B1);
    T_CHECK(bot);
    T_CHECK(!dw_seat_key(CB_SPAN_LIT("{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"LOG\"}}"), id, &bot));
    dw_query(&r, "cbase", DWQ_SCAN, NULL, true, 0, cb_span_of("", 0));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\":pk\":{\"S\":\"SCAN\"}") != NULL);
    // Every kind is valid JSON.
    for (int k = DWQ_GAME_SEATS; k <= DWQ_USERS; k++) {
        dw_query(&r, "cbase", (dw_query_kind)k, "x", k % 2, 5, cb_span_of("", 0));
        T_CHECK(valid(&r));
    }
    dw_req_free(&r);
}

static void signup_transaction(void) {
    cb_user_row u = { .id = U1, .email = "ann@x.io", .password_hash = "$2a$10$h", .username = "Ann", .created_at = 5 };
    dw_req r;
    dw_req_init(&r);
    dw_user_create(&r, "cbase", &u, 1000);
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TransactItems\":["
             "{\"Put\":{\"TableName\":\"cbase\",\"Item\":{\"pk\":{\"S\":\"EMAIL#ann@x.io\"},\"sk\":{\"S\":\"USER\"},"
             "\"user_id\":{\"S\":\"" U1 "\"}},\"ConditionExpression\":\"attribute_not_exists(pk)\"}},"
             "{\"Put\":{\"TableName\":\"cbase\",\"Item\":{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"PROFILE\"},"
             "\"gsi2pk\":{\"S\":\"USERS\"},\"gsi2sk\":{\"S\":\"000000000000005#" U1 "\"},\"id\":{\"S\":\"" U1 "\"},"
             "\"email\":{\"S\":\"ann@x.io\"},\"password_hash\":{\"S\":\"$2a$10$h\"},\"username\":{\"S\":\"Ann\"},"
             "\"created_at\":{\"N\":\"5\"}},\"ConditionExpression\":\"attribute_not_exists(pk)\"}},"
             "{\"Put\":{\"TableName\":\"cbase\",\"Item\":{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"RATING\"},"
             "\"user_id\":{\"S\":\"" U1 "\"},"
             "\"username\":{\"S\":\"Ann\"},\"elo_rating\":{\"N\":\"1000\"},\"previous_elo\":{\"N\":\"1000\"},"
             "\"games_played\":{\"N\":\"0\"},\"updated_at\":{\"N\":\"5\"}}}}]}");
    dw_req_free(&r);
}

// Every other builder writes valid JSON, quotes and control characters in
// the values included.
static void builders_valid_json(void) {
    dw_req r;
    dw_req_init(&r);
    cb_user_row u = { .id = U1, .email = "a\"b@x.io", .password_hash = "$2b$10$x", .username = "tab\there" };
    uint8_t mv[5] = { 0, 1, 2, 3, 255 };
    cb_snapshot_row sn = { .id = U2, .game_id = "ab12cd", .n_players = 2, .moves = mv, .moves_len = 5, .created_at = 7 };
    snprintf(sn.player_ids[0], sizeof(cb_uuid), U1);
    snprintf(sn.player_ids[1], sizeof(cb_uuid), U1);   // a repeat is written once
    cb_chat_row ch = { .id = U2, .game_id = "ab12cd", .user_id = U1, .message = "line\n\"quoted\"", .created_at = 9 };
    cb_bot_row bots[1] = { { .id = B1, .nickname = "%Miami 1", .strategy_key = "random", .elo_rating = 1000, .previous_elo = -1 } };
    cb_uuid stored[2];
    snprintf(stored[0], sizeof stored[0], B1);
    snprintf(stored[1], sizeof stored[1], U2);
    cb_refresh_row rt = { .token_hash = "ff", .user_id = U1, .session_id = "s1", .created_at = 3 };
    cb_rating_row rr = { .user_id = U1, .username = "A", .elo_rating = 1200, .previous_elo = -1, .games_played = 4 };
    cb_buf key;
    cb_buf_init(&key);
    cb_buf_puts(&key, "{\"pk\":{\"S\":\"CHAT#ab12cd\"},\"sk\":{\"S\":\"x\"}}");
    const char *ids[2] = { U1, B1 };
    bool isbot[2] = { false, true };

    dw_user_import(&r, "cbase", &u);                 T_CHECK(valid(&r));
    dw_user_set_password(&r, "cbase", U1, "$2b$x", "");  T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "if_not_exists(#session_gen, :gen0) + :gen1") != NULL);
    dw_user_end_sessions(&r, "cbase", U1);           T_CHECK(valid(&r));
    dw_user_touch(&r, "cbase", U1, 5);               T_CHECK(valid(&r));
    dw_user_delete_txn(&r, "cbase", &u);             T_CHECK(valid(&r));
    dw_refresh_put(&r, "cbase", &rt);                T_CHECK(valid(&r));
    dw_refresh_take(&r, "cbase", "ff", 9, 1000);     T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "if_not_exists(#revoked_at, :now)") != NULL);
    cb_session_row se = { .id = "s1", .user_id = U1, .created_at = 1, .expires_at = 2000 };
    dw_session_put(&r, "cbase", &se);                T_CHECK(valid(&r));
    dw_session_revoke(&r, "cbase", "s1", U1, 2000);  T_CHECK(valid(&r));
    dw_get_session(&r, "cbase", "s1");               T_CHECK(valid(&r));
    dw_snapshot_put(&r, "cbase", &sn);               T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\"moves\":{\"B\":\"AAECA/8=\"}") != NULL);
    {   // the main item and ONE copy for the repeated player
        const char *p = cb_buf_cstr(&r.body);
        int puts = 0;
        for (const char *q = p; (q = strstr(q, "\"Put\"")) != NULL; q++) puts++;
        T_EQ_INT(puts, 2);
    }
    dw_snapshot_unlink(&r, "cbase", body(&(dw_req){ .body = key })); T_CHECK(valid(&r));
    dw_chat_put(&r, "cbase", &ch);                   T_CHECK(valid(&r));
    dw_batch_delete(&r, "cbase", &key, 1);           T_CHECK(valid(&r));
    T_CHECK(dw_bots_replace(&r, "cbase", bots, 1, stored, 2));
    T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "BOT#" U2) != NULL);   // the stored id not in rows goes
    dw_rating_import(&r, "cbase", &rr);              T_CHECK(valid(&r));
    dw_rating_set_username(&r, "cbase", U1, NULL);   T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "REMOVE #username") != NULL);
    dw_ratings_get(&r, "cbase", ids, isbot, 2, false); T_CHECK(valid(&r));
    dw_ratings_get(&r, "cbase", ids, isbot, 2, true);  T_CHECK(valid(&r));
    dw_scan_family(&r, "cbase", "GAME", "ab", 10, cb_span_of("", 0)); T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\":p\":{\"S\":\"GAME#ab\"}") != NULL);
    dw_execute(&r, "SELECT * FROM \"cbase\"", "tok"); T_CHECK(valid(&r));
    cb_buf keys[2];
    cb_buf_init(&keys[0]);
    cb_buf_puts(&keys[0], "{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"MEMBER#" U1 "\"}}");
    T_CHECK(dw_game_delete(&r, "cbase", "ab12cd", 4, keys, 1));
    T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\"ConditionExpression\":\"#version = :expected\"") != NULL);
    cb_buf_free(&keys[0]);
    cb_buf_free(&key);
    T_CHECK(dw_statement_reads("  select * from x"));
    T_CHECK(!dw_statement_reads("UPDATE x SET a=1"));
    T_CHECK(!dw_statement_reads("SELECTED"));
    dw_req_free(&r);
}

// ---- readers ---------------------------------------------------------------------------------

// What a Put writes, a read gives back: every row type through its item.
static void items_round_trip(void) {
    dw_req r;
    dw_req_init(&r);
    cb_span it;
    uint8_t st[4] = { 0xde, 0xad, 0xbe, 0xef }, ro[3] = { 1, 2, 3 }, lg[2] = { 7, 7 };
    cb_game_row g = { .id = "ab12cd", .status = 2, .state = st, .state_len = 4, .roster = ro, .roster_len = 3,
                      .logs = lg, .logs_len = 2, .has_seed = true, .needs_bots = true, .version = 0, .round_epoch = 0,
                      .created_at = 11, .updated_at = 12 };
    for (int i = 0; i < CB_SEED_LEN; i++) g.seed[i] = (uint8_t)(200 + i);
    dw_game_create(&r, "cbase", &g, U1);
    T_CHECK(cb_json_find((const char *)r.body.p, r.body.len, "TransactItems.0.Put.Item", &it) == CB_JSON_OBJ);
    cb_game_row back;
    T_CHECK(dw_item_game(it, &back));
    T_EQ_STR(back.id, "ab12cd");
    T_EQ_INT(back.status, 2);
    T_EQ_MEM(back.state, back.state_len, st, 4);
    T_EQ_MEM(back.roster, back.roster_len, ro, 3);
    T_EQ_MEM(back.logs, back.logs_len, lg, 2);
    T_CHECK(back.has_seed);
    T_EQ_MEM(back.seed, CB_SEED_LEN, g.seed, CB_SEED_LEN);
    T_CHECK(back.needs_bots);
    T_EQ_INT(back.created_at, 11);
    T_EQ_INT(back.last_commit_at, 12);
    cb_game_row_free(&back);
    // The creator's seat, and the SCAN marker a game that needs bots gets.
    T_CHECK(cb_json_find((const char *)r.body.p, r.body.len, "TransactItems.2.Put.Item", &it) == CB_JSON_OBJ);
    char gid[CB_GAME_ID_MAX + 1];
    int64_t up, lc;
    T_CHECK(dw_item_scan(it, gid, &up, &lc));
    T_EQ_STR(gid, "ab12cd");
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\"row_version\":{\"N\":\"") != NULL);   // the fence dw_bots_idle reads

    cb_user_row u = { .id = U1, .email = "e\"@x.io", .password_hash = "$2a$10$abc", .created_at = 3, .last_sign_in_at = 4 }, ub;
    dw_user_import(&r, "cbase", &u);
    T_CHECK(cb_json_find((const char *)r.body.p, r.body.len, "TransactItems.1.Put.Item", &it) == CB_JSON_OBJ);
    T_CHECK(dw_item_user(it, &ub));
    T_EQ_STR(ub.email, "e\"@x.io");
    T_EQ_STR(ub.username, "");
    T_EQ_INT(ub.last_sign_in_at, 4);

    uint8_t mv[2] = { 5, 6 }, ex[1] = { 9 };
    cb_snapshot_row sn = { .id = U2, .game_id = "ab12cd", .n_players = 2, .moves = mv, .moves_len = 2, .extras = ex,
                           .extras_len = 1, .created_at = 8 }, sb;
    snprintf(sn.player_ids[0], sizeof(cb_uuid), U1);
    snprintf(sn.player_ids[1], sizeof(cb_uuid), B1);
    dw_snapshot_put(&r, "cbase", &sn);
    T_CHECK(cb_json_find((const char *)r.body.p, r.body.len, "TransactItems.2.Put.Item", &it) == CB_JSON_OBJ);
    T_CHECK(dw_item_snapshot(it, &sb));
    T_EQ_INT(sb.n_players, 2);
    T_EQ_STR(sb.player_ids[1], B1);
    T_EQ_MEM(sb.moves, sb.moves_len, mv, 2);
    T_EQ_MEM(sb.extras, sb.extras_len, ex, 1);
    cb_snapshot_row_free(&sb);

    cb_chat_row ch = { .id = U2, .game_id = "ab12cd", .user_id = U1, .message = "hé \"x\"", .is_system = true,
                       .created_at = 9 }, cbk;
    dw_chat_put(&r, "cbase", &ch);
    T_CHECK(cb_json_find((const char *)r.body.p, r.body.len, "TransactItems.0.Put.Item", &it) == CB_JSON_OBJ);
    T_CHECK(dw_item_chat(it, &cbk));
    T_EQ_STR(cbk.message, ch.message);
    T_CHECK(cbk.is_system);
    cb_chat_row_free(&cbk);

    cb_bot_row bot = { .id = B1, .nickname = "%Seoul 3", .strategy_key = "robusta", .elo_rating = 1111, .previous_elo = 1100,
                       .games_played = 6, .created_at = 1, .updated_at = 2 }, bb;
    cb_uuid none[1];
    dw_bots_replace(&r, "cbase", &bot, 1, none, 0);
    T_CHECK(cb_json_find((const char *)r.body.p, r.body.len, "TransactItems.0.Put.Item", &it) == CB_JSON_OBJ);
    T_CHECK(dw_item_bot(it, &bb));
    T_EQ_STR(bb.nickname, "%Seoul 3");
    T_EQ_INT(bb.elo_rating, 1111);
    T_EQ_INT(bb.previous_elo, 1100);

    cb_rating_row rt = { .user_id = U1, .elo_rating = 900, .previous_elo = -1, .games_played = 2, .updated_at = 5 }, rb;
    dw_rating_import(&r, "cbase", &rt);
    T_CHECK(cb_json_find((const char *)r.body.p, r.body.len, "Item", &it) == CB_JSON_OBJ);
    T_CHECK(dw_item_rating(it, &rb));
    T_EQ_INT(rb.elo_rating, 900);
    T_EQ_INT(rb.previous_elo, -1);
    T_EQ_STR(rb.username, "");
    dw_req_free(&r);
}

static const char COND_WITH_ITEM[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#ConditionalCheckFailedException\","
    "\"message\":\"The conditional request failed\",\"Item\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"},"
    "\"version\":{\"N\":\"8\"}}}";
static const char COND_NO_ITEM[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#ConditionalCheckFailedException\","
    "\"message\":\"The conditional request failed\"}";
static const char CAS_LOST[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
    "\"CancellationReasons\":[{\"Code\":\"ConditionalCheckFailed\",\"Item\":{\"pk\":{\"S\":\"GAME#ab12cd\"},"
    "\"sk\":{\"S\":\"ROW\"},\"version\":{\"N\":\"8\"}},\"Message\":\"The conditional request failed\"},"
    "{\"Code\":\"None\"},{\"Code\":\"None\"}],"
    "\"Message\":\"Transaction cancelled, please refer cancellation reasons for specific reasons [ConditionalCheckFailed, None, None]\"}";
static const char GAME_GONE[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
    "\"CancellationReasons\":[{\"Code\":\"ConditionalCheckFailed\",\"Message\":\"The conditional request failed\"},"
    "{\"Code\":\"None\"}],\"Message\":\"Transaction cancelled\"}";
static const char RATING_MOVED[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
    "\"CancellationReasons\":[{\"Code\":\"None\"},{\"Code\":\"None\"},{\"Code\":\"ConditionalCheckFailed\","
    "\"Message\":\"The conditional request failed\"},{\"Code\":\"None\"}],\"Message\":\"Transaction cancelled\"}";
static const char TXN_CONFLICT[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
    "\"CancellationReasons\":[{\"Code\":\"None\"},{\"Code\":\"TransactionConflict\","
    "\"Message\":\"Transaction is ongoing for the item\"}],\"Message\":\"Transaction cancelled\"}";
static const char THROTTLED[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#ProvisionedThroughputExceededException\","
    "\"message\":\"The level of configured provisioned throughput for the table was exceeded.\"}";
static const char VALIDATION[] =
    "{\"__type\":\"com.amazon.coral.validate#ValidationException\",\"message\":\"One or more parameter values were invalid\"}";

static void errors_classified(void) {
    T_EQ_INT(dw_classify(200, "{}", 2), DW_OK);
    T_EQ_INT(dw_classify(400, COND_WITH_ITEM, strlen(COND_WITH_ITEM)), DW_COND_FAILED);
    T_CHECK(dw_cond_failed_has_item(COND_WITH_ITEM, strlen(COND_WITH_ITEM)));
    T_CHECK(!dw_cond_failed_has_item(COND_NO_ITEM, strlen(COND_NO_ITEM)));
    T_EQ_INT(dw_classify(400, CAS_LOST, strlen(CAS_LOST)), DW_TXN_CANCELED);
    T_EQ_INT(dw_classify(400, THROTTLED, strlen(THROTTLED)), DW_RETRY);
    T_EQ_INT(dw_classify(500, "{}", 2), DW_RETRY);
    T_EQ_INT(dw_classify(503, "", 0), DW_RETRY);
    T_EQ_INT(dw_classify(400, VALIDATION, strlen(VALIDATION)), DW_FAILED);
    T_EQ_INT(dw_classify(403, "not json", 8), DW_FAILED);
    char why[200];
    dw_error_text(VALIDATION, strlen(VALIDATION), why, sizeof why);
    T_EQ_STR(why, "ValidationException: One or more parameter values were invalid");

    bool item;
    T_EQ_INT(dw_cancel_reason(CAS_LOST, strlen(CAS_LOST), 0, &item), DW_REASON_COND);
    T_CHECK(item);
    T_EQ_INT(dw_cancel_reason(CAS_LOST, strlen(CAS_LOST), 1, &item), DW_REASON_NONE);
    T_CHECK(!item);
    T_EQ_INT(dw_cancel_reason(CAS_LOST, strlen(CAS_LOST), 3, &item), DW_REASON_MISSING);
    T_EQ_INT(dw_cancel_reason(GAME_GONE, strlen(GAME_GONE), 0, &item), DW_REASON_COND);
    T_CHECK(!item);
    T_EQ_INT(dw_cancel_reason(TXN_CONFLICT, strlen(TXN_CONFLICT), 1, &item), DW_REASON_CONFLICT);
}

static void pages_and_batches(void) {
    static const char Q[] =
        "{\"Count\":2,\"Items\":[{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#aaaaaa\"},\"game_id\":{\"S\":\"aaaaaa\"},"
        "\"updated_at\":{\"N\":\"5\"},\"last_commit_at\":{\"N\":\"4\"}},"
        "{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#bbbbbb\"},\"game_id\":{\"S\":\"bbbbbb\"},"
        "\"updated_at\":{\"N\":\"6\"},\"last_commit_at\":{\"N\":\"6\"}}],"
        "\"LastEvaluatedKey\":{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#bbbbbb\"}},\"ScannedCount\":2}";
    cb_span items = dw_items(Q, strlen(Q)), it;
    size_t pos = 0;
    int n = 0;
    while (dw_array_next(items, &pos, &it)) {
        char id[CB_GAME_ID_MAX + 1];
        int64_t up, lc;
        T_CHECK(dw_item_scan(it, id, &up, &lc));
        n++;
        if (n == 2) {
            T_EQ_STR(id, "bbbbbb");
            T_EQ_INT(up, 6);
        }
        cb_buf k;
        cb_buf_init(&k);
        T_CHECK(dw_item_key(it, &k));
        if (n == 1) T_EQ_STR(cb_buf_cstr(&k), "{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#aaaaaa\"}}");
        cb_buf_free(&k);
    }
    T_EQ_INT(n, 2);
    cb_span last = dw_last_key(Q, strlen(Q));
    T_EQ_MEM(last.p, last.n, "{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#bbbbbb\"}}", 44);
    T_EQ_INT(dw_last_key("{\"Items\":[]}", 12).n, 0);
    T_EQ_INT(dw_items("{\"Count\":0}", 11).n, 2);   // "[]"

    T_CHECK(dw_unprocessed("{\"UnprocessedItems\":{\"cbase\":[{}]}}", 35));
    T_CHECK(!dw_unprocessed("{\"UnprocessedItems\":{}}", 23));
    T_CHECK(!dw_unprocessed("{\"Responses\":{}}", 16));

    // BatchGetItem: a bot's row wins over a user row of the same id; PROFILE marks an account.
    static const char BG[] =
        "{\"Responses\":{\"cbase\":["
        "{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"RATING\"},\"elo_rating\":{\"N\":\"1200\"}},"
        "{\"pk\":{\"S\":\"BOT#" B1 "\"},\"sk\":{\"S\":\"BOT\"},\"elo_rating\":{\"N\":\"950\"}},"
        "{\"pk\":{\"S\":\"USER#" U2 "\"},\"sk\":{\"S\":\"PROFILE\"}}]},\"UnprocessedKeys\":{}}";
    const char *ids[3] = { U1, B1, U2 };
    bool isbot[3] = { false, true, false };
    dw_rating_read rd[3];
    dw_ratings_read(BG, strlen(BG), "cbase", ids, isbot, 3, false, rd);
    T_CHECK(rd[0].exists && rd[0].elo == 1200);
    T_CHECK(rd[1].exists && rd[1].elo == 950);
    T_CHECK(!rd[2].exists && rd[2].account);
    dw_ratings_read(BG, strlen(BG), "other", ids, isbot, 3, false, rd);
    T_CHECK(!rd[0].exists);

    // The admin console's text of an item.
    static const char IT[] = "{\"pk\":{\"S\":\"GAME#x\"},\"version\":{\"N\":\"12\"},\"state\":{\"B\":\"AQI=\"},"
                             "\"needs_bots\":{\"BOOL\":false},\"gone\":{\"NULL\":true},\"topics\":{\"L\":[]}}";
    cb_buf store;
    cb_buf_init(&store);
    const char *names[8], *values[8];
    T_EQ_INT(dw_item_text(cb_span_of(IT, strlen(IT)), &store, names, values, 8), 6);
    T_EQ_STR(names[0], "pk");
    T_EQ_STR(values[0], "GAME#x");
    T_EQ_STR(values[1], "12");
    T_EQ_STR(values[2], "\\x0102");
    T_EQ_STR(values[3], "false");
    T_CHECK(values[4] == NULL);
    T_EQ_STR(values[5], "[]");   // a list or map: its raw JSON
    cb_buf_free(&store);
}

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

static const char EMPTY[] = "{\"Count\":0,\"Items\":[],\"ScannedCount\":0}";

static cb_game_commit mkcommit(cb_game_row *g, uint8_t *st) {
    *g = (cb_game_row){ .id = "ab12cd", .status = 1, .state = st, .state_len = 1, .roster = st, .roster_len = 1,
                        .version = 8, .updated_at = 2000 };
    cb_game_commit c = { .row = g, .expected_version = 7 };
    return c;
}

// Two writers read version 7; the second one's transaction is refused at item
// 0 with the stored item attached: CB_E_CONFLICT, and no second attempt.
static void flow_cas_lost(void) {
    static const step steps[] = {
        { "Query", "#sk < :sk", 200, EMPTY },
        { "TransactWriteItems", "\":expected\":{\"N\":\"7\"}", 400, CAS_LOST },
    };
    RUN(steps, s);
    uint8_t st[1] = { 1 };
    cb_game_row g;
    cb_game_commit c = mkcommit(&g, st);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_E_CONFLICT);
    T_EQ_INT(sc_.at, 2);
    cb_store_close(s);
}

static void flow_commit_game_gone(void) {
    static const step steps[] = {
        { "Query", "#sk < :sk", 200, EMPTY },
        { "TransactWriteItems", NULL, 400, GAME_GONE },
    };
    RUN(steps, s);
    uint8_t st[1] = { 1 };
    cb_game_row g;
    cb_game_commit c = mkcommit(&g, st);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_E_NOT_FOUND);
    cb_store_close(s);
}

// A rating another table changed between the read and the write: the
// transaction is refused at the rating, the commit reads again and wins.
static void flow_rating_moved(void) {
    static const char R1[] = "{\"Responses\":{\"cbase\":[{\"pk\":{\"S\":\"BOT#" B1 "\"},\"sk\":{\"S\":\"BOT\"},"
                             "\"elo_rating\":{\"N\":\"1000\"}}]},\"UnprocessedKeys\":{}}";
    static const char R2[] = "{\"Responses\":{\"cbase\":[{\"pk\":{\"S\":\"BOT#" B1 "\"},\"sk\":{\"S\":\"BOT\"},"
                             "\"elo_rating\":{\"N\":\"1010\"}}]},\"UnprocessedKeys\":{}}";
    static const char BOTSEAT[] = "{\"Items\":[{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"BOT#" B1 "\"},"
                                  "\"game_id\":{\"S\":\"ab12cd\"},\"bot_id\":{\"S\":\"" B1 "\"}}]}";
    static const step steps[] = {
        { "Query", "#sk < :sk", 200, BOTSEAT },
        { "BatchGetItem", "BOT#" B1, 200, R1 },
        { "TransactWriteItems", "\":read\":{\"N\":\"1000\"}", 400, RATING_MOVED },
        { "Query", "#sk < :sk", 200, BOTSEAT },
        { "BatchGetItem", "BOT#" B1, 200, R2 },
        { "TransactWriteItems", "\":elo_rating\":{\"N\":\"1015\"},\":previous_elo\":{\"N\":\"1010\"}", 200, "{}" },
    };
    RUN(steps, s);
    uint8_t st[1] = { 1 };
    cb_game_row g;
    cb_game_commit c = mkcommit(&g, st);
    c.n_bots = 1;
    snprintf(c.bots[0], sizeof(cb_uuid), B1);
    c.n_rated = 1;
    snprintf(c.rated_ids[0], sizeof(cb_uuid), B1);
    c.rated_is_bot[0] = true;
    c.rating_delta[0] = 5;
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(sc_.at, 6);
    // The seat query told the stored bot from a human: its seat is not put again.
    T_CHECK(strstr(sc_.bodies[5], "\"sk\":{\"S\":\"BOT#" B1 "\"}") == NULL);
    cb_store_close(s);
}

// A newly seated human whose account is gone is left out of the commit.
static void flow_commit_drops_missing_account(void) {
    static const char BG[] = "{\"Responses\":{\"cbase\":[{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"PROFILE\"}}]}}";
    static const step steps[] = {
        { "Query", "#sk < :sk", 200, EMPTY },
        { "BatchGetItem", "PROFILE", 200, BG },
        { "TransactWriteItems", "MEMBER#" U1, 200, "{}" },
    };
    RUN(steps, s);
    uint8_t st[1] = { 1 };
    cb_game_row g;
    cb_game_commit c = mkcommit(&g, st);
    c.n_humans = 2;
    snprintf(c.humans[0], sizeof(cb_uuid), U1);
    snprintf(c.humans[1], sizeof(cb_uuid), U2);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_OK);
    T_CHECK(strstr(sc_.bodies[2], "MEMBER#" U2) == NULL);
    cb_store_close(s);
}

// The row the commit is fenced on vouched for its seats (prev_known): no Query.
// The seat that stays is not written, the new one is put after its account is
// checked, the one that left is deleted (whether or not its item is there).
static void flow_commit_seats_given(void) {
    static const char BG[] = "{\"Responses\":{\"cbase\":[{\"pk\":{\"S\":\"USER#" U2 "\"},\"sk\":{\"S\":\"PROFILE\"}}]}}";
    static const step steps[] = {
        { "BatchGetItem", "USER#" U2, 200, BG },
        { "TransactWriteItems", "#seats_exact = :seats_exact", 200, "{}" },
    };
    RUN(steps, s);
    uint8_t st[1] = { 1 };
    cb_game_row g;
    cb_game_commit c = mkcommit(&g, st);
    c.n_humans = 2;
    snprintf(c.humans[0], sizeof(cb_uuid), U1);
    snprintf(c.humans[1], sizeof(cb_uuid), U2);
    c.prev_known = true;
    c.n_prev_humans = 2;
    snprintf(c.prev_humans[0], sizeof(cb_uuid), U1);
    snprintf(c.prev_humans[1], sizeof(cb_uuid), U3);
    c.n_prev_bots = 1;
    snprintf(c.prev_bots[0], sizeof(cb_uuid), B1);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(sc_.at, 2);
    const char *tx = sc_.bodies[1];
    T_CHECK(strstr(tx, "\"sk\":{\"S\":\"MEMBER#" U1 "\"}") == NULL);
    T_CHECK(strstr(tx, "{\"Update\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"MEMBER#" U2 "\"}}") != NULL);
    T_CHECK(strstr(tx, "{\"Delete\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"MEMBER#" U3 "\"}}") != NULL);
    T_CHECK(strstr(tx, "{\"Delete\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"BOT#" B1 "\"}}") != NULL);
    cb_store_close(s);
}

// Without them the seats are read, as before: one Query, then the transaction.
static void flow_commit_seats_read(void) {
    static const char SEATS[] = "{\"Items\":[{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"MEMBER#" U1 "\"}}]}";
    static const step steps[] = {
        { "Query", "#sk < :sk", 200, SEATS },
        { "TransactWriteItems", "#seats_exact = :seats_exact", 200, "{}" },
    };
    RUN(steps, s);
    uint8_t st[1] = { 1 };
    cb_game_row g;
    cb_game_commit c = mkcommit(&g, st);
    c.n_humans = 1;
    snprintf(c.humans[0], sizeof(cb_uuid), U1);
    c.n_prev_humans = 1;   // given, but not vouched for: not used
    snprintf(c.prev_humans[0], sizeof(cb_uuid), U3);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(sc_.at, 2);
    T_CHECK(strstr(sc_.bodies[1], "MEMBER#" U3) == NULL);
    T_CHECK(strstr(sc_.bodies[1], "MEMBER#" U1) == NULL);   // stored and kept: not written
    cb_store_close(s);
}

// A move between humans: the row vouches for its seats and had no bot work,
// so the commit is one conditional UpdateItem: no Query, no transaction.
static cb_game_commit mklone(cb_game_row *g, uint8_t *st) {
    cb_game_commit c = mkcommit(g, st);
    c.n_humans = 1;
    snprintf(c.humans[0], sizeof(cb_uuid), U1);
    c.prev_known = true;
    c.n_prev_humans = 1;
    snprintf(c.prev_humans[0], sizeof(cb_uuid), U1);
    return c;
}

static void flow_commit_lone(void) {
    static const step steps[] = {
        { "UpdateItem", "\"ConditionExpression\":\"#version = :expected\"", 200, "{}" },
    };
    RUN(steps, s);
    uint8_t st[1] = { 1 };
    cb_game_row g;
    cb_game_commit c = mklone(&g, st);
    cb_store_tally t;
    cb_store_tally_take(&t);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(sc_.at, 1);
    cb_store_tally_take(&t);
    T_EQ_STR(t.detail, "UpdateItem 1");
    T_CHECK(strstr(sc_.bodies[0], "\":expected\":{\"N\":\"7\"}") != NULL);
    T_CHECK(strstr(sc_.bodies[0], "\":version\":{\"N\":\"8\"}") != NULL);
    cb_store_close(s);
}

// The lone commit loses the CAS: the refusal carries the stored row, so it is
// CB_E_CONFLICT at once, sent once (the app reloads, as for the transaction).
static void flow_commit_lone_cas_lost(void) {
    static const step steps[] = {
        { "UpdateItem", "\":expected\":{\"N\":\"7\"}", 400, COND_WITH_ITEM },
    };
    RUN(steps, s);
    uint8_t st[1] = { 1 };
    cb_game_row g;
    cb_game_commit c = mklone(&g, st);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_E_CONFLICT);
    T_EQ_INT(sc_.at, 1);
    cb_store_close(s);
}

// Refused with no item: the game is gone (the condition is never met on an
// absent row, so the update creates nothing).
static void flow_commit_lone_game_gone(void) {
    static const step steps[] = {
        { "UpdateItem", NULL, 400, COND_NO_ITEM },
    };
    RUN(steps, s);
    uint8_t st[1] = { 1 };
    cb_game_row g;
    cb_game_commit c = mklone(&g, st);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_E_NOT_FOUND);
    T_EQ_INT(sc_.at, 1);
    cb_store_close(s);
}

// Another transaction holds the row for a moment (a create or a rated commit
// in flight): DynamoDB refuses a lone write with TransactionConflictException,
// and the update is sent again after a pause and wins; a refusal that is not
// a condition or a transient one is CB_E_DB.
static const char LONE_CONFLICT[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionConflictException\","
    "\"message\":\"Transaction is ongoing for the item\"}";
static void flow_commit_lone_retried(void) {
    static const step steps[] = {
        { "UpdateItem", NULL, 400, LONE_CONFLICT },
        { "UpdateItem", NULL, 200, "{}" },
        { "UpdateItem", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    uint8_t st[1] = { 1 };
    cb_game_row g;
    cb_game_commit c = mklone(&g, st);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(sc_.at, 2);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_E_DB);
    T_EQ_INT(sc_.at, 3);
    cb_store_close(s);
}

// The row had bot work: the transaction, which deletes the marker.
static void flow_commit_after_bot_work(void) {
    static const step steps[] = {
        { "TransactWriteItems", "{\"Delete\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"SCAN\"}", 200, "{}" },
    };
    RUN(steps, s);
    uint8_t st[1] = { 1 };
    cb_game_row g;
    cb_game_commit c = mklone(&g, st);
    c.prev_needs_bots = true;
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(sc_.at, 1);
    cb_store_close(s);
}

static void flow_lease(void) {
    static const step steps[] = {
        { "UpdateItem", "attribute_not_exists(#lease_until)", 400, COND_WITH_ITEM },   // held by another
        { "UpdateItem", NULL, 400, COND_NO_ITEM },                                      // no marker: no game, or no bot work
        { "UpdateItem", "\":updated_at\":{\"N\":\"5000\"}", 200, "{}" },                 // taken: one write
        { "UpdateItem", "#lease_token = :token", 400, COND_NO_ITEM },                    // renew, wrong token
        { "UpdateItem", "REMOVE #lease_until", 400, COND_NO_ITEM },                      // release, not ours
    };
    RUN(steps, s);
    cb_uuid tok;
    T_EQ_INT(CB_STORE_CALL(s, lease_acquire, "ab12cd", 5000, 25000, tok), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, lease_acquire, "nogame", 5000, 25000, tok), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, lease_acquire, "ab12cd", 5000, 25000, tok), CB_OK);
    T_CHECK(strstr(sc_.bodies[2], tok) != NULL);   // the token returned is the one written
    T_EQ_INT(CB_STORE_CALL(s, lease_renew, "ab12cd", "00000000-0000-4000-8000-000000000000", 6000, 25000), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, lease_release, "ab12cd", "00000000-0000-4000-8000-000000000000"), CB_OK);
    T_EQ_INT(sc_.at, 5);
    cb_store_close(s);
}

// Throttled twice, then answered: the read succeeds on the third send.
static void flow_throttle_retry(void) {
    static const char GOT[] = "{\"Item\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"},\"id\":{\"S\":\"ab12cd\"},"
                              "\"status\":{\"N\":\"0\"},\"state\":{\"B\":\"AQ==\"},\"roster\":{\"B\":\"Ag==\"},"
                              "\"needs_bots\":{\"BOOL\":false},\"version\":{\"N\":\"3\"},\"round_epoch\":{\"N\":\"0\"},"
                              "\"created_at\":{\"N\":\"1\"},\"updated_at\":{\"N\":\"2\"},\"last_commit_at\":{\"N\":\"2\"}}}";
    static const step steps[] = {
        { "GetItem", NULL, 400, THROTTLED },
        { "GetItem", NULL, 500, "{}" },
        { "GetItem", NULL, 200, GOT },
        { "GetItem", NULL, 200, "{}" },   // an absent item
        { "GetItem", NULL, 400, VALIDATION },
    };
    RUN(steps, s);
    cb_game_row g = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ab12cd", &g), CB_OK);
    T_EQ_INT(g.version, 3);
    T_EQ_INT(g.state_len, 1);
    cb_game_row_free(&g);
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ab12cd", &g), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ab12cd", &g), CB_E_DB);   // not retried
    T_EQ_INT(sc_.at, 5);
    cb_store_close(s);
}

// Throttling, as DynamoDB answers it on a provisioned table: the two error
// bodies (the documented __type and message of each), and a transaction
// cancelled for throttling (the CancellationReasons codes DynamoDB documents
// for TransactionCanceledException). Written from the API reference, not
// captured from AWS: DynamoDB Local never throttles.
static const char THROTTLED_PT[] =
    "{\"__type\":\"com.amazonaws.dynamodb.v20120810#ProvisionedThroughputExceededException\","
    "\"message\":\"The level of configured provisioned throughput for the table was exceeded. "
    "Consider increasing your provisioning level with the UpdateTable API.\"}";
static const char THROTTLED_RATE[] =
    "{\"__type\":\"com.amazon.coral.availability#ThrottlingException\","
    "\"message\":\"Rate of requests exceeds the allowed throughput.\"}";
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
static uint64_t gave_up_since(const cb_store_throttles *t0) {
    cb_store_throttles t;
    cb_store_throttles_get(&t);
    return t.gave_up - t0->gave_up;
}

// Both throttles, then answered: the read succeeds, and the overview's
// counter saw two throttled responses and nothing given up.
static void flow_throttled_then_answered(void) {
    static const step steps[] = {
        { "GetItem", NULL, 400, THROTTLED_PT },
        { "GetItem", NULL, 400, THROTTLED_RATE },
        { "GetItem", NULL, 200, "{}" },
    };
    RUN(steps, s);
    cb_store_throttles t0;
    cb_store_throttles_get(&t0);
    cb_game_row g = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ab12cd", &g), CB_E_NOT_FOUND);
    T_EQ_INT(sc_.at, 3);
    T_EQ_INT(throttled_since(&t0), 2);
    T_EQ_INT(gave_up_since(&t0), 0);
    cb_store_close(s);
}

// A Lambda deadline too close for a pause: the throttled request is not sent
// again, it fails at once, and is counted as given up.
static void flow_throttled_past_the_deadline(void) {
    static const step steps[] = {
        { "GetItem", NULL, 400, THROTTLED_PT },
        { "GetItem", NULL, 200, "{}" },   // never reached
    };
    RUN(steps, s);
    cb_store_throttles t0;
    cb_store_throttles_get(&t0);
    cb_store_set_deadline(cb_now_ms() + 200);
    int64_t t = cb_mono_ms();
    cb_game_row g = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ab12cd", &g), CB_E_BUSY);
    T_CHECK(cb_mono_ms() - t < 200);
    cb_store_set_deadline(0);
    T_EQ_INT(sc_.at, 1);
    T_EQ_INT(throttled_since(&t0), 1);
    T_EQ_INT(gave_up_since(&t0), 1);
    cb_store_close(s);
}

// Throttling never ends: with no deadline the pauses stop at the wall cap,
// well short of forever, and the request fails.
static void flow_throttled_to_the_wall(void) {
    static const step steps[] = {
        { "GetItem", NULL, 400, THROTTLED_RATE }, { "GetItem", NULL, 400, THROTTLED_RATE },
        { "GetItem", NULL, 400, THROTTLED_RATE }, { "GetItem", NULL, 400, THROTTLED_RATE },
        { "GetItem", NULL, 400, THROTTLED_RATE }, { "GetItem", NULL, 400, THROTTLED_RATE },
        { "GetItem", NULL, 400, THROTTLED_RATE }, { "GetItem", NULL, 400, THROTTLED_RATE },
        { "GetItem", NULL, 400, THROTTLED_RATE }, { "GetItem", NULL, 400, THROTTLED_RATE },
        { "GetItem", NULL, 400, THROTTLED_RATE }, { "GetItem", NULL, 400, THROTTLED_RATE },
        { "GetItem", NULL, 400, THROTTLED_RATE }, { "GetItem", NULL, 400, THROTTLED_RATE },
        { "GetItem", NULL, 400, THROTTLED_RATE }, { "GetItem", NULL, 400, THROTTLED_RATE },
    };
    RUN(steps, s);
    cb_store_throttles t0;
    cb_store_throttles_get(&t0);
    int64_t t = cb_mono_ms();
    cb_game_row g = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ab12cd", &g), CB_E_BUSY);
    int64_t took = cb_mono_ms() - t;
    T_CHECK(took <= CB_STORE_RETRY_WALL_MS + 500);
    T_CHECK(sc_.at >= 2 && sc_.at < 16);   // sent again, and stopped
    T_EQ_INT(throttled_since(&t0), (uint64_t)sc_.at);
    T_EQ_INT(gave_up_since(&t0), 1);
    cb_store_close(s);
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

// A batch DynamoDB left partly unprocessed (its way of throttling a batch)
// is counted as throttled and sent again.
static void flow_batch_unprocessed(void) {
    static const char PART[] = "{\"Responses\":{\"cbase\":[]},\"UnprocessedKeys\":{\"cbase\":{\"Keys\":"
                               "[{\"pk\":{\"S\":\"BOT#" B1 "\"},\"sk\":{\"S\":\"BOT\"}}]}}}";
    static const char ALL[] = "{\"Responses\":{\"cbase\":[{\"pk\":{\"S\":\"BOT#" B1 "\"},\"sk\":{\"S\":\"BOT\"},"
                              "\"elo_rating\":{\"N\":\"1234\"}}]},\"UnprocessedKeys\":{}}";
    static const step steps[] = {
        { "BatchGetItem", NULL, 200, PART },
        { "BatchGetItem", NULL, 200, ALL },
    };
    RUN(steps, s);
    cb_store_throttles t0;
    cb_store_throttles_get(&t0);
    const char *ids[1] = { B1 };
    int32_t elo[1] = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, ratings_for, ids, 1, elo), CB_OK);
    T_EQ_INT(elo[0], 1234);
    T_EQ_INT(throttled_since(&t0), 1);
    cb_store_close(s);
}

// The dashboard's rows: one BatchGetItem per 100 games (not a GetItem each),
// without the session log, the unprocessed keys and only they sent again,
// rows handed over in the order asked, a game that does not exist skipped.
static void games_get_shape(void) {
    dw_req r;
    dw_req_init(&r);
    const char *ids[3] = { "ab12cd", "ef34gh", "ab12cd" };
    dw_games_get(&r, "cbase", ids, 3);
    T_EQ_STR(r.target, "DynamoDB_20120810.BatchGetItem");
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"RequestItems\":{\"cbase\":{\"Keys\":["
             "{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"}},{\"pk\":{\"S\":\"GAME#ef34gh\"},\"sk\":{\"S\":\"ROW\"}}],"
             "\"ConsistentRead\":true,\"ProjectionExpression\":\"#c0, #c1, #c2, #c3, #c4, #c5, #c6, #c7, #c8, #c9, #c10\","
             "\"ExpressionAttributeNames\":{\"#c0\":\"id\",\"#c1\":\"status\",\"#c2\":\"version\",\"#c3\":\"state\","
             "\"#c4\":\"roster\",\"#c5\":\"game_seed\",\"#c6\":\"needs_bots\",\"#c7\":\"round_epoch\",\"#c8\":\"created_at\","
             "\"#c9\":\"updated_at\",\"#c10\":\"last_commit_at\"}}}}");
    T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "logs_packed") == NULL);
    dw_req_free(&r);
}

#define GB_N 130
typedef struct {
    int  calls, keys[8];
    char asked[8][GB_N][8];
} games_srv;

// A table with every game but g00007, answering the first request with all
// but two of its keys left unprocessed (g00100, g00050); every key is read
// from the request body itself.
static int games_send(void *ud, const char *target, const uint8_t *b, size_t len, int *status, cb_buf *resp) {
    games_srv *g = ud;
    T_EQ_STR(target, "DynamoDB_20120810.BatchGetItem");
    T_CHECK(cb_json_valid((const char *)b, len));
    int c = g->calls < 8 ? g->calls : 7;
    g->calls++;
    char body[65536];
    snprintf(body, sizeof body, "%.*s", (int)len, (const char *)b);
    cb_buf_puts(resp, "{\"Responses\":{\"cbase\":[");
    cb_buf un;
    cb_buf_init(&un);
    int items = 0, held = 0;
    g->keys[c] = 0;
    for (const char *p = body; (p = strstr(p, "GAME#")) != NULL; p += 5) {
        char id[8];
        snprintf(id, sizeof id, "%.6s", p + 5);
        if (g->keys[c] < GB_N) snprintf(g->asked[c][g->keys[c]], 8, "%s", id);
        g->keys[c]++;
        if (strcmp(id, "g00007") == 0) continue;
        if (c == 0 && (strcmp(id, "g00100") == 0 || strcmp(id, "g00050") == 0)) {
            cb_buf_printf(&un, "%s{\"pk\":{\"S\":\"GAME#%s\"},\"sk\":{\"S\":\"ROW\"}}", held++ ? "," : "", id);
            continue;
        }
        cb_buf_printf(resp, "%s{\"pk\":{\"S\":\"GAME#%s\"},\"sk\":{\"S\":\"ROW\"},\"id\":{\"S\":\"%s\"},"
                            "\"status\":{\"N\":\"1\"},\"version\":{\"N\":\"%d\"},\"state\":{\"B\":\"AQ==\"},"
                            "\"roster\":{\"B\":\"Ag==\"}}",
                      items++ ? "," : "", id, id, atoi(id + 1));
    }
    cb_buf_puts(resp, "]},\"UnprocessedKeys\":{");
    if (held) cb_buf_printf(resp, "\"cbase\":{\"Keys\":[%s]}", cb_buf_cstr(&un));
    cb_buf_puts(resp, "}}");
    cb_buf_free(&un);
    *status = 200;
    return CB_OK;
}

typedef struct { int n; char ids[GB_N][CB_GAME_ID_MAX + 1]; bool logs; } rows_seen;
static bool seen_row(const cb_game_row *r, void *ud) {
    rows_seen *s = ud;
    if (s->n < GB_N) snprintf(s->ids[s->n++], sizeof s->ids[0], "%s", r->id);
    s->logs |= r->logs != NULL || r->logs_len != 0;
    return true;
}

static void flow_games_batched(void) {
    games_srv g = { 0 };
    cb_store *s = NULL;
    T_EQ_INT(cb_store_open_dynamo_with("cbase", games_send, &g, &s), CB_OK);
    char idbuf[GB_N][8];
    const char *ids[GB_N];
    for (int i = 0; i < GB_N; i++) {
        snprintf(idbuf[i], sizeof idbuf[i], "g%05d", GB_N - i);   // newest first, not sorted
        ids[i] = idbuf[i];
    }
    cb_store_throttles t0;
    cb_store_throttles_get(&t0);
    rows_seen seen = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, games_get_views, ids, GB_N, seen_row, &seen), GB_N - 1);
    // 100 keys, then the two left unprocessed alone, then the last 30.
    T_EQ_INT(g.calls, 3);
    T_EQ_INT(g.keys[0], 100);
    T_EQ_INT(g.keys[1], 2);
    T_CHECK((strcmp(g.asked[1][0], "g00050") == 0 && strcmp(g.asked[1][1], "g00100") == 0) ||
            (strcmp(g.asked[1][0], "g00100") == 0 && strcmp(g.asked[1][1], "g00050") == 0));
    T_EQ_INT(g.keys[2], 30);
    T_EQ_INT(throttled_since(&t0), 1);
    // In the order asked, g00007 (no such game) left out, no session logs.
    T_EQ_INT(seen.n, GB_N - 1);
    int at = 0;
    for (int i = 0; i < GB_N; i++) {
        if (strcmp(ids[i], "g00007") == 0) continue;
        if (at < seen.n && strcmp(seen.ids[at], ids[i]) != 0) {
            T_EQ_STR(seen.ids[at], ids[i]);
            break;
        }
        at++;
    }
    T_CHECK(!seen.logs);
    cb_store_close(s);
}

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
        { "UpdateItem", "if_not_exists(#expires_ms, :exp)", 200, "{}" },
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
static bool collect_id(const char *id, void *ud) {
    ids_t *t = ud;
    if (t->n < 8) snprintf(t->ids[t->n++], 16, "%s", id);
    return true;
}

// The heartbeat over two pages of the SCAN partition: the second request
// starts where the first ended; the windows are strict; oldest first.
static void flow_scan_pages(void) {
    static const char P1[] =
        "{\"Items\":[{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#due002\"},\"game_id\":{\"S\":\"due002\"},"
        "\"updated_at\":{\"N\":\"50000\"},\"last_commit_at\":{\"N\":\"50000\"}},"
        "{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#edge01\"},\"game_id\":{\"S\":\"edge01\"},"
        "\"updated_at\":{\"N\":\"90000\"},\"last_commit_at\":{\"N\":\"90000\"}}],"
        "\"LastEvaluatedKey\":{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#edge01\"}}}";
    static const char P2[] =
        "{\"Items\":[{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#due001\"},\"game_id\":{\"S\":\"due001\"},"
        "\"updated_at\":{\"N\":\"80000\"},\"last_commit_at\":{\"N\":\"80000\"}},"
        "{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#aband1\"},\"game_id\":{\"S\":\"aband1\"},"
        "\"updated_at\":{\"N\":\"20000\"},\"last_commit_at\":{\"N\":\"20000\"}},"
        "{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#fresh1\"},\"game_id\":{\"S\":\"fresh1\"},"
        "\"updated_at\":{\"N\":\"95000\"},\"last_commit_at\":{\"N\":\"30000\"}}]}";
    static const step steps[] = {
        { "Query", "\"ConsistentRead\":true", 200, P1 },
        { "Query", "\"ExclusiveStartKey\":{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#edge01\"}}", 200, P2 },
    };
    RUN(steps, s);
    ids_t got = { 0 };
    // stale_before 90000 (edge01 is exactly there: not yet), active_after 20000 (aband1 exactly: abandoned)
    T_EQ_INT(CB_STORE_CALL(s, games_needing_bots, 90000, 20000, 100, collect_id, &got), 2);
    T_EQ_STR(got.ids[0], "due002");
    T_EQ_STR(got.ids[1], "due001");
    cb_store_close(s);
}

// An empty SCAN partition: nothing to sort, and no NULL handed to qsort (glibc
// declares its array nonnull, so UBSan on Linux reported the empty sort;
// fuzz/fz_dynamo_store.c found it).
static void flow_scan_empty(void) {
    static const step steps[] = {
        { "Query", "\"ConsistentRead\":true", 200, "{\"Items\":[],\"Count\":0}" },
    };
    RUN(steps, s);
    ids_t got = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, games_needing_bots, 90000, 20000, 100, collect_id, &got), 0);
    cb_store_close(s);
}

static void flow_signup_and_tokens(void) {
    static const char TAKEN[] =
        "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
        "\"CancellationReasons\":[{\"Code\":\"ConditionalCheckFailed\"},{\"Code\":\"None\"},{\"Code\":\"None\"}]}";
    static const char OLD[] = "{\"Attributes\":{\"pk\":{\"S\":\"REFRESH#aa\"},\"sk\":{\"S\":\"TOKEN\"},\"token_hash\":{\"S\":\"aa\"},"
                              "\"user_id\":{\"S\":\"" U1 "\"},\"session_id\":{\"S\":\"s1\"},\"created_at\":{\"N\":\"7\"},"
                              "\"revoked\":{\"BOOL\":false}}}";
    static const char OLD_TAKEN[] = "{\"Attributes\":{\"pk\":{\"S\":\"REFRESH#aa\"},\"sk\":{\"S\":\"TOKEN\"},\"token_hash\":{\"S\":\"aa\"},"
                              "\"user_id\":{\"S\":\"" U1 "\"},\"session_id\":{\"S\":\"s1\"},\"created_at\":{\"N\":\"7\"},"
                              "\"revoked\":{\"BOOL\":true},\"revoked_at\":{\"N\":\"100\"}}}";
    static const step steps[] = {
        { "TransactWriteItems", "EMAIL#ann@x.io", 400, TAKEN },
        { "TransactWriteItems", "EMAIL#bob@x.io", 400, TXN_CONFLICT },   // transient: sent again
        { "TransactWriteItems", "EMAIL#bob@x.io", 200, "{}" },
        { "UpdateItem", "\":now\":{\"N\":\"100\"}", 200, OLD },
        { "UpdateItem", "if_not_exists(#revoked_at, :now)", 200, OLD_TAKEN },
        { "UpdateItem", NULL, 400, COND_NO_ITEM },
        // end_sessions: one write to the user, nothing listed
        { "UpdateItem", "if_not_exists(#session_gen, :gen0) + :gen1", 200, "{}" },
    };
    RUN(steps, s);
    cb_user_row a = { .id = U1, .email = "ann@x.io", .password_hash = "h" }, b = { .id = U2, .email = "bob@x.io", .password_hash = "h" };
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &b), CB_OK);
    cb_refresh_row rt;
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "aa", 100, 1000, &rt), CB_OK);
    T_EQ_STR(rt.user_id, U1);
    T_EQ_STR(rt.session_id, "s1");
    T_CHECK(!rt.revoked);
    T_EQ_INT(rt.revoked_at, 0);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "aa", 200, 1000, &rt), CB_OK);
    T_CHECK(rt.revoked);
    T_EQ_INT(rt.revoked_at, 100);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "zz", 300, 1000, &rt), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, user_end_sessions, U1), CB_OK);
    T_EQ_INT(sc_.at, 7);
    cb_store_close(s);
}

typedef struct { int n; char msg[8][16]; } msgs_t;
static bool collect_msg(const cb_chat_row *c, void *ud) {
    msgs_t *m = ud;
    if (m->n < 8) snprintf(m->msg[m->n++], 16, "%s", c->message);
    return true;
}

// Chat: DynamoDB answers newest first; the store hands out oldest first.
static void flow_chat_order(void) {
    static const char P[] =
        "{\"Items\":["
        "{\"pk\":{\"S\":\"CHAT#ab12cd\"},\"sk\":{\"S\":\"000000000000003#c\"},\"id\":{\"S\":\"c\"},\"game_id\":{\"S\":\"ab12cd\"},"
        "\"user_id\":{\"S\":\"" U1 "\"},\"message\":{\"S\":\"three\"},\"is_system\":{\"BOOL\":false},\"created_at\":{\"N\":\"3\"}},"
        "{\"pk\":{\"S\":\"CHAT#ab12cd\"},\"sk\":{\"S\":\"000000000000002#b\"},\"id\":{\"S\":\"b\"},\"game_id\":{\"S\":\"ab12cd\"},"
        "\"user_id\":{\"S\":\"" U1 "\"},\"message\":{\"S\":\"two\"},\"is_system\":{\"BOOL\":false},\"created_at\":{\"N\":\"2\"}}],"
        "\"LastEvaluatedKey\":{\"pk\":{\"S\":\"CHAT#ab12cd\"},\"sk\":{\"S\":\"000000000000002#b\"}}}";
    static const step steps[] = {
        { "Query", "\"ScanIndexForward\":false,\"Limit\":2", 200, P },
    };
    RUN(steps, s);
    msgs_t m = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, chat_list, "ab12cd", 2, collect_msg, &m), 2);
    T_EQ_STR(m.msg[0], "two");
    T_EQ_STR(m.msg[1], "three");
    T_EQ_INT(sc_.at, 1);   // the limit was met: no second page
    cb_store_close(s);
}

// Account deletion rewrites a replay's extras on EVERY copy (the SNAP item and
// each player's PLAYER# item) in one transaction, read consistently first; a
// replay with no items is gone, and so is one that vanished under the write.
static void flow_snapshot_set_extras(void) {
    static const char ITEMS[] =
        "{\"Items\":["
        "{\"pk\":{\"S\":\"SNAP#" U2 "\"},\"sk\":{\"S\":\"PLAYER#" U1 "\"},\"id\":{\"S\":\"" U2 "\"}},"
        "{\"pk\":{\"S\":\"SNAP#" U2 "\"},\"sk\":{\"S\":\"SNAP\"},\"id\":{\"S\":\"" U2 "\"}}]}";
    static const char CANCELED[] =
        "{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\","
        "\"CancellationReasons\":[{\"Code\":\"None\"},{\"Code\":\"ConditionalCheckFailed\"}],\"Message\":\"Transaction cancelled\"}";
    static const step steps[] = {
        { "Query", "\"S\":\"SNAP#" U2 "\"", 200, ITEMS },
        { "TransactWriteItems", "\"ConditionExpression\":\"attribute_exists(pk)\"", 200, "{}" },
        { "Query", "SNAP#", 200, EMPTY },
        { "Query", "SNAP#", 200, ITEMS },
        { "TransactWriteItems", NULL, 400, CANCELED },
        { "Query", "SNAP#", 200, ITEMS },
        { "TransactWriteItems", "REMOVE #extras", 200, "{}" },
    };
    RUN(steps, s);
    const uint8_t ex[] = { 2, 1, 'X', 0 };
    T_EQ_INT(CB_STORE_CALL(s, snapshot_set_extras, U2, ex, sizeof ex), CB_OK);
    T_CHECK(strstr(sc_.bodies[0], "\"ConsistentRead\":true") != NULL);
    const char *upd =
        "{\"Update\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"SNAP#" U2 "\"},\"sk\":{\"S\":\"%s\"}},"
        "\"UpdateExpression\":\"SET #extras = :extras\",\"ConditionExpression\":\"attribute_exists(pk)\","
        "\"ExpressionAttributeNames\":{\"#extras\":\"extras\"},\"ExpressionAttributeValues\":{\":extras\":{\"B\":\"AgFYAA==\"}}}}";
    char a[1024], b[1024], want[2600];
    snprintf(a, sizeof a, upd, "PLAYER#" U1);
    snprintf(b, sizeof b, upd, "SNAP");
    snprintf(want, sizeof want, "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TransactItems\":[%s,%s]}", a, b);
    T_EQ_STR(sc_.bodies[1], want);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_set_extras, U2, ex, sizeof ex), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_set_extras, U2, ex, sizeof ex), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_set_extras, U2, NULL, 0), CB_OK);
    T_EQ_INT(sc_.at, 7);
    cb_store_close(s);
}

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

// ops/cbase.yaml creates the table this wire writes to: the same index names,
// all four index keys String (gsi1sk is "<ms>#<id>" text, not a Number),
// both projecting ALL, and the TTL on the attribute the wire writes seconds to.
static void the_template_matches_the_wire(void) {
    FILE *f = fopen("ops/cbase.yaml", "r");
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

// The free tier is provisioned capacity only, 25 WCU and 25 RCU shared by
// every table and index in the account (DESIGN.md "The free tier,
// provisioned"): the template provisions the table and both indexes, and the
// six numbers add up to no more than 25 a side. No auto scaling, which would
// provision past them.
static void the_template_stays_free(void) {
    FILE *f = fopen("ops/cbase.yaml", "r");
    T_CHECK(f != NULL);
    if (!f) return;
    static char y[64 * 1024];
    size_t n = fread(y, 1, sizeof y - 1, f);
    fclose(f);
    y[n] = 0;
    T_CHECK(strstr(y, "      BillingMode: PROVISIONED\n") != NULL);
    T_CHECK(strstr(y, "PAY_PER_REQUEST") == NULL);
    T_CHECK(strstr(y, "AWS::ApplicationAutoScaling") == NULL);
    int count = 0, rcu = 0, wcu = 0;
    static const char KEY[] = "ProvisionedThroughput: { ReadCapacityUnits: ";
    for (const char *q = y; (q = strstr(q, KEY)) != NULL; q++) {
        int r = 0, w = 0;
        T_EQ_INT(sscanf(q + strlen(KEY), "%d, WriteCapacityUnits: %d }", &r, &w), 2);
        T_CHECK(r >= 1 && w >= 1);
        rcu += r;
        wcu += w;
        count++;
    }
    T_EQ_INT(count, 3);   // the table, gsi1, gsi2
    T_CHECK(rcu <= 25);
    T_CHECK(wcu <= 25);
}

// The import's stored check (store_import.c on DynamoDB): one keys-only Scan
// of the whole table in pages of 100, each item told to the import as the
// export table it became and the id in its pk; items no export table becomes
// (a SCAN marker, a refresh token) are not told at all.
typedef struct { int n; char kinds[8][24], ids[8][48]; } told_t;
static void tell(const char *kind, const char *id, void *ud) {
    told_t *t = ud;
    if (t->n < 8) {
        snprintf(t->kinds[t->n], sizeof t->kinds[0], "%s", kind);
        snprintf(t->ids[t->n++], sizeof t->ids[0], "%s", id);
    }
}

static void flow_export_items(void) {
    static const char P1[] =
        "{\"Count\":3,\"ScannedCount\":3,\"Items\":["
        "{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"MEMBER#" U1 "\"}},"
        "{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROWS\"}},"
        "{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"}}],"
        "\"LastEvaluatedKey\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"}},"
        "\"ConsumedCapacity\":{\"TableName\":\"cbase\",\"CapacityUnits\":2.5}}";
    static const char P2[] =
        "{\"Count\":3,\"ScannedCount\":3,\"Items\":["
        "{\"pk\":{\"S\":\"CHAT#ab12cd\"},\"sk\":{\"S\":\"000000000000001#m1\"}},"
        "{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#ab12cd\"}},"
        "{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"RATING\"}}]}";
    static const step steps[] = {
        { "Scan", "\"ProjectionExpression\":\"#pk, #sk\"", 200, P1 },
        { "Scan", "\"ExclusiveStartKey\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"}}", 200, P2 },
    };
    RUN(steps, s);
    told_t t = { 0 };
    T_EQ_INT(cb_store_dynamo_export_items(s, tell, &t), CB_OK);
    T_EQ_INT(sc_.at, 2);
    T_EQ_INT(t.n, 4);
    T_EQ_STR(t.kinds[0], "player_hands");
    T_EQ_STR(t.ids[0], "ab12cd");
    T_EQ_STR(t.kinds[1], "games");   // ROWS is not a ROW: an exact sort key is not a prefix
    T_EQ_STR(t.kinds[2], "chat_messages");
    T_EQ_STR(t.ids[2], "ab12cd");
    T_EQ_STR(t.kinds[3], "user_elo_ratings");
    T_EQ_STR(t.ids[3], U1);
    T_CHECK(strstr(sc_.bodies[0], "\"ConsistentRead\":true") != NULL);
    T_CHECK(strstr(sc_.bodies[0], "\"Limit\":100") != NULL);
    T_CHECK(strstr(sc_.bodies[0], "ExclusiveStartKey") == NULL);
    cb_store_close(s);
}

// ---- bulk mode (cbase-import): a fake clock, so pauses are measured, not slept ----

static int64_t fk_now;
static int64_t fk_slept[64];
static int     fk_n;
static int64_t fk_mono(void) { return fk_now; }
static void    fk_sleep(int64_t ms) {
    if (fk_n < 64) fk_slept[fk_n++] = ms;
    fk_now += ms;
}
static void fk_reset(void) {
    fk_now = 1000000;
    fk_n = 0;
}

// DescribeTable as the live table answered it (aws dynamodb describe-table on
// cbase, 2026-10-14, cut to the fields read): 15/15 on the table, 5/5 on each
// GSI, so the import paces both buckets to 5 units a second.
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
    T_EQ_INT(r, 5);
    T_EQ_INT(w, 5);
    T_CHECK(dw_provisioned(DESCRIBE_ON_DEMAND, strlen(DESCRIBE_ON_DEMAND), &r, &w));
    T_EQ_INT(r, 0);
    T_EQ_INT(w, 0);
    T_CHECK(!dw_provisioned("{}", 2, &r, &w));
}

// The live import's failure (2026-10-14): a Scan throttled ten times in a row
// was given up on. In bulk mode the same throttling, past ten sends, is waited
// out: every pause at most 2 s, nothing given up, the read answered.
static void flow_bulk_throttle_waited_out(void) {
    static const char GOT[] = "{}";
    step steps[18] = { { "DescribeTable", "\"TableName\":\"cbase\"", 200, DESCRIBE_LIVE } };
    for (int i = 1; i < 17; i++) steps[i] = (step){ "GetItem", NULL, 400, THROTTLED_PT };
    steps[17] = (step){ "GetItem", NULL, 200, GOT };
    script sc_ = { .steps = steps, .n = 18 };
    cb_store *s = NULL;
    T_EQ_INT(cb_store_open_dynamo_with("cbase", fake_send, &sc_, &s), CB_OK);
    fk_reset();
    cb_store_dynamo_clock(s, fk_mono, fk_sleep);
    T_EQ_INT(cb_store_dynamo_bulk(s), CB_OK);
    cb_store_throttles t0;
    cb_store_throttles_get(&t0);
    cb_game_row g = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ab12cd", &g), CB_E_NOT_FOUND);
    T_EQ_INT(sc_.at, 18);
    T_EQ_INT(throttled_since(&t0), 16);
    T_EQ_INT(gave_up_since(&t0), 0);
    int64_t most = 0;
    for (int i = 0; i < fk_n; i++) most = fk_slept[i] > most ? fk_slept[i] : most;
    T_CHECK(most <= 2000);
    cb_store_close(s);
}

// Bulk mode does not wait out everything: a 5xx that never ends still stops
// after ten sends, so a dead endpoint cannot hang the import.
static void flow_bulk_errors_bounded(void) {
    step steps[16] = { { "DescribeTable", NULL, 200, DESCRIBE_ON_DEMAND } };
    for (int i = 1; i < 16; i++) steps[i] = (step){ "GetItem", NULL, 500, "{}" };
    script sc_ = { .steps = steps, .n = 16 };
    cb_store *s = NULL;
    T_EQ_INT(cb_store_open_dynamo_with("cbase", fake_send, &sc_, &s), CB_OK);
    fk_reset();
    cb_store_dynamo_clock(s, fk_mono, fk_sleep);
    T_EQ_INT(cb_store_dynamo_bulk(s), CB_OK);
    cb_game_row g = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ab12cd", &g), CB_E_DB);
    T_EQ_INT(sc_.at, 11);   // DescribeTable and ten sends
    cb_store_close(s);
}

// The pacing: a write that consumed 8 WCU against a 5 WCU/s bucket that held
// 5 leaves it 3 in debt, so the next write waits 600 ms; reads have their own
// bucket and do not wait for the writes. On-demand is unpaced.
static void flow_bulk_paced(void) {
    static const char PUT8[] = "{\"ConsumedCapacity\":{\"TableName\":\"cbase\",\"CapacityUnits\":8.0}}";
    static const char GOT[] = "{\"ConsumedCapacity\":{\"TableName\":\"cbase\",\"CapacityUnits\":1.0}}";
    static const step steps[] = {
        { "DescribeTable", NULL, 200, DESCRIBE_LIVE },
        { "PutItem", NULL, 200, PUT8 },
        { "GetItem", NULL, 200, GOT },
        { "PutItem", NULL, 200, PUT8 },
        { "PutItem", NULL, 200, PUT8 },
        { "PutItem", NULL, 200, PUT8 },
    };
    RUN(steps, s);
    fk_reset();
    cb_store_dynamo_clock(s, fk_mono, fk_sleep);
    T_EQ_INT(cb_store_dynamo_bulk(s), CB_OK);
    cb_rating_row rt = { .user_id = U1, .username = "A", .elo_rating = 1000, .previous_elo = -1 };
    T_EQ_INT(CB_STORE_CALL(s, rating_import, &rt), CB_OK);
    T_EQ_INT(fk_n, 0);   // the first write: the bucket held a second's worth
    cb_game_row g = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ab12cd", &g), CB_E_NOT_FOUND);
    T_EQ_INT(fk_n, 0);   // a read: its own bucket
    T_EQ_INT(CB_STORE_CALL(s, rating_import, &rt), CB_OK);
    T_EQ_INT(fk_n, 1);
    T_CHECK(fk_slept[0] >= 600 && fk_slept[0] <= 601);
    // Ten idle seconds refill only one second's worth (5), not 50: the next
    // write goes at once and leaves the bucket 3 in debt again.
    fk_now += 10000;
    T_EQ_INT(CB_STORE_CALL(s, rating_import, &rt), CB_OK);
    T_EQ_INT(fk_n, 1);
    T_EQ_INT(CB_STORE_CALL(s, rating_import, &rt), CB_OK);
    T_EQ_INT(fk_n, 2);
    T_CHECK(fk_slept[1] >= 600 && fk_slept[1] <= 601);
    T_EQ_INT(sc_.at, 6);
    cb_store_close(s);

    static const step od[] = {
        { "DescribeTable", NULL, 200, DESCRIBE_ON_DEMAND },
        { "PutItem", NULL, 200, PUT8 },
        { "PutItem", NULL, 200, PUT8 },
    };
    script sc2 = { .steps = od, .n = 3 };
    T_EQ_INT(cb_store_open_dynamo_with("cbase", fake_send, &sc2, &s), CB_OK);
    fk_reset();
    cb_store_dynamo_clock(s, fk_mono, fk_sleep);
    T_EQ_INT(cb_store_dynamo_bulk(s), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, rating_import, &rt), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, rating_import, &rt), CB_OK);
    T_EQ_INT(fk_n, 0);
    cb_store_close(s);
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
// json_surface.c crosses a string with memchr, reads an item through a
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

// A game row as DynamoDB answers it, its blobs in the middle of the scalars,
// read once: every field lands, the blobs decode, and the attribute order does
// not matter.
static void game_item_one_pass(void) {
    const char *it =
        "{\"pk\":{\"S\":\"GAME#ab12cd\"},\"state\":{\"B\":\"AQID\"},\"version\":{\"N\":\"7\"},"
        "\"roster\":{\"B\":\"BAUG\"},\"sk\":{\"S\":\"ROW\"},\"logs_packed\":{\"B\":\"Bw==\"},"
        "\"status\":{\"N\":\"2\"},\"needs_bots\":{\"BOOL\":true},\"id\":{\"S\":\"ab12cd\"},"
        "\"game_seed\":{\"B\":\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\"},\"round_epoch\":{\"N\":\"5\"},"
        "\"created_at\":{\"N\":\"100\"},\"updated_at\":{\"N\":\"200\"},\"last_commit_at\":{\"N\":\"150\"},"
        "\"seats_exact\":{\"BOOL\":true}}";
    cb_game_row g;
    T_CHECK(dw_item_game(lit_span(it), &g));
    T_CHECK(g.seats_exact);
    T_EQ_STR(g.id, "ab12cd");
    T_EQ_INT(g.status, 2);
    T_EQ_INT(g.version, 7);
    T_EQ_INT(g.state_len, 3);
    T_CHECK(g.state && g.state[0] == 1 && g.state[2] == 3);
    T_EQ_INT(g.roster_len, 3);
    T_CHECK(g.roster && g.roster[0] == 4);
    T_EQ_INT(g.logs_len, 1);
    T_CHECK(g.logs && g.logs[0] == 7);
    T_CHECK(g.has_seed && g.seed[31] == 31);
    T_CHECK(g.needs_bots);
    T_EQ_INT(g.round_epoch, 5);
    T_EQ_INT(g.created_at, 100);
    T_EQ_INT(g.updated_at, 200);
    T_EQ_INT(g.last_commit_at, 150);
    cb_game_row_free(&g);
    // A row written before the flag, or imported, does not vouch.
    T_CHECK(dw_item_game(lit_span("{\"id\":{\"S\":\"g\"},\"status\":{\"N\":\"1\"},\"version\":{\"N\":\"1\"}}"), &g));
    T_CHECK(!g.seats_exact);
    cb_game_row_free(&g);
    // The create writes it when the row vouches, and only then.
    dw_req r;
    dw_req_init(&r);
    uint8_t b[1] = { 1 };
    cb_game_row row = { .id = "ab12cd", .status = 1, .state = b, .state_len = 1, .roster = b, .roster_len = 1, .seats_exact = true };
    dw_game_create(&r, "cbase", &row, U1);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\"seats_exact\":{\"BOOL\":true}") != NULL);
    dw_req_free(&r);
    dw_req_init(&r);
    row.seats_exact = false;
    dw_game_create(&r, "cbase", &row, U1);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "seats_exact") == NULL);
    dw_req_free(&r);
}

// The index answers as a walk from the start would: the first attribute of a
// name wins, a value whose first member is not the type asked for is searched,
// an untyped value has no type, and a blob with an escaped slash is unescaped
// before it is decoded.
static void item_index_rules(void) {
    cb_user_row u;
    // id twice: the first is read.
    const char *dup = "{\"id\":{\"S\":\"first\"},\"email\":{\"S\":\"e\"},\"password_hash\":{\"S\":\"h\"},\"id\":{\"S\":\"second\"}}";
    T_CHECK(dw_item_user(lit_span(dup), &u));
    T_EQ_STR(u.id, "first");
    // created_at's value has another member before N: N is still found.
    const char *second = "{\"id\":{\"S\":\"i\"},\"email\":{\"S\":\"e\"},\"password_hash\":{\"S\":\"h\"},"
                         "\"created_at\":{\"X\":{\"N\":\"1\"},\"N\":\"42\"}}";
    T_CHECK(dw_item_user(lit_span(second), &u));
    T_EQ_INT(u.created_at, 42);
    // A type twice in one value: the first is read.
    const char *twice = "{\"id\":{\"S\":\"i\"},\"email\":{\"S\":\"e\"},\"password_hash\":{\"S\":\"h\"},"
                        "\"created_at\":{\"N\":\"1\",\"N\":\"2\"}}";
    T_CHECK(dw_item_user(lit_span(twice), &u));
    T_EQ_INT(u.created_at, 1);
    // An untyped first attribute of a name hides a typed second one.
    const char *hidden = "{\"id\":\"x\",\"id\":{\"S\":\"y\"},\"email\":{\"S\":\"e\"},\"password_hash\":{\"S\":\"h\"}}";
    T_CHECK(!dw_item_user(lit_span(hidden), &u));
    // A value that is not a typed object, or is empty, reads as absent.
    const char *untyped = "{\"id\":\"i\",\"email\":{},\"password_hash\":{\"S\":\"h\"}}";
    T_CHECK(!dw_item_user(lit_span(untyped), &u));
    // A walk that breaks stops the index there: what came before is read.
    cb_member_row m;
    T_CHECK(dw_item_member(lit_span("{\"game_id\":{\"S\":\"g1\"},\"updated_at\":{\"N\":\"9\"},\"x\":"), &m));
    T_EQ_STR(m.game_id, "g1");
    T_EQ_INT(m.updated_at, 9);
    // "\/" is "/" in JSON: AQ/= decodes once unescaped.
    cb_game_row g;
    const char *esc = "{\"id\":{\"S\":\"g\"},\"status\":{\"N\":\"1\"},\"version\":{\"N\":\"1\"},\"state\":{\"B\":\"AQ\\/9\"}}";
    T_CHECK(dw_item_game(lit_span(esc), &g));
    T_EQ_INT(g.state_len, 3);
    T_CHECK(g.state && g.state[0] == 0x01 && g.state[1] == 0x0f && g.state[2] == 0xfd);
    cb_game_row_free(&g);
    // A control byte in a blob is refused whether or not the blob has an escape.
    const char *ctl = "{\"id\":{\"S\":\"g\"},\"status\":{\"N\":\"1\"},\"version\":{\"N\":\"1\"},\"state\":{\"B\":\"AQ\nA\"}}";
    T_CHECK(!dw_item_game(lit_span(ctl), &g));
    T_CHECK(g.state == NULL);
}

// More attributes than the index holds: the ones past it are still found.
static void item_longer_than_the_index(void) {
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_puts(&b, "{");
    for (int i = 0; i < 40; i++) cb_buf_printf(&b, "\"pad%d\":{\"S\":\"x\"},", i);
    cb_buf_puts(&b, "\"game_id\":{\"S\":\"late\"},\"updated_at\":{\"N\":\"77\"}}");
    cb_member_row m;
    T_CHECK(dw_item_member(cb_span_of((const char *)b.p, b.len), &m));
    T_EQ_STR(m.game_id, "late");
    T_EQ_INT(m.updated_at, 77);
    cb_buf_free(&b);
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
    T_CHECK(!dw_cond_failed_has_item(notobj, strlen(notobj)));
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
    T_RUN(the_template_stays_free);
    T_RUN(get_item_shape);
    T_RUN(lease_requests);
    T_RUN(commit_transaction);
    T_RUN(commit_lone);
    T_RUN(query_shapes);
    T_RUN(signup_transaction);
    T_RUN(builders_valid_json);
    T_RUN(items_round_trip);
    T_RUN(errors_classified);
    T_RUN(pages_and_batches);
    T_RUN(flow_signin_fails);
    T_RUN(flow_sessions);
    T_RUN(flow_cas_lost);
    T_RUN(flow_commit_game_gone);
    T_RUN(flow_rating_moved);
    T_RUN(flow_commit_drops_missing_account);
    T_RUN(flow_commit_seats_given);
    T_RUN(flow_commit_seats_read);
    T_RUN(flow_commit_lone);
    T_RUN(flow_commit_lone_cas_lost);
    T_RUN(flow_commit_lone_game_gone);
    T_RUN(flow_commit_lone_retried);
    T_RUN(flow_commit_after_bot_work);
    T_RUN(flow_lease);
    T_RUN(flow_throttle_retry);
    T_RUN(flow_throttled_then_answered);
    T_RUN(flow_throttled_past_the_deadline);
    T_RUN(flow_throttled_to_the_wall);
    T_RUN(flow_transaction_throttled);
    T_RUN(flow_batch_unprocessed);
    T_RUN(games_get_shape);
    T_RUN(flow_games_batched);
    T_RUN(flow_scan_pages);
    T_RUN(flow_scan_empty);
    T_RUN(flow_signup_and_tokens);
    T_RUN(flow_chat_order);
    T_RUN(flow_snapshot_set_extras);
    T_RUN(flow_admin_query_guard);
    T_RUN(flow_export_items);
    T_RUN(provisioned_read);
    T_RUN(flow_bulk_throttle_waited_out);
    T_RUN(flow_bulk_errors_bounded);
    T_RUN(flow_bulk_paced);
    T_RUN(consumed_capacity_read);
    T_RUN(strings_skipped_by_parity);
    T_RUN(game_item_one_pass);
    T_RUN(item_index_rules);
    T_RUN(item_longer_than_the_index);
    T_RUN(response_top_level);
    return T_DONE();
}
