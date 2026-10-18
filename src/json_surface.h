// json_surface.h - EVERY JSON SHAPE c_base speaks, and nothing else.
//
// JSON is tolerated in c_base only where something outside it sets the format
// (DESIGN.md "Where JSON is tolerated"): AWS (the Lambda event and response
// envelopes, the Runtime API's error body, DynamoDB, the API Gateway
// Management API, Lambda Invoke), the access token's JWT header and claims,
// and the admin console's API. The clients' surface is binary (the kernel's
// netwire.h), so none of it is here. All of it is contained in ONE file,
// json_surface.c: every key name, every shape, every call into the low-level
// writer and reader (util/json.h, which nothing else includes). Everything
// else in c_base hands this file structs and spans and gets bytes or structs
// back. test/unit_json_gate.c fails the build when a JSON key literal, a
// util/json.h include or a cb_jw_/cb_json_ call appears in any other file
// under src/, bench/ or migrate/.
//
// The functions are pure over buffers (no socket, no clock, no randomness: a
// token or a time is an argument), so each shape is checked byte for byte
// against a recorded body in the unit tests.
#ifndef CB_JSON_SURFACE_H
#define CB_JSON_SURFACE_H

#include "store/store.h"
#include "aws/event.h"
#include "auth/auth.h"
#include "app/table_ops.h"
#include "roster.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// =====================================================================================
// DynamoDB (dw_): the JSON requests the DynamoDB backend sends and the readers for
// what DynamoDB answers (store_dynamo.c, store_import.c and test/unit_dynamo.c).
//
// EVERY DynamoDB STRING LIVES IN json_surface.c: the X-Amz-Target names, the
// item layout (pk/sk spellings, attribute names, the GSI names), the key,
// condition, update and filter expressions. store_dynamo.c decides which
// requests to send in which order and never spells one.
//
// THE ITEM LAYOUT (one table, string pk and sk; GSI1 = gsi1pk/gsi1sk and
// GSI2 = gsi2pk/gsi2sk, all four String keys, both projecting ALL; TTL
// attribute expires_at, in seconds):
//
//   pk                 sk                 what (GSI keys)
//   USER#<id>          PROFILE            the account (GSI2 USERS / <created>#<id>)
//   USER#<id>          RATING             the leaderboard row (GSI2 RATINGS / E#<rank>#<id>, only
//                                         once games_played > 0: a sparse, rating-ordered key)
//   EMAIL#<email>      USER               the user id; signup's uniqueness
//   AUTHFAIL#<email>   FAILS              failed sign-ins at the account (auth.h CB_AUTH_FAIL_*; TTL an hour)
//   REFRESH#<hash>     TOKEN              a refresh token (GSI1 USERREFRESH#<user>; TTL its session's
//                                         end, a week once spent; rows before sessions also GSI2 SESSION#)
//   SESSION#<id>       SESSION            a session: revoked, its absolute end (TTL)
//   GAME#<id>          ROW                the game
//   GAME#<id>          MEMBER#<user>      a seated human (GSI1 USERGAMES#<user> / <updated>#<game>)
//   GAME#<id>          BOT#<bot>          a seated bot
//   BOT#<id>           BOT                a bot and its rating (GSI2 BOTS / <id>)
//   SNAP#<id>          SNAP               a replay (GSI1 GAMESNAPS#<game> / <created>#<id>)
//   SNAP#<id>          PLAYER#<user>      the replay again, once per player (GSI1 as above,
//                                         GSI2 PLAYER#<user> / <created>#<id>)
//   CHAT#<game>        <created>#<id>     a chat message (GSI1 USERCHAT#<user> / <created>#<id>)
//   SCAN               GAME#<id>          present while a bot can act (table_bots_due): the
//                                         heartbeat's partition, the bot lease, row_version
//
// Times in sort keys are 15-digit zero-padded ms so they sort as text. A
// rating's GSI2 sort key is "E#" + (2147483647 - rating) as 10 zero-padded
// digits + "#" + the user id, so a forward Query is the leaderboard (highest
// first, then user id) and a Limit bounds it; a row with no games carries no
// GSI2 key and is not in the index at all. The "E#" prefix keeps the Query
// off the rows written before this layout (GSI2 RATINGS / <id>), which
// cbase-import --reindex-ratings rewrites once.
// The index names and the TTL attribute the table must be created with.
// ops/cbase.yaml spells them too; test/unit_dynamo.c reads the template and
// fails when the two disagree.
#define DW_GSI1     "gsi1"
#define DW_GSI2     "gsi2"
#define DW_TTL_ATTR "expires_at"

