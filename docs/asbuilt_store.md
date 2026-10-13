# As built: store, sql, migrate

Builder: store. Branch `claude/c_base-store`.

## What was built

- `src/store/store_sqlite.c`: every `cb_store_ops` entry over one SQLite connection (WAL, foreign keys, cached prepared statements, the embedded schema applied at every open under a `user_version` check that refuses a newer database).
  One process mutex spans each operation, so a pool thread never runs statements inside another thread's transaction; list functions call their callback after the lock is released, so a callback may call the store again.
  `game_commit` is one `BEGIN IMMEDIATE` transaction mirroring `commit_table`: the version-fenced `UPDATE`, membership and bot seats rewritten, ratings added to the rows as they stand (clamped at 0, `games_played + 1`, `previous_elo` the old value, a missing human row created when the account exists).
- `src/store/dynamo_wire.{c,h}`: every DynamoDB string (targets, item layout, key, condition and update expressions; every attribute name aliased so reserved words cannot break an expression), request builders and response readers, pure functions over buffers.
- `src/store/store_dynamo.c`: every operation over DynamoDB, deciding only what to send in what order; the transport is a function pointer (`cb_store_open_dynamo_with`, in `store_priv.h`), HTTPS + SigV4 in production.
- `src/store/store_priv.h`, `store.c`: the list orders both backends share (bot ladder, leaderboard, user search).
- `src/store/store_import.{c,h}` and `migrate/import.c`: the import, as a library so a unit test runs it; the command is a thin main.
- `migrate/export.sh`, `migrate/README.md`; `sql/schema.sql` finished (lease columns, realtime connection tables).
- Tests: `test/unit_store.c` (18 tests), `test/unit_dynamo.c` (19), `test/unit_import.c` (3), green on macOS, under ASan/UBSan, and on Ubuntu 24.04 (gcc) in Docker.

## Rehearsal against the real export (2026-10-13)

`export.sh` (read-only endpoint) then `cbase-import` into a scratch SQLite file: users 41, bots 49, ratings 35, games 60, player seats 47, bot seats 44, replays 144, chat 42, every count equal to the export's own and to the store's.
41 of 41 password hashes are bcrypt-shaped; 60 of 60 games load through `table_load`.
Every game's state, roster, log, seed and scalars and every replay's bytes compared equal to the CSV; a rerun reported the replays and messages as present and duplicated nothing; `PRAGMA integrity_check` ok, no orphan seats.
**The export (`migrate/out/`) and the scratch database were deleted afterwards**, since both held password hashes.

## Differences from the design, and why

