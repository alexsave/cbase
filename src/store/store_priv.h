// store_priv.h - what the two store backends share and nothing else needs:
// the orders the list functions promise, so SQLite and DynamoDB cannot drift.
#ifndef CB_STORE_PRIV_H
#define CB_STORE_PRIV_H

#include "store.h"
#include "util/str.h"

// Ladder order (bots_list): the kernel's strength tier of the strategy key
// (bot_roster.h, weakest first; an unknown key last), then the nickname.
int  cb_store_bot_ladder_cmp(const cb_bot_row *a, const cb_bot_row *b);
void cb_store_bots_sort(cb_bot_row *rows, int n);
// Leaderboard order (ratings_list): rating highest first, then user id.
void cb_store_ratings_sort(cb_rating_row *rows, int n);
// THE ADMIN CONSOLE'S COLLECTIONS, the same on both backends: the key
// families of the item layout (json_surface.h "THE ITEM LAYOUT"). The i-th
// (NULL past the last); and the pk prefix a collection's items start with,
// with `prefix` after it ("GAME#ab", or "SCAN": a whole partition), into out:
// false for a name that is not one (CB_ADMIN_GAMES is GAME).
const char *cb_store_collection(int i);
bool cb_store_collection_pk(const char *c, const char *prefix, char *out, size_t cap);

// users_list's search: `needle` ("" matches all) is in the email or the
// username, ASCII case-insensitively.
bool cb_store_user_matches(const cb_user_row *u, const char *needle);

// The DynamoDB backend over a transport the caller supplies: what
// cb_store_open_dynamo does with HTTPS and SigV4, and what test/unit_dynamo.c
// does with recorded answers. `send` posts one request (its X-Amz-Target and
// JSON body) and fills the HTTP status and the response body; it returns
// CB_OK, or CB_E_IO when nothing came back.
typedef int (*cb_dynamo_send_fn)(void *ud, const char *target, const uint8_t *body, size_t len, int *status,
                                 cb_buf *resp);
int cb_store_open_dynamo_with(const char *table, cb_dynamo_send_fn send, void *ud, cb_store **out);

// Bulk mode, for cbase-import (an offline tool with no one waiting): throttling
// is retried with no give-up (jittered pauses up to 2 s), and every send is
// paced to the table's provisioned capacity, read with DescribeTable (the
// smallest of the table's and its indexes' units; on-demand is unpaced), out
// of a token bucket each response's ConsumedCapacity is taken from. The
// request path never turns it on. CB_OK, or CB_E_DB when DescribeTable failed
// (bulk retrying is on, unpaced), CB_E_INVALID for a store that is not DynamoDB.
int cb_store_dynamo_bulk(cb_store *s);
// The clock the pauses and the pacing use (tests: a fake one).
void cb_store_dynamo_clock(cb_store *s, int64_t (*mono)(void), void (*sleep)(int64_t ms));

// The import's check on DynamoDB (dw_scan_keys): every item of the table one
// export table became, as fn(export_name, id) with the id in its pk (a game's
// seats and a game's chat give the game's), in pages of 100 keys, paced in
// bulk mode. CB_E_INVALID for a store that is not DynamoDB, CB_E_DB on a
// failed Scan.
typedef void (*cb_dynamo_item_fn)(const char *export_name, const char *id, void *ud);
int cb_store_dynamo_export_items(cb_store *s, cb_dynamo_item_fn fn, void *ud);

#endif
