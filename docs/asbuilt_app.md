# As built: app (src/app/)

## What was built

- `table_ops.c`: `cb_table_run` (load from the row cache or the store, 32 fresh deal-seed bytes, one kernel section on heap scratch, the CAS commit with five attempts), `cb_table_create` (five id draws), `cb_table_view` and `cb_table_views_of_user` (envelopes computed from the stored row, seat for a member, -1 for a spectator), the 256-entry row cache of PLAYING games, and the end of a game (replay code and extras into `snapshot_put`).
- `app_internal.h` (new, app-only): the section (`cb_section_load`, `cb_section_commit`, `cb_section_envelope`) that `cb_table_run` and the bot loop share, the row cache, and two seams for tests (`cb_app_now`, `cb_app_sleep`).
- `fanout.c`: pv- INSERT/UPDATE/DELETE per human row on every commit, gu- casts per human seat and the game- cast only when `n_events > 0 || goods_changed`.
- `functions_http.c`: create, action, meta and delete-account, every method routed to the handler (delete-account answers its own 405), `CB_ROUTE_APIKEY`.
- `bots.c`: `cb_bots_drive` (lease, cycles with the kernel's wait, renew per cycle, release on every exit, re-kick when the budget ends with work left), `cb_bots_scan`, `cb_bots_kick_local` (bot pool task), `cb_bots_kick_lambda` (`cb_lambda_invoke_bot_drive`).
- Tests: `test/unit_app.c` (16 tests through `cb_handle`), `test/lib_app_store_mem.c` (an in-RAM `cb_store` with a CAS-stealing hook), and the brief's full scenario in `test/smoke.c` behind `CBASE_SMOKE_GAME=1`.

## Where it differs from the design, and why

- `commit_table`'s plpgsql arithmetic moved into C, because the store writes whole rows: the log is appended (reset on a deal, emptied in a lobby), `round_epoch` is 0 on a deal or in a lobby and the new version when the operation closed a round, the seed is the fresh one when the operation dealt, none in a lobby, else the stored one.
- The row cache also holds the session log, because the append happens here and not in SQL.
- Ratings: when the products want ratings the host reads `ratings_for`, calls `table_rate` and `table_commit_products` again in the same section, as DESIGN.md says; no re-run of the operation.
- finalize.ts cleared `logs_packed` with a second, unfenced UPDATE after the snapshot. The store has no such write, so the log stays until the next commit that leaves the finished table (a lobby commit empties it, a deal replaces it).
- The snapshot's `player_ids` are every seat, bots included, as finalize.ts wrote them; store.h's comment says "the seated humans".
- Statuses follow wrap400, not the per-reason table functions_http.h first listed: 400 for every refusal with a reason (AUTH, MALFORMED, REFUSED, NOT_FOUND, NO_BOT), 415, 500 INTERNAL (write contention too), 503 when the store is down; delete-account 405/401/500. The header comment now says so.
- `cb_table_fn` gained the loaded row (`const cb_game_row *row`): a move needs its `round_epoch`.
- The per-op `noCommit`/`freshOnly` became one rule for every operation: MOOT, STALE_ROUND and REJECTED commit nothing; STALE_ROUND, REJECTED and any refusal read from a cached row re-run fresh. Asks never return the last two, and redact never MOOTs in practice, so behaviour is unchanged.
- The bot budget: the edge runtime's 1700 ms CPU budget is gone; a Lambda drive stops when the predicted next cycle (max of average x 1.5 and the slowest) would pass the invocation deadline minus 5 s, or a wait would; the 120 s wall ceiling stays. Either stop re-kicks (TS left continuation to the heartbeat).
- `tm_store_new` lives in `test/lib_app_store_mem.c`, not `lib_store_mem.c`, to stay inside the `lib_<module>_*` namespace.
- Makefile (outside src/app): `ASAN_KERNEL_CFLAGS` gained `-fno-sanitize=object-size`, as foolish/c's own sanitized build has, because replay.c's DealSlot is a Game cut short on purpose and every finished game's replay walks it.

## Header changes made in my directory

- `table_ops.h`: `cb_table_fn` takes the row.
- `functions_http.h`: the status rules; `cb_app_delete_account` (for the admin Users panel); the `cb_functions_verify` test seam.
- `fanout.h`: the `cb_fanout_observer` test seam.

## For the integrator

- Turn the smoke scenario on: `CBASE_SMOKE_GAME=1 build/test/smoke build/cbase` in the Makefile's smoke target once auth, store_sqlite, postgrest and realtime are merged. It has never run green; expect to fix shapes on first contact (signup body, Phoenix vsn 2 frames, PostgREST paths).
- serve has no heartbeat timer yet: main_serve.c should call `cb_bots_scan(ctx, cb_now_ms())` every 10 s (DESIGN.md). Lambda's main already routes the schedule.
- `cb_bots_kick_local` with the default one bot thread runs drives one at a time, and a drive sleeps through the kernel's pacing for up to 120 s, so under serve a second bot game waits for the first. Fine for development; a kernel `_Thread_local` on `bot_drive_pre_action_hook` lets the pool grow.
- The store must accept a commit whose humans include a deleted account (delete-account redacts the seat but the roster keeps the id), or skip that membership row; store_sqlite with foreign keys on would otherwise refuse every later commit on that table.

## Untested

- `fanout.c`'s mapping onto `rt_publish_*` (the publish functions are stubs here; the tests observe the `cb_fanout` struct instead).
- `cb_bots_kick_local`, `cb_bots_kick_lambda`, the Lambda deadline budget and the re-kick.
- `cb_table_views_of_user` beyond limit 1; create's id redraw on a collision; `ratings_for` failing (the game then ends unrated).
- The smoke scenario (see above).

## Mutation checks (test/unit_app.c; each went red, then restored by rewriting the file)

- table_ops.c epoch kept on a closed round: a_stale_intent_is_stale_round `play_until_epoch(gid)`.
- REJECTED commits: an_illegal_action `resp.version == v`.
- MOOT commits: a_finished_game `version_of(gid) == v`.
- No fresh re-check of a cached refusal: a_refusal_from_a_stale_cache `resp.status == TABLE_STATUS_APPLIED`.
- Lost CAS not retried: a_lost_cas `resp.status == TABLE_STATUS_APPLIED`.
- finalize skipped: a_finished_game `snapshots == 1`.
- table_rate skipped: a_finished_game `ra.games_played == 1`.
- Old log dropped on append: a_finished_game `snapshots == 1`.
- Empty table not deleted: the_last_seat_leaving `game_get == CB_E_NOT_FOUND`.
- View always spectator: create_answers `r.body == v.view`.
- Deal seed not stored: join_add_bot_and_start `row.has_seed`.
- PLAYING rows not cached: a_legal_action `cb_row_cache_size() == 1`.
- No broadcast: join_add_bot_and_start `g_fan.broadcast`.
- Joined not detected: join_add_bot_and_start `g_fan.joined == 1`.
- functions_http.c no 415: the_wire_surface `r.status == 415`.
- AUTH as 401: the_wire_surface `r.status == 400`.
- Deal does not wake bots: join_add_bot_and_start `g_kicks - kicks == ...`.
- NO_BOT as REFUSED: join_add_bot_and_start `refusal_why == ASK_WHY_NO_BOT`.
- Wire error not MALFORMED: an_illegal_action `refusal_why == ASK_WHY_MALFORMED`.
- NOT_FOUND as INTERNAL: the_wire_surface `r.status == 400`.
- delete-account takes GET: the_wire_surface `r.status == 405`.
- Redaction name: delete_account_redacts `name_len == strlen("Deleted player")`.
- Leaderboard name kept: delete_account_redacts `rb.username == ""`.
- add-bot ignores seated: add_bot_never_picks_a_seated_bot `refusal_why == ASK_WHY_NO_BOT`.
- bots.c lease never released: the_bot_loop `until == 0`.
- Held lease ignored: a_held_lease_skips_the_drive `cb_bots_drive == CB_OK`.
- No pacing sleep: the_bot_loop hangs (the runner's 60 s timeout); a spin on the fake clock, no assertion.
- Scan ignores staleness, or abandonment: the_scan `cb_bots_scan == 1`.
- One cycle per drive: the_bot_loop `status != PLAYING || pick_move(...) > 0`.

## Open defects

- Kernel: a pickup of more than `MAX_LOG_PAIRS` (16) cards, which a three-seat passing game reaches, is logged with 16 pairs while the replay carries all of them, so `table_replay_code`'s gate refuses the game and no snapshot is written (about 1 in 15 random three-seat games here). The same kernel serves today's TypeScript host, so production has it too.
- By the same code (table.c `log_read`), a session over `MAX_LOGS` records would be refused the same way; not observed here.
- The tests' games are random (fresh deal seeds), so their check counts vary run to run; 150 consecutive runs were green.
