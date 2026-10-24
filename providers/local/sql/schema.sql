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

-- An account. email is lowercase and unique (the sign-in lookup). username is the name as it is shown,
-- exactly as it was stored (names made before the case rule keep their lower case); username_key is
-- the form two names compare by (core/src/auth/names.h cb_name_key, the one owner) and is unique, so
-- two spellings of one name are refused, a race included (signup answers "name taken"). NULL (no name)
-- may repeat.
-- kept_session: the one session a session_gen bump spared (NULL: none).
CREATE TABLE IF NOT EXISTS users (
    id              BLOB PRIMARY KEY,
    email           TEXT NOT NULL UNIQUE,
    password_hash   TEXT NOT NULL,
    username        TEXT,
    username_key    TEXT,
    created_at      INTEGER NOT NULL,
    last_sign_in_at INTEGER NOT NULL DEFAULT 0,
    session_gen     INTEGER NOT NULL DEFAULT 0,
    kept_session    BLOB
) WITHOUT ROWID;
CREATE UNIQUE INDEX IF NOT EXISTS users_username_key ON users(username_key);

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

-- An identity at a provider (core/src/store/store.h cb_identity_row; schema 9): unique by (idp,
-- subject), read so at every provider sign-in, and by user_id when its account is deleted (with it).
-- grant_sealed: the provider's refresh token sealed by the server (core/src/auth/oidc.h), "" none.
CREATE TABLE IF NOT EXISTS identities (
    idp            INTEGER NOT NULL,
    subject        TEXT NOT NULL,
    user_id        BLOB NOT NULL,
    client_id      TEXT NOT NULL,
    email          TEXT NOT NULL,
    email_verified INTEGER NOT NULL,
    private_email  INTEGER NOT NULL,
    grant_sealed   TEXT NOT NULL,
    created_at     INTEGER NOT NULL,
    updated_at     INTEGER NOT NULL,
    PRIMARY KEY (idp, subject)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS identities_user ON identities(user_id);

-- A one-time key's claim (store.h once_claim; schema 9): a provider token's hash, held until the
-- token could no longer be used. Each claim first sweeps the claims past their time.
CREATE TABLE IF NOT EXISTS once_claims (
    key        BLOB PRIMARY KEY,
    expires_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS once_claims_expiry ON once_claims(expires_at);
