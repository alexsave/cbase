// dynamo_wire.h - the DynamoDB wire, and nothing else: the JSON requests the
// DynamoDB backend sends and the readers for what DynamoDB answers. Private to
// src/store (store_dynamo.c and test/unit_dynamo.c).
//
// EVERY DynamoDB STRING LIVES IN dynamo_wire.c: the X-Amz-Target names, the
// item layout (pk/sk spellings, attribute names, the GSI names), the key,
// condition, update and filter expressions. store_dynamo.c decides which
// requests to send in which order and never spells one. Pure functions over
// buffers: no socket, no clock, no randomness (a token or a time is an
// argument), so a request is checked byte for byte against a recorded body.
//
// THE ITEM LAYOUT (one table, string pk and sk; GSI1 = gsi1pk/gsi1sk and
// GSI2 = gsi2pk/gsi2sk, both projecting ALL; TTL attribute expires_at):
//
//   pk                 sk                 what (GSI keys)
//   USER#<id>          PROFILE            the account (GSI2 USERS / <created>#<id>)
//   USER#<id>          RATING             the leaderboard row (GSI2 RATINGS / <id>)
//   EMAIL#<email>      USER               the user id; signup's uniqueness
//   REFRESH#<hash>     TOKEN              a refresh token (GSI1 USERREFRESH#<user>, GSI2 SESSION#<session>)
//   GAME#<id>          ROW                the game, its lease
//   GAME#<id>          MEMBER#<user>      a seated human (GSI1 USERGAMES#<user> / <updated>#<game>)
//   GAME#<id>          BOT#<bot>          a seated bot
//   BOT#<id>           BOT                a bot and its rating (GSI2 BOTS / <id>)
//   SNAP#<id>          SNAP               a replay (GSI1 GAMESNAPS#<game> / <created>#<id>)
//   SNAP#<id>          PLAYER#<user>      the replay again, once per player (GSI1 as above,
//                                         GSI2 PLAYER#<user> / <created>#<id>)
//   CHAT#<game>        <created>#<id>     a chat message (GSI1 USERCHAT#<user> / <created>#<id>)
//   CONN#<conn>        CONN               a realtime connection (TTL)
//   TOPIC#<topic>      CONN#<conn>        the connection again, per joined topic (TTL)
//   SCAN               GAME#<id>          present while the game needs bots: the heartbeat's partition
//
// Times in sort keys are 15-digit zero-padded ms so they sort as text.
#ifndef CB_DYNAMO_WIRE_H
#define CB_DYNAMO_WIRE_H

#include "store.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

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
// The error's __type after the '#', and its message, for the log.
void dw_error_text(const char *body, size_t n, char *out, size_t cap);

