// store_sqlite.c - the SQLite backend (store.h): `cbase serve`, the tests, the
// import rehearsal. STUB: the store builder fills this in.
//
// To build: one connection opened with SQLITE_OPEN_FULLMUTEX, WAL,
// synchronous=NORMAL, foreign_keys=ON, busy_timeout; sql/schema.sql applied in
// one transaction at open (cbase_schema_sql), then PRAGMA user_version =
// CB_SCHEMA_VERSION, refusing a database whose user_version is newer.
// game_commit is one BEGIN IMMEDIATE transaction: UPDATE games ... WHERE id = ?
// AND version = ? (0 rows changed -> CB_E_CONFLICT, rollback), then
// game_members / game_bots rewritten from the commit, then the ratings.
#include "store.h"

int cb_store_open_sqlite(const char *path, cb_store **out) {
    (void)path;
    *out = NULL;
    return CB_E_NOTIMPL;
}