// One request: the X-Amz-Target header value and the JSON body.
typedef struct {
    char   target[64];
    cb_buf body;
} dw_req;

void dw_req_init(dw_req *r);
void dw_req_free(dw_req *r);

// Where and how a request goes: the regional endpoint, the body's media type
// and the SigV4 service name.
void        dw_host(const char *region, char *out, size_t cap);
const char *dw_content_type(void);
const char *dw_service(void);

// ---- what DynamoDB answered ----------------------------------------------------------

typedef enum {
    DW_OK = 0,            // 200
    DW_COND_FAILED,       // ConditionalCheckFailedException (a CAS or a lease refused)
    DW_TXN_CANCELED,      // TransactionCanceledException: read dw_cancel_reason
    DW_RETRY,             // throttled, a 5xx, a transient conflict: send again
    DW_FAILED,            // anything else: a bug or a misconfiguration
} dw_status;

dw_status dw_classify(int http_status, const char *body, size_t n);
// A DW_RETRY that is throttling (ProvisionedThroughputExceededException,
// ThrottlingException, RequestLimitExceeded), not a 5xx or a conflict.
bool dw_throttled(const char *body, size_t n);
// The error's __type after the '#', and its message, for the log.
void dw_error_text(const char *body, size_t n, char *out, size_t cap);

// One entry of a TransactionCanceledException's CancellationReasons.
typedef enum {
    DW_REASON_NONE = 0,       // "None": this item was fine
    DW_REASON_COND,           // "ConditionalCheckFailed"
    DW_REASON_CONFLICT,       // "TransactionConflict" (another transaction on the item: retry)
    DW_REASON_OTHER,
    DW_REASON_THROTTLED,      // "ThrottlingError" or "ProvisionedThroughputExceeded": retry after a pause
    DW_REASON_MISSING = -1,   // no entry at that index
} dw_reason;
// The reason at `index`; *has_item says whether the failed item came back
// (ReturnValuesOnConditionCheckFailure ALL_OLD), i.e. whether it exists.
dw_reason dw_cancel_reason(const char *body, size_t n, int index, bool *has_item);
// A failed single-item condition (DW_COND_FAILED): did the item exist?
bool dw_cond_failed_has_item(const char *body, size_t n);

// ---- reading items -------------------------------------------------------------------

// Walks the elements of a JSON array value: *pos starts at 0. false at the end
// or on malformed input.
bool dw_array_next(cb_span arr, size_t *pos, cb_span *elem);
// Walks the members of a JSON object value: key (a string token, quotes
// included) and value.
bool dw_object_next(cb_span obj, size_t *pos, cb_span *key, cb_span *val);
// A response's top-level members, walked once (json_surface.c "a response's
// top-level members"): call() scans every answer, reads its ConsumedCapacity
// from the index and hands the index on, so the operation's own reader does
// not walk the body again. Past DW_TOP_MAX members a reader walks the body.
#define DW_TOP_MAX 16
typedef struct {
    cb_span body;
    int     n;
    bool    full;
    struct {
        cb_span key, val;
    } m[DW_TOP_MAX];
} dw_top;
void dw_top_scan(const char *body, size_t n, dw_top *t);
// dw_item, dw_items, dw_last_key and dw_consumed over an index.
bool    dw_top_item(const dw_top *t, cb_span *item);
cb_span dw_top_items(const dw_top *t);
cb_span dw_top_last_key(const dw_top *t);
double  dw_top_consumed(const dw_top *t);
// The response's "Item" (GetItem). false: none.
bool dw_item(const char *body, size_t n, cb_span *item);
// The response's "Attributes" (UpdateItem with ReturnValues). false: none.
bool dw_attributes(const char *body, size_t n, cb_span *item);
// The response's "Items" array (Query, Scan, ExecuteStatement); an empty
// array when absent.
cb_span dw_items(const char *body, size_t n);
// BatchGetItem's Responses.<table> array.
cb_span dw_batch_items(const char *body, size_t n, const char *table);
// LastEvaluatedKey (Query, Scan) as raw JSON, or an empty span on the last page.
cb_span dw_last_key(const char *body, size_t n);
// BatchWriteItem / BatchGetItem left some work undone (UnprocessedItems or
// UnprocessedKeys is a non-empty object): the caller sends it all again.
bool dw_unprocessed(const char *body, size_t n);
// ExecuteStatement's NextToken, or "" on the last page.
void dw_next_token(const char *body, size_t n, char *out, size_t cap);

