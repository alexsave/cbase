-- c_base schema (DESIGN.md "Store"). SQLite, WAL.
--
-- APPLIED AT EVERY START, inside one transaction, by
-- providers/local/src/store/store_sqlite.c: every statement here is
-- idempotent (IF NOT EXISTS, DROP ... IF EXISTS), and the database's PRAGMA
-- user_version is then set to CB_SCHEMA_VERSION (core/src/store/store.h). A database whose user_version is
-- NEWER than the binary's is refused: an old binary must not write to a
-- schema it does not know. The product's own tables follow in the same
-- transaction (store.h cb_store_app sql_schema; foolish: foolish/server/sql/schema.sql).
--
-- PLAIN TABLES (schema 7). Every row is a row: no item records, no key
-- strings. A UUID is its 16 bytes (BLOB); the text form exists only where a
-- person or a client sees it. A table whose primary key is how it is read is
-- WITHOUT ROWID, and an index exists only for a query the code runs. Schema 6
-- kept every row as a DynamoDB-shaped item in one `items` table; SQLite then
-- held no production data, so it is dropped with what a development database
-- had in it (production arrives through cbase-export).

DROP TABLE IF EXISTS items;

-- An account. email is lowercase and unique (signup's uniqueness).
-- kept_session: the one session a session_gen bump spared (NULL: none).
CREATE TABLE IF NOT EXISTS users (
    id              BLOB PRIMARY KEY,
    email           TEXT NOT NULL UNIQUE,
    password_hash   TEXT NOT NULL,
    username        TEXT,
    created_at      INTEGER NOT NULL,
    last_sign_in_at INTEGER NOT NULL DEFAULT 0,
    session_gen     INTEGER NOT NULL DEFAULT 0,
    kept_session    BLOB
) WITHOUT ROWID;

-- A session; read by id on every refresh. Rows of a user are deleted with the
-- account and, once expired, by the next session_put of the same user.
CREATE TABLE IF NOT EXISTS sessions (
    id         BLOB PRIMARY KEY,
    user_id    BLOB NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    gen        INTEGER NOT NULL,
    revoked    INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id);

-- A refresh token by its SHA-256 (32 bytes). expires_at 0: none.
CREATE TABLE IF NOT EXISTS refresh_tokens (
    token_hash BLOB PRIMARY KEY,
    user_id    BLOB NOT NULL,
    session_id BLOB,
    created_at INTEGER NOT NULL,
    revoked    INTEGER NOT NULL,
    revoked_at INTEGER NOT NULL DEFAULT 0,
    expires_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS refresh_tokens_user ON refresh_tokens(user_id);

-- Failed sign-ins at one email; gone (read as none) at expires_at, an hour
-- after the last write.
CREATE TABLE IF NOT EXISTS signin_fails (
    email        TEXT PRIMARY KEY,
    n            INTEGER NOT NULL,
    last_at      INTEGER NOT NULL,
    locked_until INTEGER NOT NULL,
    expires_at   INTEGER NOT NULL
) WITHOUT ROWID;
