// unit_oom.c - every allocation a request makes, failed one at a time.
//
// test/oom.h replaces malloc, calloc, realloc and free in this program only.
// Each sweep runs one operation with its k-th allocation failing, for k = 0,
// 1, 2, ... until a run makes no failure, and holds every run to three rules:
//   * it returns: no crash, no hang (the ASan lane runs it unfailed, once);
//   * it answers as the server's fault, never as the caller's: a request
//     either succeeds or answers 5xx (a 401 would sign the user out, a 404
//     would tell them their game is gone);
//   * it leaks nothing: every block the armed thread allocated is freed once
//     the operation and the caches it may keep are let go.
//
// The operations are the routes a client calls, through cb_handle on a whole
// booted context (the in-RAM store), and boot itself.
#include "oom.h"
#include "t.h"

#include "app/app_internal.h"
#include "app/table_ops.h"
#include "auth/auth.h"
#include "boot.h"
#include "config.h"
#include "ctx.h"
#include "handler.h"
#include "net/http.h"
#include "util/clock.h"
#include "util/log.h"

#include "ask.h"
#include "client_table.h"
#include "netclient.h"
#include "view.h"
#include "netwire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECRET "unit-oom-secret-unit-oom-secret-unit-oom-secret"
#define ADMIN "unit-oom-admin-token"

static cb_ctx g_ctx;
static cb_config g_cfg;

static int boot(void) {
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&g_cfg, 0, sizeof g_cfg);
    snprintf(g_cfg.jwt_secret, sizeof g_cfg.jwt_secret, SECRET);
    snprintf(g_cfg.admin_token, sizeof g_cfg.admin_token, ADMIN);
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, ":memory:");
    snprintf(g_cfg.allowed_origins, sizeof g_cfg.allowed_origins, "*");
    snprintf(g_cfg.public_url, sizeof g_cfg.public_url, "http://127.0.0.1:1");
    snprintf(g_cfg.table, sizeof g_cfg.table, "cbase");
    g_cfg.jwt_ttl_s = 3600;
    g_cfg.max_body = 1 << 20;
    g_ctx.cfg = &g_cfg;
    g_ctx.mode = CB_MODE_SERVE;
    char err[256];
    return cb_boot(&g_ctx, err, sizeof err);
}

// ---- one request ---------------------------------------------------------------------------

typedef struct {
    const char *method;
    char        path[96];
    char        auth[NW_ACCESS_MAX + 16];
    const char *ctype;
    uint8_t     body[1024];
    size_t      body_len;
} rq;

static int g_status;
static uint8_t g_body[1 << 18];
static int g_len;

static void run(const rq *q) {
    char path[96];
    snprintf(path, sizeof path, "%s", q->path);
    char *qs = strchr(path, '?');
    if (qs) *qs++ = 0;
    cb_http_req r;
    memset(&r, 0, sizeof r);
    snprintf(r.method, sizeof r.method, "%s", q->method);
    r.path = path;
    r.query = qs ? qs : "";
    r.minor = 1;
    if (q->auth[0]) r.headers[r.n_headers++] = (cb_http_header){ "Authorization", q->auth };
    if (q->ctype) r.headers[r.n_headers++] = (cb_http_header){ "Content-Type", q->ctype };
    r.body = q->body;
    r.body_len = q->body_len;
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&g_ctx, &r, &res);
    g_status = res.status;
    g_len = res.body.len < sizeof g_body ? (int)res.body.len : 0;
    if (g_len) memcpy(g_body, res.body.p, (size_t)g_len);
    cb_res_free(&res);
}

// The k-th allocation of `q` fails, for every k the request makes. `ok` is the
// status of a request that had all its memory; `prep` (optional) runs before
// each attempt, unarmed (a fresh name for a sign-up, say).
// g_everywhere: the sweep reaches into the libraries too (oom_everywhere, glibc
// only), and leaks are not counted, since a library keeps what it caches.
static bool g_everywhere;