// Attribute readers over one item (a map of name -> typed value).
bool dw_get_s(cb_span item, const char *name, char *out, size_t cap);
bool dw_get_n(cb_span item, const char *name, int64_t *out);
// A Binary attribute, base64-decoded into a malloc'd buffer (*out NULL and
// *len 0 when absent). false: present but not valid base64, or no memory.
bool dw_get_b(cb_span item, const char *name, uint8_t **out, size_t *len);

// Items as rows. Each returns false when a required attribute is missing.
bool dw_item_user(cb_span item, cb_user_row *u);
bool dw_item_refresh(cb_span item, cb_refresh_row *r);
// A SESSION item; false when it is not one or has ended by now_ms.
bool dw_item_session(cb_span item, int64_t now_ms, cb_session_row *s);
bool dw_item_game(cb_span item, cb_game_row *g);       // byte fields malloc'd
bool dw_item_member(cb_span item, cb_member_row *m);   // a MEMBER item (or its GSI1 copy)
// A DWQ_GAME_SEATS item's seat: the id after MEMBER# (a human, *bot false)
// or BOT# (a bot) in its sort key. false for any other item.
bool dw_seat_key(cb_span item, cb_uuid id, bool *bot);
bool dw_item_bot(cb_span item, cb_bot_row *b);
bool dw_item_rating(cb_span item, cb_rating_row *r);
bool dw_item_snapshot(cb_span item, cb_snapshot_row *s);   // byte fields malloc'd
bool dw_item_chat(cb_span item, cb_chat_row *c);           // message malloc'd
bool dw_item_scan(cb_span item, char game_id[CB_GAME_ID_MAX + 1], int64_t *updated_at, int64_t *last_commit_at);
// A SCAN marker's lease_until, or INT64_MIN when it carries none.
int64_t dw_item_lease_until(cb_span item);
bool dw_item_email_user(cb_span item, cb_uuid id);
// An AUTHFAIL item; false when it is not one or has expired by now_ms.
bool dw_item_signin_fails(cb_span item, int64_t now_ms, cb_signin_fails *f);
// The item's keys as raw JSON ({"pk":..,"sk":..}), for a delete or an update.
bool dw_item_key(cb_span item, cb_buf *out);
// The item rendered for the admin console: each attribute as name and text
// (S as is, N as its digits, B as "\x" hex, BOOL as true/false, NULL as a NULL
// value, anything else as its JSON). The strings live in *storage until it is
// freed; names[i] and values[i] point into it. Returns the attribute count,
// at most cap, or -1 when the item does not parse.
int dw_item_text(cb_span item, cb_buf *storage, const char **names, const char **values, int cap);

// ---- the requests ---------------------------------------------------------------------

// Point reads (ConsistentRead).
void dw_get_user(dw_req *r, const char *table, const char *id);
void dw_get_email(dw_req *r, const char *table, const char *email);
void dw_get_signin_fails(dw_req *r, const char *table, const char *email);
void dw_get_session(dw_req *r, const char *table, const char *id);
void dw_get_game(dw_req *r, const char *table, const char *id);
void dw_get_member(dw_req *r, const char *table, const char *game, const char *user);
// The game's SCAN marker (store.h lease_held), consistent.
void dw_get_scan(dw_req *r, const char *table, const char *game);

// The access patterns the backend reads by, one Query each.
typedef enum {
    DWQ_GAME_SEATS,        // arg game: every item under GAME#<id> but the ROW, which is its
                           // seats (dw_seat_key), keys only (consistent)
    DWQ_USER_GAMES,        // arg user: GSI1, newest first
    DWQ_USER_CHATS,        // arg user: GSI1, keys
    DWQ_USER_TOKENS,       // arg user: GSI1, keys
    DWQ_GAME_SNAPSHOTS,    // arg game: GSI1, keys
    DWQ_PLAYER_SNAPSHOTS,  // arg user: GSI2 (forward chooses the order)
    DWQ_SNAPSHOT_ITEMS,    // arg snapshot id: SNAP#<id>, consistent
    DWQ_CHAT,              // arg game: newest first (consistent)
    DWQ_SCAN,              // arg unused: the SCAN partition (consistent)
    DWQ_BOTS,              // GSI2 BOTS
    DWQ_RATINGS,           // GSI2 RATINGS: every row the index holds, either layout
    DWQ_RATINGS_TOP,       // GSI2 RATINGS, "E#" keys only: the leaderboard order
    DWQ_USERS,             // GSI2 USERS, newest first
} dw_query_kind;
// `limit` 0 for none; `forward` only matters for DWQ_PLAYER_SNAPSHOTS;
// `start` is the previous page's dw_last_key (empty for the first).
void dw_query(dw_req *r, const char *table, dw_query_kind k, const char *arg, bool forward, int limit, cb_span start);

