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
- `a_goods_only_commit_is_broadcast` (good_is_a_move): `goods_changed` dropped from the broadcast rule -> `g_fan.broadcast`.
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
