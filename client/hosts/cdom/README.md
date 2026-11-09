# client/hosts/cdom: the cdom adapter, the C SDK in a browser app on cdom

`cbc_cdom` runs a cbase client (`cbase/client.h`) inside a C app built on [cdom](https://github.com/alexsave/cdom), in the app's own wasm module.
It is an adapter: the shared host (`client/hosts/common/cbc_host.h`) with cdom's socket, fetch, timers, clock, store and entropy as its environment, one cdom call each.
`cbc_cdom.h` holds the contract; this file says how to build and test it.

## A cdom page on a cbase server, from the two starters

This is the path from the two `make new` commands to one app whose page calls its server, worked through [example/minimal](../../../example/minimal).
Every block marked as quoted below is a run of lines from that app's files, held so by `quote_check.sh` here; the app itself is built, served and opened in Chrome by `example/minimal/check.sh` in CI.

### 1. The starters, and which cdom

```
make -C <cbase> new DIR=<scratch>/server
make -C <cdom> new DIR=<scratch>/page
```

Each writes a directory with a Makefile and one source: cbase's `src/server.c`, a server whose `GET /` answers `cbase`, and cdom's `src/app.c`, a page.
An app with both keeps the two sources (say `server/` and `page/`) and writes the one Makefile of part 2 in place of the two.
Take cdom at the pin this cbase is tested with ("The cdom pin", below), or a later commit of cdom's main.
A page that calls a server is opened at the URL `make serve` prints, never from disk: the adapter talks to the page's own http: or https: origin.

### 2. One Makefile, four includes

`example/minimal/Makefile` sets the server's lines first (`CBASE_PROVIDERS := local`, `APP_SRC_ROOTS := server`, `APP_SERVE := minimal`), then includes four fragments, each once and in this order:

<!-- quoted: example/minimal/Makefile -->
```make
include $(CBASE)/cbase.mk
include $(CDOM)/cdom.mk
include $(CBASE)/client/client.mk
include $(CBASE)/client/hosts/cdom/adapter.mk
```

`cbase.mk` comes first because `cdom.mk` turns on `.SECONDEXPANSION` for every rule read after it, and cbase's rules are tested without it.
`adapter.mk` comes last because it needs `cdom.mk`'s and `client.mk`'s variables, and stops, naming the fix, when either is missing.
The page's lines come after them (`APP_WASM`, `APP_SRC` with `$(CBC_SRC) $(CBC_B64_SRC) $(CBC_CDOM_SRC)`, `APP_MODULES := $(CBC_CDOM_MODULES)`, `APP_CFLAGS` with `$(CBC_INC) $(CBC_CDOM_CFLAGS)`, and `APP_PAGE`), since `cdom.mk`'s rules read them when make builds.

### 3. The page at `GET /`

The starter's `GET /` answers `cbase`; a page app takes `/` over for its page, here cdom's default page with the module bundled into it, one file:

<!-- quoted: example/minimal/server/minimal.c -->
```c
static void h_page(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_send_file(ctx, req, res, "build/page/index.html", NULL, "no-store");
}
```

Its route is `{ .method = "GET", .pattern = "/", .fn = h_page }`, beside `GET /api/hello`, the route the page calls.
`cb_send_file` (`core/src/net/files.h`) reads the file when asked, so a rebuilt page needs no restart.
A separate module is the better choice for a large app, for a host that caches a hashed `.wasm` for long, and for cdom's no-WebAssembly fallback: `APP_WASM_SEPARATE := 1` (`cdom.mk`'s header) keeps it a file beside the page, and the server answers it with a second `cb_send_file` route (its content type is `wasm`).

### 4. The call

<!-- quoted: example/minimal/page/page.c -->
```c
static void on_event(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes) {
  (void)ctx; (void)c;
  if (e->kind != CBC_EV_DONE) return;
  if (e->status == 200 && bytes) cd_set_text(answer, cd_tmp((const char *)bytes + e->off, e->len));
  else cd_set_text(answer, CD_LIT("the server did not answer"));
}
```

<!-- quoted: example/minimal/page/page.c -->
```c
  if (cbc_cdom_start(&client, &cfg, NULL, on_event, NULL, NULL) != 0) return;
  /* Idempotent (safe to send again) and fail fast (a dead server is reported at once). */
  cbc_call(cbc_cdom_client(&client), CBC_GET, CBC_C_IDEMPOTENT | CBC_C_FAIL_FAST, "/api/hello", 10, NULL, 0,
           cbc_cdom_now());
  cbc_cdom_pump(&client);   /* after every call into the client */
```

