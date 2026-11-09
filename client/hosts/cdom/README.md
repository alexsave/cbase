# client/hosts/cdom: the cdom adapter, the C SDK in a browser app on cdom

`cbc_cdom` runs a cbase client (`cbase/client.h`) inside a C app built on [cdom](https://github.com/alexsave/cdom), in the app's own wasm module.
It is an adapter: the shared host (`client/hosts/common/cbc_host.h`) with cdom's socket, fetch, timers, clock, store and entropy as its environment, one cdom call each.
`cbc_cdom.h` holds the contract; this file says how to build and test it.

## The dependency

cbase depends on cdom here and nowhere else, and cdom never depends on cbase.
The repository gate (`tools/repo_gate_test.sh`) allows cdom's name only in this directory and in `client/test/unit_host_cdom.c`.
`client/Makefile` includes this directory's `host.mk`, CI runs it through `make check-hosts`, and the mutation check reads `mutants.py` here, all without naming cdom.

## The cdom pin

The adapter is built and tested against one cdom, pinned by commit in `host.mk` (`CDOM_PIN`), and fetched into `client/build/cdom` by the target itself.
The pin is [3d7e4c3d](https://github.com/alexsave/cdom/commit/3d7e4c3df4c449c8920de513b5caa0af854d6a2d), the commit where cdom declares its I/O calls a stable contract (the rule at the top of `src/cdom.h`, held by `tests/link/contract.c`).
A new pin is that one line, moved once the adapter passes against the newer cdom.
`CDOM=<a cdom checkout>` runs everything against that checkout instead.

## Build and test

`make -C client check-host-cdom` (or `make -C client check-hosts`, every adapter that needs its platform) runs:

- the shared host suite on this adapter (`client/test/unit_host_cdom.c` is cdom, as far as the adapter reaches it), natively and under ASan + UBSan, every case with socket ids from 1 and from 0 and on an http: and an https: page;
- the adapter's own cases: a stop and a new start, a stop from inside a callback, late answers dropped, the key, a page that is not http: or https:, the early call that waits for the load;
- the adapter, the shared host, the core and `lib/b64` compiled for wasm32 against cdom's `src/libc/string.h`, the libc gate over them, and a link with cdom's sources (`link_app.c`).

`client/test/mutate.sh` runs this adapter's mutants (`mutants.py`) with the rest of the client's.

An app compiles `CBC_SRC`, `CBC_HOST_SRC` and `lib/b64/b64.c` from `client/client.mk`, plus `cbc_cdom.c`, with `-isystem <cdom>/src/libc` and `-I<cdom>/src`, and links cdom's `cdom.c`, `cdom_net.c`, `cdom_store.c`, `cdom_random.c` and `mini.c`.

## The session

The session lives in cdom's store (IndexedDB) under a key the app names, as standard base64 with padding of `cbc_session_save`'s bytes.
That is what the TS SDK writes to localStorage (`btoa`), so the bytes read the same, but the two SDKs keep them in different places: a page that moves from one SDK to the other signs in again.
The key is copied into the handle, at most `CBC_CDOM_KEY_MAX` bytes; a longer one makes `cbc_cdom_start` return -1.
An anonymous client keeps no session.

## The ready callback

IndexedDB answers later, so with a key `cbc_cdom_start` holds the shared host until the store has answered (a hit, a miss, or `CD_STORE_DEADLINE_MS`).
The app's calls wait in the client meanwhile and go out after.
Then the adapter calls `ready(ctx, c)` once: the app's first moment to read `cbc_user_of(c)->signed_in` and sign in if it must.
Without a key, or for an anonymous client, `ready` is called before `cbc_cdom_start` returns.

## Stop and start again

`cbc_cdom_stop` is the shared host's `cbc_host_stop`: it cancels the timers, closes the socket, and nothing more is sent or told the app.
The app may call it from the loop or from inside its own callback (an event, `ready`); the call that callback came from then sends, opens and arms nothing after it.
`cbc_cdom_start` is from the loop only: a start from inside a callback traps.
cdom cannot cancel a request or a store get, so each sits in a slot of the handle stamped with the start's generation until cdom answers, and an older start's answer is dropped.
The handle must be zeroed before its first start (static storage is), and starting a live handle traps.
The handle lives for the page: never free it, stopped or not, and keep it static.
Every context the adapter hands cdom points into it, and cdom may call back after a stop, a request's answer up to its `wait_ms` later, or with no limit when `http_wait_ms` is 0.

## What cdom cannot say

A browser cannot say why a socket failed (the WebSocket error event carries nothing), so every failure reaches the app as a `CBC_EV_SOCKET_FAILED` with `CBC_SOCK_FAILED`, and the host suite runs this adapter with `no_sock_why`.
A page that is not http: or https: is the adapter's own check: `CBC_SOCK_UNSUPPORTED`, again after every reconnect wait, and no socket is made.
A WebSocket constructor that throws (a malformed URL, a page's policy) is not reported by cdom's `cd_ws_open_cb` at this pin, so the adapter cannot raise `CBC_SOCK_REFUSED`; on an http: or https: page with the page's own host, the URL the adapter builds is well formed.
