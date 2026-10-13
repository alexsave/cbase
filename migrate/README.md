# Migrating the hosted Supabase data into c_base

Two steps: a read-only export from the hosted project, then `cbase-import` writing it through the store interface.
The same import fills a local SQLite file (the rehearsal) and the production DynamoDB table.

## 1. Export (read-only)

```
c_base/migrate/export.sh            # writes c_base/migrate/out/
```

It needs the Supabase CLI to be logged in on this Mac: the token is read from the keychain (service "Supabase CLI", account supabase) and never printed.
Every statement is a `SELECT`, sent to the Management API's read-only query endpoint (`/v1/projects/<ref>/database/query/read-only`), so the script cannot change the hosted database.

It writes one CSV per table, and `counts.txt` with each table's row count as the database reported it:

| file | source | columns |
|---|---|---|
| `users.csv` | `auth.users` | id, email (lowercased), encrypted_password, username (`raw_user_meta_data->>'username'`), created_at, last_sign_in_at |
| `bots.csv` | `bots` | id, nickname, strategy_key, elo_rating, previous_elo, games_played, created_at, updated_at |
| `user_elo_ratings.csv` | `user_elo_ratings` | user_id, username, elo_rating, previous_elo, games_played, updated_at |
| `games.csv` | `games` | id, status, state, roster, needs_bots, game_seed, logs_packed, version, round_epoch, created_at, updated_at, last_commit_at |
| `player_hands.csv` | `player_hands` | game_id, player_id, joined_at |
| `bot_hands.csv` | `bot_hands` | game_id, bot_id |
| `game_snapshots.csv` | `game_snapshots` | id, game_id, player_ids, moves, extras, created_at |
| `chat_messages.csv` | `chat_messages` | id, game_id, user_id, message, is_system, created_at |

The dialect: RFC 4180 with a header row; NULL is an empty unquoted field and every other value is quoted; bytea is lowercase hex; every timestamp is integer milliseconds since the epoch; `player_ids` is joined by `;`.

**The export holds password hashes.**
`out/` is gitignored, created mode 0700 with files 0600.
Delete it as soon as the import has run: `rm -rf c_base/migrate/out`.

Refresh tokens, sessions, the view caches (`player_views`, `spectator_views`) and the bot leases are not exported: everyone signs in again once, and views are computed on demand.

## 2. Import

```
make -C c_base build/cbase-import
CBASE_STORE=sqlite CBASE_DB=/path/cbase.sqlite c_base/build/cbase-import c_base/migrate/out
CBASE_STORE=dynamo CBASE_TABLE=cbase            c_base/build/cbase-import c_base/migrate/out   # with AWS_* credentials and AWS_REGION set
```

It writes users as they are (GoTrue's bcrypt hashes unchanged, so passwords keep working), replaces the seeded bots with the hosted rows (the hosted rosters name the hosted bot ids), then ratings, games at their own versions with their seats, replays and chat.
A rerun is safe: rows are replaced, and a replay or message already present is counted as present.

It prints read, written and stored counts per table and exits non-zero unless every hash is bcrypt-shaped (`$2a$`/`$2b$`, a cost, 53 characters), every game loads through the kernel (`table_load` on its state and roster), and every count agrees with `counts.txt` (and, on SQLite, with the store's own count).

## Rehearsal, 2026-10-13

Against the real export into a scratch SQLite file: 41 users, 49 bots, 35 ratings, 60 games, 47 player seats, 44 bot seats, 144 replays and 42 chat messages, every count equal to the export's.
41 of 41 hashes parse as bcrypt and 60 of 60 games load through the kernel.
Every game's state, roster, log, seed and scalars and every replay's bytes compared equal to the CSV.
The export and the scratch database were then deleted.
