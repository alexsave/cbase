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

#include "store/dynamo_wire.h"
#include "store/store_priv.h"
#include "table.h"
#include "util/clock.h"
#include "util/json.h"

#include <stdlib.h>

#define TOK "11111111-2222-4333-8444-555555555555"
#define U1 "aaaaaaaa-0000-4000-8000-000000000001"
#define U2 "aaaaaaaa-0000-4000-8000-000000000002"
#define B1 "bbbbbbbb-0000-4000-8000-000000000001"

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

// try_acquire_bot_lease: free when never taken, released or run out; the
// failure brings the old item back so a missing game is told apart.
static void lease_requests(void) {
    dw_req r;
    dw_req_init(&r);
    dw_lease_acquire(&r, "cbase", "ab12cd", TOK, 1000000, 25000);
    T_EQ_STR(r.target, "DynamoDB_20120810.UpdateItem");
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"}},"
             "\"UpdateExpression\":\"SET #lease_token = :lease_token, #lease_until = :lease_until, #updated_at = :updated_at\","
             "\"ConditionExpression\":\"attribute_exists(pk) AND (attribute_not_exists(#lease_until) OR #lease_until < :updated_at)\","
             "\"ExpressionAttributeNames\":{\"#lease_token\":\"lease_token\",\"#lease_until\":\"lease_until\",\"#updated_at\":\"updated_at\"},"
             "\"ExpressionAttributeValues\":{\":lease_token\":{\"S\":\"" TOK "\"},\":lease_until\":{\"N\":\"1025000\"},"
             "\":updated_at\":{\"N\":\"1000000\"}},\"ReturnValuesOnConditionCheckFailure\":\"ALL_OLD\"}");
    dw_lease_renew(&r, "cbase", "ab12cd", TOK, 1010000, 25000);
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"}},"
             "\"UpdateExpression\":\"SET #lease_until = :lease_until, #updated_at = :updated_at\","
             "\"ConditionExpression\":\"#lease_token = :token\","
             "\"ExpressionAttributeNames\":{\"#lease_until\":\"lease_until\",\"#updated_at\":\"updated_at\",\"#lease_token\":\"lease_token\"},"
             "\"ExpressionAttributeValues\":{\":lease_until\":{\"N\":\"1035000\"},\":updated_at\":{\"N\":\"1010000\"},"
             "\":token\":{\"S\":\"" TOK "\"}}}");
    dw_lease_release(&r, "cbase", "ab12cd", TOK);
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"GAME#ab12cd\"},\"sk\":{\"S\":\"ROW\"}},"
             "\"UpdateExpression\":\"REMOVE #lease_until\",\"ConditionExpression\":\"#lease_token = :token\","
             "\"ExpressionAttributeNames\":{\"#lease_until\":\"lease_until\",\"#lease_token\":\"lease_token\"},"
             "\"ExpressionAttributeValues\":{\":token\":{\"S\":\"" TOK "\"}}}");
    dw_req_free(&r);
}

