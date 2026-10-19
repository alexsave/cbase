// store_sql.h - what a product's rows use on the SQL store (providers/local,
// store_sqlite.c): the locked connection, its prepared statements, and the
// group-committed write transaction. A product names its tables and
// statements in its cb_store_app (sql_schema, sql_stmts) and its rows table
// (sql_rows) is written against this, with SQLite's own API for binding and
// reading columns.
//
// THREADS AND TRANSACTIONS. cb_sql_read runs fn with the connection locked:
// nothing else runs on it meanwhile, so several statements read one state.
// cb_sql_write queues fn for the next group: the first writer to find no group
// running runs every queued fn in ONE transaction (a SAVEPOINT each when there
// are several, so a fn that answers an error undoes only its own writes) and
// COMMITs; each caller returns only after that COMMIT, so a write a client saw
// acknowledged is in the WAL. fn must not call the store (it would deadlock):
// collect rows in fn, hand them to callbacks after.
//
// IDS. A UUID is stored as its 16 bytes (cb_sql_bind_uuid, cb_sql_col_uuid);
// a text that is not a canonical lowercase UUID binds as NULL, which matches
// no row (a read then answers CB_E_NOT_FOUND) and refuses a NOT NULL column.
#ifndef CB_STORE_SQL_H
#define CB_STORE_SQL_H

#include "store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sqlite3_stmt;
typedef struct cb_sql cb_sql;
typedef int (*cb_sql_fn)(cb_sql *x, void *ud);

// Is s the SQL store (so its product rows are the app's sql_rows)?
bool cb_store_is_sql(const cb_store *s);

int cb_sql_read(cb_store *s, cb_sql_fn fn, void *ud);
int cb_sql_write(cb_store *s, cb_sql_fn fn, void *ud);

// The product's statement `id` (its cb_store_app sql_stmts[id]), prepared on
// first use, reset and unbound. NULL after logging why.
struct sqlite3_stmt *cb_sql_stmt(cb_sql *x, int id);
// Steps st to its end and resets it: CB_OK, or the store's code for SQLite's
// answer (a unique or primary key: CB_E_CONFLICT; else logged, CB_E_DB).
// NULL st (a failed prepare) is CB_E_DB.
int cb_sql_run(cb_sql *x, struct sqlite3_stmt *st, const char *what);
// SQLite's answer `rc` to `what` as a store code (as cb_sql_run maps it).
int cb_sql_fail(cb_sql *x, const char *what, int rc);
// Rows the last statement changed.
int cb_sql_changes(cb_sql *x);

// A UUID's 16 bytes at parameter i (NULL when id is not one); true when bound as bytes.
bool cb_sql_bind_uuid(struct sqlite3_stmt *st, int i, const char *id);
// Text, or NULL for NULL or "".
void cb_sql_bind_text_or_null(struct sqlite3_stmt *st, int i, const char *v);
// Bytes (an empty blob for n 0: never NULL).
void cb_sql_bind_bytes(struct sqlite3_stmt *st, int i, const void *p, size_t n);
// Column c as UUID text into out ("" for NULL or not 16 bytes).
void cb_sql_col_uuid(struct sqlite3_stmt *st, int c, char out[CB_UUID_LEN + 1]);
// Column c's text into out (cut to cap - 1; "" for NULL).
void cb_sql_col_text(struct sqlite3_stmt *st, int c, char *out, size_t cap);
// A malloc'd copy of column c's bytes (*out NULL and *len 0 for NULL or empty). false: no memory.
bool cb_sql_col_bytes(struct sqlite3_stmt *st, int c, uint8_t **out, size_t *len);

#endif
