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
//                    Query on the table or an index (core_json.c or aws_json.c spells them)
//   store_sqlite.c   one `items` table (providers/local/sql/schema.sql), the attributes in a
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

// THE LIMITS, the tightest of every provider (docs/PLATFORM.md 8.7 decision
// 1): every backend refuses a write past one the same way, CB_E_INVALID with
// nothing sent or written (cb_iwrite_check), so a product that works on
// local works everywhere.
#define CB_ITEM_ATTRS        48                 // attributes one item may hold, pk and sk among them
#define CB_ITEM_KEY_MAX      1024               // bytes of pk, sk and each index key (DynamoDB's sort key limit; its pk allows 2048)
#define CB_ITEM_NAME_MAX     30                 // bytes of an attribute's name (the DynamoDB spelling aliases each as #name)
#define CB_ITEM_UPDATE_NAMES 32                 // attributes one update names: its sets, its TTL's two, its clauses'
#define CB_ITEM_SIZE_MAX     (400 * 1024)       // an item's bytes as DynamoDB counts them (cb_av_size)
#define CB_ITEM_TXN_MAX      100                // DynamoDB's TransactWriteItems limit
#define CB_ITEM_TXN_SIZE_MAX (4 * 1024 * 1024)  // and its limit on the writes' bytes in all

typedef enum { CB_AV_S = 1, CB_AV_N, CB_AV_BOOL, CB_AV_B, CB_AV_SL } cb_av_kind;   // SL: a list of strings

// One attribute value, the strings and bytes the caller's.
typedef struct {
    const char    *name;
    cb_av_kind     kind;
    const char    *s;        // CB_AV_S
    int64_t        n;        // CB_AV_N
    bool           b;        // CB_AV_BOOL
    const uint8_t *bytes;    // CB_AV_B
    size_t         len;      // CB_AV_B
    const char *const *list; // CB_AV_SL
    int            n_list;   // CB_AV_SL
} cb_av;

#define CB_AVS(nm, v)    ((cb_av){ .name = (nm), .kind = CB_AV_S, .s = (v) })
#define CB_AVN(nm, v)    ((cb_av){ .name = (nm), .kind = CB_AV_N, .n = (v) })
#define CB_AVBOOL(nm, v) ((cb_av){ .name = (nm), .kind = CB_AV_BOOL, .b = (v) })
#define CB_AVB(nm, p, l) ((cb_av){ .name = (nm), .kind = CB_AV_B, .bytes = (p), .len = (l) })
#define CB_AVSL(nm, l, n) ((cb_av){ .name = (nm), .kind = CB_AV_SL, .list = (l), .n_list = (n) })

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

#define CB_ITEM_LIST_MAX 16   // strings one list attribute holds

typedef struct {
    int    n;
    struct {
        size_t     name;     // offsets into store
        cb_av_kind kind;
        size_t     off, len; // S: text (NUL-terminated), B: bytes, SL: len strings, NUL-terminated, one after another
        int64_t    n;
        bool       b;
    } a[CB_ITEM_ATTRS];
    cb_buf store;
    // cb_item_get's list for an SL attribute (valid until the next get of one).
    const char *list[CB_ITEM_LIST_MAX];
} cb_item;

void cb_item_init(cb_item *it);
void cb_item_reset(cb_item *it);
void cb_item_free(cb_item *it);
// Adds one attribute (copied). false when the item is full or out of memory.
bool cb_item_add(cb_item *it, const cb_av *v);
// Do a and b hold the same value (the kind and the value; the names are not
// compared)? A condition's comparison, on SQLite and in DynamoDB's spelling.
bool cb_av_same(const cb_av *a, const cb_av *b);
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

// The item as the admin console's text row: each attribute's name and its
// value as text (S as is, N its digits, BOOL true/false, B "\x" hex, SL its
// strings joined by ','), at most cap of them, in store (reset first), which
// names[i] and values[i] point into until it changes. The count.
int cb_item_text(const cb_item *it, cb_buf *store, const char **names, const char **values, int cap);

// The binary record SQLite keeps an item's attributes in (no JSON): for each
// attribute, u8 name length, the name, u8 kind, then S/B: u32 length and the
// bytes, N: 8 bytes little-endian, BOOL: one byte. Encoded from an item into
// out (appended); decoded into an item (added). false on a malformed record.
// SL: u32 count, then each string as an S.
bool cb_item_record_encode(const cb_item *it, cb_buf *out);
bool cb_item_record_decode(const uint8_t *p, size_t n, cb_item *it);

// ---- writes ------------------------------------------------------------------------------

typedef enum {
    CB_IW_PUT = 1,   // the whole item: key + attrs (+ ttl)
    CB_IW_UPDATE,    // sets on an item (made when absent unless the condition says otherwise)
    CB_IW_DELETE,
    CB_IW_CHECK,     // a condition only (in a transaction)
} cb_iw_op;