// Users.
// signup: EMAIL (absent), PROFILE (absent) and the RATING row at `start`.
void dw_user_create(dw_req *r, const char *table, const cb_user_row *u, int32_t start);
// import: EMAIL and PROFILE as given, no condition, no rating row.
void dw_user_import(dw_req *r, const char *table, const cb_user_row *u);
// Present-only updates (ConditionalCheckFailed when the user is gone).
void dw_user_set_password(dw_req *r, const char *table, const char *id, const char *hash, const char *keep);
void dw_user_end_sessions(dw_req *r, const char *table, const char *id);
void dw_user_touch(dw_req *r, const char *table, const char *id, int64_t at);
// The AUTHFAIL item replaced (PutItem), expiring an hour after now_ms; and deleted.
void dw_signin_fails_put(dw_req *r, const char *table, const char *email, const cb_signin_fails *f, int64_t now_ms);
void dw_signin_fails_delete(dw_req *r, const char *table, const char *email);
// Deletes the PROFILE, RATING and EMAIL items (the caller deletes the rest).
void dw_user_delete_txn(dw_req *r, const char *table, const cb_user_row *u);

// Refresh tokens.
void dw_refresh_put(dw_req *r, const char *table, const cb_refresh_row *t);
// revoked = true and revoked_at = now_ms unless already set, the old item
// returned; ConditionalCheckFailed when unknown.
void dw_refresh_take(dw_req *r, const char *table, const char *hash, int64_t now_ms, int64_t spent_ms);
void dw_session_put(dw_req *r, const char *table, const cb_session_row *s);
void dw_session_revoke(dw_req *r, const char *table, const char *id, const char *user_id, int64_t expires_at);

// Games.
void dw_game_create(dw_req *r, const char *table, const cb_game_row *g, const char *creator);
// The bench's fixture: the GetItem answer ({"Item": ..}) for the game row a
// dw_game_create request writes. false when the request holds no such item.
bool dw_get_answer_of_create(const dw_req *create, cb_buf *out);
// What a commit found before it wrote: the seats stored now, and for each
// rated seat its rating row as read (the transaction is fenced on it, so a
// rating another game changed meanwhile cancels and the commit re-reads).
typedef struct {
    bool    exists;     // the rating row (or the bot) is there
    bool    account;    // a human without a row: the account exists, so a row is created
    int32_t elo;        // as read
} dw_rating_read;
typedef struct {
    int     n_humans, n_bots;
    cb_uuid humans[64];
    cb_uuid bots[64];
} dw_seats;
// THE CAS as one TransactWriteItems. Item 0 is the game: Update conditioned on
// version = expected (or, `import`, a Put with no condition). Then the
// member and bot seat puts and deletes, the rating updates (each fenced on the
// rating it read), and the SCAN marker. ratings may be NULL when c->n_rated is 0.
// When item 0 would be the only item (dw_commit_is_lone) it is instead the
// body of one UpdateItem with the same condition (dw_req_is_txn tells which).
// false when the transaction would exceed DynamoDB's 100 items.
bool dw_game_commit(dw_req *r, const char *table, const cb_game_commit *c, const dw_seats *stored,
                    const dw_rating_read *ratings, bool import);
// The commit changes the row alone: not the import, no seat to put or delete
// against `stored`, no rating, and no SCAN marker to write (the game had no
// bot work at expected_version, c->prev_known vouching for it, and has none
// after).
bool dw_commit_is_lone(const cb_game_commit *c, const dw_seats *stored, const dw_rating_read *ratings, bool import);
// The request is a TransactWriteItems (refusals in CancellationReasons), not
// a single-item write (a refusal is DW_COND_FAILED).
bool dw_req_is_txn(const dw_req *r);
// The rows a commit's ratings live in (BatchGetItem, consistent): for each
// rated seat its RATING (a human, plus the PROFILE) or BOT item. Also used by
// ratings_for (every id as both a bot and a human).
void dw_ratings_get(dw_req *r, const char *table, const char *const *ids, const bool *is_bot, int n, bool both_kinds);
// The game rows of ids (at most 100) for a list of views: BatchGetItem,
// consistent, every column but logs_packed; read the answer with
// dw_batch_items and dw_item_game (logs NULL).
void dw_games_get(dw_req *r, const char *table, const char *const *ids, int n);
// The ids of a dw_games_get answer's UnprocessedKeys (throttled, or past the
// 16 MB answer limit), as pointers into ids, into out (room for count); how many.
int dw_unprocessed_games(const char *body, size_t n, const char *table, const char *const *ids, int count,
                         const char **out);
