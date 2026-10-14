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

// The import's stored count on DynamoDB (dw_scan_count): the items one
// export table became, every page summed. CB_E_INVALID for a store that is not
// DynamoDB or an unknown name, CB_E_DB on a failed Scan.
int cb_store_dynamo_count(cb_store *s, const char *export_name, long long *out);

#endif
