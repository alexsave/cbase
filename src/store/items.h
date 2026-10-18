// items.h - the store's item layer: DynamoDB's model, on both backends
// (docs/PLATFORM.md 2.2 and 8.3.2).
//
// An ITEM is a string partition key (pk) and sort key (sk) with typed
// attributes (string, number, boolean, bytes). Two overloaded indexes,
// gsi1 (gsi1pk, gsi1sk) and gsi2 (gsi2pk, gsi2sk), list an item under the
// keys it carries in those attributes. A write may be conditioned on the
// item existing or not, and a transaction applies up to CB_ITEM_TXN_MAX
// writes atomically. TTL is an attribute: a write's ttl_ms sets expires_ms
// (ms) and expires_at (seconds, DynamoDB's TTL attribute); DynamoDB deletes
// an expired item within days and SQLite never does, so a reader that cares
// checks expires_ms itself.
//
//   store_dynamo.c   GetItem, PutItem/UpdateItem/DeleteItem, TransactWriteItems,
//                    Query on the table or an index (json_surface.c spells them)
//   store_sqlite.c   one `items` table (sql/schema.sql), the attributes in a
//                    binary record (cb_item_record_*), the index keys in columns
//
// Rows written once above it (rows_auth.c: sessions, refresh tokens, sign-in
// failures) behave the same on both backends by construction.
#ifndef CB_ITEMS_H
#define CB_ITEMS_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CB_ITEM_ATTRS   48    // attributes one item may hold
#define CB_ITEM_KEY_MAX 1024  // DynamoDB's sort key limit (its pk allows 2048)
#define CB_ITEM_TXN_MAX 100   // DynamoDB's TransactWriteItems limit

typedef enum { CB_AV_S = 1, CB_AV_N, CB_AV_BOOL, CB_AV_B } cb_av_kind;

// One attribute value, the strings and bytes the caller's.
typedef struct {
    const char    *name;
    cb_av_kind     kind;
    const char    *s;        // CB_AV_S
    int64_t        n;        // CB_AV_N
    bool           b;        // CB_AV_BOOL
    const uint8_t *bytes;    // CB_AV_B
    size_t         len;      // CB_AV_B
} cb_av;

#define CB_AVS(nm, v)    ((cb_av){ .name = (nm), .kind = CB_AV_S, .s = (v) })
#define CB_AVN(nm, v)    ((cb_av){ .name = (nm), .kind = CB_AV_N, .n = (v) })
#define CB_AVBOOL(nm, v) ((cb_av){ .name = (nm), .kind = CB_AV_BOOL, .b = (v) })
#define CB_AVB(nm, p, l) ((cb_av){ .name = (nm), .kind = CB_AV_B, .bytes = (p), .len = (l) })

// The attributes the layer itself names.
#define CB_ITEM_PK      "pk"
#define CB_ITEM_SK      "sk"
#define CB_ITEM_GSI1PK  "gsi1pk"
#define CB_ITEM_GSI1SK  "gsi1sk"
#define CB_ITEM_GSI2PK  "gsi2pk"
#define CB_ITEM_GSI2SK  "gsi2sk"
#define CB_ITEM_TTL_MS  "expires_ms"   // ms since the epoch
#define CB_ITEM_TTL_S   "expires_at"   // seconds: DynamoDB's TTL attribute

typedef struct {
    const char *pk, *sk;
} cb_ikey;

// ---- an item as read ----------------------------------------------------------------

typedef struct {
    int    n;
    struct {
        size_t     name;     // offsets into store
        cb_av_kind kind;
        size_t     off, len; // S: text (NUL-terminated), B: bytes
        int64_t    n;
        bool       b;
    } a[CB_ITEM_ATTRS];
    cb_buf store;
} cb_item;

void cb_item_init(cb_item *it);
void cb_item_reset(cb_item *it);
void cb_item_free(cb_item *it);
// Adds one attribute (copied). false when the item is full or out of memory.
bool cb_item_add(cb_item *it, const cb_av *v);
// The attribute `name`: true and *out (its strings point into the item, valid
// until it changes), or false when it is absent.
bool cb_item_get(const cb_item *it, const char *name, cb_av *out);
// The i-th attribute (0 <= i < it->n), as cb_item_get gives it.
void cb_item_at(const cb_item *it, int i, cb_av *out);
// Typed reads: false when absent or of another kind (out untouched).
bool cb_item_s(const cb_item *it, const char *name, char *out, size_t cap);   // false also when it does not fit
bool cb_item_n(const cb_item *it, const char *name, int64_t *out);
bool cb_item_bool(const cb_item *it, const char *name, bool *out);
int64_t cb_item_n_or(const cb_item *it, const char *name, int64_t def);

// The binary record SQLite keeps an item's attributes in (no JSON): for each
// attribute, u8 name length, the name, u8 kind, then S/B: u32 length and the
// bytes, N: 8 bytes little-endian, BOOL: one byte. Encoded from an item into
// out (appended); decoded into an item (added). false on a malformed record.
bool cb_item_record_encode(const cb_item *it, cb_buf *out);
bool cb_item_record_decode(const uint8_t *p, size_t n, cb_item *it);

// ---- writes ------------------------------------------------------------------------------

typedef enum {
    CB_IW_PUT = 1,   // the whole item: key + attrs (+ ttl)
    CB_IW_UPDATE,    // sets on an item (made when absent unless the condition says otherwise)
    CB_IW_DELETE,
    CB_IW_CHECK,     // a condition only (in a transaction)
} cb_iw_op;

typedef enum {
    CB_IF_ANY = 0,
    CB_IF_EXISTS,    // the item exists
    CB_IF_ABSENT,    // the item does not exist
} cb_icond;

typedef enum {
    CB_SET_TO = 0,     // name = value
    CB_SET_IF_ABSENT,  // name = value only when the item has no name yet
    CB_SET_INCR,       // name = (name, or 0 when absent) + value (a number)
} cb_set_how;

typedef struct {
    cb_av      v;
    cb_set_how how;
} cb_iset;

typedef struct {
    cb_iw_op       op;
    cb_ikey        key;
    const cb_av   *attrs;  int n_attrs;   // PUT: every attribute but pk and sk
    const cb_iset *sets;   int n_sets;    // UPDATE
    int64_t        ttl_ms;                // > 0: expires_ms and expires_at (PUT, UPDATE)
    cb_set_how     ttl_how;               // UPDATE: how the TTL is set
    cb_icond       cond;
} cb_iwrite;

// What the backends implement (store.h cb_store_ops item_*), each CB_OK or:
//   get     CB_E_NOT_FOUND; consistent asks for a strongly consistent read
//   write   CB_E_CONFLICT when the condition fails; old (may be NULL) gets the
//           item as it was before (empty when it was absent)
//   txn     CB_E_CONFLICT when any condition fails (nothing written), with
//           *failed the index of the first that did (-1: unknown)
//   query   index 0 (the table: pk), 1 (gsi1pk) or 2 (gsi2pk); sk_prefix
//           NULL or a begins_with on the matching sort key; forward is
//           ascending sort key; page asks for that many items a page (0: the
//           backend's own); fn returns false to stop. CB_E_DB on a failure.
typedef bool (*cb_item_fn)(const cb_item *it, void *ud);

#endif