// Reads a dw_ratings_get answer into one dw_rating_read per id (bots first
// when both kinds were asked for).
void dw_ratings_read(const char *body, size_t n, const char *table, const char *const *ids, const bool *is_bot,
                     int count, bool both_kinds, dw_rating_read *out);
// Deletes the ROW at `expected` (ALL_OLD on failure tells gone from moved),
// with the partition's other items (`keys`, raw JSON keys from
// DWQ_GAME_SEATS) and the SCAN marker. false past 100 items.
bool dw_game_delete(dw_req *r, const char *table, const char *id, int64_t expected, const cb_buf *keys, int n_keys);
// A replay's game is gone: game_id "" and its GSI1 entry removed.
void dw_snapshot_unlink(dw_req *r, const char *table, cb_span key);
// Account deletion: one replay's extras on every copy of it (`keys`, raw JSON
// keys from DWQ_SNAPSHOT_ITEMS), as one transaction, each conditioned on the
// item still being there; extras_len 0 removes the attribute (snapshot_put
// writes none then). false past 100 items.
bool dw_snapshot_set_extras(dw_req *r, const char *table, const cb_buf *keys, int n_keys,
                            const uint8_t *extras, size_t extras_len);

// The lease (seed.sql try_acquire / renew / release_bot_lease), on the game's
// SCAN marker; acquire and renew move the marker's updated_at. Each fails
// with ConditionalCheckFailed (ALL_OLD returned when the marker exists).
void dw_lease_acquire(dw_req *r, const char *table, const char *game, const char *token, int64_t now, int64_t ttl);
void dw_lease_renew(dw_req *r, const char *table, const char *game, const char *token, int64_t now, int64_t ttl);
void dw_lease_release(dw_req *r, const char *table, const char *game, const char *token);
// store.h bots_idle: DeleteItem of the game's SCAN marker, on condition its
// row_version is `version` (or it has none); fails with ConditionalCheckFailed.
void dw_bots_idle(dw_req *r, const char *table, const char *game, int64_t version);

// Ratings and bots.
void dw_rating_set_username(dw_req *r, const char *table, const char *user, const char *username);
void dw_rating_import(dw_req *r, const char *table, const cb_rating_row *rt);
// UpdateItem: the GSI2 keys of rt's row set to the rating-ordered layout (or
// removed for a row with no games), on condition its rating and games are rt's
// and its key is not already that: a row already in the layout is refused.
void dw_rating_reindex(dw_req *r, const char *table, const cb_rating_row *rt);
// Every bot replaced: deletes for the stored ids not in rows, puts for rows.
// false past 100 items.
bool dw_bots_replace(dw_req *r, const char *table, const cb_bot_row *rows, int n, const cb_uuid *stored, int n_stored);

// Snapshots and chat.
void dw_snapshot_put(dw_req *r, const char *table, const cb_snapshot_row *s);
// A transaction: the message (a reused id cancels item 0), and checks that its
// game (item 1) and its author (item 2) exist, as the foreign keys did.
void dw_chat_put(dw_req *r, const char *table, const cb_chat_row *c);

// BatchWriteItem deletes of up to 25 raw JSON keys.
void dw_batch_delete(dw_req *r, const char *table, const cb_buf *keys, int n);

// Admin.
// The key families, as the console's collections ("USER", "GAME", ...).
const char *dw_collection(int i);   // NULL past the last
bool dw_collection_valid(const char *c);
// A page of a Scan over one family, pk beginning with family#prefix.
void dw_scan_family(dw_req *r, const char *table, const char *family, const char *prefix, int limit, cb_span start);
// The import's check (store_import.c): a page of a consistent Scan over the
// whole table, keys only (Limit caps the items a page reads, so the pages stay
// small enough to pace), and which export table ("users", "bots",
// "user_elo_ratings", "games", "player_hands", "bot_hands", "game_snapshots",
// "chat_messages") an item became, with the id after its pk's family (a game,
// user, bot, replay, or for chat the game): NULL for an item no export table
// becomes.
void        dw_scan_keys(dw_req *r, const char *table, int limit, cb_span start);
const char *dw_export_kind(cb_span item, char *id, size_t cap);
// The table's provisioned capacity (the import paces itself to it): the
// smallest read and write units of the table and its indexes, a write to the
// table also being a write to every index it lands in. 0 for on-demand.
// false when the answer has no table throughput.
void dw_describe_table(dw_req *r, const char *table);
bool dw_provisioned(const char *body, size_t n, int64_t *rcu, int64_t *wcu);
// The capacity units a response says it consumed (every request asks for
// them), 0 when it does not say.
double dw_consumed(const char *body, size_t n);

