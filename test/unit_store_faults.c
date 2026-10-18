// unit_store_faults.c - every store call a request makes, failed one at a time.
//
// test/lib_faultstore.c wraps the in-RAM store; each sweep runs one request
// with its k-th store call answering CB_E_DB (then CB_E_BUSY, a throttle past
// its backoff), for k = 0, 1, 2, ... until a run makes no store failure, and
// holds every run to two rules: it returns, and it answers as the server's
// fault (5xx; BUSY as 503 with Retry-After), never as the caller's (a 401
// signs a user out, a 404 says their game is gone, a 200 claims a write that
// did not happen). The unfailed run must answer as it always does.
#include "t.h"
#include "app/limits.h"

#include "app/app_internal.h"
#include "boot.h"
#include "config.h"
#include "handler.h"
#include "lib_faultstore.h"
#include "net/http.h"
#include "util/log.h"

#include "ask.h"
#include "client_table.h"
#include "netwire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECRET "unit-faults-secret-unit-faults-secret-unit"
#define ADMIN "unit-faults-admin"

static cb_ctx g_ctx;
static cb_config g_cfg;

static void boot(void) {
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&g_cfg, 0, sizeof g_cfg);
    snprintf(g_cfg.jwt_secret, sizeof g_cfg.jwt_secret, SECRET);
    snprintf(g_cfg.admin_token, sizeof g_cfg.admin_token, ADMIN);
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, ":memory:");
    snprintf(g_cfg.allowed_origins, sizeof g_cfg.allowed_origins, "*");
    snprintf(g_cfg.table, sizeof g_cfg.table, "cbase");
    g_cfg.jwt_ttl_s = 3600;
    g_ctx.cfg = &g_cfg;
    g_ctx.mode = CB_MODE_SERVE;
    char err[256];
    T_EQ_INT(cb_boot(&g_ctx, err, sizeof err), CB_OK);
    // The same store, behind the wrapper: every call the routes make goes through it.
    g_ctx.store = fs_wrap(g_ctx.store);
    // auth keeps its own pointer to the store it was made with.
    cb_auth_free(g_ctx.auth);
    T_EQ_INT(cb_auth_new(&g_ctx, &g_ctx.auth), CB_OK);
}

typedef struct {
    const char *method;
    char        path[96];
    char        auth[NW_ACCESS_MAX + 16];
    const char *ctype;
    uint8_t     body[1024];
    size_t      body_len;
} rq;

static int g_status;
static char g_retry[16];
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
    // A sweep repeats a request once per store call: the in-memory rate
    // limits (limits.h) are not what it measures.
    cb_limits_reset();
    cb_handle(&g_ctx, &r, &res);
    g_status = res.status;
    g_len = res.body.len < sizeof g_body ? (int)res.body.len : 0;
    if (g_len) memcpy(g_body, res.body.p, (size_t)g_len);
    const char *h = strstr(cb_buf_cstr(&res.headers), "Retry-After: ");
    snprintf(g_retry, sizeof g_retry, "%s", h ? h + 13 : "");
    cb_res_free(&res);
}

static int g_sweeps;

static void sweep_rc(const char *what, rq *q, int ok, void (*prep)(rq *, int), int rc) {
    int failures = 0, wrong = 0;
    for (int k = 0;; k++) {
        if (prep) prep(q, k);
        fs_fail_at(k, rc);
        run(q);
        bool failed = fs_failed();
        fs_off();
        cb_row_cache_clear();
        if (!failed) break;
        failures++;
        bool right = g_status >= 500 && (rc != CB_E_BUSY || g_status != 503 || g_retry[0] == '1');
        // The two public reads and the console may answer what they could read.
        if (!right && g_status == ok && q->method[0] == 'G') right = true;
        if (!right && wrong++ < 3)
            fprintf(stderr, "     %s: store call %d failed (%s), answered %d\n", what, k, rc == CB_E_BUSY ? "busy" : "db", g_status);
        if (k > 500) { T_CHECK(!"the sweep did not end"); break; }
    }
    if (g_status != ok) fprintf(stderr, "     %s: unfailed run answered %d, want %d\n", what, g_status, ok);
    T_EQ_INT(g_status, ok);
    T_CHECK(failures > 0);
    T_EQ_INT(wrong, 0);
    g_sweeps += failures;
    printf("  faults %-28s %3d store calls failed one at a time (%s)\n", what, failures, rc == CB_E_BUSY ? "busy" : "db");
}

static void sweep(const char *what, rq *q, int ok, void (*prep)(rq *, int)) {
    sweep_rc(what, q, ok, prep, CB_E_DB);
    sweep_rc(what, q, ok, prep, CB_E_BUSY);
}

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

static char g_auth[2][NW_ACCESS_MAX + 16], g_refresh[NW_REFRESH_MAX + 1], g_gid[16];

static void sign_up(const char *name, char *auth) {
    rq q = { .method = "POST", .path = "/v1/auth/signup", .ctype = "application/octet-stream" };
    q.body_len = credentials(q.body, sizeof q.body, NW_SIGNUP, name, "faults-pass");
    run(&q);
    T_EQ_INT(g_status, 200);
    static NwSession s;
    T_EQ_INT(nw_session_decode(g_body, g_len, &s), NW_OK);
    snprintf(auth, NW_ACCESS_MAX + 16, "Bearer %.*s", (int)s.access_len, s.access);
    snprintf(g_refresh, sizeof g_refresh, "%.*s", (int)s.refresh_len, s.refresh);
}

