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

## Bench (bench/unit_bench.c)

- stats.c: the percentile's `- 1e-9` dropped -> `percentiles_of_one_to_hundred` bs_percentile(v, 100, 7) == 7 (7.000000000000001 rounds up to the 8th).
- stats.c: the rank truncated instead of rounded up -> `percentiles_round_up` s.p50 == 30.
- stats.c: no sort before the summary -> `percentiles_of_one_to_hundred` s.p50 == 50.
- stats.c: max read from the bottom -> `percentiles_of_one_to_hundred` s.max == 100.
- stats.c: the ms rounding `(a + 5) / 10` made `a / 10` -> `report_tables` the whole report (12.35 became 12.34).
- stats.c: the "-0.00" guard dropped -> `report_tables` the push_pv row of -3 us reads 0.00.
- stats.c: the 5 ms share off by ten -> `report_tables` the whole report.
- stats.c: a cpu row with trailing text accepted -> `run_file_round_trip` !bs_run_parse(JUNK).
- stats.c: the cpu table never printed -> `report_tables` the whole report.
