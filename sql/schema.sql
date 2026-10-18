-- c_base schema (DESIGN.md "Store"). SQLite, WAL, foreign keys on.
--
-- APPLIED AT EVERY START, inside one transaction, by src/store/store_sqlite.c:
-- every statement here is idempotent (IF NOT EXISTS, INSERT OR IGNORE), and the
-- database's PRAGMA user_version is then set to CB_SCHEMA_VERSION
-- (src/store/store.h). A database whose user_version is NEWER than the binary's
-- is refused: an old binary must not write to a schema it does not know. A
-- change that is not idempotent (an ALTER, a backfill) is a numbered step in
-- store_sqlite.c that runs once when user_version is below it, and bumps
-- CB_SCHEMA_VERSION.
--
-- TIMES are INTEGER milliseconds since the Unix epoch, the unit every kernel
-- call takes (now_ms). The facade prints them as ISO-8601 text.
-- IDS are canonical lowercase UUID text, except games.id (12 hex characters; 6 for games made before).
-- BYTES are BLOBs: the kernel's state blob, roster, session log, replay code.
--
-- There are NO player_views or spectator_views tables: a view is computed on
-- demand from the resident table (src/app/table_ops.h), never stored.

-- Accounts. password_hash is bcrypt ($2a$ / $2b$), so GoTrue's hashes import
-- unchanged and existing players keep their passwords.
CREATE TABLE IF NOT EXISTS users (
    id              TEXT PRIMARY KEY,
    email           TEXT NOT NULL UNIQUE,
    password_hash   TEXT NOT NULL,
    username        TEXT,
    created_at      INTEGER NOT NULL,
    last_sign_in_at INTEGER,
    -- A session begun at a lower session_gen is over, but kept_session
    -- (a global sign-out, a password change: store.h cb_user_row).
    session_gen     INTEGER NOT NULL DEFAULT 0,
    kept_session    TEXT
);

-- Sessions (store.h cb_session_row): ended by one write, read by every refresh.
CREATE TABLE IF NOT EXISTS sessions (
    id         TEXT PRIMARY KEY,
    user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    gen        INTEGER NOT NULL DEFAULT 0,
    revoked    INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at);

