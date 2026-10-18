-- c_base schema (DESIGN.md "Store"). SQLite, WAL.
--
-- APPLIED AT EVERY START, inside one transaction, by src/store/store_sqlite.c:
-- every statement here is idempotent (IF NOT EXISTS, INSERT OR IGNORE, DROP
-- ... IF EXISTS), and the database's PRAGMA user_version is then set to
-- CB_SCHEMA_VERSION (src/store/store.h). A database whose user_version is
-- NEWER than the binary's is refused: an old binary must not write to a
-- schema it does not know.
--
-- EVERY ROW IS AN ITEM (schema 6): the store's item layer (src/store/items.h)
-- is DynamoDB's model on both backends, and the rows above it are written
-- once (src/store/rows_auth.c, rows_foolish.c) with the keys and attributes
-- json_surface.h "THE ITEM LAYOUT" lists. SQLite holds no production data,
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

-- The tables of schemas 1 to 5, children first.
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

-- The seed of the bots (below), read once into BOT# items.
CREATE TABLE IF NOT EXISTS bot_seed (
    id            TEXT PRIMARY KEY,
    nickname      TEXT NOT NULL,
    strategy_key  TEXT NOT NULL
);

-- THE BOTS: seven of every family, in ladder order, exactly the nicknames and
-- strategy keys of the kernel's c/src/bot_roster.c (which the old Postgres
-- seed carried too). That seed left the ids to uuid_generate_v4(), so they differ per database;
-- these are FIXED (UUIDv5 of the nickname in a c_base namespace) so every fresh
-- c_base database agrees. store_sqlite.c writes them as the BOT# items when a
-- database has none; a migrated store replaces them with the hosted rows
-- (c_base/migrate), because the hosted rosters name the hosted ids.
INSERT OR IGNORE INTO bot_seed (id, nickname, strategy_key) VALUES
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