// THE CAS: item 0 the version-fenced row, then a new member, a member gone,
// the ratings fenced on what was read (clamped arithmetic done here), the
// SCAN marker. The bot already seated is not written again.
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
             "#version = :version, #round_epoch = :round_epoch, #updated_at = :updated_at, #last_commit_at = :last_commit_at "
             "REMOVE #logs_packed, #game_seed\","
             "\"ConditionExpression\":\"#version = :expected\","
             "\"ExpressionAttributeNames\":{\"#status\":\"status\",\"#state\":\"state\",\"#roster\":\"roster\","
             "\"#logs_packed\":\"logs_packed\",\"#game_seed\":\"game_seed\",\"#needs_bots\":\"needs_bots\",\"#version\":\"version\","
             "\"#round_epoch\":\"round_epoch\",\"#updated_at\":\"updated_at\",\"#last_commit_at\":\"last_commit_at\"},"
             "\"ExpressionAttributeValues\":{\":status\":{\"N\":\"1\"},\":state\":{\"B\":\"AQID\"},\":roster\":{\"B\":\"CQk=\"},"
             "\":needs_bots\":{\"BOOL\":true},\":version\":{\"N\":\"8\"},\":round_epoch\":{\"N\":\"5\"},\":updated_at\":{\"N\":\"2000\"},"
             "\":last_commit_at\":{\"N\":\"2000\"},\":expected\":{\"N\":\"7\"}},"
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
             "\"UpdateExpression\":\"SET #elo_rating = :elo_rating, #previous_elo = :previous_elo, #updated_at = :updated_at, "
             "#games_played = #games_played + :one\",\"ConditionExpression\":\"#elo_rating = :read\","
             "\"ExpressionAttributeNames\":{\"#elo_rating\":\"elo_rating\",\"#previous_elo\":\"previous_elo\","
             "\"#updated_at\":\"updated_at\",\"#games_played\":\"games_played\"},"
             "\"ExpressionAttributeValues\":{\":elo_rating\":{\"N\":\"1012\"},\":previous_elo\":{\"N\":\"1000\"},"
             "\":updated_at\":{\"N\":\"2000\"},\":one\":{\"N\":\"1\"},\":read\":{\"N\":\"1000\"}}}},"
             "{\"Update\":{\"TableName\":\"cbase\",\"Key\":{\"pk\":{\"S\":\"BOT#" B1 "\"},\"sk\":{\"S\":\"BOT\"}},"
             "\"UpdateExpression\":\"SET #elo_rating = :elo_rating, #previous_elo = :previous_elo, #updated_at = :updated_at, "
             "#games_played = #games_played + :one\",\"ConditionExpression\":\"#elo_rating = :read\","
             "\"ExpressionAttributeNames\":{\"#elo_rating\":\"elo_rating\",\"#previous_elo\":\"previous_elo\","
             "\"#updated_at\":\"updated_at\",\"#games_played\":\"games_played\"},"
             "\"ExpressionAttributeValues\":{\":elo_rating\":{\"N\":\"0\"},\":previous_elo\":{\"N\":\"990\"},"
             "\":updated_at\":{\"N\":\"2000\"},\":one\":{\"N\":\"1\"},\":read\":{\"N\":\"990\"}}}},"
             "{\"Put\":{\"TableName\":\"cbase\",\"Item\":{\"pk\":{\"S\":\"SCAN\"},\"sk\":{\"S\":\"GAME#ab12cd\"},"
             "\"game_id\":{\"S\":\"ab12cd\"},\"updated_at\":{\"N\":\"2000\"},\"last_commit_at\":{\"N\":\"2000\"}}}}"
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
                   "\"gsi2pk\":{\"S\":\"RATINGS\"},\"gsi2sk\":{\"S\":\"" U1 "\"},\"user_id\":{\"S\":\"" U1 "\"},"
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
    // A game's seat keys: a key range (a Query cannot filter on a key).
    dw_query(&r, "cbase", DWQ_GAME_PARTITION, "ab12cd", true, 0, cb_span_of("", 0));
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"KeyConditionExpression\":\"#pk = :pk AND #sk < :sk\",\"ProjectionExpression\":\"#pk, #sk\","
             "\"ExpressionAttributeNames\":{\"#pk\":\"pk\",\"#sk\":\"sk\"},"
             "\"ExpressionAttributeValues\":{\":pk\":{\"S\":\"GAME#ab12cd\"},\":sk\":{\"S\":\"ROW\"}},\"ConsistentRead\":true}");
    dw_query(&r, "cbase", DWQ_GAME_MEMBERS, "ab12cd", true, 0, cb_span_of("", 0));
    T_EQ_STR(cb_buf_cstr(&r.body),
             "{\"ReturnConsumedCapacity\":\"TOTAL\",\"TableName\":\"cbase\",\"KeyConditionExpression\":\"#pk = :pk AND begins_with(#sk, :sk)\","
             "\"ExpressionAttributeNames\":{\"#pk\":\"pk\",\"#sk\":\"sk\"},"
             "\"ExpressionAttributeValues\":{\":pk\":{\"S\":\"GAME#ab12cd\"},\":sk\":{\"S\":\"MEMBER#\"}},\"ConsistentRead\":true}");
    dw_query(&r, "cbase", DWQ_SCAN, NULL, true, 0, cb_span_of("", 0));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\":pk\":{\"S\":\"SCAN\"}") != NULL);
    // Every kind is valid JSON.
    for (int k = DWQ_GAME_MEMBERS; k <= DWQ_USERS; k++) {
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
             "\"gsi2pk\":{\"S\":\"RATINGS\"},\"gsi2sk\":{\"S\":\"" U1 "\"},\"user_id\":{\"S\":\"" U1 "\"},"
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
    cb_conn_row cn = { .conn_id = "Abc=", .vsn = 2, .n_topics = 1, .expires_at = 7200123 };
    snprintf(cn.topics[0], sizeof cn.topics[0], "gu-ab12cd-" U1);
    cb_conn_row old = cn;
    snprintf(old.topics[0], sizeof old.topics[0], "game-zzzzzz");
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
    dw_user_set_password(&r, "cbase", U1, "$2b$x");  T_CHECK(valid(&r));
    dw_user_touch(&r, "cbase", U1, 5);               T_CHECK(valid(&r));
    dw_user_delete_txn(&r, "cbase", &u);             T_CHECK(valid(&r));
    dw_refresh_put(&r, "cbase", &rt);                T_CHECK(valid(&r));
    dw_refresh_take(&r, "cbase", "ff", 9);           T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "if_not_exists(#revoked_at, :now)") != NULL);
    dw_refresh_revoke(&r, "cbase", body(&(dw_req){ .body = key }));
    T_CHECK(valid(&r));
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
    dw_conn_put(&r, "cbase", &cn, &old);             T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\"expires_at\":{\"N\":\"7200\"}") != NULL);   // TTL in seconds
    T_CHECK(strstr(cb_buf_cstr(&r.body), "TOPIC#game-zzzzzz") != NULL);                // the topic left is deleted
    dw_conn_delete(&r, "cbase", &cn);                T_CHECK(valid(&r));
    dw_batch_delete(&r, "cbase", &key, 1);           T_CHECK(valid(&r));
    T_CHECK(dw_bots_replace(&r, "cbase", bots, 1, stored, 2));
    T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "BOT#" U2) != NULL);   // the stored id not in rows goes
    dw_rating_import(&r, "cbase", &rr);              T_CHECK(valid(&r));
    dw_rating_set_username(&r, "cbase", U1, NULL);   T_CHECK(valid(&r));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "REMOVE #username") != NULL);
    dw_ratings_get(&r, "cbase", ids, isbot, 2, false); T_CHECK(valid(&r));
    dw_ratings_get(&r, "cbase", ids, isbot, 2, true);  T_CHECK(valid(&r));
    dw_scan_touch(&r, "cbase", "ab12cd", 77);        T_CHECK(valid(&r));
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

    cb_conn_row cn = { .conn_id = "Abc=", .user_id = U1, .vsn = 2, .n_topics = 2, .expires_at = 7200123 }, cnb;
    snprintf(cn.topics[0], sizeof cn.topics[0], "pv-" U1);
    snprintf(cn.join_refs[0], sizeof cn.join_refs[0], "3");
    snprintf(cn.topics[1], sizeof cn.topics[1], "game-ab12cd");
    snprintf(cn.join_refs[1], sizeof cn.join_refs[1], "4");
    dw_conn_put(&r, "cbase", &cn, NULL);
    T_CHECK(cb_json_find((const char *)r.body.p, r.body.len, "RequestItems.cbase.2.PutRequest.Item", &it) == CB_JSON_OBJ);
    T_CHECK(dw_item_conn(it, &cnb));   // a TOPIC copy carries the whole row
    T_EQ_INT(cnb.vsn, 2);
    T_EQ_INT(cnb.n_topics, 2);
    T_EQ_STR(cnb.topics[1], "game-ab12cd");
    T_EQ_STR(cnb.join_refs[1], "4");
    T_EQ_INT(cnb.expires_at, 7200123);

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
        { "Query", "MEMBER#", 200, EMPTY },
        { "Query", "BOT#", 200, EMPTY },
        { "TransactWriteItems", "\":expected\":{\"N\":\"7\"}", 400, CAS_LOST },
    };
    RUN(steps, s);
    uint8_t st[1] = { 1 };
    cb_game_row g;
    cb_game_commit c = mkcommit(&g, st);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_E_CONFLICT);
    T_EQ_INT(sc_.at, 3);
    cb_store_close(s);
}