static void sweep(const char *what, rq *q, int ok, void (*prep)(rq *, int)) {
    // OOM_ONLY=<text>: only the sweeps whose name holds it (to chase one by hand).
    const char *only = getenv("OOM_ONLY");
    // The skipped ones still run once, unarmed, so what follows finds its state.
    if (only && *only && !strstr(what, only)) {
        if (prep) prep(q, 0);
        run(q);
        return;
    }
    int k = 0, failures = 0, wrong = 0, leaks = 0;
    for (;; k++) {
        if (prep) prep(q, k);
        oom_fail_after(k);
        oom_everywhere(g_everywhere);
        run(q);
        cb_row_cache_clear();   // the one thing a request may keep
        long live = g_everywhere ? 0 : oom_live();
        oom_everywhere(false);
        bool failed = oom_failed();
        oom_off();
        if (!failed) break;
        failures++;
        if (g_status != ok && g_status < 500) {
            if (wrong++ < 3) fprintf(stderr, "     %s: allocation %d failed, answered %d (not %d, not 5xx)\n", what, k, g_status, ok);
        }
        if (live) {
            if (leaks++ < 3) fprintf(stderr, "     %s: allocation %d failed, %ld blocks leaked\n", what, k, live);
        }
        if (k > 20000) { T_CHECK(!"the sweep did not end"); break; }
    }
    // The run with every allocation: the request's own answer, nothing kept.
    if (g_status != ok) fprintf(stderr, "     %s: unfailed run answered %d, want %d\n", what, g_status, ok);
    T_EQ_INT(g_status, ok);
    if (OOM_AVAILABLE && !g_everywhere) T_CHECK(failures > 0);
    if (wrong) fprintf(stderr, "     %s: %d of %d failed allocations answered as the caller's fault\n", what, wrong, failures);
    if (leaks) fprintf(stderr, "     %s: %d of %d failed allocations leaked\n", what, leaks, failures);
    T_EQ_INT(wrong, 0);
    T_EQ_INT(leaks, 0);
    printf("  oom %-28s %4d allocations failed one at a time\n", what, failures);
}

// ---- the scenario ---------------------------------------------------------------------------

static char g_auth_a[NW_ACCESS_MAX + 16], g_auth_b[NW_ACCESS_MAX + 16], g_refresh_a[NW_REFRESH_MAX + 1];
static char g_gid[16];

static size_t credentials(uint8_t *out, size_t cap, int kind, const char *name, const char *pw) {
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = (uint8_t)kind;
    q.name_len = (uint8_t)strlen(name);
    memcpy(q.name, name, q.name_len);
    q.password_len = (uint8_t)strlen(pw);
    memcpy(q.password, pw, q.password_len);
    int n = nw_request_encode(&q, out, (int)cap);
    return n > 0 ? (size_t)n : 0;
}

static void sign_up(const char *name, char *auth, char *refresh) {
    rq q = { .method = "POST", .path = "/v1/auth/signup", .ctype = "application/octet-stream" };
    q.body_len = credentials(q.body, sizeof q.body, NW_SIGNUP, name, "oom-pass-1");
    run(&q);
    T_EQ_INT(g_status, 200);
    static NwSession s;
    T_EQ_INT(nw_session_decode(g_body, g_len, &s), NW_OK);
    snprintf(auth, NW_ACCESS_MAX + 16, "Bearer %.*s", (int)s.access_len, s.access);
    if (refresh) snprintf(refresh, NW_REFRESH_MAX + 1, "%.*s", (int)s.refresh_len, s.refresh);
}

static void fresh_signup(rq *q, int k) {
    char name[32];
    snprintf(name, sizeof name, "OOM-%d", k);
    q->body_len = credentials(q->body, sizeof q->body, NW_SIGNUP, name, "oom-pass-1");
}