`cbc_cdom_start` runs once, on a handle in static storage, with a `cbc_cfg` from `cbc_cfg_default` whose `anonymous = 1` makes every call plain HTTP; `cbc_call` answers a ticket, and `cbc_cdom_pump` sends what the call queued.
Every ticket ends in exactly one `CBC_EV_DONE`, whose body is `e->len` bytes at `bytes + e->off`.
Status 0 is an answer that never arrived: no server is listening, or the deadline passed; with `CBC_C_FAIL_FAST` it is told at once instead of tried again.
A non-2xx answer's body is the server's error body, read the same way: `cb_res_text(res, 400, "bad name\n")` reaches the page as status 400 and those 9 bytes, with `e->why` read from the status (`client.h`, "WHAT A DONE'S SPAN HOLDS").

A multi-field answer needs no JSON: newline-separated `key=value` lines, written with `cb_res_text` and read in the page with plain C (a value never holds a newline, so one from user text is refused or encoded first):

<!-- compiled: server -->
```c
// GET /api/stats answers two fields, one key=value a line: "served=3\nversion=1.2\n".
static int served;   // CB_ROUTE_LOOP: only the loop thread touches it
static void h_stats(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char text[64];
    snprintf(text, sizeof text, "served=%d\nversion=%s\n", ++served, "1.2");
    cb_res_text(res, 200, text);
}
```

<!-- compiled: page -->
```c
/* The DONE's body, one key=value a line; field(key, key_len, value, value_len) is the page's own. */
static void read_fields(const char *p, size_t n) {
  while (n) {
    size_t line = 0, key = 0;
    while (line < n && p[line] != '\n') line++;
    while (key < line && p[key] != '=') key++;
    if (key < line) field(p, key, p + key + 1, line - key - 1);
    if (line < n) line++;   /* past the '\n' */
    p += line;
    n -= line;
  }
}
```

The page calls it from its event callback, `read_fields((const char *)bytes + e->off, e->len)`, as part 4's `on_event` shows the text.
For anything bigger, cbase's binary wire is the tool: `tools/structgen` generates the readers and writers of C structs, and `client/include/cbase/client.h` is what the client speaks with it.

### 5. User text in a query

`cbc_call` sends its path as given (the parameter is `encoded_path`), so the page builds a query with `cbc_query_add`, which percent-encodes each key and value as RFC 3986 asks (all but `A-Z a-z 0-9 - . _ ~`):

```c
/* name: n bytes the user typed */
char path[CBC_PATH_MAX] = "/api/hello";
size_t len = 10;
if (cbc_query_add(path, sizeof path, &len, "name", name, n) != 0) return;   /* CBC_E_FULL: too long */
cbc_call(cbc_cdom_client(&client), CBC_GET, CBC_C_IDEMPOTENT | CBC_C_FAIL_FAST, path, len, NULL, 0, cbc_cdom_now());
cbc_cdom_pump(&client);
```

The server reads the value back with `cb_http_query_find(req, "name", buf, sizeof buf)`, as the top README's `src/myapp.c` does; `tools/new_test.sh` builds that file and asks it for "Ada & Grace".
A raw value breaks on its first `&`, `=`, `+` or `#`, which the browser's `fetch` leaves alone; cdom's `cd_url_encode` is for the page's own address, not for this path.

### 6. The make targets cbase.mk owns, and `start`

`cbase.mk` owns `all`, `test`, `smoke`, `check`, `check-asan`, `test-asan`, `smoke-asan`, `coverage`, `bench`, `serve`, `clean` and the `fuzz` targets ([README.md](../../../README.md), "The targets cbase.mk owns"), and `cdom.mk` owns no target, only the files `APP_WASM` and `APP_PAGE` name.
An app's own work goes under a name of its own (`e2e`, not `test`), joined to cbase's targets by a line with no recipe.
A `start` target is `start: serve`, with the page as `serve`'s prerequisite (`serve: $(APP_WASM) $(APP_PAGE)`, as `example/minimal/Makefile` has it), never a copy of serve's recipe.

### 7. An end-to-end test in Chrome

cdom's `tools/cdp.mjs` is a small DevTools client: it starts a headless Chrome of its own (a temporary profile, killed by its PID) and drives it.
It is in the cdom the app builds with: its `cdom/` submodule, or `client/build/cdom` here once a cdom goal has fetched it.
It is a test-only dependency on Node 22 or later and Google Chrome (`CHROME` names another binary); the app and its build need neither.
`example/minimal/e2e.mjs` is one: `launch()`, `Page.navigate` to the URL `make serve` printed, then `evaluate` of the mount's text until it shows the server's answer, and `close()`.
`example/minimal/check.sh` runs it after its curl checks, while the server is up, and skips it, saying so, when Node or Chrome is missing (CI sets `MINIMAL_E2E=1`, which makes that a failure).

## The dependency