static void flow_commit_game_gone(void) {
    static const step steps[] = {
        { "Query", "MEMBER#", 200, EMPTY },
        { "Query", "BOT#", 200, EMPTY },
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
        { "Query", "MEMBER#", 200, EMPTY },
        { "Query", "BOT#", 200, BOTSEAT },
        { "BatchGetItem", "BOT#" B1, 200, R1 },
        { "TransactWriteItems", "\":read\":{\"N\":\"1000\"}", 400, RATING_MOVED },
        { "Query", "MEMBER#", 200, EMPTY },
        { "Query", "BOT#", 200, BOTSEAT },
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
    T_EQ_INT(sc_.at, 8);
    cb_store_close(s);
}

// A newly seated human whose account is gone is left out of the commit.
static void flow_commit_drops_missing_account(void) {
    static const char BG[] = "{\"Responses\":{\"cbase\":[{\"pk\":{\"S\":\"USER#" U1 "\"},\"sk\":{\"S\":\"PROFILE\"}}]}}";
    static const step steps[] = {
        { "Query", "MEMBER#", 200, EMPTY },
        { "Query", "BOT#", 200, EMPTY },
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
    T_CHECK(strstr(sc_.bodies[3], "MEMBER#" U2) == NULL);
    cb_store_close(s);
}

static void flow_lease(void) {
    static const step steps[] = {
        { "UpdateItem", "attribute_not_exists(#lease_until)", 400, COND_WITH_ITEM },   // held by another
        { "UpdateItem", NULL, 400, COND_NO_ITEM },                                      // no such game
        { "UpdateItem", "\":updated_at\":{\"N\":\"5000\"}", 200, "{}" },                 // taken
        { "UpdateItem", "\"S\":\"SCAN\"", 400, COND_NO_ITEM },                           // no marker: fine
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
    T_EQ_INT(sc_.at, 6);
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
        // revoke_user: the user's tokens by GSI1, each revoked with its stamp zeroed
        { "Query", "USERREFRESH#" U1, 200, "{\"Items\":[{\"pk\":{\"S\":\"REFRESH#aa\"},\"sk\":{\"S\":\"TOKEN\"}}]}" },
        { "UpdateItem", "\":revoked_at\":{\"N\":\"0\"}", 200, "{}" },
    };
    RUN(steps, s);
    cb_user_row a = { .id = U1, .email = "ann@x.io", .password_hash = "h" }, b = { .id = U2, .email = "bob@x.io", .password_hash = "h" };
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &b), CB_OK);
    cb_refresh_row rt;
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "aa", 100, &rt), CB_OK);
    T_EQ_STR(rt.user_id, U1);
    T_EQ_STR(rt.session_id, "s1");
    T_CHECK(!rt.revoked);
    T_EQ_INT(rt.revoked_at, 0);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "aa", 200, &rt), CB_OK);
    T_CHECK(rt.revoked);
    T_EQ_INT(rt.revoked_at, 100);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "zz", 300, &rt), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, refresh_revoke_user, U1), CB_OK);
    T_EQ_INT(sc_.at, 8);
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