-- One row per refresh token issued and not yet expired. token_hash is the
-- lowercase hex SHA-256 of the token: the token itself is never stored. A use
-- revokes it (rotation), stamps revoked_at once, for the 10 s reuse grace, and
-- moves expires_at to a week on; a row past expires_at reads as none and goes
-- at the next refresh_put.
CREATE TABLE IF NOT EXISTS refresh_tokens (
    token_hash TEXT PRIMARY KEY,
    user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    session_id TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    revoked    INTEGER NOT NULL DEFAULT 0,
    revoked_at INTEGER,
    expires_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_refresh_tokens_expires ON refresh_tokens(expires_at);
CREATE INDEX IF NOT EXISTS idx_refresh_tokens_user ON refresh_tokens(user_id);
CREATE INDEX IF NOT EXISTS idx_refresh_tokens_session ON refresh_tokens(session_id);

-- Failed sign-ins per account (auth.h CB_AUTH_FAIL_*), by the normalized
-- email. A row past expires_at (an hour after its last write) reads as none.
CREATE TABLE IF NOT EXISTS signin_fails (
    email        TEXT PRIMARY KEY,
    n            INTEGER NOT NULL,
    last_at     INTEGER NOT NULL,
    locked_until INTEGER NOT NULL,
    expires_at   INTEGER NOT NULL
);

-- The bots, seeded below. Not a foreign-key target of games: a roster names a
-- bot by id inside the kernel's roster blob, and game_bots is the index.
CREATE TABLE IF NOT EXISTS bots (
    id            TEXT PRIMARY KEY,
    nickname      TEXT NOT NULL,
    strategy_key  TEXT NOT NULL,
    elo_rating    INTEGER NOT NULL DEFAULT 1000,
    previous_elo  INTEGER,
    games_played  INTEGER NOT NULL DEFAULT 0,
    created_at    INTEGER NOT NULL DEFAULT 0,
    updated_at    INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_bots_strategy ON bots(strategy_key);

-- The durability log of the resident tables. status is the kernel's
-- GAME_STATUS_* number; state, roster and logs_packed are the kernel's bytes,
-- written whole by every commit; game_seed is the 32-byte deal seed (NULL in a
-- lobby). version increments per commit and is what clients gate on.
-- last_commit_at is the commit's time, written only when version moves (the
-- heartbeat's abandon guard); updated_at also moves when a bot drive takes or
-- renews the lease, so the scan does not dispatch a second drive on top of a
-- live one. bot_lease_token / bot_lease_until are the bot lease (a NULL or
-- past bot_lease_until is free), as try_acquire_bot_lease kept them.
CREATE TABLE IF NOT EXISTS games (
    id             TEXT PRIMARY KEY,
    status         INTEGER NOT NULL,
    state          BLOB NOT NULL,
    roster         BLOB NOT NULL,
    needs_bots     INTEGER NOT NULL DEFAULT 0,
    game_seed      BLOB,
    logs_packed    BLOB,
    version        INTEGER NOT NULL DEFAULT 0,
    round_epoch    INTEGER NOT NULL DEFAULT 0,
    created_at     INTEGER NOT NULL,
    updated_at     INTEGER NOT NULL,
    last_commit_at INTEGER NOT NULL,
    bot_lease_token TEXT,
    bot_lease_until INTEGER
);
CREATE INDEX IF NOT EXISTS idx_games_updated_at ON games(updated_at);
CREATE INDEX IF NOT EXISTS idx_games_bots_due ON games(last_commit_at) WHERE needs_bots = 1;

-- Who sits at a game (the humans). Rewritten by every commit from the roster.
CREATE TABLE IF NOT EXISTS game_members (
    game_id   TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    user_id   TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    joined_at INTEGER NOT NULL,
    PRIMARY KEY (game_id, user_id)
);
CREATE INDEX IF NOT EXISTS idx_game_members_user ON game_members(user_id);

-- Which bots sit at a game. Rewritten by every commit from the roster.
CREATE TABLE IF NOT EXISTS game_bots (
    game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    bot_id  TEXT NOT NULL,
    PRIMARY KEY (game_id, bot_id)
);

-- One row per human who has a rating. username is the leaderboard's name
-- (NULL after an account is deleted, as the old delete_account did).
CREATE TABLE IF NOT EXISTS ratings (
    user_id      TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    username     TEXT,
    elo_rating   INTEGER NOT NULL DEFAULT 1000,
    previous_elo INTEGER,
    games_played INTEGER NOT NULL DEFAULT 0,
    updated_at   INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_ratings_elo ON ratings(elo_rating);

-- A finished game's replay: the kernel's v6 replay code and extras blob.
-- player_ids is the seated humans' ids, comma-separated, in seat order.
-- game_id is kept when the game row is deleted (as ON DELETE SET NULL was).
CREATE TABLE IF NOT EXISTS game_snapshots (
    id         TEXT PRIMARY KEY,
    game_id    TEXT REFERENCES games(id) ON DELETE SET NULL,
    player_ids TEXT NOT NULL,
    moves      BLOB NOT NULL,
    extras     BLOB,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_game_snapshots_game ON game_snapshots(game_id);
CREATE INDEX IF NOT EXISTS idx_game_snapshots_created ON game_snapshots(created_at);

CREATE TABLE IF NOT EXISTS chat_messages (
    id         TEXT PRIMARY KEY,
    game_id    TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    message    TEXT NOT NULL,
    is_system  INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_chat_messages_game ON chat_messages(game_id, created_at);

-- Realtime connections under API Gateway (store.h cb_conn_row): `cbase serve`
-- keeps its sockets in memory and never writes these; they exist so the SQLite
-- backend answers every store call the DynamoDB one does. expires_at is the
-- TTL: a row past it reads as absent, and conn_put prunes expired rows. ver is
-- the row's write count, conn_put's compare-and-set (store.h).
CREATE TABLE IF NOT EXISTS rt_conns (
    conn_id    TEXT PRIMARY KEY,
    user_id    TEXT NOT NULL DEFAULT '',
    expires_at INTEGER NOT NULL,
    ver        INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS rt_conn_topics (
    conn_id  TEXT NOT NULL REFERENCES rt_conns(conn_id) ON DELETE CASCADE,
    ord      INTEGER NOT NULL,
    topic    TEXT NOT NULL,
    PRIMARY KEY (conn_id, ord)
);
CREATE INDEX IF NOT EXISTS idx_rt_conn_topics_topic ON rt_conn_topics(topic);

-- THE BOTS: seven of every family, in ladder order, exactly the nicknames and
-- strategy keys of the kernel's c/src/bot_roster.c (which the old Postgres
-- seed carried too). That seed left the ids to uuid_generate_v4(), so they differ per database;
-- these are FIXED (UUIDv5 of the nickname in a c_base namespace) so every fresh
-- c_base database agrees. A migrated database replaces this table with the
-- hosted rows (c_base/migrate), because the hosted rosters name the hosted ids.
INSERT OR IGNORE INTO bots (id, nickname, strategy_key) VALUES
  ('2c601cde-63d2-5291-8fcb-735f5860b1ec', '%Miami 1', 'random'),
  ('98d81d2b-94d3-540f-a5c9-106293670763', '%Miami 2', 'random'),
  ('283efd87-26ce-5364-af9e-a947112b1958', '%Miami 3', 'random'),
  ('a412f34c-9aad-5fb4-a1b1-4edc48eeaf5c', '%Miami 4', 'random'),
  ('ff580062-96c9-571e-a0f0-ca29b4b43f60', '%Miami 5', 'random'),
  ('6bba657d-a02b-5067-9a0b-c173d587761e', '%Miami 6', 'random'),
  ('bddc5ebf-f123-590b-8000-bc82dc002030', '%Miami 7', 'random'),
  ('5847bff1-2330-534e-87f3-19af55d66e60', '%New York 1', 'handwritten'),
  ('23472e08-c2c0-56aa-aca4-ecf321124976', '%New York 2', 'handwritten'),
  ('4fe66421-b0c8-5f40-953f-cac1cb9e4e63', '%New York 3', 'handwritten'),
  ('a9bc397d-47b8-5680-acf8-c1719d953d9c', '%New York 4', 'handwritten'),
  ('3a4ea7c0-6e01-501f-af30-213788c4bdde', '%New York 5', 'handwritten'),
  ('35b91cb4-c981-586e-b01e-c049c9610773', '%New York 6', 'handwritten'),
  ('09ce4028-595c-5ccc-87bd-371186eb7fd6', '%New York 7', 'handwritten'),
  ('3e2ad143-95b7-58c9-bbce-e46f6fc0b545', '%Seoul 1', 'robusta'),
  ('a9b24785-6b20-5945-91c0-6021b141981e', '%Seoul 2', 'robusta'),
  ('aa652c85-86cc-5a47-88ad-67fd524e9710', '%Seoul 3', 'robusta'),
  ('ba6ba52a-7c88-526c-83a8-a612efd599b0', '%Seoul 4', 'robusta'),
  ('bf13fd55-b1e8-5454-9839-defbae5c56c6', '%Seoul 5', 'robusta'),
  ('4b5767f2-6c56-5d0e-ab81-03401e8e0f9f', '%Seoul 6', 'robusta'),
  ('2413ac32-8417-5a9b-a559-c525b4bcd1f6', '%Seoul 7', 'robusta'),
  ('89fffd65-9bbb-52ed-bc9c-a947a02482bd', '%Madrid 1', 'firecracker'),
  ('9065fd97-b504-513c-b457-eafe1973e84a', '%Madrid 2', 'firecracker'),
  ('0be39f45-d47c-5f43-beed-fb8daec26251', '%Madrid 3', 'firecracker'),
  ('84b15705-3e39-5049-9393-4f31e364f741', '%Madrid 4', 'firecracker'),
  ('17af3762-79ab-55dc-a46d-3b79b869d0ec', '%Madrid 5', 'firecracker'),
  ('7daf8ba4-2bab-5569-9e01-35979aef4015', '%Madrid 6', 'firecracker'),
  ('41883963-95f3-514a-8d12-acff29086ecd', '%Madrid 7', 'firecracker'),
  ('d5907db4-0f40-5a1d-9bd3-6ba65f69e381', '%Vienna 1', 'blackpowder'),
  ('74991079-20b9-5982-bf83-ddfbb878753e', '%Vienna 2', 'blackpowder'),
  ('2bae7716-95ca-5bc7-87fd-52125d6759d9', '%Vienna 3', 'blackpowder'),
  ('ecfe3e47-c6b4-5819-9d41-b243794f2784', '%Vienna 4', 'blackpowder'),
  ('5a90f0de-14a6-5d14-aa3a-464fae52bfaf', '%Vienna 5', 'blackpowder'),
  ('01dad0f5-9adb-5daf-b6b5-faf65a4c3a92', '%Vienna 6', 'blackpowder'),
  ('1bc1e3b2-6a4c-500a-87f5-59971775eace', '%Vienna 7', 'blackpowder'),
  ('6127a054-561f-5354-adac-db89739d26a7', '%St. Petersburg 1', 'cordite'),
  ('5026aae0-6210-5a52-91a0-ef66a60d0058', '%St. Petersburg 2', 'cordite'),
  ('d5a90efc-010c-560c-ae07-16f3155e1cc6', '%St. Petersburg 3', 'cordite'),
  ('fed17868-3716-5533-87ef-31d59fd58374', '%St. Petersburg 4', 'cordite'),
  ('73500d09-609c-58be-8169-b2be00d66824', '%St. Petersburg 5', 'cordite'),
  ('092d30d5-f2ed-58aa-9db7-83b1ff0e4b8a', '%St. Petersburg 6', 'cordite'),
  ('13c3fb7d-b830-5a45-88fc-cc5265d1bf8c', '%St. Petersburg 7', 'cordite'),
  ('4b6ecbc2-45cf-5a0f-be01-4eb716ffb6c0', '%Moscow 1', 'octogen'),
  ('040599d9-c94b-5736-8956-c3802d19b176', '%Moscow 2', 'octogen'),
  ('bf78b6e6-4d62-5c44-b673-0fa59c7791c5', '%Moscow 3', 'octogen'),
  ('70ad5197-fa05-5cd0-a2cf-e18170c7cdd0', '%Moscow 4', 'octogen'),
  ('597a6e99-78d9-5070-a7ec-e2a52f22ece8', '%Moscow 5', 'octogen'),
  ('e600ebbe-6bd7-5703-a44a-b3f2b2128e52', '%Moscow 6', 'octogen'),
  ('0a00cbfc-0994-59d2-b49e-bccda6767766', '%Moscow 7', 'octogen');
