# As built: realtime and PostgREST (src/realtime/)

Builder: realtime.
Branch: claude/c_base-realtime.

## What was built

- `phoenix.c`: both Phoenix serializers (vsn 2.0.0 arrays for realtime-js, 1.0.0 objects for supabase-swift; no vsn means 1.0.0, as Phoenix defaults), heartbeat, `phx_join`, `phx_leave` (ok, then `phx_close` with ref = join_ref, as Phoenix sends it), `access_token`, and the topic policy `cb_phx_may_join` mirroring the `realtime.messages` policies.
  A gu- topic is rebuilt from the caller's uid and compared whole, then membership is checked, as `20260917120000_realtime_channel_exact_topics.sql` does.
- `realtime.c`: the three publish calls build the frame once per serializer and hand both to the transport.
  Payloads are Supabase Realtime's own shapes, keys sorted as Jason writes small maps.
  postgres_changes carries `columns`, `commit_timestamp` ("...sssZ", which supabase-swift's date decoder needs), `errors:null`, `record` (not on DELETE), `old_record` with the primary key only (not on INSERT).
- `rt_rows.c`: timestamps as Postgres prints them in JSON (fraction without trailing zeros; "+00:00" only for TIMESTAMPTZ), and the chat row writer.
- `rt_local.c`: `GET /realtime/v1/websocket` (CB_ROUTE_LOOP | CB_ROUTE_APIKEY), refusing an unknown vsn with 400 and echoing `Sec-WebSocket-Protocol: phoenix` when offered (Chrome fails the handshake otherwise).
  Sockets and the connection list stay on the loop thread; each client frame runs on the request pool (a join reads membership from the store), one job per connection at a time so frames are answered in order; a publish posts its two frames to the loop.
- `rt_apigw.c`: `$connect` (apikey and vsn from the query, CONN row with a 2 h TTL), `$disconnect`, `$default` (CONN row in, `cb_phx_on_frame`, row out only when it changed or a third of the TTL has gone, so heartbeats cost no write).
  Replies and pushes go through `cb_apigw_post`; 410 deletes the row; a non-Phoenix frame deletes the connection; expired rows that the TTL sweep has not reached yet are skipped.
  A `$default` with no CONN row rebuilds one, taking the serializer from the frame's first byte.
  The endpoint is `CBASE_WS_ENDPOINT`, else `https://<domain>/<stage>` from the event (replies only; publishes need the setting).
- `postgrest.c`: GET for player_views, spectator_views, chat_messages, bots, user_elo_ratings, game_snapshots, and POST for chat_messages, with one generic filter/order/page/select pipeline after a per-table fetch that applies the RLS.

## Differences from the design, and why

- **Binding ids are a hash**, not per-connection state: `cb_phx_binding_id(event, schema, table, filter)` (FNV-1a, 31 bits, never 0).
  A publisher tags a pv- change with the ids of (its event, "*") x (the player filter, no filter), so neither transport stores anything per binding and the CONN row needed no new field.
- **`gt` is supported** in PostgREST filters: the leaderboard sends `games_played=gt.0` (Leaderboard.tsx), which the brief's eq-only list missed.
- **`access_token` is handled per channel, with no reply**, as Realtime does: a token that does not verify, the anon key, or another user's valid token closes that channel with `phx_close`.
  A join as a different user than the connection's closes the old user's channels first (one user per connection, since the CONN row has one `user_id`).
- **A join token equal to `CBASE_ANON_KEY` is "nobody"**, because supabase-js hands Realtime the anon key when signed out; every topic needs a user, so it is refused by policy ("Unauthorized: ..."), not as an invalid token.
- **A PostgREST bearer equal to the anon key, or to the request's own apikey header, is anon** (the second covers development with no anon key configured).
  Any other bearer that does not verify is 401 PGRST301.
- **Topics must carry "realtime:"**: only the bare topic is stored, and pushes go out as "realtime:<bare>"; both SDKs always prefix.
- **Unknown topic families are refused** (`user-`, `room:` and so on): the clients use only pv-, gu-, game- and chat:.
- **spectator_views and chat_messages need `game_id=eq.`**: the store has no "all games" scan, and no client reads either table without it; a read without it is 400 PGRST100 rather than a silently empty list.
- **Timestamps**: the brief said "ISO-8601 with offset"; seed.sql makes only the views' `updated_at` TIMESTAMPTZ, and every other time column is TIMESTAMP, which PostgREST prints without an offset. Both are printed as Postgres does.
- **user_elo_ratings.created_at is null**: `cb_rating_row` has no created_at. `previous_elo` of -1 (the store's "none") is printed as null; seed.sql's column is NOT NULL DEFAULT 1000. No client reads either.
- **The chat insert is all or nothing for validation** (every row is checked before any is written), but the writes themselves are sequential `chat_put` calls; a store failure halfway through a multi-row insert leaves the first rows. The clients insert one row.
- **Unknown keys in the chat insert body are ignored** (PostgREST answers PGRST204); the JSON reader has no key iteration.

## Header changes

- `phoenix.h` (mine): added `cb_phx_binding_id` and `cb_phx_vsn_of_query`, and documented the access_token, anon-key and binding-id rules.
- `postgrest.h` (mine): documented gt, offset, the bearer rules and the timestamp rule.
- New in my directory: `rt_deps.h` (the four calls into auth, table_ops and the API Gateway client, behind one table the tests swap; production never touches it) and `rt_rows.h` (timestamps, the chat row writer, the push payload builders the tests compare byte for byte).
- Nothing outside src/realtime was changed.

## Header changes I would like (outside my directory)

- `store.h` `cb_conn_row`: nothing needed.
- `net/ws.h`: a setter for a socket's user data (`cb_ws_set_userdata`) would let rt_local drop its linear connection lookup by id; at hundreds of sockets the scan is fine.
- `table_ops.h` `cb_view_row` for spectator_views: fine as is.

## Tests (all mutation-checked)

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

The runner (scratchpad `rt/rtmut.py`) deletes the object and the test binary before and after each mutation, because make compares whole seconds and a binary linked in the same second as the new object is not relinked; the first pass without that reported stale results.

## Untested

- Against the real clients: no browser or supabase-swift run yet (needs the app builder's table_ops and the auth builder's tokens). The shapes are taken from realtime-js 2.106.2, postgrest-js 2.106.2 and supabase-swift RealtimeV2 sources.
- rt_apigw against real API Gateway (`cb_apigw_post` is the aws builder's).
- The live dependency table with the real auth and table_ops: the tests use fakes through `rt_deps.h`. Once auth and app are merged, a test that mints a real JWT (`cb_jwt_mint`) and joins would close that gap.
- Under Lambda, `$default` with an `isBase64Encoded` body is event.c's to decode.

## Open defects

- None known in this module.
- Carried over on purpose: no catch-up after a missed push; chat `is_system` can be set by a client (the RLS never checked it); chat message length is unbounded but for the 1 MB body limit (TEXT has no limit either).