// One entry of a TransactionCanceledException's CancellationReasons.
typedef enum {
    DW_REASON_NONE = 0,       // "None": this item was fine
    DW_REASON_COND,           // "ConditionalCheckFailed"
    DW_REASON_CONFLICT,       // "TransactionConflict" (another transaction on the item: retry)
    DW_REASON_OTHER,
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
bool dw_get_bool(cb_span item, const char *name, bool *out);

// Items as rows. Each returns false when a required attribute is missing.
bool dw_item_user(cb_span item, cb_user_row *u);
bool dw_item_refresh(cb_span item, cb_refresh_row *r);
bool dw_item_game(cb_span item, cb_game_row *g);       // byte fields malloc'd
bool dw_item_member(cb_span item, cb_member_row *m);   // a MEMBER item (or its GSI1 copy)
bool dw_item_seat_id(cb_span item, cb_uuid id);        // a MEMBER or BOT item: whose seat
bool dw_item_bot(cb_span item, cb_bot_row *b);
bool dw_item_rating(cb_span item, cb_rating_row *r);
bool dw_item_snapshot(cb_span item, cb_snapshot_row *s);   // byte fields malloc'd
bool dw_item_chat(cb_span item, cb_chat_row *c);           // message malloc'd
bool dw_item_conn(cb_span item, cb_conn_row *c);
bool dw_item_scan(cb_span item, char game_id[CB_GAME_ID_MAX + 1], int64_t *updated_at, int64_t *last_commit_at);
bool dw_item_email_user(cb_span item, cb_uuid id);
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
void dw_get_game(dw_req *r, const char *table, const char *id);
void dw_get_member(dw_req *r, const char *table, const char *game, const char *user);
void dw_get_conn(dw_req *r, const char *table, const char *conn_id);

// The access patterns the backend reads by, one Query each.
typedef enum {
    DWQ_GAME_MEMBERS,      // arg game: its MEMBER items (consistent)
    DWQ_GAME_BOTS,         // arg game: its BOT items (consistent)
    DWQ_GAME_PARTITION,    // arg game: every item under GAME#<id> but the ROW, keys only (consistent)
    DWQ_USER_GAMES,        // arg user: GSI1, newest first
    DWQ_USER_CHATS,        // arg user: GSI1, keys
    DWQ_USER_TOKENS,       // arg user: GSI1, keys
    DWQ_SESSION_TOKENS,    // arg session: GSI2, keys
    DWQ_GAME_SNAPSHOTS,    // arg game: GSI1, keys
    DWQ_PLAYER_SNAPSHOTS,  // arg user: GSI2 (forward chooses the order)
    DWQ_SNAPSHOT_ITEMS,    // arg snapshot id: SNAP#<id>, consistent
    DWQ_CHAT,              // arg game: newest first (consistent)
    DWQ_TOPIC_CONNS,       // arg topic (consistent)
    DWQ_SCAN,              // arg unused: the SCAN partition (consistent)
    DWQ_BOTS,              // GSI2 BOTS
    DWQ_RATINGS,           // GSI2 RATINGS
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
void dw_user_set_password(dw_req *r, const char *table, const char *id, const char *hash);
void dw_user_touch(dw_req *r, const char *table, const char *id, int64_t at);
// Deletes the PROFILE, RATING and EMAIL items (the caller deletes the rest).
void dw_user_delete_txn(dw_req *r, const char *table, const cb_user_row *u);

// Refresh tokens.
void dw_refresh_put(dw_req *r, const char *table, const cb_refresh_row *t);
// revoked = true and revoked_at = now_ms unless already set, the old item
// returned; ConditionalCheckFailed when unknown.
void dw_refresh_take(dw_req *r, const char *table, const char *hash, int64_t now_ms);
// revoked = true, revoked_at = 0 on a token found through DWQ_SESSION_TOKENS
// or DWQ_USER_TOKENS (`key` is its dw_item_key).
void dw_refresh_revoke(dw_req *r, const char *table, cb_span key);

// Games.
void dw_game_create(dw_req *r, const char *table, const cb_game_row *g, const char *creator);
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
// false when the transaction would exceed DynamoDB's 100 items.
bool dw_game_commit(dw_req *r, const char *table, const cb_game_commit *c, const dw_seats *stored,
                    const dw_rating_read *ratings, bool import);
// The rows a commit's ratings live in (BatchGetItem, consistent): for each
// rated seat its RATING (a human, plus the PROFILE) or BOT item. Also used by
// ratings_for (every id as both a bot and a human).
void dw_ratings_get(dw_req *r, const char *table, const char *const *ids, const bool *is_bot, int n, bool both_kinds);
// Reads a dw_ratings_get answer into one dw_rating_read per id (bots first
// when both kinds were asked for).
void dw_ratings_read(const char *body, size_t n, const char *table, const char *const *ids, const bool *is_bot,
                     int count, bool both_kinds, dw_rating_read *out);
// Deletes the ROW at `expected` (ALL_OLD on failure tells gone from moved),
// with the partition's other items (`keys`, raw JSON keys from
// DWQ_GAME_PARTITION) and the SCAN marker. false past 100 items.
bool dw_game_delete(dw_req *r, const char *table, const char *id, int64_t expected, const cb_buf *keys, int n_keys);
// A replay's game is gone: game_id "" and its GSI1 entry removed.
void dw_snapshot_unlink(dw_req *r, const char *table, cb_span key);

// The lease (seed.sql try_acquire / renew / release_bot_lease). Each fails
// with ConditionalCheckFailed (ALL_OLD returned when the game exists).
void dw_lease_acquire(dw_req *r, const char *table, const char *game, const char *token, int64_t now, int64_t ttl);
void dw_lease_renew(dw_req *r, const char *table, const char *game, const char *token, int64_t now, int64_t ttl);
void dw_lease_release(dw_req *r, const char *table, const char *game, const char *token);
// The SCAN marker follows the lease's updated_at, when the marker exists.
void dw_scan_touch(dw_req *r, const char *table, const char *game, int64_t now);

// Ratings and bots.
void dw_rating_set_username(dw_req *r, const char *table, const char *user, const char *username);
void dw_rating_import(dw_req *r, const char *table, const cb_rating_row *rt);
// Every bot replaced: deletes for the stored ids not in rows, puts for rows.
// false past 100 items.
bool dw_bots_replace(dw_req *r, const char *table, const cb_bot_row *rows, int n, const cb_uuid *stored, int n_stored);

// Snapshots, chat, connections.
void dw_snapshot_put(dw_req *r, const char *table, const cb_snapshot_row *s);
// A transaction: the message (a reused id cancels item 0), and checks that its
// game (item 1) and its author (item 2) exist, as the foreign keys did.
void dw_chat_put(dw_req *r, const char *table, const cb_chat_row *c);
// The CONN item and one TOPIC item per topic, and deletes of the TOPIC items
// of `old_topics` the row no longer has.
void dw_conn_put(dw_req *r, const char *table, const cb_conn_row *c, const cb_conn_row *old);
void dw_conn_delete(dw_req *r, const char *table, const cb_conn_row *c);

// BatchWriteItem deletes of up to 25 raw JSON keys.
void dw_batch_delete(dw_req *r, const char *table, const cb_buf *keys, int n);

// Admin.
// The key families, as the console's collections ("USER", "GAME", ...).
const char *dw_collection(int i);   // NULL past the last
bool dw_collection_valid(const char *c);
// A page of a Scan over one family, pk beginning with family#prefix.
void dw_scan_family(dw_req *r, const char *table, const char *family, const char *prefix, int limit, cb_span start);
// One PartiQL statement.
void dw_execute(dw_req *r, const char *statement, const char *next_token);
// The statement only reads (its first keyword is SELECT).
bool dw_statement_reads(const char *statement);

#endif