// One PartiQL statement.
void dw_execute(dw_req *r, const char *statement, const char *next_token);
// The statement only reads (its first keyword is SELECT).
bool dw_statement_reads(const char *statement);

// =====================================================================================
// The Lambda event envelope (cb_event_*): Lambda's event JSON in, the response JSON out
// (main_lambda.c; the struct is aws/event.h).
//
// The kinds of event the one function receives:
//   HTTP       a Function URL (payload format 2.0, same as an HTTP API):
//              requestContext.http.method, rawPath, rawQueryString, headers
//              (lowercase names; repeated values comma-joined), cookies (joined
//              into one "cookie" header, "; " between), body, isBase64Encoded.
//              Decoded into a cb_http_req for cb_handle.
//   SCHEDULE   the bot scan: an EventBridge event ("source":"aws.events"), or
//              the JSON string literal "scan" (the Scheduler's configured input).
//   BOT_DRIVE  our self-invoke: the JSON string literal "bot_drive:<game id>".
//   IOT_AUTH   AWS IoT Core's custom authorizer request at an MQTT CONNECT
//              (an object with "protocolData"): protocolData.mqtt.password
//              (base64) and clientId, the top-level "token", and
//              protocolData.http.queryString and headers (X-Cbase-Origin).
//              Answered with cb_iot_auth_encode.
// The two literals are valid JSON for Lambda and are read by stripping the
// quotes: no object parsing on that path (decision record, 2026-10-13).
// The HTTP response is {statusCode, headers:{..}, body, isBase64Encoded} plus
// "cookies" for any Set-Cookie; a body goes base64 when its Content-Type is
// application/octet-stream (every function answer) or it is not valid UTF-8.
//
// Decoding validates the whole event as JSON first (cb_json_valid), then reads
// only the fields above. Every string the request keeps is copied into
// ev->storage; pointers into it are set at the end, once the buffer has
// stopped growing, so a realloc can never leave one dangling.

// CB_OK and *out (free with cb_event_free), or CB_E_INVALID.
int  cb_event_decode(const char *json, size_t len, cb_event *out);
// The response JSON for an HTTP event.
int  cb_event_encode_http(const cb_http_res *res, cb_buf *out);
// The answer to an IoT authorizer request (realtime/rt_iot.c decides it):
//   {"isAuthenticated":false} when !ok; else
//   {"isAuthenticated":true,"principalId":"<user id, no dashes>",
//    "disconnectAfterInSeconds":N,"refreshAfterInSeconds":N,
//    "policyDocuments":["<the policy as a JSON string>"]}
// with the policy allowing iot:Connect on client/<user>-*, iot:Subscribe on
// topicfilter/u/<user>/* and topicfilter/s/*, and iot:Receive on
// topic/u/<user>/* and topic/s/*, every ARN under arn:aws:iot:<region>:<account>.
// `user_id` must already be a checked UUID (stream_mqtt.h cb_mqtt_user_ok)
// and region and account plain [a-z0-9-] text: CB_E_INVALID otherwise, so no
// caller can write a wildcard into a policy. CB_OK or CB_E_NOMEM.
int  cb_iot_auth_encode(bool ok, const char *user_id, int64_t lifetime_s, const char *region, const char *account,
                        cb_buf *out);
// The response JSON for an event that answers nothing (the bot scan, a
// self-invoke): {}. CB_OK or CB_E_NOMEM.
int  cb_event_encode_none(cb_buf *out);
// The self-invoke payload "<job>:<key>" (with its quotes) into out, the
// writer of what cb_event_decode reads. Returns its length, or CB_E_INVALID
// for a name or key jobs.h refuses, or a short cap.
int  cb_event_job_payload(const char *job, const char *key, char *out, size_t cap);
// The first `max` bytes of an event with every secret string cut to
// "REDACTED <n>": the values of authorization, apikey, cookie(s), body,
// password and token keys (any case), and the apikey and token parameters of
// rawQueryString. For the log when an event does not decode.
int  cb_event_redact(const char *json, size_t len, size_t max, cb_buf *out);

// =====================================================================================
// The Lambda Runtime API (runtime_api.c): the error body Lambda documents for an
// invocation or init error, {"errorMessage": .., "errorType": ..}, into b.
void cb_runtime_error_body(cb_buf *b, const char *type, const char *message);

// =====================================================================================
// JWT (jwt.c): the header and claims JSON of an access token. jwt.c does the
// base64url, the HMAC and the time and UUID checks.