static void fresh_signup(rq *q, int k) {
    static int n;
    char name[32];
    snprintf(name, sizeof name, "FAULT-%d", n++);
    q->body_len = credentials(q->body, sizeof q->body, NW_SIGNUP, name, "faults-pass");
}

static void fresh_refresh(rq *q, int k) {
    if (g_status == 200) {
        static NwSession s;
        if (nw_session_decode(g_body, g_len, &s) == NW_OK) snprintf(g_refresh, sizeof g_refresh, "%.*s", (int)s.refresh_len, s.refresh);
    }
    NwRequest r;
    memset(&r, 0, sizeof r);
    r.kind = NW_REFRESH;
    r.refresh_len = (uint8_t)strlen(g_refresh);
    memcpy(r.refresh, g_refresh, r.refresh_len);
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

static void gid_of(char *gid, size_t cap) {
    static ClientSlot slot;
    static ClientTable t;
    client_init(&t, &slot);
    gid[0] = 0;
    if (client_adopt_envelope(&t, g_body, g_len) >= 0) snprintf(gid, cap, "%.*s", (int)t.gid_len, t.gid);
}

// A fresh lobby of the first user's for each attempt, the second asks to join it.
static void join_a_new_lobby(rq *q, int k) {
    rq c = { .method = "POST", .path = "/v1/create", .ctype = "application/octet-stream" };
    snprintf(c.auth, sizeof c.auth, "%s", g_auth[0]);
    run(&c);
    T_EQ_INT(g_status, 200);
    char gid[16];
    gid_of(gid, sizeof gid);
    ask_body(q, ASK_JOIN, gid);
}

// A fresh account for each delete.
static void fresh_account(rq *q, int k) {
    static int n;
    char name[32];
    snprintf(name, sizeof name, "GONE-%d", n++);
    sign_up(name, q->auth);
}

static void routes_fail_as_the_server(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    boot();
    sign_up("FAULT-A", g_auth[0]);
    sign_up("FAULT-B", g_auth[1]);
    rq q;
    memset(&q, 0, sizeof q);
    q.method = "POST";
    q.ctype = "application/octet-stream";
    snprintf(q.path, sizeof q.path, "/v1/auth/signup");
    sweep("POST /v1/auth/signup", &q, 200, fresh_signup);
    snprintf(q.path, sizeof q.path, "/v1/auth/signin");
    q.body_len = credentials(q.body, sizeof q.body, NW_SIGNIN, "FAULT-A", "faults-pass");
    sweep("POST /v1/auth/signin", &q, 200, NULL);
    snprintf(q.path, sizeof q.path, "/v1/auth/refresh");
    g_status = 0;
    sweep("POST /v1/auth/refresh", &q, 200, fresh_refresh);

    snprintf(q.path, sizeof q.path, "/v1/create");
    snprintf(q.auth, sizeof q.auth, "%s", g_auth[0]);
    q.body_len = 0;
    sweep("POST /v1/create", &q, 200, NULL);
    gid_of(g_gid, sizeof g_gid);
    snprintf(q.path, sizeof q.path, "/v1/meta");
    snprintf(q.auth, sizeof q.auth, "%s", g_auth[1]);
    sweep("POST /v1/meta (join)", &q, 200, join_a_new_lobby);
    ask_body(&q, ASK_JOIN, g_gid);
    run(&q);
    T_EQ_INT(g_status, 200);
    ask_body(&q, ASK_START, g_gid);
    run(&q);
    T_EQ_INT(g_status, 200);
    snprintf(q.auth, sizeof q.auth, "%s", g_auth[0]);
    sweep("POST /v1/meta (start, deals)", &q, 200, NULL);
    ask_body(&q, ASK_BUMP, g_gid);
    sweep("POST /v1/meta (bump)", &q, 200, NULL);

    snprintf(q.path, sizeof q.path, "/v1/chat/%s", g_gid);
    NwRequest c;
    memset(&c, 0, sizeof c);
    c.kind = NW_CHAT_SEND;
    c.message_len = 2;
    memcpy(c.message, "hi", 2);
    q.body_len = (size_t)nw_request_encode(&c, q.body, sizeof q.body);
    sweep("POST /v1/chat/<id>", &q, 204, NULL);

    static const struct { const char *path; int ok; } reads[] = {
        { "/v1/games", 200 }, { "/v1/games/%s", 200 }, { "/v1/chat/%s", 200 }, { "/v1/history", 200 },
        { "/v1/replay/%s", 404 }, { "/v1/bots", 200 }, { "/v1/leaderboard", 200 },
    };
    for (size_t i = 0; i < sizeof reads / sizeof reads[0]; i++) {
        memset(&q, 0, sizeof q);
        q.method = "GET";
        snprintf(q.path, sizeof q.path, reads[i].path, g_gid);
        snprintf(q.auth, sizeof q.auth, "%s", g_auth[0]);
        sweep(q.path, &q, reads[i].ok, NULL);
    }

    memset(&q, 0, sizeof q);
    q.method = "POST";
    q.ctype = "application/octet-stream";
    snprintf(q.path, sizeof q.path, "/v1/account/delete");
    sweep("POST /v1/account/delete", &q, 204, fresh_account);
    cb_unboot(&g_ctx);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    T_RUN(routes_fail_as_the_server);
    return T_DONE();
}
