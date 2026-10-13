// store_dynamo.c - the DynamoDB backend (store.h): production under Lambda.
// STUB: the store builder fills this in.
//
// One table, PAY_PER_REQUEST, pk/sk strings (DESIGN.md "Store" has the item
// layout: USER#, EMAIL#, REFRESH#, GAME#, MEMBER#, BOT#, SNAP#, CHAT#, CONN#,
// and the GSIs). Bytes are Binary (B) attributes. game_commit is one
// TransactWriteItems: the game Update with ConditionExpression
// "version = :expected", the membership Puts and Deletes, the rating Updates;
// a TransactionCanceledException whose first reason is
// ConditionalCheckFailed is CB_E_CONFLICT.
//
// THE JSON TAX STAYS HERE: DynamoDB's API is JSON over HTTPS with SigV4
// (aws/https.h, aws/sigv4.h); this file is the only one that writes or reads it.
#include "store.h"

int cb_store_open_dynamo(const char *table, cb_store **out) {
    (void)table;
    *out = NULL;
    return CB_E_NOTIMPL;
}
