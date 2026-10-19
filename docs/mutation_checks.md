# Mutation checks

Every test in c_base was mutation-checked: the code it covers was broken, the named assertion went red, and the code was restored by rewriting it (never `git checkout`) with its objects deleted, because make compares whole seconds.
Each line is: file, mutation, the assertion that failed.
This file collects the six builders' records and the integration's; DESIGN.md says what each module is.

## Store, sql, migrate

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

## Auth

1. bcrypt.c, `$2a$` mapped back to the anti-collision variant: unit_bcrypt.c:40 two_a_is_gos_algorithm.
2. bcrypt.c, verify compares no hash bytes: unit_bcrypt.c:32 `cb_bcrypt_verify("wrong", ...) == 0`.
3. bcrypt.c, parse accepts cost 3: unit_bcrypt.c:57 `!cb_bcrypt_parse(bad[i])`.
4. bcrypt.h, cost 12: unit_bcrypt.c:87 cost_ten_budget (prefix, and the 100 ms budget).
5. jwt.c, alg not checked: unit_jwt.c:140 alg none refused.
6. jwt.c, skew 31 s: unit_jwt.c:113 expired at exp + 31.
7. jwt.c, role not checked: unit_jwt.c:155 service_role refused.
8. jwt.c, last signature character not compared: unit_jwt.c:188.
9. auth.c, reuse does not revoke the session: unit_auth.c:135 `revoked == 3`.
10. auth.c, '%' allowed: unit_auth.c:101.
11. auth.c, email not lowercased: unit_auth.c:52.
12. auth.c, verify with a stale clock: unit_auth.c:201 expired token refused.
13. auth.c, dummy hash unparseable (a fast miss): unit_auth.c:83 the miss takes at least 10 ms.
14. auth.c, same password accepted: unit_auth.c:160.
15. auth_http.c, integer `code` to a 2024 client: unit_auth.c:298.
16. auth_http.c, API version header not echoed: unit_auth.c:299.
17. auth.c, dates end "+00:00" instead of "Z": unit_auth.c:275 jdate.
18. auth_http.c, logout revokes nothing: unit_auth.c:438 the refresh after logout is 400.
19. auth_http.c, signup without the anon-key flag: unit_auth.c:358 401.
20. auth_http.c, session without refresh_token: unit_auth.c:269.

## App

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

## Realtime and PostgREST

The Phoenix and PostgREST tests below were deleted with the facade in stage 4; the section is the record of what they held.

- `test/unit_phoenix.c` (17 tests): decode in both serializers and 13 refusals, byte-exact writers, vsn parsing, binding ids, the may_join matrix, join echo with ids, refusals with reasons, the 16-topic cap, leave and access_token, another user's join, a vsn 1 connection, the three push payloads byte for byte (and the broadcast against the fake backend's recorded frame), timestamps, publish through a capturing transport, and a join's id appearing in the matching push.
- `test/unit_postgrest.c` (11 tests): the parser on every client query shape and 26 refusals; every table through `cb_handle`.
- `test/unit_rt_apigw.c` (4 tests): the three events against CONN rows, TTL writes, a lost row, garbage, publishes per serializer, 410, expired rows.
- `test/unit_rt_local.c` (4 tests): a real loopback server with the request pool: the upgrade route, joins and publishes from another thread across both serializers, frame order, leave, a chat insert over HTTP reaching a socket, and garbage closing with 1003.
- `test/lib_rt_fakestore.c`: the in-memory store, token check, view rows and API Gateway recorder.

Mutation checks (file, mutation, what failed):

