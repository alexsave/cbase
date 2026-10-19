// store_priv.h - what the store backends share and nothing else needs, so
// SQLite and DynamoDB cannot drift.
#ifndef CB_STORE_PRIV_H
#define CB_STORE_PRIV_H

#include "store.h"
#include "util/str.h"

// THE ADMIN CONSOLE'S COLLECTIONS, the same on both backends: the key
// families of the item layout (aws_json.h "THE ITEM LAYOUT"). The i-th
// (NULL past the last); and the pk prefix a collection's items start with,
// with `prefix` after it ("GAME#ab", or "SCAN": a whole partition), into out:
// false for a name that is not one (CB_ADMIN_GAMES is GAME).
const char *cb_store_collection(int i);
bool cb_store_collection_pk(const char *c, const char *prefix, char *out, size_t cap);

// users_list's search: `needle` ("" matches all) is in the email or the
// username, ASCII case-insensitively.
bool cb_store_user_matches(const cb_user_row *u, const char *needle);

// The DynamoDB backend over a transport the caller supplies: what
// cb_store_open_dynamo does with HTTPS and SigV4, and what providers/aws/test/unit_dynamo.c
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
// Temporary credentials run out (`aws login` exports last 15 minutes). With a command set, a request
// answered ExpiredToken / UnrecognizedClient runs it (a credential_process: it prints
// {"AccessKeyId","SecretAccessKey","SessionToken"} on stdout), puts the result in AWS_ACCESS_KEY_ID,
// AWS_SECRET_ACCESS_KEY and AWS_SESSION_TOKEN, and sends the request again, at most 3 times a request.
// CB_E_INVALID for a store that is not DynamoDB.
int cb_store_dynamo_creds_process(cb_store *s, const char *command);
// After cb_store_dynamo_bulk: holds the READ pacing to `share` (> 0, at most 1)
// of the provisioned read capacity, so an export can leave the rest to the
// live traffic. Writes keep their pace. CB_E_INVALID outside bulk mode; on an
// on-demand table (unpaced) it changes nothing.
int cb_store_dynamo_read_share(cb_store *s, double share);
// The clock the pauses and the pacing use (tests: a fake one).
void cb_store_dynamo_clock(cb_store *s, int64_t (*mono)(void), void (*sleep)(int64_t ms));


#endif