cbase depends on cdom here and nowhere else, and cdom never depends on cbase.
The repository gate (`tools/repo_gate_test.sh`) allows cdom's name only in this directory, in `client/test/unit_host_cdom.c`, and in the minimal app built on it, `example/minimal/`.
`client/Makefile` includes this directory's `host.mk`, CI runs it through `make check-hosts`, and the mutation check reads `mutants.py` here, all without naming cdom.

## The cdom pin

The adapter is built and tested against one cdom, pinned by commit in `host.mk` (`CDOM_PIN`), and fetched into `client/build/cdom` by the target itself.
That cdom is the one this cbase pairs with: an app on both takes cdom at the pin below, with cbase at the same commit as this file.
The pin is [35c93773](https://github.com/alexsave/cdom/commit/35c93773ef5e705cf3dbb05b94f135c0074f6afd), a cdom with `cdom.mk` at its root, included once (its sources and flags, which `adapter.mk` and `host.mk` take from it), and whose stable I/O contract (the rule at the top of `src/cdom.h`, held by `tests/link/contract.c`) declares every call the adapter makes, `cd_page_origin`, `cd_random_bytes` and `CD_WS_REFUSED` among them.
A new pin is that one line, moved once the adapter passes against the newer cdom, and the line above with it: `pin_doc_check.sh` (run by `make -C client check` and `check-host-cdom`) fails when this file's commit is not `CDOM_PIN`.
`CDOM=<a cdom checkout>` runs everything against that checkout instead.

## Build and test

`make -C client check-host-cdom` (or `make -C client check-hosts`, every adapter that needs its platform) runs:

- the shared host suite on this adapter (`client/test/unit_host_cdom.c` is cdom, as far as the adapter reaches it), natively and under ASan + UBSan, every case with socket ids from 1 and from 0 and on an http: and an https: page;
- the adapter's own cases: a stop and a new start, a stop from inside a callback, late answers dropped, the key, a page that is not http: or https:, a host past 256 bytes, a socket the browser refuses to make, the early call that waits for the load;
- the adapter, the shared host, the core and `lib/b64` compiled for wasm32 with `cdom.mk`'s flags (`CDOM_CFLAGS`: cdom's own `string.h`), the libc gate over them, and a module linked from them and `link_app.c` by `cdom.mk`'s `APP_WASM` rule, with the modules `CBC_CDOM_MODULES` names;
- the minimal app (`example/minimal/check.sh`, also `make -C client check-minimal-example`): its one Makefile builds the server and the page against this cdom, and `make serve`, run outside dev mode with no setting but a free port, answers the route over curl and serves cdom's default page with the linked module bundled into it, as one file, which headless Chrome then opens and shows the answer in (`e2e.mjs`, skipped and said without Node 22 or Chrome, a failure under CI's `MINIMAL_E2E=1`);
- `quote_check.sh` (also in `make -C client check`, nothing fetched): every block this file marks as quoted is a run of lines of the file it names;
- `compile_check.sh` (also in `make -C client check`, nothing fetched): the server block this file marks as compiled compiles with cbase's headers and warnings, and the page block's `key=value` reader is built and run over an answer it must split;
- `adapter_mk_check.sh` (also in `make -C client check`, nothing fetched): `adapter.mk` read without `cdom.mk` stops and names the fix, no file is in two of the lists an app adds (`lib/b64/b64.c` once), and every doc that shows the includes (the top README, this file, `adapter.mk`'s header, `example/minimal/Makefile`) shows them in the one order.

`client/test/mutate.sh` runs this adapter's mutants (`mutants.py`) with the rest of the client's.

## In an app's Makefile

cdom's sources and flags are cdom's own `cdom.mk` (at the cdom root), and this directory names only cbase's, so a file cdom renames is one edit, in cdom.
An app with a server and a page includes four fragments, each once and in this order, with `CBASE` and `CDOM` set to the two checkouts as its Makefile reaches them: cbase's `cbase.mk`, cdom's `cdom.mk`, `client/client.mk`, then `adapter.mk` here.
`cbase.mk` comes first because `cdom.mk` turns on `.SECONDEXPANSION` for every rule read after it, and cbase's rules are tested without it; `adapter.mk` comes last because it needs `cdom.mk`'s and `client.mk`'s variables, and stops, naming the fix, when either has not been read.
A page with no cbase server of its own leaves out `cbase.mk` and keeps the other three in the same order.
`example/minimal/Makefile` is the four, built and checked.

With `cdom.mk`'s `APP_WASM` rule, the app names the adapter's cdom modules in `APP_MODULES`; the rule reads `APP_*` when make builds, so they are set after the includes:

```make
CBASE := cbase
CDOM  := cdom
include $(CBASE)/cbase.mk
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
include $(CBASE)/cbase.mk
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
