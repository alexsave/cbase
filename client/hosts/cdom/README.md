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
That cdom is the one this cbase pairs with: an app on both takes cdom at the pin below, with cbase at the same commit as this file.
The pin is [118604ef](https://github.com/alexsave/cdom/commit/118604efddc9123d71537356ac1643157aa1015c), a cdom with `cdom.mk` at its root, included once (its sources and flags, which `adapter.mk` and `host.mk` take from it), and whose stable I/O contract (the rule at the top of `src/cdom.h`, held by `tests/link/contract.c`) declares every call the adapter makes, `cd_page_origin`, `cd_random_bytes` and `CD_WS_REFUSED` among them.
A new pin is that one line, moved once the adapter passes against the newer cdom, and the line above with it: `pin_doc_check.sh` (run by `make -C client check` and `check-host-cdom`) fails when this file's commit is not `CDOM_PIN`.
`CDOM=<a cdom checkout>` runs everything against that checkout instead.

## Build and test

`make -C client check-host-cdom` (or `make -C client check-hosts`, every adapter that needs its platform) runs:

- the shared host suite on this adapter (`client/test/unit_host_cdom.c` is cdom, as far as the adapter reaches it), natively and under ASan + UBSan, every case with socket ids from 1 and from 0 and on an http: and an https: page;
- the adapter's own cases: a stop and a new start, a stop from inside a callback, late answers dropped, the key, a page that is not http: or https:, a host past 256 bytes, a socket the browser refuses to make, the early call that waits for the load;
- the adapter, the shared host, the core and `lib/b64` compiled for wasm32 with `cdom.mk`'s flags (`CDOM_CFLAGS`: cdom's own `string.h`), the libc gate over them, and a module linked from them and `link_app.c` by `cdom.mk`'s `APP_WASM` rule, with the modules `CBC_CDOM_MODULES` names;
- `adapter_mk_check.sh` (also in `make -C client check`, nothing fetched): `adapter.mk` read without `cdom.mk` stops and names the fix, and no file is in two of the lists an app adds (`lib/b64/b64.c` once).

`client/test/mutate.sh` runs this adapter's mutants (`mutants.py`) with the rest of the client's.

## In an app's Makefile

cdom's sources and flags are cdom's own `cdom.mk` (at the cdom root), and this directory names only cbase's, so a file cdom renames is one edit, in cdom.
An app includes `cdom.mk`, `client/client.mk` and then `adapter.mk` here, with `CBASE` and `CDOM` set to the two checkouts as its Makefile reaches them.
`adapter.mk` stops, naming the fix, when `cdom.mk` has not been included before it.

Each is included once, `cdom.mk` first.
With `cdom.mk`'s `APP_WASM` rule, the app names the adapter's cdom modules in `APP_MODULES`; the rule reads `APP_*` when make builds, so they are set after the includes:

```make
CBASE := cbase
CDOM  := cdom
include $(CDOM)/cdom.mk
include $(CBASE)/client/client.mk
include $(CBASE)/client/hosts/cdom/adapter.mk
APP_WASM    := ui/out/app.wasm
APP_SRC     := ui/src/app.c $(CBC_SRC) $(CBC_B64_SRC) $(CBC_CDOM_SRC)
APP_MODULES := $(CBC_CDOM_MODULES)
APP_CFLAGS  := $(CBC_INC) $(CBC_CDOM_CFLAGS)
```

With a rule of its own, the app takes cdom's sources and flags from `cdom.mk` by name:

```make
CBASE := cbase
CDOM  := cdom
include $(CDOM)/cdom.mk
include $(CBASE)/client/client.mk
include $(CBASE)/client/hosts/cdom/adapter.mk
UI_SRC    := ui/src/app.c $(CBC_SRC) $(CBC_B64_SRC) $(CBC_CDOM_SRC) \
             $(CDOM_SRC) $(CDOM_NET_SRC) $(CDOM_STORE_SRC) $(CDOM_RANDOM_SRC)
UI_CFLAGS := -Oz $(CDOM_CFLAGS) $(CBC_INC) $(CBC_CDOM_CFLAGS)
ui/out/app.wasm: $(UI_SRC)
	$(WASM_CC) $(UI_CFLAGS) $(CDOM_LDFLAGS) -o $@ $(UI_SRC)
```

`CBC_CDOM_SRC` is the adapter and the shared host (`CBC_HOST_SRC`).
`CBC_CDOM_CFLAGS` is cbase's include flags for it, the shared host's and this directory's; cdom's are in `CDOM_CFLAGS`.
`CBC_CDOM_MODULES` is the cdom modules the adapter calls, `net store random`, as `APP_MODULES` names them.
`lib/b64/b64.c`, which the adapter needs for the session, is `client.mk`'s `CBC_B64_SRC` and in no other list, so an app that also builds the provider sign-in adds `CBC_OIDC_SRC` and has nothing to dedupe.
`host.mk` builds and links the adapter from these same variables, through `cdom.mk`'s `APP_WASM` rule, so the list an app compiles is the one cbase tests.
The page's origin comes from `cd_page_origin` (cdom's net module).
Serving a page: the app's cbase server answers with the page and its wasm module through `core/src/net/files.h` (`cb_send_file` for one file, `cb_serve_dir` for a directory under a prefix route), whose content types include `wasm`.

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

A browser cannot say why a socket failed (the WebSocket error event carries nothing), so every failure of a socket it made reaches the app as a `CBC_EV_SOCKET_FAILED` with `CBC_SOCK_FAILED`, and the host suite runs this adapter with `no_sock_why`.
A page that is not http: or https: is the adapter's own check: `CBC_SOCK_UNSUPPORTED`, again after every reconnect wait, and no socket is made.
A socket the browser refuses to make (its WebSocket constructor throws: a URL it does not take, a page's policy where the browser refuses on the spot) is cdom's `CD_WS_REFUSED`, from the event loop and followed by its close, and reaches the app as one `CBC_EV_SOCKET_FAILED` with `CBC_SOCK_REFUSED`.
What a browser refuses later (a blocked port, a Content-Security-Policy rule in Chrome) is a socket that failed: `CBC_SOCK_FAILED`.