// Each refresh spends its token: the next attempt presents the new one.
static void fresh_refresh(rq *q, int k) {
    if (k > 0 && g_status == 200) {
        static NwSession s;
        if (nw_session_decode(g_body, g_len, &s) == NW_OK) snprintf(g_refresh_a, sizeof g_refresh_a, "%.*s", (int)s.refresh_len, s.refresh);
    }
    NwRequest r;
    memset(&r, 0, sizeof r);
    r.kind = NW_REFRESH;
    r.refresh_len = (uint8_t)strlen(g_refresh_a);
    memcpy(r.refresh, g_refresh_a, r.refresh_len);
    int n = nw_request_encode(&r, q->body, sizeof q->body);
    q->body_len = n > 0 ? (size_t)n : 0;
}

static void ask_body(rq *q, int kind, const char *gid) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = (uint8_t)kind;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    int n = ask_encode(&a, q->body, sizeof q->body);
    q->body_len = n > 0 ? (size_t)n : 0;
}

// A new lobby of A's for each attempt (a create commits, so a failed attempt
// may or may not have made one; either way the next asks afresh).
static void join_a_new_lobby(rq *q, int k) {
    rq c = { .method = "POST", .path = "/v1/create", .ctype = "application/octet-stream" };
    snprintf(c.auth, sizeof c.auth, "%s", g_auth_a);
    run(&c);
    T_EQ_INT(g_status, 200);
    EnvHeader h;
    (void)h;
    // The game id is the envelope's roster's; the client table reads it.
    static ClientSlot slot;
    static ClientTable t;
    client_init(&t, &slot);
    T_CHECK(client_adopt_envelope(&t, g_body, g_len) >= 0);
    char gid[16];
    snprintf(gid, sizeof gid, "%.*s", (int)t.gid_len, t.gid);
    ask_body(q, ASK_JOIN, gid);
}