// The import's stored count (store_import.c on DynamoDB): a counting Scan per
// export table, its pages summed. player_hands are the GAME# partitions'
// MEMBER# items, whatever else those partitions hold.
static void flow_import_count(void) {
    static const char P1[] =
        "{\"Count\":3,\"ScannedCount\":40,\"LastEvaluatedKey\":{\"pk\":{\"S\":\"GAME#a\"},\"sk\":{\"S\":\"ROW\"}},"
        "\"ConsumedCapacity\":{\"TableName\":\"cbase\",\"CapacityUnits\":2.5}}";
    static const char P2[] = "{\"Count\":2,\"ScannedCount\":12}";
    static const step steps[] = {
        { "Scan", "\"Select\":\"COUNT\"", 200, P1 },
        { "Scan", "\"ExclusiveStartKey\":{\"pk\":{\"S\":\"GAME#a\"},\"sk\":{\"S\":\"ROW\"}}", 200, P2 },
    };
    RUN(steps, s);
    long long n = 0;
    T_EQ_INT(cb_store_dynamo_count(s, "player_hands", &n), CB_OK);
    T_EQ_INT(n, 5);
    T_CHECK(strstr(sc_.bodies[0], "\"FilterExpression\":\"begins_with(#pk, :p) AND begins_with(#sk, :s)\"") != NULL);
    T_CHECK(strstr(sc_.bodies[0], "\":p\":{\"S\":\"GAME#\"},\":s\":{\"S\":\"MEMBER#\"}") != NULL);
    T_CHECK(strstr(sc_.bodies[0], "\"ConsistentRead\":true") != NULL);
    T_EQ_INT(cb_store_dynamo_count(s, "nosuch", &n), CB_E_INVALID);
    cb_store_close(s);

    // An exact sort key is an equality, not a prefix (a ROW, not ROWS).
    dw_req r;
    dw_req_init(&r);
    T_CHECK(dw_scan_count(&r, "cbase", "games", cb_span_of("", 0)));
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\"FilterExpression\":\"begins_with(#pk, :p) AND #sk = :s\"") != NULL);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\":s\":{\"S\":\"ROW\"}") != NULL);
    dw_req_free(&r);
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

int main(void) {
    T_RUN(the_template_matches_the_wire);
    T_RUN(get_item_shape);
    T_RUN(lease_requests);
    T_RUN(commit_transaction);
    T_RUN(query_shapes);
    T_RUN(signup_transaction);
    T_RUN(builders_valid_json);
    T_RUN(items_round_trip);
    T_RUN(errors_classified);
    T_RUN(pages_and_batches);
    T_RUN(flow_cas_lost);
    T_RUN(flow_commit_game_gone);
    T_RUN(flow_rating_moved);
    T_RUN(flow_commit_drops_missing_account);
    T_RUN(flow_lease);
    T_RUN(flow_throttle_retry);
    T_RUN(flow_scan_pages);
    T_RUN(flow_signup_and_tokens);
    T_RUN(flow_chat_order);
    T_RUN(flow_snapshot_set_extras);
    T_RUN(flow_admin_query_guard);
    T_RUN(flow_import_count);
    T_RUN(consumed_capacity_read);
    return T_DONE();
}
