# The cdom host

`cbc_cdom.h` and `cbc_cdom.c` are the C SDK's browser host: a cbase client (`cbase/client.h`) run by [cdom](https://github.com/alexsave/cdom) inside a C app's own wasm module.
`hosts/posix` does the same for a native app's event loop.
Only an app built on cdom compiles this host; nothing else in cbase includes it.

## The contract

The header's opening comment maps each line of `cbase/client.h`'s host contract onto one cdom call.
In short:

- The socket is `cd_ws_open_cb` on `ws://` or `wss://` plus the page's host and `cbc_stream_path`, with `cd_ws_send` and `cd_ws_close`.
- HTTP is `cd_http_cb` on the same origin, with `Authorization: Bearer ...` and `Content-Type: application/octet-stream` as header lines and `cfg.call_wait_ms` as the deadline.
- A borrowed body (`CBC_RQ_REF`, `cbc_call_ref`) is sent from `cbc_req_body`, the caller's own bytes: the host never copies it, and it never goes on the socket.
- One `cd_timer_cb` follows `cbc_due_ms`, and none is re-armed at 0 ms while the socket's close is on its way.
- Time is `cd_epoch_ms`, and the seed is `cd_random_bytes`.
- The session is kept in cdom's store (`cd_store_get_cb`, `cd_store_put_cb`, `cd_store_del_cb`) under a key the app names, as standard base64 with padding, the bytes the TS SDK writes.
- It keeps no global and defines no cdom hook, so the app keeps every hook for itself and may run several clients.

## Building it into an app

An app on cdom compiles, into its own module:

- cdom's `src/cdom.c`, `cdom_net.c`, `cdom_store.c`, `cdom_random.c` and `mini.c`;
- the core from `client/client.mk` (`CBC_SRC`, with `CBC_INC`);
- `lib/b64/b64.c`, which is already in `CBC_OIDC_SRC`, so a build with `CBC_OIDC` lists it once;
- `client/hosts/cdom/cbc_cdom.c`, with `-I<cdom>/src -I<cbase>/client/hosts/cdom`.

Every file is compiled `--target=wasm32 -ffreestanding -nostdlib -std=c11` with `-isystem <cdom>/src/libc`, cdom's program `string.h`.

## Testing it

`make -C client check-host-cdom` runs the shared host suite (`client/test/host/host_suite.h`) on this host through `client/test/unit_host_cdom.c`, natively and under ASan + UBSan.
It then compiles the host, the core and `lib/b64` for wasm32 against cdom's `string.h`, and runs the libc gate over them.
The adapter defines cdom's calls over the suite's world, and adds the rules cdom has that the world does not: the socket URL is on the page's host, a body carries its octet-stream type, the store holds base64 and answers later, and nothing is sent or opened while the session's load is out.
It also has one case of its own: a sign-up made before the load answers waits for it, then goes out.
`client/test/mutants.py` (`HOST_CDOM`) holds the host's mutants, which `client/test/mutate.sh` runs.

## The pinned cdom

The test needs cdom's headers, and the wasm32 compile needs cdom's `string.h`.
`CDOM_PIN` in `client/Makefile` names the cdom commit the host is proven against, and the target fetches exactly that commit into `client/build/cdom`.
`CDOM=<a cdom checkout>` runs the same targets against another cdom instead.
A new pin is that one line, moved once the host passes against the newer cdom.

## Decisions for cbase to look at

- **The session lives in cdom's store, IndexedDB, not in `localStorage`.**
  cdom's store is the `_cb` API cdom has, so the blob is the TS SDK's format but not in the TS SDK's place.
  A TS client's session on the same origin is not read by this host, nor the other way round.
  If a session should survive a move between the two SDKs on one origin, either the TS SDK moves to IndexedDB, or cdom grows a synchronous small key-value call and this host moves back to `localStorage`.
- **`cbc_cdom_start` takes a `ready(ctx, c)` callback, because the load answers later.**
  Until the store answers (a hit, a miss or `CD_STORE_DEADLINE_MS`), the pump sends and opens nothing, and the app's calls wait in the client.
  At `ready` the app reads `cbc_user_of(c)->signed_in` and signs in if it must.
  The posix host loads synchronously and has no such callback.
- **One platform-free host core, shared with posix, is the recommended next step.**
  This host and `hosts/posix` are the same logic over different I/O: the pump, the pending table, and the socket and timer bookkeeping are written twice, and the 0 ms spin lived in both.
  A `cbc_host.c` over an io table with an asynchronous load, with posix and cdom as thin io mappings, would give that logic one owner.
  `cbc_posix.c` cannot be reused as it is, since it includes `time.h`, `unistd.h` and `sys/random.h`, none of which a cdom module has.

## What cdom lacks for it

- A socket URL relative to the page: the host builds `ws://` or `wss://` and the page's host from `location.protocol` and `location.host`.
- A synchronous small key-value store, which sharing a session with the TS SDK would take.

## The browser drive

The host was also driven in headless Chrome against `cbase serve` (v0.1.0) and cdom, from cdom's side: sign-up, HELLO and READY, socket calls, PING and PONG, the reload restoring the session from cdom's store, and an anonymous listener on `/ws/public`.
That drive is not in cbase yet.
It needs the example app to have an `account_domain` and a room family anyone may hear, and those are cbase's to decide.