1. **The snapshot is not inside `game_commit`.** `cb_game_commit` has no snapshot field, so the snapshot stays a separate `snapshot_put` after the winning commit, as `finalize.ts` did. Header change I would make: an optional `const cb_snapshot_row *snapshot` on `cb_game_commit`, written in the same transaction.
2. **DynamoDB snapshots are `SNAP#<snapshot id>` / `SNAP`, plus one `SNAP#<id>` / `PLAYER#<user>` copy per player (GSI2 `PLAYER#<user>`), and GSI1 `GAMESNAPS#<game>`**, not `SNAP#<game id>` / `<created>#<id>`. A replay must outlive its game with `game_id` cleared (`ON DELETE SET NULL`), which a game-keyed partition cannot do, and the per-player copies make "my replays" one query.
3. **Topic fan-out uses base-table items `TOPIC#<topic>` / `CONN#<conn>` (as DESIGN.md says), not a GSI (as the task text said)**: a base-table Query is strongly consistent, so a push right after a join is not missed. Each TOPIC item carries the whole connection row.
4. **The indexes the ops template must create**: `gsi1` (`gsi1pk`, `gsi1sk`, both S) and `gsi2` (`gsi2pk`, `gsi2sk`, both S), projection ALL; TTL attribute `expires_at` (seconds; `expires_ms` holds the exact value). GSI1 carries USERGAMES, USERCHAT, USERREFRESH, GAMESNAPS; GSI2 carries USERS, RATINGS, BOTS, SESSION, PLAYER.
5. **Ratings on DynamoDB** are read, clamped in C and written fenced on the value read (`#elo_rating = :read`); a rating another table moved cancels the transaction, and the commit re-reads and resends (up to 5 times). An update expression cannot clamp.
6. **Membership is rewritten on every commit** (the header's contract), where `commit_table` touched it only when the roster changed. So a seat whose account is gone (delete-account keeps the id in the roster) is **skipped** in both backends instead of failing every later commit on the foreign key.
7. **`last_commit_at` is stamped by the store** from `row->updated_at` on every commit (the trigger's rule: every commit moves the version); `row->last_commit_at` is read only by the import.
8. **The lease**: acquire and renew move `updated_at` (never `last_commit_at`), and the DynamoDB SCAN marker follows. Release clears `lease_until` instead of setting it to now minus one second, and does not move `updated_at`: same meaning, no clock needed.
9. **NOT_FOUND where the header said only CONFLICT**: `game_commit`, `game_delete` and `lease_acquire` on a game that is gone return `CB_E_NOT_FOUND` (DynamoDB tells the two apart by `ReturnValuesOnConditionCheckFailure: ALL_OLD`).
10. **`store.h` additions**: four import-only operations (`user_import`, `rating_import`, `bots_replace`, `game_import`), because the import must write rows as they are; and comments pinning the semantics above. No existing signature changed.
11. **Bot ladder order** is the kernel's `bot_roster` tier, then the nickname (shorter first), so it holds for the hosted ids too.
12. **The export** writes timestamps as epoch milliseconds, so the import parses no time zones; `export.sh` uses `python3` only to wrap and unwrap the Management API's JSON (it is not shipped).
13. **The import checks bcrypt shape itself** (`cb_import_bcrypt_shaped`), not `auth/bcrypt.h cb_bcrypt_parse`, which was a stub while this was built; switching is one line once auth lands.

## Untested

- The DynamoDB backend has never talked to DynamoDB (no AWS account yet, and `https.h`/`sigv4.h` are the aws builder's stubs here).
  The "recorded" answers in `unit_dynamo.c` are written from the API reference's documented shapes, not captured.
  First thing to run once a table exists: a create, join, rated game end and lease cycle against it, compared with SQLite.
- GSI eventual consistency (a dashboard or replay list read right after a write) is not exercised.
- DynamoDB `admin_browse`/`admin_query` (Scan and PartiQL) are only checked as request shapes and the write guard.
- `cb_store_open_dynamo` returns the credentials error once the aws builder implements `cb_aws_creds_from_env`; `smoke_lambda` will then need `CBASE_STORE=sqlite` or AWS variables (integration).

## Mutation checks (file: mutation -> failing assertion)

- store_sqlite.c: CAS `AND version = ?12` dropped -> `cas_second_writer_loses` game_commit == CB_E_CONFLICT.
- store_sqlite.c: lease acquire ignores a live lease -> `lease_semantics` second acquire == CB_E_CONFLICT.
- store_sqlite.c: renew `AND token` made `OR` -> `lease_semantics` renew with wrong token == CB_E_CONFLICT.
- store_sqlite.c: release frees nothing -> `lease_semantics` acquire after release == CB_OK.
- store_sqlite.c: lease writes last_commit_at -> `lease_semantics` last_commit_at == 10, and the scan window test.
- store_sqlite.c: scan `<` made `<=` -> `games_needing_bots_windows` count == 2.
- store_sqlite.c: scan abandon window dropped -> `games_needing_bots_windows` count and order.
- store_sqlite.c: rating clamp removed -> `ratings_applied_by_commit` rating == 0.
- store_sqlite.c: missing rating row not inserted -> `ratings_applied_by_commit` rating == 1012.
- store_sqlite.c: stale member not deleted -> `membership_follows_commits` is_member == 0.
- store_sqlite.c: member insert without the account check -> `cas_second_writer_loses` commit with a deleted seat == CB_OK.
- store_sqlite.c: COMMIT before seats and ratings -> `cas_second_writer_loses` version == 2 after a failed commit.
- store_sqlite.c: no mutex -> `cas_under_threads` version == wins.
- store_sqlite.c: conn TTL ignored -> `conns_round_trip` conn_get == CB_E_NOT_FOUND.
- store_sqlite.c: refresh_take does not revoke -> `refresh_tokens_rotate` got.revoked.
- store_sqlite.c: admin write guard removed -> `admin_reads` DELETE == CB_E_FORBIDDEN.
- store_sqlite.c: snapshot ACL removed -> `snapshots_query_shapes` non-player count == 0.
- store_sqlite.c: chat order flipped -> `chat_newest_oldest_first` first message == "three".
- store.c: ladder tier ignored -> `open_applies_schema_and_seeds_bots` last bot == "%Moscow 7".
- dynamo_wire.c: CAS condition dropped -> `commit_transaction` body byte-equal.
- dynamo_wire.c: rating clamp removed -> `commit_transaction` body byte-equal.
- dynamo_wire.c: lease expiry `<` made `>` -> `lease_requests` body byte-equal.
- dynamo_wire.c: throttle classified as failure -> `errors_classified` and `flow_throttle_retry`.
- dynamo_wire.c: base64 decode off by one -> `items_round_trip` state/roster/logs equal.
- dynamo_wire.c: stale member not deleted -> `commit_transaction` body byte-equal.
- dynamo_wire.c: TTL in ms instead of seconds -> `builders_valid_json` expires_at == 7200.
- store_dynamo.c: cancellation reason read at index 1 -> `flow_cas_lost` == CB_E_CONFLICT (and a request past the script).
- store_dynamo.c: paging inverted -> every flow's script assertions.
- store_dynamo.c: scan window `<=` -> `flow_scan_pages` count == 2.
- store_dynamo.c: account check skipped -> `flow_commit_drops_missing_account` body lacks the member.
- store_dynamo.c: chat not reversed -> `flow_chat_order` first == "two".
- store_dynamo.c: admin write guard removed -> `flow_admin_query_guard` == CB_E_FORBIDDEN.
- store_import.c: CSV `""` unescape broken -> `clean_export_imports_everything` chat text.
- store_import.c: table_load result ignored -> `failing_checks` BAD_GAME returns 1.
- store_import.c: export count ignored -> `failing_checks` BAD_COUNT returns 1.
- store_import.c: store count ignored -> `failing_checks` LOST_SEAT returns 1.
- store_import.c: bcrypt cost bound removed -> `bcrypt_shapes` cost 03 refused.
- store_import.c: player_ids split on ',' -> `clean_export_imports_everything` snapshot players.

## Header changes I would make

- `cb_game_commit`: an optional snapshot to write in the same transaction (item 1 above).
- `cb_game_commit`: a `membership_changed` flag, so a commit that did not touch the roster skips the seat reads and writes (DynamoDB pays two Queries per commit for them now).

## Open defects

- DynamoDB `user_delete` and `game_delete` cascade in steps after the main delete (GSI lookups, then batch deletes); a crash between them leaves unreachable orphans (a chat partition of a deleted game, a member item of a deleted user).
- DynamoDB `users_list`, `bots_list`, `ratings_list` read a whole index partition and sort in C: fine at 41 users and 49 bots, not at thousands.
- Six hosted users have no rating row (they predate the trigger); they get one at their first rated game, as on Supabase.
- `store_import.c` is linked into `cbase` and `bootstrap` too (the Makefile links every `src/*/*.c`), unused there.