static void routes_fail_cleanly(void) {
    T_EQ_INT(boot(), CB_OK);
    sign_up("OOM-A", g_auth_a, g_refresh_a);
    sign_up("OOM-B", g_auth_b, NULL);

    rq q;
    memset(&q, 0, sizeof q);
    q.method = "POST";
    snprintf(q.path, sizeof q.path, "/v1/auth/signup");
    q.ctype = "application/octet-stream";
    sweep("POST /v1/auth/signup", &q, 200, fresh_signup);

    memset(&q, 0, sizeof q);
    q.method = "POST";
    snprintf(q.path, sizeof q.path, "/v1/auth/signin");
    q.ctype = "application/octet-stream";
    q.body_len = credentials(q.body, sizeof q.body, NW_SIGNIN, "OOM-A", "oom-pass-1");
    sweep("POST /v1/auth/signin", &q, 200, NULL);

    memset(&q, 0, sizeof q);
    q.method = "POST";
    snprintf(q.path, sizeof q.path, "/v1/auth/refresh");
    q.ctype = "application/octet-stream";
    g_status = 0;
    sweep("POST /v1/auth/refresh", &q, 200, fresh_refresh);

    // A's game: create, B joins, both start.
    memset(&q, 0, sizeof q);
    q.method = "POST";
    snprintf(q.path, sizeof q.path, "/v1/create");
    snprintf(q.auth, sizeof q.auth, "%s", g_auth_a);
    q.ctype = "application/octet-stream";
    sweep("POST /v1/create", &q, 200, NULL);
    static ClientSlot slot;
    static ClientTable t;
    client_init(&t, &slot);
    T_CHECK(client_adopt_envelope(&t, g_body, g_len) >= 0);
    snprintf(g_gid, sizeof g_gid, "%.*s", (int)t.gid_len, t.gid);

    memset(&q, 0, sizeof q);
    q.method = "POST";
    snprintf(q.path, sizeof q.path, "/v1/meta");
    snprintf(q.auth, sizeof q.auth, "%s", g_auth_b);
    q.ctype = "application/octet-stream";
    sweep("POST /v1/meta (join)", &q, 200, join_a_new_lobby);

    ask_body(&q, ASK_JOIN, g_gid);
    run(&q);
    T_EQ_INT(g_status, 200);
    ask_body(&q, ASK_START, g_gid);
    run(&q);
    T_EQ_INT(g_status, 200);
    snprintf(q.auth, sizeof q.auth, "%s", g_auth_a);
    sweep("POST /v1/meta (start, deals)", &q, 200, NULL);
    ask_body(&q, ASK_BUMP, g_gid);
    sweep("POST /v1/meta (bump)", &q, 200, NULL);

    // The reads.
    static const struct { const char *path; int ok; } reads[] = {
        { "/v1/games", 200 }, { "/v1/games/%s", 200 }, { "/v1/chat/%s", 200 }, { "/v1/bots", 200 },
        { "/v1/leaderboard", 200 }, { "/v1/history", 200 }, { "/m/abc", 200 },
    };
    for (size_t i = 0; i < sizeof reads / sizeof reads[0]; i++) {
        memset(&q, 0, sizeof q);
        q.method = "GET";
        snprintf(q.path, sizeof q.path, reads[i].path, g_gid);
        snprintf(q.auth, sizeof q.auth, "%s", g_auth_a);
        sweep(q.path, &q, reads[i].ok, NULL);
    }

    // The same reads and a bump with every library's allocations failing too
    // (glibc): the token check's HMAC, sqlite's. Still the server's fault.
    g_everywhere = true;
    memset(&q, 0, sizeof q);
    q.method = "GET";
    snprintf(q.path, sizeof q.path, "/v1/games");
    snprintf(q.auth, sizeof q.auth, "%s", g_auth_a);
    sweep("GET /v1/games (everywhere)", &q, 200, NULL);
    memset(&q, 0, sizeof q);
    q.method = "POST";
    snprintf(q.path, sizeof q.path, "/v1/meta");
    snprintf(q.auth, sizeof q.auth, "%s", g_auth_a);
    q.ctype = "application/octet-stream";
    ask_body(&q, ASK_BUMP, g_gid);
    sweep("POST /v1/meta bump (everywhere)", &q, 200, NULL);
    g_everywhere = false;

    // A chat line, then a move.
    memset(&q, 0, sizeof q);
    q.method = "POST";
    snprintf(q.path, sizeof q.path, "/v1/chat/%s", g_gid);
    snprintf(q.auth, sizeof q.auth, "%s", g_auth_a);
    q.ctype = "application/octet-stream";
    NwRequest c;
    memset(&c, 0, sizeof c);
    c.kind = NW_CHAT_SEND;
    c.message_len = 2;
    memcpy(c.message, "hi", 2);
    q.body_len = (size_t)nw_request_encode(&c, q.body, sizeof q.body);
    sweep("POST /v1/chat/<id>", &q, 204, NULL);

    // The account goes last: B deletes theirs (a failed attempt leaves it, or
    // half of it, which a retry finishes: the delete is idempotent by design).
    memset(&q, 0, sizeof q);
    q.method = "POST";
    snprintf(q.path, sizeof q.path, "/v1/account/delete");
    snprintf(q.auth, sizeof q.auth, "%s", g_auth_b);
    q.ctype = "application/octet-stream";
    sweep("POST /v1/account/delete", &q, 204, NULL);

    // The console's reads.
    static const char *const admin[] = {
        "/admin/api/overview", "/admin/api/collections", "/admin/api/users", "/admin/api/bots", "/admin/api/logs",
        "/admin/api/config", "/admin/api/backups", "/admin/api/game?id=%s", "/admin/api/games", "/admin/api/browse?c=games",
    };
    for (size_t i = 0; i < sizeof admin / sizeof admin[0]; i++) {
        memset(&q, 0, sizeof q);
        q.method = "GET";
        snprintf(q.path, sizeof q.path, admin[i], g_gid);
        snprintf(q.auth, sizeof q.auth, "Bearer " ADMIN);
        sweep(q.path, &q, 200, NULL);
    }

    memset(&q, 0, sizeof q);
    q.method = "POST";
    snprintf(q.path, sizeof q.path, "/admin/api/query");
    snprintf(q.auth, sizeof q.auth, "Bearer " ADMIN);
    q.ctype = "application/json";
    const char *stmt = "{\"statement\":\"SELECT id, status FROM games\"}";
    memcpy(q.body, stmt, strlen(stmt));
    q.body_len = strlen(stmt);
    sweep("POST /admin/api/query", &q, 200, NULL);

    cb_unboot(&g_ctx);
}

