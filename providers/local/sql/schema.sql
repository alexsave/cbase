-- c_base schema (DESIGN.md "Store"). SQLite, WAL.
--
-- APPLIED AT EVERY START, inside one transaction, by
-- providers/local/src/store/store_sqlite.c: every statement here is
-- idempotent (IF NOT EXISTS, DROP ... IF EXISTS), and the database's PRAGMA
-- user_version is then set to CB_SCHEMA_VERSION (core/src/store/store.h). A database whose user_version is
-- NEWER than the binary's is refused: an old binary must not write to a
-- schema it does not know.
--
-- EVERY ROW IS AN ITEM (schema 6): the store's item layer (core/src/store/items.h)
-- is DynamoDB's model on both backends, and the rows above it are written
-- once (core/src/store/rows_auth.c, the product's own: foolish/server/src/store/rows_foolish.c)
-- with the keys and attributes aws_json.h "THE ITEM LAYOUT" lists. SQLite holds no production data,
-- so the tables of schemas 1 to 5 are dropped with what a development
-- database had in them.

-- An item is its pk and sk, the index keys it is listed under, its TTL in ms,
-- and every attribute in one binary record (items.h cb_item_record_*). An item
-- past expires_ms stays until it is overwritten or deleted, as DynamoDB's TTL
-- may keep one for days: a reader treats it as gone (the tightest of the two,
-- docs/PLATFORM.md 8.3.2).
CREATE TABLE IF NOT EXISTS items (
    pk         TEXT NOT NULL,
    sk         TEXT NOT NULL,
    gsi1pk     TEXT,
    gsi1sk     TEXT,
    gsi2pk     TEXT,
    gsi2sk     TEXT,
    expires_ms INTEGER,
    attrs      BLOB NOT NULL,
    PRIMARY KEY (pk, sk)
);
CREATE INDEX IF NOT EXISTS idx_items_gsi1 ON items(gsi1pk, gsi1sk) WHERE gsi1pk IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_items_gsi2 ON items(gsi2pk, gsi2sk) WHERE gsi2pk IS NOT NULL;

-- The tables of schemas 1 to 5, children first (rt_conns and rt_conn_topics:
-- schema 3's API Gateway connections, gone since realtime moved to IoT Core).
DROP TABLE IF EXISTS rt_conn_topics;
DROP TABLE IF EXISTS rt_conns;
DROP TABLE IF EXISTS chat_messages;
DROP TABLE IF EXISTS game_snapshots;
DROP TABLE IF EXISTS game_members;
DROP TABLE IF EXISTS game_bots;
DROP TABLE IF EXISTS ratings;
DROP TABLE IF EXISTS games;
DROP TABLE IF EXISTS sessions;
DROP TABLE IF EXISTS refresh_tokens;
DROP TABLE IF EXISTS signin_fails;
DROP TABLE IF EXISTS users;
DROP TABLE IF EXISTS bots;

-- The bots' seed table of schemas 1 to 6: the seed is foolish's own now
-- (foolish/server/src/store/bot_seed.c), written as BOT# items at open.
DROP TABLE IF EXISTS bot_seed;