// A CONDITION is up to CB_ICOND_MAX clauses, all of which must hold.
typedef enum {
    CB_C_EXISTS = 1,     // the item exists
    CB_C_ABSENT,         // the item does not exist
    CB_C_EQ,             // the attribute v.name equals v (an absent one does not)
    CB_C_NE,             // the attribute differs from v (an absent one does: DynamoDB's <>)
    CB_C_LT,             // the attribute is a number below v.n
    CB_C_ATTR_EXISTS,    // the attribute v.name is present
    CB_C_ABSENT_OR_EQ,   // the attribute is absent or equals v
    CB_C_ABSENT_OR_LT,   // the attribute is absent or a number below v.n
} cb_ccheck;

typedef struct {
    cb_ccheck check;
    cb_av     v;   // the attribute's name and the value it is held to (unused by EXISTS, ABSENT)
} cb_iclause;

#define CB_ICOND_MAX 4
// The common ones.
#define CB_IF_EXISTS ((cb_iclause){ .check = CB_C_EXISTS })
#define CB_IF_ABSENT ((cb_iclause){ .check = CB_C_ABSENT })

typedef enum {
    CB_SET_TO = 0,     // name = value
    CB_SET_IF_ABSENT,  // name = value only when the item has no name yet
    CB_SET_INCR,       // name = (name, or 0 when absent) + value (a number)
    CB_SET_REMOVE,     // name removed (its value unused)
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
    cb_iclause     cond[CB_ICOND_MAX];    // all must hold
    int            n_cond;
    // In a transaction: a refusal of this write brings the item back
    // (cb_store_ops item_txn *failed_had_item). item_write asks it whenever it
    // is handed `old`.
    bool           old_on_fail;
} cb_iwrite;

// A QUERY of one partition of the table or of an index.
typedef struct {
    int         index;       // 0 the table (pk), 1 gsi1 (gsi1pk), 2 gsi2 (gsi2pk)
    const char *pk;
    const char *sk_prefix;   // NULL, or the sort key begins with it
    const char *sk_below;    // NULL, or the sort key sorts below it
    bool        forward;     // ascending sort key
    bool        keys_only;   // pk and sk only, on every index
    int         page;        // items a page (0: the backend's own)
} cb_iquery;

// THE LIMITS' ONE OWNER (the CB_ITEM_* limits above), which every backend
// calls before it sends or writes anything, so all refuse alike:
//   cb_av_size     an attribute's bytes as DynamoDB counts them: its name, and
//                  S its bytes, B its bytes, N 21 (the most a number takes),
//                  BOOL 1, SL 3 and each string's bytes and 1
//   cb_item_size   the sum over an item's attributes
//   cb_ikey_check  pk and sk each 1 to CB_ITEM_KEY_MAX bytes
//   cb_iwrite_check  w[0..n): item_write's one write (txn false: no CHECK),
//                  or item_txn's 1 to CB_ITEM_TXN_MAX (no item twice, the
//                  bytes in all at most CB_ITEM_TXN_SIZE_MAX). Each write: a
//                  known op, its key, at most CB_ICOND_MAX clauses (a CHECK at
//                  least one), every name 1 to CB_ITEM_NAME_MAX bytes and
//                  none twice (a set's, the TTL's, an attribute's), pk and sk
//                  never among them, an index key a string of a key's length,
//                  a PUT's item at most CB_ITEM_ATTRS and CB_ITEM_SIZE_MAX, an
//                  UPDATE naming at most CB_ITEM_UPDATE_NAMES.
// Each answers CB_OK or CB_E_INVALID. What an update makes of a stored item
// is the backend's to check (SQLite refuses it; DynamoDB answers an error).
size_t cb_av_size(const cb_av *v);
size_t cb_item_size(const cb_item *it);
int cb_ikey_check(cb_ikey k);
int cb_iwrite_check(const cb_iwrite *w, int n, bool txn);
// A query: an index 0 to 2, its pk a key's, a prefix or bound NULL or 1 to
// CB_ITEM_KEY_MAX bytes.
int cb_iquery_check(const cb_iquery *q);

// What the backends implement (store.h cb_store_ops item_*), each CB_OK or
// CB_E_INVALID past a limit (the checks above, nothing sent), or:
//   get     CB_E_NOT_FOUND; consistent asks for a strongly consistent read
//   write   CB_E_CONFLICT when the condition fails; old (may be NULL) gets the
//           item as it was before (empty when it was absent), on success
//           (UPDATE, DELETE) and on a refusal alike
//   txn     CB_E_CONFLICT when any condition fails (nothing written), with
//           *failed the index of the first that did (-1: unknown) and
//           *failed_had_item whether that item existed (asked by old_on_fail)
//   query   the items of a cb_iquery, in its order; fn returns false to stop.
//           CB_E_DB on a failure.
//   batch   the items at keys[0..n) (a repeated key read once), each handed
//           to fn with its key's index; an absent one is not. proj NULL (every
//           attribute) or the attributes wanted, pk and sk always among them.
typedef bool (*cb_item_fn)(const cb_item *it, void *ud);
typedef bool (*cb_item_at_fn)(int i, const cb_item *it, void *ud);
typedef bool (*cb_ikey_fn)(const char *pk, const char *sk, void *ud);

#endif