- phoenix.c, gu- check without the uid compare: may_join_matrix (a member joining another member's gu- topic).
- phoenix.c, binding-id separator removed: vsn_and_binding_ids.
- phoenix.c, escape allowed in a ref: decode_refusals.
- phoenix.c, anon-key token treated as a token: join_refusals (reason).
- phoenix.c, phx_close without ref = join_ref: leave_and_access_token.
- phoenix.c, access_token accepts any valid user: leave_and_access_token.
- phoenix.c, topic cap lifted: too_many_channels.
- realtime.c, "*" ids dropped: pv_payloads.
- realtime.c, old_record on INSERT: pv_payloads.
- rt_rows.c, trailing zeros kept: pv_payloads, timestamps_as_postgres_prints_them.
- postgrest.c, chat membership check removed: chat_history_is_members_only.
- postgrest.c, gt refused: parse_client_shapes, leaderboard_and_lobby.
- postgrest.c, 406 only above one row: single_object_and_406 (crashes on the zero-row object, a kill).
- postgrest.c, insert user_id check removed: chat_insert_refusals.
- postgrest.c, apikey-equal bearer not anon: player_views_dashboard_and_resync.
- postgrest.c, spectator_views without game_id allowed: spectator_views.
- postgrest.c, anon snapshots: snapshots_are_their_players_only.
- postgrest.c, chat broadcast removed: chat_insert.
- postgrest.c, partial insert written: chat_insert_refusals.
- postgrest.c, text ordering broken: leaderboard_and_lobby.
- postgrest.c, limit ignored: chat_history_is_members_only.
- rt_apigw.c, 410 not deleting: publish_posts_each_subscriber_in_its_serializer.
- rt_apigw.c, TTL refresh never: frames_round_trip_through_the_conn_row.
- rt_apigw.c, a changed row not written: frames_round_trip_through_the_conn_row.
- rt_apigw.c, apikey not checked: connect_and_disconnect.
- rt_apigw.c, expired rows posted: publish_posts_each_subscriber_in_its_serializer.
- rt_apigw.c, lost row always vsn 2: a_lost_row_is_rebuilt_and_garbage_closes.
- rt_local.c, serializer swapped on publish: join_publish_and_isolation.
- rt_local.c, job state not taken back: join_publish_and_isolation.
- rt_local.c, no subprotocol echo: the_upgrade_route.
- rt_local.c, garbage not closed: garbage_closes_the_socket.
- rt_local.c, bad vsn upgraded: the_upgrade_route.
- rt_local.c, publish to every socket: join_publish_and_isolation.


## AWS

- sigv4.c, header sort skipped: unit_sigv4 get-header-value-trim canonical request.
- sigv4.c, path not re-encoded: unit_sigv4 path_double_encoded.
- sigv4.c, query sort skipped: unit_sigv4 get-vanilla-query-order-key-case canonical request.
- sigv4.c, inner blanks not collapsed: unit_sigv4 get-header-value-trim canonical request.
- sigv4.c, signature keyed with kService not kSigning: unit_sigv4 every signature.
- sigv4.c, session token left unsigned: unit_sigv4 post-sts-header-before canonical request.
- sigv4.c, duplicate header names allowed: unit_sigv4 bad_headers_refused.
- sigv4.c, AWS_DEFAULT_REGION fallback removed: unit_sigv4 creds_from_env.
- sigv4.c, X-Amz-Security-Token line not emitted: unit_sigv4 signed_header_lines.
- https.c, Content-Length ignored: unit_https content_length.
- https.c, chunk data dropped: unit_https chunked_body.
- https.c, SSL_set1_host removed: unit_https verified_and_kept_alive (127.0.0.1 accepted).
- https.c, SSL_VERIFY_NONE: unit_https verified_and_kept_alive (untrusted cert accepted).
- https.c, connection never pooled: unit_https accepts == 1.
- https.c, no retry on a dead reused connection: unit_https dead_idle_connection_retried.
- https.c, socket timeouts 10x: unit_https times_out.
- https.c, 100 Continue not skipped: unit_https special_statuses.
- https.c, 204 given a body: unit_https special_statuses.
- https.c, EOF inside the head not an error: unit_https content_length prefix loop.
- https.c, clean EOF on a reused connection not retried (the bug this check found, fixed): unit_https dead_idle_connection_retried "close".
- event.c, cookies joined with ", ": unit_event function_url_request.
- event.c, header names not lowercased: unit_event function_url_binary.
- event.c, base64 body not decoded: unit_event function_url_binary and ws_default_and_disconnect.
- event.c, $connect query not re-encoded: unit_event ws_connect.
- event.c, game id alphabet unchecked: unit_event schedule_and_bot_drive.
- event.c, "aws.events" unrecognised: unit_event schedule_and_bot_drive.
- event.c, octet-stream not forced to base64: unit_event encode_binary.
- event.c, surrogates accepted as UTF-8: unit_event utf8_rule.
- event.c, overlong 3-byte UTF-8 accepted: unit_event utf8_rule.
- event.c, duplicate response headers not merged: unit_event encode_text.
- event.c, Set-Cookie also left in headers: unit_event encode_text.
- event.c, literal spelled "bot-drive": unit_aws_calls lambda_self_invoke body.
- event.c, "scan" literal unrecognised: smoke_lambda req-2 path.
- event.c, text body sent base64: smoke_lambda req-1 isBase64Encoded.
- aws_call.c, path segment not encoded: unit_aws_calls request line.
- aws_call.c, caller headers not signed: unit_aws_calls SignedHeaders and recomputed signature.
- aws_call.c, Host signed without its port: unit_aws_calls recomputed signature.
- apigw_mgmt.c, 410 reported as 200: unit_aws_calls apigw_post_and_delete.
- lambda_invoke.c, non-202 accepted: unit_aws_calls lambda_self_invoke.
- main_lambda.c, HTTP response encoded as a WS one: smoke_lambda req-1 fields.

## Admin and ops

- admin.c `console_on` always true: `unset_token_hides_the_console` GET /admin == 404.
- admin.c gate ignores `const_eq`: `the_token_gates_every_call` "wrong" == 401.
- admin.c `const_eq` drops the length check: the prefix token == 401.
- admin.c any non-empty confirm allows a write: confirm "write" == 403 and `!last_allow_write`.
- admin.c browse passes NULL for the cursor: `last_cursor == "3"`.
- admin.c browse limit uncapped: `last_limit == 500`.
- admin.c logs ignore the level: `entries` length == 2.
- admin.c backup path quote not doubled: `strstr(..., "/q''d/backups/cbase-")`.
- admin.c needing-bots count not incremented: overview `n == 3`.
- admin.c games list keeps a row `game_get` did not find: `games` length == 1.
- admin.c API method not checked: GET query == 405, POST backups under dynamo == 409.

Ops: `cfn-lint 1.57.2` passes on `ops/cbase.yaml` (and fails as expected on a copy with a bad runtime and a misspelled `IndexName`); `bash -n` and `shellcheck` pass on `ops/package.sh`; `actionlint` passes on the workflow; `ops/package.sh` ran end to end on this Mac (2,202,382-byte zip holding one executable `bootstrap`, static aarch64 ELF).
The console was checked by eye in headless Chrome against a mock API, light and dark, at 1280 px and at 390 px (inside an iframe, since headless Chrome will not size a window under 500 px); the header badge now truncates instead of pushing Sign out off a phone screen.

## Integration

- auth.c, grace `<=` made `<`: unit_auth refresh_rotates_and_reuse_revokes, the retry at exactly 10 s gets a pair.
- auth.c, grace check removed: unit_auth, 27 assertions (no retry gets a pair).
- auth.c, `revoked_at > 0` clause removed: equivalent (a zeroed stamp is always outside the window); the clause was deleted.
- auth.c, global logout revokes only the session: unit_auth logout_password_delete, the second device's token still refreshes.
- auth_http.c, default scope local: unit_auth http_user_and_logout, the other device's refresh after a scopeless logout is 400.
- lib_auth_fakestore.c, every take restamps revoked_at: unit_auth, the retry 1 ms past the window is still a pair.
- store_sqlite.c, take always restamps: unit_store refresh_tokens_rotate `revoked_at == 100`.
- store_sqlite.c, session revoke keeps the stamp: unit_store `revoked_at == 0`.
- store_sqlite.c, user revoke by session: unit_store, the user's other sessions stay live.
- store_sqlite.c, admin write guard removed: unit_store admin_reads, DELETE and VACUUM INTO are refused.
- dynamo_wire.c, revoke keeps the stamp, take without if_not_exists, revoked_at not read: unit_dynamo flow_signup_and_tokens and builders_valid_json.
- store_dynamo.c, user revoke queries sessions: unit_dynamo flow_signup_and_tokens script.
- dynamo_wire.c, `games` not mapped to GAME: unit_dynamo flow_admin_query_guard.
- dynamo_wire.h, `DW_GSI1` spelled GSI1; ops/cbase.yaml, gsi1sk a Number; ops/cbase.yaml, the schedule input an object: unit_dynamo the_template_matches_the_wire.
- store_dynamo.c, one signed header, or the port ignored: unit_aws_calls dynamo_through_aws_call; header order is equivalent (SigV4 sorts).
- store_dynamo.c, a path accepted in AWS_ENDPOINT_URL_DYNAMODB: unit_aws_calls `cb_store_open_dynamo == CB_E_INVALID`.
- admin.c, delete skipped: unit_admin user_delete_is_the_delete_account_path, the user still exists.
- admin.c, games browsed as GAME: unit_admin games_list_reads_each_row.
- main_lambda.c, a missing game's drive answered as an error; the scan not dispatched: smoke_lambda req-4 and req-2.
- main_serve.c, no heartbeat timer: smoke game_scenario `now_v > ver` (the bots never resume after the crash).
- fanout.c, no pv- publish: smoke `LB->n_pv > 0`; no spectator cast: smoke `LB->n_cast > 0`.
- bots.c, every drive returns at once: smoke game_scenario `status == GAME_STATUS_GAME_OVER` after the 90 s stall rule (the run stops instead of hanging).
- Makefile, c_base's objects built without `KERNEL_CAPS`: the compile fails at table_ops.c's `_Static_assert(MAX_LOG_PAIRS >= MAX_DECK)`.

## DynamoDB Local (2026-10-13)

- aws_call.c, `http://` leaves tls set: unit_aws_calls url_parse `!tls`, port 80, and dynamo_over_plain_http `user_get == CB_OK`.
- aws_call.c, the amazonaws.com guard removed: unit_aws_calls url_parse, both `CB_E_INVALID` for http:// to AWS, and dynamo_over_plain_http's refused open.
- dynamo_wire.c, an `#`-ending sort key read as exact: unit_dynamo flow_import_count, the `begins_with(#sk, :s)` filter.
- dynamo_wire.c, the array shape of ConsumedCapacity not read: unit_dynamo consumed_capacity_read `== 18.0`.
- store_import.c, the seed never writes: unit_store (dynamo) open_applies_schema_and_seeds_bots, 49 bots and their order.
- store_import.c, DynamoDB's stored count taken as the read count: unit_import (dynamo) failing_checks, the lost seat passes.
- dynamo_wire.c, expires_at written in ms: unit_store (dynamo) conn_ttl_is_seconds, both `1760000000`.
- store_dynamo.c, query_all stops after one page: unit_store (dynamo) big_items_and_pages, 24 replays and the chat; chat_list stops after one page: the 300 and the newest 250.
- bots.c, the game unmarked after its drive instead of before: unit_bots_kick, the kick during a running drive is lost (1 drive, want 2); no dedupe at all (before the fix): 502 drives and 3 queued.
- lib_proc.c, the pump starts reading 12 s late: unit_proc, the 4 MB writer has not exited within 3 s and its last lines never arrive.
## Per-invocation timing line (2026-10-17)

- https.c: a new connection not counted as a dial -> `verified_and_kept_alive` tally.other.dials == 1.
- https.c: every host counted as DynamoDB -> `verified_and_kept_alive` tally.other.calls == 2 (and dials, us).
- main_lambda.c: the timing line logged at DEBUG, so never at the function's INFO -> smoke_lambda `tp_wait_line("inv: http GET /health 200 ms=")` and the scan and drive lines.

## Answer first (2026-10-17)

- fanout.c: cb_fanout_commit publishes at once, window or not -> `the_answer_goes_before_its_pushes` g_nseq == 0 (nothing left before cb_after_run).
- after.c: the queue drained newest first -> `the_answer_goes_before_its_pushes` g_seq[i] == last + 1 (version order) and the kick last.
- after.c: a kick never queued -> `the_answer_goes_before_its_pushes` g_nseq == 0 and the kick last.
- after.c: a failed push after the answer not logged -> `a_push_that_fails_after_the_answer_is_logged` found.

Equivalent, recorded so nobody chases it: dropping the order lock around a drained fan-out survives, because the test (like Lambda) has one committing thread; the lock is kept for the invariant table_ops.c documents. Not unit-tested: main_lambda.c opening the window for an HTTP event (smoke_lambda's events commit nothing); the production timing line's `after` field shows it.

## My games in one batch (2026-10-17)

- store_dynamo.c: batches of 130 keys, not 100 -> `flow_games_batched` g.keys[0] == 100 (and the call count).
- store_dynamo.c: an unprocessed answer sends the whole batch again -> `flow_games_batched` g.keys[1] == 2.
- store_dynamo.c: unprocessed keys never sent again -> `flow_games_batched` the row count, g.calls == 3.
- json_surface.c: logs_packed in the projection -> `games_get_shape` the exact body and strstr(body, "logs_packed") == NULL.
- store.c: cb_store_games_get_each keeps the log -> `game_round_trip` !v.logs (unit_store, also run on DynamoDB Local).

## Independent reads at once (2026-10-17)

- store.c: cb_store_side_start runs the side read in the caller, one after the other -> `independent_reads_run_at_once` g_overlapped == 2 (leaderboard and history).
- store.c: the side thread without the request's store deadline -> `independent_reads_run_at_once` g_side_deadline[1] == deadline.

## Bench (bench/unit_bench.c)

- bench_stats.c: the percentile's `- 1e-9` dropped -> `percentiles_of_one_to_hundred` bs_percentile(v, 100, 7) == 7 (7.000000000000001 rounds up to the 8th).
- bench_stats.c: the rank truncated instead of rounded up -> `percentiles_round_up` s.p50 == 30.
- bench_stats.c: no sort before the summary -> `percentiles_of_one_to_hundred` s.p50 == 50.
- bench_stats.c: max read from the bottom -> `percentiles_of_one_to_hundred` s.max == 100.
- bench_stats.c: the ms rounding `(a + 5) / 10` made `a / 10` -> `report_tables` the whole report (12.35 became 12.34).
- bench_stats.c: the "-0.00" guard dropped -> `report_tables` the push_pv row of -3 us reads 0.00.
- bench_stats.c: the 5 ms share off by ten -> `report_tables` the whole report.
- bench_stats.c: a cpu row with trailing text accepted -> `run_file_round_trip` !bs_run_parse(JUNK).
- bench_stats.c: the cpu table never printed -> `report_tables` the whole report.

## Binary surface (unit_netwire, unit_netclient, unit_v1, unit_stream)

Run by a script that copies each file aside, applies the mutation, refuses one that did not change the file, deletes the object, rebuilds the one test and restores the copy (2026-10-14).

- netwire.c: a body read whole no longer required (trailing bytes taken) -> `requests` refused_everywhere(signin) == 0.
- netwire.c: a u48 written as 5 bytes -> `session` out == want.
- netwire.c: padding allowed mid-text -> `text_form` nw_text_decode("Zg==Zg==") == NW_E_WIRE.
- netwire.c: is_bot past 1 taken -> `lists` nw_list_open(not_bool) == NW_E_WIRE.
- netwire.c: a rating beside "none" taken -> `lists` nw_list_open(none) == NW_E_WIRE.
- netwire.c: a separate blob-length check against the body (a second guard) survived its removal; r_take's own bound refuses the same lengths, so the second guard was deleted, and `make check-asan` (UBSan) then caught `r->at + n` overflowing on a 2^31 - 1 length: the bound is now `n > len - at`, in the reader and the writer.
- netclient.c: refresh due one ms late (`<=` made `<`) -> `requests` q->is_refresh at expiry less NC_REFRESH_EARLY_MS.
- netclient.c: the gate drops only older, not equal, versions -> `gates` the same version again is NC_EV_NONE.
- netclient.c: the retry backoff not doubled -> `stream_order_and_retries` no frame before 1 s after the second refusal.
- netclient.c: a used refresh token leaves the client signed in -> `answers` nc_session_read == ASK_WHY_ACCOUNT then signed_in == 0.
- netclient.c: a resubscription without what is held -> `gates` s.version == 6.
- netclient.c: an id put into a path unchecked -> `requests` nc_req_game("ab/../x") == NULL.
- netclient.c: the reconnect delay not reset by a frame -> `stream_order_and_retries` nc_stream_closed == NC_RETRY_FIRST_MS.
- netclient.c: a subscription sent without its token (NW_WS_SUB) -> `stream_order_and_retries` s1.kind == NW_WS_SUB_AS.
- v1_http.c: no spectator fallback -> `games_views_and_chat` call(b_game) == 200 for a non-member.
- v1_http.c: chat open to non-members -> `games_views_and_chat` the non-member's empty list (nc_list_read == 0), and the post 403.
- v1_http.c: 15 hex digits of the name's digest -> `the_email_is_the_clients_derivation` the address.
- v1_http.c: a taken name answered 400 -> `accounts` status == 409.
- v1_http.c: unplayed humans on the leaderboard -> `bots_board_history_replay` the one human is FAY.
- v1_http.c: the replay not filtered by game -> `bots_board_history_replay` the moves are the asked game's.
- v1_http.c: a chat message not published -> `games_views_and_chat` g_pub.n == 1.
- stream.c: no catch-up -> `protocol` two frames (ACK, VIEW).
- stream.c: seated inverted -> `protocol` one frame when the client holds the version (the spectator view caught up instead).
- stream.c: a token that does not verify keeps the old user -> `protocol` c.user_id == "".
- stream.c: the catch-up ignores `since` -> `protocol` g_out.n == 1.
- stream.c: chat open to non-members -> `protocol` ACK FORBIDDEN.
- rt_apigw.c: the stream's frames posted raw, not base64 -> `apigw` the post decodes as a frame (that test moved to unit_rt_apigw.c in stage 4).
- rt_local.c: the stream's frames sent as text -> `serve_end_to_end` the first event is NC_EV_SEATED.
- realtime.c: a DELETE carries the envelope -> `publishes` g_cap.nw == want_gone.
- realtime.c: the cast's version dropped -> `publishes` g_cap.nw == want_cast.

## Facade deleted (stage 4: unit_rt_apigw, unit_rt_local, unit_net, unit_v1, smoke)

`unit_rt_apigw.c` and `unit_rt_local.c` were rewritten for the stream, `unit_stream.c`'s API Gateway test moved into `unit_rt_apigw.c`, `unit_v1.c` took over unit_auth's "no backend" test, and the game smoke plays over `/v1`.
Each mutant below was applied, the named test went red on the named assertion, and the source was restored (scratchpad runner, perl over the file, the object rebuilt).

- rt_apigw.c: a lost CAS gives up instead of re-running -> `concurrent_frames_of_one_connection_both_stick` n_topics == 2.
- rt_apigw.c: `$connect` takes any query -> `connect_and_disconnect` an empty query is 400.
- rt_apigw.c: an expired row still posted to -> `publish_posts_each_subscriber` n_posts == 2.
- rt_apigw.c: garbage keeps the socket -> `a_lost_row_is_rebuilt_and_garbage_closes` n_deletes == 1.
- rt_apigw.c: the TTL never pushed forward by a frame that changes nothing -> `frames_round_trip_through_the_conn_row` conn_puts == puts0 + 2.
- rt_local.c: pushes and replies sent as text frames -> `subscribe_publish_and_isolation` the first ACK.
- rt_local.c: the upgrade takes any proto -> `the_upgrade_route` 400.
- stream.c: UNSUB of a game leaves the spectators' topic -> `subscribe_publish_and_isolation` the spectator hears nothing after its UNSUB (found surviving first; the spectator UNSUB was added for it).
- stream.c: chat open to non-members -> `subscribe_publish_and_isolation` the second ACK carries a why.
- router.c: the router's refusals as JSON -> `http_basics` content-type text/plain.
- v1_http.c: no store answered 500 -> `without_a_backend` the games list is 503.
- v1_http.c: no auth module answered 500 -> `without_a_backend` the sign-up's status is 503.
- realtime.c: `rt_publish_view` sends nothing -> the game smoke, B heard no views.


## Ported from the TypeScript e2e (stage 4)

The TypeScript server's e2e suite went with it; these unit_app.c tests carry over what it guarded that no C test did. Two seams were added for them: `cb_app_game_uuid` (the UUID a game id is cut from) and `cb_bots_cycle_observer` (what each bot cycle is handed). Each mutant was applied, the named test failed on the named assertion, and the source was restored.

- `a_taken_game_id_is_drawn_again` (create_game_id): a taken id ends create instead of drawing again; CB_TABLE_ID_DRAWS + 3 draws -> `g_draws == CB_TABLE_ID_DRAWS`.
- `a_goods_only_commit_is_broadcast` (good_is_a_move): `goods_changed` dropped from the broadcast rule -> `g_fan.casts > 0 && g_fan.spectator_cast` (the rule itself is gone since: every commit casts).
- `every_commit_is_cast` (table_ops.c, a commit that moves the version and casts nothing): the casts built only when `n_events > 0 || goods_changed || roster_changed` (main before the fix) -> `g_fan.casts == 2` and `g_fan.spectator_cast`, on the quiet goods and the rearranged hands; the casts skipped only for a quiet commit with no log records (a hand rearranged, not a silent good) -> the same two, on the rearrangements alone.
- smoke.c `game_scenario` cast gaps: the same binary as main before the fix -> `cast_gaps(LA) == 0` and `cast_gaps(LC) == 0` (a cast at v7 follows v5, ...).
- `a_bump_wakes_the_bots` (heartbeat_needs_bots): a bump wakes only on a commit -> `g_kicks == kicks + 1`.
- `continue_returns_a_clean_lobby` (waiting_stale_blob, meta): a lobby appends to the log -> `row.logs_len == 0`; a lobby keeps the seed -> `!row.has_seed`.
- `a_stranger_changes_nothing` (security_seat_from_auth, table_server_seat): an unseated bearer acts as seat 0 -> `r.status == 400`.
- `concurrent_moves_lose_and_duplicate_nothing` (adversarial_ts_layer, concurrent_games): a lost CAS reported as stored -> `version_of(gid) == v0 + applied` (a_lost_cas_reloads_and_wins fails on it too).
- `every_view_goes_to_its_own_seat` (security_hidden_info 2p/4p/6p): each seat's pushed envelope cut for the next seat -> `g_hidden_bad == 0`; an uncommitted answer (bump) cut for seat 0 -> `g_hidden_bad == 0`.
- `every_bot_cycle_gets_the_whole_log_and_its_choices_back` (belief_logs_wiring, belief_human_freshness, resilience): half the log handed to the cycle -> `g_cyc.short_log == 0`; the chosen moves not offered back after a lost CAS -> `g_cyc.retries_with_prefs == g_cyc.retries`.
- `bot_moves_wait_for_the_table_to_settle` (web_bot_first_move_pace, web_throwin_vs_bot, win_screen_rating_is_the_games): the kernel's wait ignored -> `g_pace.at[k] >= t_last + wait`.

Equivalent, recorded so nobody chases it: a bot cycle reading the row cache instead of the store survives, because in one process the cache holds the newest commit. Not ported: the casts' per-seat masking (the test checks every envelope against the seat's view, not every cast, which the kernel's own table_cast tests hold).

## Edge cache, the bounded leaderboard read, and busy (2026-10-17)

Each mutation rewritten in, its one test binary rebuilt with the touched object deleted, then rewritten back (DESIGN.md "Edge cache").

- v1_http.c: CC_BOARD s-maxage 3600 -> 3601 -> `cache_control_per_route` Cache-Control == "public, max-age=0, s-maxage=3600".
- v1_http.c: h_board answered as a private list -> the same assertion.
- v1_http.c: h_bots reads the caller again -> `bots_board_history_replay` call(b_bots) == 200.
- v1_http.c: public answer without `Access-Control-Allow-Origin: *` -> `cache_control_per_route` ACAO == "*".
- netclient.c: nc_req_bots carries the token again -> `cache_control_per_route` q->auth_len == 0.
- v1_http.c: CB_E_BUSY not mapped -> `a_throttled_read_is_busy_and_retried` board == 503.
- v1_http.c: no Retry-After -> the same test, Retry-After == "1".
- v1_http.c: history reads every rating row again -> `bots_board_history_replay` has_rating and rating == 1600 (the fake's full read fails).
- store.c: CB_STORE_CALL never turns CB_E_DB into CB_E_BUSY -> `a_throttled_read_is_busy_and_retried` board == 503.
- store.c: the busy mark never cleared -> the same test, a plain failure == 500.
- netclient.c: nc_retry_ms retries a POST -> NC_POST == 0; retries a fourth time -> attempt 3 == 0; retries any 503 -> the no-backend 503 == 0.
- ask.c: ASK_WHY_BUSY not a known reason -> `a_throttled_read_is_busy_and_retried` nc_list_read == -ASK_WHY_BUSY.
- store_dynamo.c: a throttle given up on not marked busy -> `flow_throttled_past_the_deadline` == CB_E_BUSY.
- store_sqlite.c: ratings_top without `games_played > 0` -> `users_round_trip` ratings_top == 0; lowest first -> `ratings_applied_by_commit` rows[0] == b; ratings_list without the filter -> `users_round_trip` ratings_list == 0.
- store_dynamo.c: ratings_top does not stop at its limit -> under DynamoDB Local only, `ratings_applied_by_commit` ratings_top(1) == 1 (the fake transport never pages it).
- json_surface.c: the top Query without `begins_with(E#)` -> `query_shapes` body; a rated commit's Update without gsi2sk -> `commit_transaction` body; the rank not inverted -> the same; a sign-up's rating row in the index -> `signup_transaction` body; the reindex without its condition -> `query_shapes` body.
- json_surface.c: Quantity off by one -> `cloudfront_invalidation` body == CF_BODY; `&` not escaped -> the escape assertion.
- cloudfront.c: signed for AWS_REGION instead of us-east-1 -> `cloudfront_invalidation` Authorization names us-east-1/cloudfront; any 2xx accepted -> a 200 == CB_E_IO.
- edge.c: flush does not clear the marks -> `cloudfront_invalidation` pending == 0; the bots path never named -> cb_edge_paths(ALL) == 2.
- table_ops.c: a rated commit not marked -> `a_finished_game_writes_a_snapshot_and_ratings` marks == 1; every commit marked -> the same assertion.
- functions_http.c: an account deleted not marked -> `delete_account_redacts` marks == 1.
- unit_json_gate.c: `xmlns` not seen -> `the_scanner_sees_each_kind` planted namespace == 1; an XML literal planted in cloudfront.c -> `only_json_surface_speaks_json` violations == 0.
- json_surface.c: the reindex rewrites a row already keyed -> `query_shapes` body (and, on DynamoDB Local, `ratings_applied_by_commit` reindex == CB_E_CONFLICT).
- store_sqlite.c: the reindex reports a row as rewritten -> `ratings_applied_by_commit` reindex == CB_E_CONFLICT.
- changeset_replacements.jq: every conditional counted -> `changeset_gate_test.sh` code_only empty; a conditional on a replaced resource's attribute not counted -> mixed == "Other Dep Big".
- netclient.c, the pump and its book (unit_netclient; 33 mutants, all killed): a stale answer taken (`version < top` dropped) -> `a_cast_and_a_stale_answer_end_alike` the two orders' tables; an answer at the top version taken twice -> `a_skipped_version_refetches` stale == 1; at the top over a dashboard view refused -> the same test, table_v == 9; a gapped cast applied -> h.casts == 1; a stale cast applied -> `gates`; a moot refetch sent -> `a_refused_move_catches_up`; a re-ACK or a refusal not refetching -> `accepted_again_refetches`; a join not asking again, a create not subscribing, any ask's answer taken as a board -> `create_and_join_move_the_subscription`; the session event dropped -> `the_session_is_told_once`; an own refetch answered as a ticket, its view not an event -> `a_skipped_version_refetches`; the refetch not delayed, or after an applied move -> `a_refused_move_catches_up`; holds not counted -> `stream_order_and_retries`; two refetches of one game -> `a_skipped_version_refetches`; the book kept, or always dropped, across users, the subscriptions kept -> `another_user_starts_the_book_over`; a held game's dashboard view handed out -> `gates`; the arena ignoring overlap, its cap, its zero case, or read after the slot was let go -> `waiting_intents_keep_their_bytes` and the stale-order test; boards mode off, suppressing views, pinning a create, caught_up ignoring views -> `a_host_of_boards_gets_boards`; a busy read not sent again, a busy write sent again, the busy count not kept -> `a_busy_read_goes_again_a_write_does_not`; a plain SUB sent -> `stream_order_and_retries`.

## The scan kicks only what the kernel says a bot can act on (2026-10-17)

Each mutant built with its source's mtime pushed past the objects (make's same-second trap), run, then restored the same way and run green.

- table.c (kernel): `table_bots_due` answers `table_needs_bots` -> tests.c `bots_due: a human's opening is no bot's move` and `so the row it commits needs no bots`.
- table.c (kernel): TableCommit.needs_bots from `table_needs_bots` -> tests.c `so the row it commits needs no bots`; unit_app `the_scan_never_kicks_a_game_waiting_on_its_human` !row.needs_bots, kicked == 0, and `the_answer_goes_before_its_pushes` kicks.
- table_ops.c: the bump asks `table_needs_bots` -> unit_app `a_bump_wakes_the_bots` g_kicks == kicks, `the_answer_goes_before_its_pushes` kicks.
- bots.c: the drive never calls `bots_idle` -> unit_app `a_drive_that_finds_nothing_takes_the_game_out_of_the_scan` bots_idle == idle0 + 1, !row.needs_bots, the next scan == 0.
- lib_app_store_mem.c: `bots_idle` unfenced -> the same test, bots_idle(v - 1) == CB_E_CONFLICT.
- store_sqlite.c: ST_BOTS_IDLE unfenced -> unit_store `bots_idle_semantics` bots_idle(0) == CB_E_CONFLICT.
- json_surface.c: the commit's marker without row_version -> unit_dynamo `commit_transaction` body; the create's marker without it -> `items_round_trip` row_version present; the idle delete unfenced -> `lease_requests` body.
- main_lambda.c: the timing line without the request id -> smoke_lambda ` rid=req-2` and ` rid=req-4`.
- Equivalent by design: bots.c's `!table_bots_due` exit swapped back to `table_needs_bots` survives, because the `drv.n == 0` exit below it takes the game out of the scan too (a WARN, since the kernel would then disagree with itself).
- E2E red first: smoke `an_idle_human_turn_is_never_kicked` against the old binary failed on nb == 0 and on the scan clock (a heartbeat kick 20 s into the human's turn).
- The realtime write cut (2026-10-17; 21 mutants, all killed, each with its own fresh object and test binary):
  rt_apigw.c: a ping reading the row -> `frames_round_trip_through_the_conn_row` conn_gets == gets0 and `the_last_reply_is_the_route_response` conn_gets == 0; the route response never used -> n_posts == 0; every frame writing the row -> conn_puts == puts0 + 1; an empty, nobody's row kept -> rtf_conn_find == -1; the TTL at two hours flat -> ttl > 7700000; a lost compare-and-set not run again -> `concurrent_frames_of_one_connection_both_stick` n_topics == 2; a 410 on a pong keeping the row -> the posts and row checks.
  stream.c: a batch entry that is a server frame run -> `batches` run_batch == CB_E_INVALID; a reply batch of NW_BATCH_MAX + 1 -> `long_replies_split` g_out.n == 2; a lone reply sent as a batch of one -> out(1).kind == NW_WS_ACK.
  netwire.c: an empty batch read -> `batches` empty == NW_E_WIRE; a batch inside a batch -> nw_batch_add(inner) == NW_E_WIRE; more than NW_BATCH_MAX entries -> encode and decode == NW_E_WIRE.
  json_surface.c: a CONN item stored before 2026-10-17 unread -> `items_round_trip` the old item reads; the version-0 condition swapped -> `builders_valid_json` both ConditionExpressions; an index entry carrying topics -> "topics" absent; the route response body dropped -> `encode_ws`.
  store_sqlite.c: conn_put without its version check -> `conns_round_trip` CB_E_CONFLICT twice.
  store_dynamo.c (DynamoDB Local only): the lost topics' entries not deleted -> `conns_round_trip` conns_by_topic(game-ab12cd) == 1; the gained topics' entries not written -> == 2; the row put unconditionally -> CB_E_CONFLICT twice.

## Defects the coverage work found (2026-10-17)

Each test was red against the unfixed code first; the mutants below re-break each fix and its test goes red again (the source rewritten back after each, its mtime moved past the objects').

- store_sqlite.c: textrow_read counting columns before its arrays exist -> unit_oom crashes in the console's browse (SIGSEGV in textrow_free).
- functions_http.c: a retried delete answering the store's NOT_FOUND -> smoke_matrix "POST /v1/account/delete (again)" and unit_oom's delete sweep (unfailed run 500).
- functions_http.c: create without account_lives -> smoke_matrix `a_token_for_nobody` and the deleted user's create cell.
- realtime.c: live_verify not reading the account -> smoke_matrix: the deleted account's token subscribes again, and a token for nobody subscribes.
- rt_local.c: the deleted user's sockets not closed -> smoke_matrix sE->closed, close code 1008.
- rt_apigw.c: forget not filtering by user -> unit_forget: B1, another user's connection, deleted.
- v1_http.c: a non-member's chat post stored -> smoke_matrix D's POST /v1/chat cell; a replay of a game the caller did not play answered -> the replay cells; failed() answering 404 for every store error -> unit_store_faults "answered 404".
- admin.c: any bearer taken for the admin token -> smoke_matrix's console and bad-identity cells (58 checks).
- view.c (the kernel): seat 0's hand shown to every viewer -> smoke_matrix "is shown to viewer"; evwire.c: deals never masked -> smoke_matrix's casts "shown to viewer".
- stream.c: a FORBIDDEN subscription accepted -> smoke_matrix `stream_frames`.
- oom.h: `the_seam_reaches_cbase` holds the seam itself (a failure delivered to util/str.c, a held block counted, then freed).
- auth.c/jwt.c: a token check whose HMAC could not be computed answered CB_E_AUTH (the code before the fix) -> unit_oom `a_token_check_that_cannot_be_made_is_not_a_bad_token`, red on CI's ubuntu lane (allocations 1-3 of the check: CB_E_AUTH), green after; on macOS OpenSSL's allocations are out of the seam's reach and the sweep fails nothing.
- lib_faultstore.c: its operation count one short -> the static assert stops the build (store.h has an operation lib_faultstore.c does not wrap).

## A kick leaves the move to the drive holding the lease (2026-10-17)

Same method as above (mtime pushed past the objects both ways).

- bots.c: `kick_wanted` never sees a held lease (the old behaviour) -> unit_app `a_kick_leaves_the_move_to_the_drive_holding_the_lease` g_kicks == kicks (three), `a_move_made_while_the_lease_is_held_is_driven` g_kicks == kicks.
- bots.c: no look after the release (the old behaviour) -> `a_move_made_while_the_lease_is_held_is_driven` version moved past the move, !bots_due.
- store_sqlite.c: ST_LEASE_HELD `>` for `>=` -> unit_store `lease_semantics` held at T0 + TTL == 1; held without the game's bot work and lease -> four assertions of the same test.
- json_surface.c: a marker with no lease reads 0, not INT64_MIN -> unit_dynamo `lease_requests` dw_item_lease_until(nolease); the marker's key misspelled -> the GetItem body.
- `cbase serve` never showed the waste (its one bot thread runs a kicked drive after the running one ends), so the reproduction is production's own: game f6b571, 24 "lease held" skips among 34 drive invocations.
## Coverage: every function tested (2026-10-17)

- The client's batching (2026-10-17; netclient.c, 14 mutants, all killed; unit_netclient): a reply batch longer than the event array without the guard -> `subscriptions_batch` got == NC_EVENTS_MAX - 1; batches past NC_BATCH_OUT -> biggest == NC_BATCH_OUT; subscriptions sent one per message -> `stream_order_and_retries` g_batches == batches0 + 1; a lone frame sent as a batch of one -> the next_frame batch-of-two check; the room for the batch's head ignored -> o != NULL at the text's edge; a batch of replies read as one unknown frame -> k == 4; an empty message read as BAD -> nc_stream_in == 0; no wait set on a sent subscription -> next_frame == 0 (it also loops forever, so the tests' loops are bounded); a sent one never asked again -> `an_unanswered_subscription_is_asked_again`; asked again without marking pushes missed -> the refetch; a ref of 0 after the wrap -> zero == 0; nc_stream_due_ms ignoring sent ones -> due == NC_ACK_WAIT_MS.
The tests docs/COVERAGE.md lists, each mutation applied by rewriting the source (its mtime set past the objects', so make cannot skip it), the test run, the source rewritten back.
A mutation that would not compile (an unused variable under -Werror) was replaced by one that does; none is listed as killed that did not run.

- err.c: CB_E_BUSY's name changed -> `every_error_has_its_name`.
- http.c: 426's reason changed -> `every_status_has_its_reason`.
- config.c: the JWT secret's floor 32 made 3 -> `configuration_refusals`.
- json.c: a fraction with no digits accepted -> `json_numbers_and_literals`; any \u after a high surrogate taken as its low half -> `json_strings`; a positive past INT64_MAX accepted -> `json_paths`; `\b` not escaped -> `json_writer_escapes`.
- sigv4.c: a shorter key sorted after a longer one -> `sigv4_query_order`.
- jwt.c: '+' and '/' accepted in a part -> `signed_parts_must_be_base64url` (the standard-alphabet header; padded and empty parts were already refused by the decoder and the JSON check, so a first mutant allowing '=' survived and the case was added).
- str.c: cb_url_decode's room for the NUL dropped -> `url_decode_and_levels`.
- store_dynamo.c (unit_dynamo_ops): an email not found answered CB_E_DB -> `users_by_email_password_touch`; a batch left partly undone not sent again -> `user_delete_cascade`; the offset ignored -> `users_list_pages`; a taken refresh hash not a conflict -> `refresh_put_and_revoke_session`; a delete's fence item and gone swapped -> `game_create_delete`; the games-of-user limit ignored -> `games_of_user_and_membership`; an unplayed row put on the leaderboard -> `bots_and_ratings`; a snapshot query's game filter ignored -> `snapshots_put_and_query`; a chat put to a gone game not NOT_FOUND -> `chat_put_refusals`; a put that leaves the lost topic's index entry -> `conns_put_get_delete_by_topic` (rewritten for #328's three-write conn_put); the collections callback's stop ignored -> `admin_collections_are_the_families`; a reindex already done counted -> `import_writes`.
- store_import.c: bots seeded over existing ones -> `import_seed_and_reindex`; an extra item counted as stored -> `import_counts_by_scan`.
- admin.c (unit_admin_edges): FORBIDDEN answered 409 -> `store_errors_map_to_statuses`; no store answered 500 -> `no_store_is_503`; browse without c accepted -> `malformed_asks`; '/' left unescaped in CloudWatch's queryDetail -> `lambda_links`; backups oldest first -> `backups_newest_first`; a seat item listed as a game -> `a_game_that_does_not_load`.
- bots.c: Lambda's kick sends nothing -> `lambda_kick_self_invokes`; runtime_api.c: init errors posted to /runtime/error -> `runtime_init_error_posts`; json_surface.c: the DynamoDB host without its region -> `dynamo_host_from_region`, the create answer read from the wrong item -> `bench_fixtures`; netclient.c: nc_row's bots case dropped -> `the_list_reader_by_kind`, nc_due_ms the later of the two -> `when_to_call_again`.
- server.c (unit_server_edges): a stalled request closed without its 408 -> `http_defences`; max_conns ignored -> `one_connection_too_many` (first survived: the seventh connection asked nothing, and the idle sweep closed it anyway; the test now sends a request on it and wants no answer); a continuation with nothing to continue accepted -> `websocket_rules`; the drain closing sockets 1000 -> `drain_closes_sockets`.
- stream.c (unit_stream_policy): the dash before a seat topic's user id not required -> `topic_policy`; a chat UNSUB that keeps its topic -> `unsub_leaves_only_its_topic`; one topic past CB_CONN_TOPICS allowed -> `a_full_connection_refuses_more`.
- after.c (unit_after): the window left open after the drain -> `open_window_waits_and_keeps_order`; a queued fan-out holding the caller's pointers instead of copies -> the same test aborts (a free of the caller's buffers).
- main_lambda.c: the HTTP event's window never opened -> `smoke_lambda_after` push_after > create_resp.
- main_serve.c: any first argument accepted -> `smoke_boot` cbase_refuses; json_surface.c: every init error typed Runtime -> bootstrap_refuses.
- store_dynamo.c: bots_idle's lost condition taken as done -> `bots_idle_deletes_the_marker`.
- config.c (unit_edges): the origin secret listed in the clear and not flagged secret -> `configuration_refusals` (secrets == 3, leaked, wrong_secret); the listing is held by name, not by count, so a new setting does not break it.
- store_dynamo.c: lease_held's last millisecond not counted as held -> `lease_held_reads_the_marker` (the gate named dy_lease_held as never called the day it landed).
- stream.c: a batch's entries not checked for a client's kind -> smoke_matrix `stream_frames` "server frame in a batch" (an ACK inside ran nothing before; now the batch's SUB_AS was answered and the socket stayed).
- rt_apigw.c: route replies taken when CBASE_WS_ROUTE_REPLY says no, and not when it says yes -> smoke_lambda_after: `the_answer_goes_first` (the SUB's ACK never POSTed) and `route_replies_ride_in_the_answer` (no body in the $default answer).
- smoke.c's two deadlines (the transport's 10 s, "the version held 5 s"): each reproduced red by freezing the server with SIGSTOP (11-15 s, mid-game; 12 s, while the idle test waited for the bots), and green after with the same freeze.



## MQTT on IoT Core, stage 1 (2026-10-17: unit_mqtt, unit_stream_mqtt, unit_rt_iot, unit_rt_local, unit_store, unit_ws, smoke_authz)

Run one mutant at a time from a script that deletes the mutated file's objects before and after (make compares whole seconds); equivalent or build-breaking mutants were reworded until they compiled and changed behaviour.

- mqtt.c: a fifth remaining-length byte accepted -> `lengths_and_types` parse(five) == -1.
- mqtt.c: the reader's limit not checked -> `publish_both_ways` parse(out, 5, 4096) == -1 (refused from the length alone).
- mqtt.c: QoS 2 and DUP at QoS 0 accepted -> `publish_both_ways` QoS 2 == -1, dup0 == -1.
- mqtt.c: password without username accepted -> `connect_refused_shapes` parse(pw) == -1.
- mqtt.c: '#' not last accepted -> `client_packets` "u/#/v" == -1 (and the writer's refusal).
- mqtt.c: '+' as part of a level accepted -> `topics` mqtt_topic_ok("+u") == 0.
- mqtt.c: '$' topics matched by a leading wildcard -> `topics` match("#", "$SYS/x").
- mqtt.c: "a/#" not matching "a" -> `topics` match("u/a/#", "u/a").
- mqtt.c: a level matched past its end -> `topics` match.
- mqtt.c: CONNECT's trailing bytes accepted -> `connect_refused_shapes` parse(t) == -1.
- mqtt.c: a 17th filter accepted -> `client_packets` parse(big) == -1.
- mqtt.c: SUBSCRIBE QoS byte's reserved bits accepted -> `client_packets` 0x41 == -1.
- mqtt.c: a SUBACK code of 3 accepted -> `connack_suback_unsuback_pingresp` parse(bad_code) == -1.
- mqtt.c: printable range widened to 0x20 -> `connect_written_and_read` "a b" == -1.
- mqtt.c: the length-byte boundary at 16384 moved -> `publish_both_ways` mqtt_publish_len == written length.
- mqtt.c: a type the side never sends waited for -> `lengths_and_types` parse(first) == -1.
- stream_mqtt.c: "s/" alone allowed -> `subscribe_policy` "s/".
- stream_mqtt.c: the '/' after the user not required -> `subscribe_policy` "u/<A>0/v".
- stream_mqtt.c: client id prefix only -> `connect_policy` "<A>", "<A>-", "<A>x".
- stream_mqtt.c: the policy not asked on SUBSCRIBE -> `a_connection_ends` mixed SUBSCRIBE == CB_E_FORBIDDEN (and `smoke_authz` mqtt subscribe as stranger).
- stream_mqtt.c: no end at the token's expiry -> `a_connection_ends` PINGREQ at expiry == CB_E_AUTH.
- stream_mqtt.c: a second CONNECT taken -> `a_connection_ends` == CB_E_INVALID.
- stream_mqtt.c: PUBLISH taken -> `a_connection_ends` == CB_E_FORBIDDEN.
- stream_mqtt.c: CB_MQTT_SUBS not enforced -> `a_connection_ends` 33rd filter == CB_E_FULL.
- stream_mqtt.c: the same filter twice takes two slots -> `a_connection_lives` n_subs == 2.
- stream_mqtt.c: no 300 s floor -> `connect_policy` expires_s == now + 300.
- stream_mqtt.c: a user id that is not a UUID granted -> `connect_policy` "tok-u+" == CB_E_AUTH.
- stream_mqtt.c: a NUL inside the token passed on -> `connect_policy` "tok-<A>\0junk" == CB_E_AUTH.
- stream_mqtt.c: keep-alive past 1200 s taken -> `a_connection_ends` keep-alive row.
- rt_local.c: an MQTT connection hears every publish -> `mqtt_publishes_reach_their_topics` b hears nothing of U1's.
- rt_local.c: text frames taken on MQTT -> `mqtt_refusals_end_the_connection` "wAA=" closes 1003.
- rt_local.c: a refusal not closing the socket -> `mqtt_refusals_end_the_connection` close 1008.
- rt_local.c: /mqtt without the subprotocol -> `mqtt_upgrade_route` fd == -1.
- rt_local.c: forget skipping MQTT connections -> `mqtt_publishes_reach_their_topics` close 1008 after forget.
- ws.c: the 101 not naming the subprotocol -> `subprotocols` header present.
- ws.c: an offered list not checked for the whole token -> `subprotocols` "mqttv3.1" == CB_E_INVALID.
- rt_iot.c: the origin not checked -> `only_through_the_edge` !ok without and with a wrong header.
- rt_iot.c: a 403 publish counted as delivered -> `pushes_are_publishes` failures == 1.
- rt_iot.c: the whole-game chat target published -> `pushes_are_publishes` n == 4.
- rt_iot.c: the event's client id ignored -> `the_policy` another user's client id !ok.
- rt_iot.c: a fixed lifetime -> `the_policy` disconnectAfterInSeconds == 1800.
- json_surface.c: a password holding a NUL taken -> `decoded_events` token_len == 0.
- json_surface.c: the user id not checked before a policy is written -> `the_encoder_refuses_what_a_policy_cannot_hold`.
- json_surface.c: a '*' allowed in a region -> `the_policy` "us-east-*" == CB_E_INVALID.
- json_surface.c: the origin header matched by case -> `decoded_events` has_origin.
- json_surface.c: the event's token ignored -> `decoded_events` token == "tok-<B>".
- json_surface.c: IoT's leading '?' kept on the query -> `decoded_events` "?token=..." read.
- config.c: CBASE_IOT_ENDPOINT taken with a scheme or path -> `the_endpoint_setting` == CB_E_INVALID.
- store_sqlite.c: game_members not stopping when fn says so -> `membership_follows_commits` count == 1.
- store_dynamo.c: game_members handing out a bot -> `membership_follows_commits` who.n == 2 (DynamoDB Local).
- realtime.c: the account not read on the token check -> `smoke_authz` a deleted account's token connects.
- mqtt.c: a non-minimal remaining length accepted -> `fz_mqtt` replay of `repro-nonminimal-length`, FZ_CHECK w == k.

## MQTT on IoT Core, stage 2: the kernel's client (2026-10-18: unit_netclient)

- netclient.c: spectators' casts taken while seated -> `seat_and_spectator_casts` casts == 1, == 4.
- netclient.c: a spectators' cast applied while the seat is unknown -> `seat_and_spectator_casts` casts == 0.
- netclient.c: a message whose game is not its topic's taken -> `what_is_not_the_brokers` BAD.
- netclient.c: a text frame taken -> `what_is_not_the_brokers` PINGRESP as text is BAD.
- netclient.c: a CONNACK not awaited taken -> `connect_carries_the_token` second CONNACK is BAD.
- netclient.c: CONNECT sent with a token about to expire -> `connect_carries_the_token`, `connect_waits_for_a_fresh_token`.
- netclient.c: every refused CONNECT forces a refresh -> `closed_before_connack_refreshes` no second refresh in the window.
- netclient.c: no catch-up read on SUBACK -> `suback_takes_the_game_whole`, `an_unanswered_subscription_is_asked_again`.
- netclient.c: the keep-alive one tick late -> `stream_order_and_retries` PINGREQ at NC_PING_MS.
- netclient.c: client id from the clock alone -> `connect_carries_the_token` two connections in one millisecond differ.
- netclient.c: "u/<me>/vx" read as the views topic -> `what_is_not_the_brokers` BAD.
- netclient.c: the stream due at once while waiting for a token -> `connect_waits_for_a_fresh_token` due == -1.
- netclient.c: a first session wiping holds made before it -> `holds_made_before_the_session_are_kept` SUBSCRIBE sent.

## MQTT on IoT Core, stage 3: API Gateway removed (2026-10-18)

The entries above for `stream.c`, `rt_apigw.c`, `apigw_mgmt.c`, the connection rows and the stream's AUTH, SUB, SUB_AS, ACK, PONG, CATCHUP and BATCH frames record code that no longer exists.

- rt_local.c: forget closing only the user's first socket -> `unit_rt_local` `mqtt_publishes_reach_their_topics` the second socket ends too.
- realtime.c: a chat sent to an empty member slot -> `unit_stream` `publishes` g_cap.n == 8.
- realtime.c: a chat sent to one more target than its members -> `unit_stream` `publishes` g_cap.n == 6.
- netwire.c: a removed client frame kind (PING) decoded -> `unit_netwire` `frames` every gone kind is NW_E_WIRE.
- store_sqlite.c: schema 3 to 4 keeping the connection tables -> `unit_store` `open_applies_schema_and_seeds_bots` no rt_conn table.
- iot_data.c: AWS_ENDPOINT_URL_IOT_DATA_PLANE ignored -> `unit_aws_calls` `iot_publish` the override answers 200.
- iot_data.c: a plain http:// override taken -> `unit_aws_calls` `iot_publish` CB_E_INVALID.
- config.c: a ":port" in CBASE_IOT_ENDPOINT taken -> `unit_edges` `configuration_refusals`.
- realtime.c: the views topic misnamed -> `smoke_lambda_after` the publish's path is `u/<uid>/v`.
- stream_mqtt.c: any user's topics allowed -> `smoke_matrix` `stream_frames` another user's views closes the socket.

## The bot speedup (2026-10-18: unit_app, unit_edges, unit_config)

`CBASE_BOT_SPEEDUP` (config.h) divides every bot timer for the smoke tests.
Each mutation was applied, its object deleted and the test rebuilt, then restored the same way.

- bots.c: the wait asked at the wall clock, not the board's time -> `bots_pace_at_production_speed_and_at_a_speedup` cb_bots_wait_ms and the first commit's landing time.
- bots.c: the kernel's wait not divided by k -> `bots_pace_at_production_speed_and_at_a_speedup` cb_bots_wait_ms == ceil(wait / k).
- bots.c: the lease acquired for production's 25 s -> `a_drive_takes_its_lease_over_the_speedup` lease_left == 2500.
- bots.c: the lease renewed for production's 25 s -> `bots_pace_at_production_speed_and_at_a_speedup` every cycle's lease_left.
- bots.c: the scan's stale window not divided -> `the_scan_windows_follow_the_speedup` kicked == 1.
- bots.c: the scan's abandon window not divided -> `the_scan_windows_follow_the_speedup` kicked == 0.
- bots.c: the wall ceiling not divided -> `the_wall_ceiling_follows_the_speedup` version unchanged, one rekick.
- bots.c: cb_bots_ms rounding down -> `production_bot_timers` 15 ms over 10 == 2, 1 ms == 1.
- bots.c: the board's time for a row with no creation time -> `the_boards_time` created_at 0 and -5 give now.
- bots.c: the board's time for a creation in the future -> `the_boards_time` now - 50.
- table_ops.c: the commit stamped on the wall clock -> `bots_pace_at_production_speed_and_at_a_speedup` shown_ms == the board's time.
- table_ops.c: the rating commit stamped on the wall clock -> `bots_pace_at_production_speed_and_at_a_speedup` the finished board's shown_ms.
- bots.h: CB_BOT_STALE_MS 5000, CB_BOT_SCAN_EVERY_MS 60000 -> `production_bot_timers` each pinned value.
- config.c: the range 1..1000 or 0..100 -> `unit_edges` `configuration_refusals` 101 and 0 refused.
- config.c: the default 2 -> `unit_config` `lambda_refuses_a_bot_speedup` default 1, and `unit_app`'s production pacing.
- config.c: the value read but not stored -> `unit_edges` `configuration_refusals` bot_speedup == 100.
- boot.c: the Lambda refusal removed, or made `> 20` -> `unit_config` `lambda_refuses_a_bot_speedup` the error names CBASE_BOT_SPEEDUP.

## Numbers from text (2026-10-18: unit_num, unit_num_gate, fz_num and the call sites)

`num_parse_i64` (shared/c/num_parse.h) is the one decimal integer parser; DESIGN.md "Numbers from text".
Each mutation was applied, its object deleted and the test rebuilt, then restored from a copy the same way.
The parser was written as c_base/src/util/num.c and moved, unchanged, into shared/c/num_parse.h (header only) before the PR landed; the mutations below were made on those same lines.

- num_parse.h: leading blanks skipped -> `unit_num` `shapes_refused` " 20" and "\t20" refused; `unit_edges` `configuration_refusals` CBASE_BOT_SPEEDUP " 20".
- num_parse.h: a leading '+' taken -> `unit_num` `shapes_refused` "+20" and "+0"; `unit_edges` CBASE_BOT_SPEEDUP "+20"; `fz_num` m == p from the seed corpus, and from an empty corpus within 60 s.
- num_parse.h: the overflow check removed -> `unit_num` `int64_extremes` INT64_MAX + 1 and INT64_MIN - 1 refused.
- num_parse.h: the overflow check one past (`/ 10 + 1`) -> `fz_num` m == p from the seed corpus (seed-max-plus-one); an empty corpus did not find it in 60 s, which is what the seeds are for.
- num_parse.h: '-' taken when lo >= 0 -> `unit_num` `shapes_refused` "-0" refused in 0..100; `unit_edges` CBASE_LISTEN "h:-0".
- num_parse.h: a non-digit ends the number instead of refusing it -> `unit_num` `shapes_refused` "20 " and "20\n" (13 checks).
- num_parse.h: hi not checked -> `unit_num` `bounds` "101" in 1..100 refused.
- num_parse.h: a refusal writes *out -> `unit_num` `bounds` g_v == 7777.
- num_parse.h: a lone '-' taken -> `unit_num` `shapes_refused` "-" refused, and `with_a_length` "-12" cut to 1 byte.
- num_parse.h: INT64_MIN read as INT64_MIN + 1 -> `unit_num` `int64_extremes` "-9223372036854775808".
- num_parse.h: a `lo > hi` check was removed as equivalent (an empty range refuses every value at the range check); `bounds` keeps "5" in 6..5 refused.
- admin.c: `after` takes a '-' -> `unit_admin` `logs_tail_after_a_seq_at_a_level` after=-1 == 400.
- admin.c: a bad `after` read as 0 (strtoull's answer) -> `unit_admin` after=-1, " 5", "+5" and "5x" == 400.
- admin.c: q_int refuses negatives (no clamp to lo) -> `unit_admin` `browse_pages_by_cursor` limit=-5 gives last_limit == 1.
- store_sqlite.c: a bad cursor read as the first page -> `unit_store` `admin_reads` "3x", " 3", "+3", "x", "3 " and an overflow == CB_E_INVALID.
- aws_call.c: the port read to the end of the string, not to the path -> `unit_aws_calls` `url_parse` "wss://h:8443/a/b//" == CB_OK.
- runtime_api.c: port 0 taken -> `unit_runtime_api` `runtime_api_port` "127.0.0.1:0" == CB_E_INVALID.
- runtime_api.c: a negative deadline taken -> `unit_runtime_api` `deadline_is_digits` " -1" == 0.
- runtime_api.c: a header's trailing blanks kept -> `unit_runtime_api` `deadline_is_digits` " 1791998921003 \t" read.
- store_import.c: a malformed counts.txt line skipped like a missing one (-1 for -2) -> `unit_import` `failing_checks` TEXT_COUNT returns 1 with "users -2" and MISMATCH.
- store_import.c: a malformed cell read as 0, not the column's none -> `unit_import` `failing_checks` TEXT_COUNT last_commit_at == 4000.
- config.c: CBASE_LISTEN's port range from -1 -> `unit_edges` `configuration_refusals` "h:-0" refused.
- json_surface.c: a DynamoDB N only non-negative -> `unit_dynamo` `numbers_are_digits` INT64_MIN read.
- unit_num_gate.c: a strtol, a strtod and an sscanf each planted at the end of src/util/log.c -> `only_num_parse_i64_reads_numbers` violations == 0.

## The IoT authorizer's token, unescaped whole or not at all (2026-10-18)

The fuzz-smoke run on main at 56a9fe87c found `fz_event`'s invariant broken: an IoT authorizer event whose `"token"` unescaped one byte (`r`) and then failed on a lone surrogate left `ev.iot.token` holding `r` with `token_len` 0.
Two fixes, for two vectors, each held by its own test: `cb_json_get_str` leaves `out` empty on any failure (every caller), and `decode_iot_auth` reads `"token"` by `cb_json_unescape` and keeps it only when it is whole and holds no NUL (a `\u0000` would have cut it short, which `cb_json_get_str` cannot see).
The hand-minimised input is `fuzz/corpus/event/crash-40a6db73`.

- json_surface.c: the old token path (`cb_json_get_str` then `strlen`), json.c fixed -> `unit_rt_iot` `decoded_events` the `\u0000` token: token_len == 0 and token == "".
- json.c: no clear on failure, json_surface.c fixed -> `unit_util` `json_reader` s == "" after a too-long value, a lone surrogate and a missing key.
- both reverted -> `unit_rt_iot` the lone-surrogate token == "", and `fz_event` replay of crash-40a6db73 fails its FZ_CHECK.

## The key-capture proof's classes are positional (2026-10-18)

- json_surface.c shape_value: a value whose first character is a digit printed as {n} instead of its template -> `unit_items` `shapes_ignore_what_an_id_looks_like` (the seat's dashboard key `{}#{}` for a game id `ab12cd`; and every all-digit game id's shape differs from it).
- json_surface.c fits: a variable slot refusing a 10- or 15-character value that starts with '0' (the old width rule, narrowed so only the new ids trip it) -> `unit_items` `shapes_ignore_what_an_id_looks_like` (the game ids `0000000000` and `000000000000000` shape differently from `ab12cd`).
- Proof against main: origin/main dcc186a8b's store code with this classifier and golden gives the same 99 shapes, md5 c75fcd08f8a4aa9830aaadd0a39b4559, over two runs.

## A replay's copies hold no names (stage 6, 2026-10-18)

- rows_foolish.c cb_rows_snapshot_put: a player's copy written with the whole replay (snapshot_attrs) -> `unit_store` `replay_copies_hold_no_names` (the PLAYER# copies hold extras, moves, player_ids and gsi1pk).
- rows_foolish.c fat_step: no copy found fat (redaction leaves a legacy copy as it was) -> `unit_store` `replay_copies_hold_no_names` (after snapshot_set_extras the legacy copy still holds extras and moves).

## Held to the coverage floor in stage 6 (2026-10-18)

- store_dynamo.c capture: Query requests not recorded -> `unit_items` `the_capture_records_every_request` (queries == 1, got 0).
- rows_foolish.c cb_rows_rating_reindex: a row with no games given a leaderboard key (`games_played >= 0`) -> `unit_store` `ratings_reindexed` (the unplayed row still holds gsi2pk).
- items.c cb_av_same: two string lists of one length always the same -> `unit_items` `values_compare_by_kind_and_value` (`a1` and `a3` differ in their second string).

## The name gate, the compiler gate and fuzz.mk's bookkeeping (stage 8, 2026-10-18)

The name gate (PLATFORM.md 8.4) is `core/test/unit_name_gate.c` over `shared/tools/name_gate/words.txt` (products, providers, foolish's domain words) and the shrinking allowlist `shared/tools/name_gate/allow.txt` (exact counts per file and pattern); `shared_is_shared_validation.test.ts` reads its product list from the same file.
The compiler gate is `make compiler-gate` over one file per side under `<side>/test/gate/`.
One bug found: `fuzz-seeds` copied every target's seeds before refusing a stray seed directory, leaving the tree half-seeded; the refusal now runs first.

- unit_name_gate.c word_char: `_` counted as a word character -> `unit_name_gate` `the_matcher_sees_each_shape` (planted(L_DOMAIN, "cb_rows_bots_replace(s);") == 1).
- unit_name_gate.c match_here: `\b` taken as `before || after` -> `unit_name_gate` `the_matcher_sees_each_shape` (planted(L_DOMAIN, "both robots") == 0).
- unit_name_gate.c scan_tree: providers scanned without L_DOMAIN -> `unit_name_gate` `each_side_names_only_its_own` (stale == 0, got 19).
- core/src/util/random.c: a planted "DynamoDB" comment -> `unit_name_gate` `each_side_names_only_its_own` (new_hits == 0).
- foolish/server/src/app/admin_panels.c: a planted "SQLite" comment -> `unit_name_gate` `each_side_names_only_its_own` (new_hits == 0).
- allow.txt: the app.h /eventbridge/i line deleted -> `unit_name_gate` `each_side_names_only_its_own` (new_hits == 0).
- allow.txt: that count raised from 1 to 2 -> `unit_name_gate` `each_side_names_only_its_own` (stale == 0).
- allow.txt: a line for a file with no hits -> `unit_name_gate` `each_side_names_only_its_own` (stale == 0).
- allow.txt: a pattern not in words.txt -> `unit_name_gate` `each_side_names_only_its_own` (load_allow() == 0).
- shared/c/b32.h: a planted "pick em up" comment -> `shared_is_shared_validation.test.ts` `no file under shared/ names a product` (deepEqual bad, []).
- shared_is_shared_validation.test.ts SKIP_DIR: name_gate dropped -> the same test (bad lists name_gate/allow.txt).
- words.txt: the /pick ?'?em ?up/i line removed -> the same test (PRODUCT.length >= 15).
- Makefile PRODUCT_INC: -Iproviders/aws/src added -> `make compiler-gate` `product_includes_aws.c` (the include across the line got through).
- Makefile side_inc_of: core mapped to ALL_INC -> `make compiler-gate` `core_includes_local.c` and `core_includes_product.c` (got through).
- core/test/gate/core_includes_product.c: the include pointed at a missing header (and, separately, a syntax error) -> `make compiler-gate` (does not compile even with every root on its path).
- Makefile GATE: the "file not found" grep text broken -> `make compiler-gate` (failed, but not on its include across the line).
- fuzz.mk: the duplicate-name `$(error)` replaced by `$(info)` -> `fuzz/test/fuzz_mk_test.sh` ("two roots holding fz_one.c were taken").
- fuzz.mk: providers/aws/fuzz dropped from FUZZ_ROOTS -> `fuzz_mk_test.sh` ("print-fuzz-targets lists [...]").
- fuzz.mk: fz_corpus put under the first root -> `fuzz_mk_test.sh` ("two's seed is not in b/corpus/two").
- fuzz.mk: the refusal's exact-word match loosened to a substring -> `fuzz_mk_test.sh` ("seeds for on (a part of one) were taken").
- fuzz.mk: the refusal moved back after the copy -> `fuzz_mk_test.sh` ("seeds for no target were refused only after corpora were written").

## Queue batch: netclient dead-socket watch and view hint (foolish/c/src/netclient.c, unit_netclient.c)

- netclient.c: the dead test `>=` made `>` (and `+ 1`) -> `a_half_open_socket_is_closed_after_the_pong_wait` o != NULL && o->len == 0 at exactly NC_PONG_MS.
- netclient.c: the dead test `NC_PONG_MS - 1` -> the same test, nc_stream_next(t + NC_PONG_MS - 1) == NULL.
- netclient.c: `g_ping_at = 0` dropped from take_packet -> `a_live_socket_is_never_closed` (a len-0 frame after a PINGRESP), and every quiet-pump test.
- netclient.c: `g_ping_at = now_ms` dropped on PINGREQ -> `a_half_open_socket_is_closed_after_the_pong_wait` nc_stream_due_ms(t) == NC_PONG_MS (got 30000).
- netclient.c: g_ping_at + NC_PONG_MS dropped from nc_stream_due_ms -> the same assertion.
- netclient.c: `g_dead = 0` dropped from nc_stream_opened -> `a_half_open_socket_is_closed_after_the_pong_wait` (the reopened socket never SUBSCRIBEs: s.type == MQTT_SUBSCRIBE).
- netclient.c: `g_ping_at = 0` dropped from nc_stream_opened -> SURVIVES, equivalent: the first packet of the new connection (its CONNACK) clears it and the dead test only runs on a live socket. Kept as the connection's explicit start state (the reset in nc_stream_closed was the redundant one and is deleted: opened owns both, and removing the g_dead one is caught above).
- netclient.c: a newer dashboard view of a held game queues no read -> `a_newer_view_of_a_held_game_queues_one_read` path_is(q, "/v1/games/g1").
- netclient.c: that read queued for `now_ms` instead of `now_ms + NC_RECONCILE_MS` -> the same test, nc_next_request(... NC_RECONCILE_MS - 1) == NULL.
- netclient.c: the held game's view applied (the `return` dropped) -> `gates` (feed ... kind == NC_EV_NONE) and the same test h.views == 0.
- netclient.c: `>` made `>=` in the view hint -> SURVIVES, equivalent: own_settle drops a queued read whose min_version is already reached at the end of the same feed.

## Queue batch: history summary on the pointers (rows_foolish.c, unit_dynamo_ops.c)

- rows_foolish.c: the pointer's rank, seat or dur attribute not written -> `history_pointers_carry_the_summary` (the per-attribute counts).
- rows_foolish.c: rank written as `i + 1` instead of cb_snapshot_rank -> the same test, `"seat":{"N":"0"},"rank":{"N":"2"}` (a first draft counted only the ranks, which a permutation passes: the mutant survived until the assertion pinned seat to rank).
- rows_foolish.c: the summary attributes written for a pointer with no summary -> the same test, count of "rank" in the second put == 0.
- rows_foolish.c: the replay batch always projected whole -> the same test, count of "moves" in the projected BatchGetItem == 0.
- rows_foolish.c: a pointer without a summary not marking the replay needed -> the same test, the batch carries "moves" and "player_ids".
- rows_foolish.c: cb_snapshot_query.ref ignoring the replay id / ignoring the game id / matching everything -> `snapshot_ref_matches_the_replay_or_its_game` (ids[0], counts, and a request past the script's end).

## Queue batch: cb_table_snap_summary and the stored pointer (table_ops.c, unit_app.c)

- table_ops.c: the duration rounding `+ 0.5` dropped -> `a_finished_game_writes_a_snapshot_and_ratings` timed.duration_s == want (3.9 s).
- table_ops.c: the no-times duration -1 made 0 -> the same test, m.duration_s == -1.
- table_ops.c: the elimination order not copied / the fool not copied / `has` not set -> the same test (m.elim[i], m.fool, m.has).
- table_ops.c: `x.n_gaps > 0` made `>= 0` -> the same test, a names-only blob must give -1 (got 0).
- rows_foolish.h: cb_snapshot_rank returning `i + 2` -> `history_pointers_carry_the_summary` (seat 0 rank 2 pinned) (unit_app's two-player rank check runs through the mem store's own rank).

## Queue batch: the history rows over a real replay (v1_http.c, table_ops.c, unit_v1.c)

A REAL BUG came out of the first run of these tests: history_fn and cb_table_snap_summary read the STORED extras (the encoded version-2 bytes) with replay_extras_unpack, which reads the decoder's ANSWER blob, so every stored blob was refused: no row ever had names or a duration. Both now read through cb_table_extras_read (replay_extras_decode with the seat and move counts, then unpack), the one reader.

- v1_http.c: the names read through the old replay_extras_unpack -> `history_rows_from_a_real_replay` hr->n_names == 2.
- table_ops.c: the duration read through the old replay_extras_unpack -> the same test (hr->duration_s == 0) and `a_finished_game_writes_a_snapshot_and_ratings` (m.duration_s == 0).
- v1_http.c: no cut for a pointer without a summary -> `history_rows_from_a_real_replay` hr->rank == rank_of_fay.
- v1_http.c: the pre-summary rank not computed from the viewer's seat -> the same assertion.
- v1_http.c: /v1/replay/<id> id length capped at a game id's (path_id) -> `bots_board_history_replay` call(b_replay, s.id) == 200 (found by this test run: a history row's id, a uuid, was a 404).
- v1_http.c: `x.n_names == s->n_players` and `<= MAX_PLAYERS` -> deleted, not mutated: the decode reads exactly n_players names, so both were redundant guards (the mutant survived).
- smoke_matrix.c `account_deletion`: the list's names read through the old unpack -> `history_holds(&A, &name) == 1` (the positive control: the list shows E's name before the delete; after it, no history body names E).
- smoke.c: the history body holds no moves (a row has no moves field: the shape enforces it; the test pins that the first 16 bytes of the replay are not in the body) and a row's id opens the replay (a uuid) as the game id does.

## Queue batch: the store conformance suite (core/test/conformance/store.c, unit_conf_store_local.c, unit_conf_store_dynamo.c)

Cherry-picked from origin/stage8-owed-tests (2bf3960ce with f316ad639's Makefile line); the dynamo runner is on `make check-dynamo` outside the key capture (its keys are the suite's own).
- core/src/store/items.c: `n_cond > CB_ICOND_MAX` unchecked (the old cond[4] read past the array on n_cond 5) -> `four_clauses_all_must_hold` write_once == CB_E_INVALID (got -3).
- store_sqlite.c: cond_holds stops at the third clause -> `four_clauses_all_must_hold` "one failing clause of four refuses".
- store_sqlite.c: a keys-only query answers every attribute -> `query_the_table` it->n == 2 (got 4).
- store_dynamo.c: the batch-get key buffers 400 bytes (a 1024-byte pk dropped) -> `key_limits` b.calls == 1 (DynamoDB Local).
- store_dynamo.c: the key walk's buffers 400 bytes -> `key_limits` k.wanted == 1 (DynamoDB Local).

## Queue batch: jobs, schedules and the job pool (cherry-picked from origin/stage8-owed-tests: unit_jobs.c, unit_job_pool.c, unit_event_literal.c, unit_core.c)

- jobs.c: a job name allowing A-Z / the name max + 1 / an empty key allowed -> `names_and_keys` (name_bad == 0, !cb_job_name_ok(.., MAX + 1), !cb_job_key_ok(s, 0)).
- jobs.c: cb_job_start with no starter answering CB_E_INVALID instead of CB_E_STATE -> `job_start_checks_before_the_starter`.
- jobs.c: an unknown job answering CB_OK / the schedule total not summed -> `job_run_by_name` (== CB_E_NOT_FOUND), `schedule_run_by_name` (== 9).
- config.c: cb_config_scaled_ms rounding down -> `scaled_periods` (== 8572). The `bot_speedup > 0` variant SURVIVES, equivalent: a speedup of 1 divides by 1 either way and 0 is mapped to 1 in both.
- provider_local.c: the pending set never marking / unmarking after the run / no pool answering CB_OK -> `one_waiting_per_job_and_key`, `a_start_while_one_runs_queues_one_more`, `no_pool_is_a_state_error`.
- provider_local.c: no unmark when the pool refuses a submit -> SURVIVES, not reachable: cb_pool_submit refuses only while cb_pool_free is stopping the pool (a race with the caller), so no deterministic test can make it. cb_core_job replaced by cb_job_run in job_run -> SURVIVES, equivalent from outside (only a log line differs: cb_core_job turns a NOT_FOUND into a quiet success).
- jobs.c / aws_json.c: the job key and name caps + 1, the schedule name unchecked, the key unchecked, the closing quote unchecked, cb_event_job_payload unchecked -> `a_job_literal_at_the_edges`, `a_schedule_literal_at_the_edges`, `the_payload_is_what_the_decoder_reads`.
- core.c: a NOT_FOUND job left an error / every error swallowed / 5xx counted from 400 / the origin check dropped / the schedule entry ignoring its name -> `job_with_nothing_to_do_is_done`, `http_counts_requests_and_5xx`, `connect_checks_the_origin_before_the_token`.

## Queue batch: import keeps a session generation (rows_auth.c, unit_store.c import_never_lowers_a_session_generation)

- rows_auth.c: the import not reading the stored profile ("write session_gen 0 again") -> `import_never_lowers_a_session_generation` got.session_gen == 4 (a sign-out's generation lost).
- rows_auth.c: `>=` made `>` -> the same test, got.kept_session == "" (an equal generation must keep the stored kept session).
- rows_auth.c: the kept session not carried / the stored generation always winning / acct_writes not writing session_gen -> the same test (got.kept_session, got.session_gen == 9, got.session_gen == 3).
- sigv4.c: the key hashed from 64 bytes up (`>=`) -> `hmac_long_key_boundaries` sig == want (a 64-byte key is used as is); hashed from 66 up -> the test crashes (a 65-byte key overruns the 64-byte block: killed by the trap).

## Queue batch: the viewer address (net/viewer_addr.c, unit_viewer_addr.c, fz_viewer_addr.c, fz_viewer_key.c)

- viewer_addr.c: the port top 65536 / the port low bound 0 / a port of more than 5 digits allowed / a bracket holding IPv4 / IPv6 refused / the bracket needing no colon after it / the first colon splitting the port / the output not cleared on entry -> `unit_viewer_addr` (what_it_refuses `!ok`, what_it_takes `ok`, `out[0] == 0`).
- viewer_addr.c: three guards DELETED because their mutants survived (each was redundant with the next check): `close == s + 1` (an empty bracket: inet_pton refuses ""), `close + 1 >= s + n` (close[1] is the NUL of the string), `pn == 0` (num_parse refuses an empty port), and the final `out[0] = 0` (the entry clears it).
- viewer_addr.c: the 63-byte cap is unobservable from outside (no valid address is longer than 51 bytes; it only sizes the local buffer): not mutation-checkable.
- fz_viewer_addr: the port top 65536 -> the corpus seed port-65536 aborts (FZ_CHECK port_ok).
- fz_viewer_key: the origin-secret test dropped from cb_viewer_key -> the seed serve-no-secret aborts; X-Forwarded-For read as a fallback -> the seed xff-only aborts. 20 s of libFuzzer on each target found nothing.

## Queue batch: the read share (store_dynamo.c cb_store_dynamo_read_share, unit_dynamo.c flow_bulk_read_share)

- store_dynamo.c: the share not applied to the rate -> `flow_bulk_read_share` fk_n == 1 (the fourth 1-unit read waits 200 ms at 2.5 RCU/s).
- store_dynamo.c: a share past 1 allowed / a share of 0 allowed / allowed outside bulk mode -> the same test's CB_E_INVALID assertions.
- store_dynamo.c: the explicit clamp of the balance to the new rate DELETED, not mutated: its mutant survived because the pacing's own refill caps the balance at the rate before every call.

## Queue batch: cbase-export (foolish/server/migrate/export.c, foolish/server/test/unit_export.c on `make check-dynamo`)

A REAL BUG came out of the first run: a file made new by the export is opened with the schema's 49 bots, so the "already holds items" refusal fired on every new file (exit 2 at the first run), and a bot the table did not have would have been left in the file as an EXTRA. A file absent or empty before the run is now cleared of its seed first; a file that held something before is still refused without --into-existing.
The test is a 1051-item table (all five attribute kinds, the index keys, the TTL pair, a 1024-byte pk, a 300 KB item, the schema's bots), run through the real build/cbase-export, every item read back out of the SQLite file and compared attribute by attribute with the table's. The table is made on demand (the export paces itself to a provisioned table's reads, 5 RCU/s here: four minutes for the same run).

- export.c: put without the index attributes -> `the_export_end_to_end` r.code == 0 and the per-item comparison.
- export.c: a value dropped on put (the BOOL attribute) -> the same assertions (the read-back compare reports it: exit 1).
- export.c: the verify pass without the sort -> r.code == 0 and "0 missing, 0 extra" (false MISSING/EXTRA).
- export.c: the verify pass skipping the EXTRA side -> r.code == 1 and "EXTRA in the file:     EXP#EXTRA / X".
- export.c: a non-empty file accepted without --into-existing -> r.code == 2 and "already holds".
- export.c: --rate range (0 to 1000) -> the usage assertions for 0, 101, x and -5 (on a path that does not exist: a first draft used the existing file, whose refusal also exits 2, and the mutant survived).
- export.c: the new file's seed not cleared -> the third export (a bot retired from the table) r.code == 0 and "0 missing, 0 extra".
- export.c: same_in_file always true -> SURVIVES alone: it is observable only with a store that stores wrong, and the put-side mutants above are caught by the per-item comparison and by exit codes before it matters.
- Not tested: a key vanishing between the scan and the read (it needs a delete between two calls inside one process): the code counts it ("keys were gone"), unexercised.