// The header {"alg":"HS256","typ":"JWT"} and its length.
const char *cb_js_jwt_header(size_t *n);
// The header decodes to a JSON object whose alg is HS256.
bool cb_js_jwt_header_ok(const char *json, size_t n);
// GoTrue's claims for c, in jwt.h's order (iss only when issuer is not empty),
// into out. CB_OK or CB_E_NOMEM.
int  cb_js_jwt_claims(cb_buf *out, const cb_jwt_claims *c, const char *issuer);
// The claims back: false unless the body is a valid JSON object with aud and
// role "authenticated", an integer exp, string sub and session_id, and nbf
// (when present, *has_nbf) an integer. iat defaults to 0, email and username
// to "".
bool cb_js_jwt_claims_decode(const char *json, size_t n, cb_jwt_claims *c, bool *has_nbf, int64_t *nbf);

// =====================================================================================
// The admin console's API (admin/admin.c; admin.h lists the calls). JSON
// because the console is a browser page we own and JSON is what fetch reads.
// Times are cb_iso8601's spelling, null for 0 or less.
//
// A list answer is written as it streams out of the store: a *_begin opens
// the document (NULL when out of memory), the matching adds append one
// element each, and *_end closes it, frees the stream and returns CB_OK, or
// CB_E_NOMEM when the document did not encode. A stream is always ended,
// even when the store failed and the document is thrown away.
typedef struct cb_js_stream cb_js_stream;

// The function's AWS console pages under Lambda; lambda NULL writes
// "links": null (outside Lambda).
typedef struct {
    const char *lambda, *logs_insights, *dynamodb, *backups;
} cb_js_admin_links;

typedef struct {
    const char *version, *mode, *store, *table, *public_url;
    int64_t     uptime_s, started_ms, requests, errors_5xx;
    bool        conns_known;             // serve: the process holds the sockets
    int64_t     http_conns, ws_conns;
    int64_t     games_needing_bots;      // < 0: null (the store could not say)
    uint64_t    throttled, gave_up;      // store.h cb_store_throttles
    bool        continuous_backup;       // DynamoDB: point-in-time recovery
    int64_t     last_backup_ms;          // SQLite: the newest backup file, 0 for none
    int64_t     pid, max_rss_kb, cpu_user_ms, cpu_sys_ms;
    cb_js_admin_links links;
} cb_js_admin_overview;
int cb_js_admin_overview_write(cb_buf *out, const cb_js_admin_overview *o);

// GET collections: {"store","collections":[name..]}.
cb_js_stream *cb_js_admin_collections_begin(cb_buf *out, const char *store);
void          cb_js_admin_collections_add(cb_js_stream *s, const char *name);
int           cb_js_admin_collections_end(cb_js_stream *s);

// One store row as an object of strings (a NULL name is "?", a NULL value
// null): the element of browse's and query's "rows".
void cb_js_admin_row(cb_js_stream *s, int n, const char *const *names, const char *const *values);
// GET browse: {"collection","rows":[..],"next"}.
cb_js_stream *cb_js_admin_browse_begin(cb_buf *out, const char *collection);
int           cb_js_admin_browse_end(cb_js_stream *s, const char *next);
// POST query: {"dialect","write_allowed","rows":[..],"truncated"}.
cb_js_stream *cb_js_admin_query_begin(cb_buf *out, const char *dialect, bool write_allowed);
int           cb_js_admin_query_end(cb_js_stream *s, bool truncated);
// POST query's body {"statement","confirm"}: false when statement is missing,
// not a string, or does not fit (stmt has room for the whole body); confirm
// "" when absent or too long for its buffer.
bool cb_js_admin_query_body(const char *body, size_t n, char *stmt, size_t stmt_cap, char *confirm, size_t confirm_cap);

// GET users: {"users":[{id,email,username,created_at,last_sign_in_at}..],
// "offset","next_offset"} (next_offset < 0: null).
cb_js_stream *cb_js_admin_users_begin(cb_buf *out);
void          cb_js_admin_users_add(cb_js_stream *s, const cb_user_row *u);
int           cb_js_admin_users_end(cb_js_stream *s, int64_t offset, int64_t next_offset);
// POST users/delete's body {"id"}: false when id is missing or does not fit.
bool cb_js_admin_id_body(const char *body, size_t n, char *id, size_t cap);
// {"deleted": id} and {"written": path}.
int  cb_js_admin_deleted(cb_buf *out, const char *id);
int  cb_js_admin_written(cb_buf *out, const char *path);