// The seam itself: c_base's own objects reach it (cb_buf in util/str.c), it
// fails exactly the chosen allocation, and it sees a block left behind.
static void the_seam_reaches_cbase(void) {
    if (!OOM_AVAILABLE) { T_CHECK(1); return; }
    cb_buf b;
    cb_buf_init(&b);
    oom_fail_after(0);
    T_CHECK(!cb_buf_reserve(&b, 10));
    T_CHECK(oom_failed());
    T_EQ_INT(oom_count(), 1);
    oom_off();
    cb_buf_free(&b);
    cb_buf_init(&b);
    oom_fail_after(5);
    T_CHECK(cb_buf_reserve(&b, 10));
    T_EQ_INT(oom_live(), 1);   // the buffer, still held
    cb_buf_free(&b);
    T_EQ_INT(oom_live(), 0);
    T_CHECK(!oom_failed());
    oom_off();
}

// A token's check, with every allocation failing in turn, the libraries'
// included (oom_everywhere: the HMAC is OpenSSL's, which glibc lets the seam
// reach; on macOS OpenSSL's allocations are its own and this sweep fails
// nothing). A check that could not be made is the server's fault, never a
// token that does not verify: that answer signs the user out.
static void a_token_check_that_cannot_be_made_is_not_a_bad_token(void) {
    T_EQ_INT(boot(), CB_OK);
    char auth[NW_ACCESS_MAX + 16];
    sign_up("OOM-JWT", auth, NULL);
    const char *tok = auth + 7;
    int failures = 0, wrong = 0;
    oom_everywhere(true);
    for (int k = 0; k < 10000; k++) {
        cb_user u;
        oom_fail_after(k);
        int rc = cb_auth_verify_token(g_ctx.auth, tok, &u);
        bool failed = oom_failed();
        oom_off();
        if (!failed) { T_EQ_INT(rc, CB_OK); break; }
        failures++;
        if (rc == CB_E_AUTH && wrong++ < 3) fprintf(stderr, "     allocation %d of the token check failed: CB_E_AUTH\n", k);
    }
    oom_everywhere(false);
    T_EQ_INT(wrong, 0);
#if defined(__linux__) && OOM_AVAILABLE
    T_CHECK(failures > 0);   // glibc's interposition reaches the HMAC
#endif
    printf("  oom %-28s %4d allocations failed one at a time\n", "the token check", failures);
    cb_unboot(&g_ctx);
}

// Boot itself, its every allocation failed: an error, never a crash, nothing kept.
static void boot_fails_cleanly(void) {
    int k = 0, failures = 0;
    for (;; k++) {
        oom_fail_after(k);
        int rc = boot();
        cb_unboot(&g_ctx);
        long live = oom_live();
        bool failed = oom_failed();
        oom_off();
        if (!failed) { T_EQ_INT(rc, CB_OK); break; }
        failures++;
        if (live) fprintf(stderr, "     boot: allocation %d failed (rc %d), %ld blocks leaked\n", k, rc, live);
        T_EQ_INT(live, 0);
        if (k > 20000) break;
    }
    if (OOM_AVAILABLE && !g_everywhere) T_CHECK(failures > 0);
    printf("  oom %-28s %4d allocations failed one at a time\n", "cb_boot", failures);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);   // a crash under a sweep must not eat what ran before it
    cb_log_init(CB_LOG_ERROR + 1, 0);   // the failures log; nobody needs to read them here
    if (!OOM_AVAILABLE) printf("NOTE unit_oom: under a sanitizer the allocator is the sanitizer's; each sweep runs once, unfailed\n");
    T_RUN(the_seam_reaches_cbase);
    T_RUN(boot_fails_cleanly);
    T_RUN(a_token_check_that_cannot_be_made_is_not_a_bad_token);
    T_RUN(routes_fail_cleanly);
    return T_DONE();
}