// One game as the console shows it: its scalar columns and its seats, read
// with the kernel's roster_decode by the caller (roster_rc not ROSTER_OK:
// "seats": null and "roster_error").
typedef struct {
    const cb_game_row *g;
    const char        *status_label;
    const Roster      *roster;
    int                roster_rc;
} cb_js_admin_game;
// GET games: {"games":[game..],"next"}.
cb_js_stream *cb_js_admin_games_begin(cb_buf *out);
void          cb_js_admin_games_add(cb_js_stream *s, const cb_js_admin_game *g);
int           cb_js_admin_games_end(cb_js_stream *s, const char *next);
// GET game: the game, its byte sizes, and the spectator envelope as hex
// (view_ok false: envelope_hex null and envelope_error; envelope_hex NULL
// with view_ok: null).
typedef struct {
    bool        view_ok;
    const char *envelope_hex;
    int64_t     envelope_version;
    const char *envelope_error;
} cb_js_admin_envelope;
int cb_js_admin_game_write(cb_buf *out, const cb_js_admin_game *g, const cb_js_admin_envelope *e);

// GET bots: {"bots":[{id,nickname,strategy_key,elo_rating,previous_elo (null
// when < 0),games_played,updated_at}..]}.
cb_js_stream *cb_js_admin_bots_begin(cb_buf *out);
void          cb_js_admin_bots_add(cb_js_stream *s, const cb_bot_row *b);
int           cb_js_admin_bots_end(cb_js_stream *s);

// GET logs: {"last_seq","entries":[{seq,at,level,module,msg}..],"links"}.
cb_js_stream *cb_js_admin_logs_begin(cb_buf *out, uint64_t last_seq);
void          cb_js_admin_logs_add(cb_js_stream *s, uint64_t seq, int64_t at_ms, const char *level, const char *module,
                                   const char *msg);
int           cb_js_admin_logs_end(cb_js_stream *s, const cb_js_admin_links *links);

// GET config: {"mode","store","table","allowed_origins","aws_region",
// "function_name","settings":[{name,value,secret}..]}.
typedef struct {
    const char *mode, *store, *table, *allowed_origins, *aws_region, *function_name;
} cb_js_admin_config;
cb_js_stream *cb_js_admin_config_begin(cb_buf *out, const cb_js_admin_config *c);
void          cb_js_admin_config_add(cb_js_stream *s, const char *name, const char *value, bool secret);
int           cb_js_admin_config_end(cb_js_stream *s);

// GET backups: SQLite's VACUUM INTO files ({"store","kind":"vacuum_into",
// "dir","files":[{name,size,at}..],"links"}), or DynamoDB's point-in-time
// recovery explained ({"store","kind":"pitr","explain","links"}).
typedef struct {
    const char *name;
    int64_t     size, mtime_ms;
} cb_js_admin_backup;
typedef struct {
    const char               *store;
    bool                      sqlite;
    const char               *dir;        // sqlite
    const char               *explain;    // not sqlite
    const cb_js_admin_backup *files;
    int                       n_files;
    cb_js_admin_links         links;
} cb_js_admin_backups;
int cb_js_admin_backups_write(cb_buf *out, const cb_js_admin_backups *b);

// =====================================================================================
// CloudFront (cf_): THE ONE XML SHAPE c_base speaks. CloudFront's API is XML,
// not JSON; it lives here so every tolerated foreign text format has one
// file, and the containment gate (test/unit_json_gate.c) fails any other
// file that holds an XML-looking literal (an "<?xml" prolog or an xmlns).

// The body of CreateInvalidation (API version 2020-05-31): an
// InvalidationBatch of `n` paths and the caller reference, the paths in the
// order given. Text is escaped (& < > " '). false when `out` ran out of memory.
bool cf_invalidation_xml(cb_buf *out, const char *const *paths, int n, const char *caller_ref);

// =====================================================================================
// SSM Parameter Store (ssm_): where the function's secrets live (aws/ssm.h).

// The body of GetParameters for these names, decrypted (WithDecryption).
// false when `out` ran out of memory.
bool ssm_get_parameters_body(cb_buf *out, const char *const *names, int n);
// The Value of the parameter `name` in a GetParameters answer, into out: 1;
// 0 when the answer does not list it; -1 when it does but the value is not
// one to hold (not text, longer than cap - 1, a NUL in it, an answer that
// does not parse). out is "" unless 1.
int ssm_parameter_value(const char *body, size_t len, const char *name, char *out, size_t cap);

// =====================================================================================
// The client's side (bench/): what AWS hands the function for a browser's
// request, the mirror of the event envelope above.

// A Function URL event (payload 2.0) for POST /v1/action as AWS
// delivers a browser's request through CloudFront: its recorded headers, the
// bearer, the binary body as base64.
void cb_js_client_function_url_event(cb_buf *out, const char *bearer, int content_length, const char *body_b64);

#endif
