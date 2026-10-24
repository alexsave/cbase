// unit_auth_http.c - the platform's session routes (auth/auth_http.h), their codec
// (auth/auth_wire.h) and the platform's refusal (net/refuse.h), with no app but an account domain,
// against the in-RAM store (lib_auth_fakestore.c), every request through cb_handle. The bodies are
// written here byte by byte, in cbase/auth_wire.h's layout, so the codec is checked against the
// header and not against itself. The same routes under a real app and store are foolish's unit_v1.c,
// and before/after a change, tools/auth_route_diff.sh.
#include "t.h"
#include "cbase/app.h"
#include "cbase/provider.h"

#include "auth/auth.h"
#include "auth/auth_http.h"
#include "auth/auth_wire.h"
#include "handler.h"
#include "lib_auth_fakestore.h"
#include "net/refuse.h"
#include "net/router.h"
#include "rate_limits.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#include "../../../shared/c/le_bytes.h"

#include <stdlib.h>

static cb_config g_cfg;
static cb_ctx g_ctx;
static const cb_app APP = { .name = "unit-auth-http", .account_domain = "accounts.example" };
static const cb_app NO_DOMAIN = { .name = "unit-auth-http" };

static void setup_app(const cb_app *app) {
    memset(&g_cfg, 0, sizeof g_cfg);
    strcpy(g_cfg.jwt_secret, "unit-auth-http-secret-with-enough-bytes");
    strcpy(g_cfg.allowed_origins, "*");
    strcpy(g_cfg.public_url, "https://api.example/");
    g_cfg.jwt_ttl_s = 3600;
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = &cb_provider_local;
    g_ctx.app = app;
    g_ctx.store = auth_fakestore_new();
    T_EQ_INT(cb_auth_new(&g_ctx, &g_ctx.auth), CB_OK);
    g_ctx.router = cb_router_new();
    T_EQ_INT(cb_auth_routes(&g_ctx), CB_OK);
    cb_limits_reset();
}
static void setup(void) { setup_app(&APP); }

static void teardown(void) {
    cb_router_free(g_ctx.router);
    cb_auth_free(g_ctx.auth);
    auth_fakestore_free(g_ctx.store);
    memset(&g_ctx, 0, sizeof g_ctx);
}

// ---- one request ------------------------------------------------------------------------

typedef struct {
    int     status;
    uint8_t body[4096];
    size_t  len;
    char    headers[1024];
    bool    to_pool;
} answer;

static answer g_a;
static const char *g_peer;   // the socket's address (a limit key); NULL: none, not limited

static int send_req(const char *method, const char *path, const char *bearer, const uint8_t *body, size_t n) {
    cb_http_req r;
    memset(&r, 0, sizeof r);
    cb_strlcpy(r.method, method, sizeof r.method);
    r.path = path;
    r.query = "";
    r.minor = 1;
    char authz[CBW_ACCESS_MAX + 16];
    if (bearer) {
        snprintf(authz, sizeof authz, "Bearer %s", bearer);
        r.headers[r.n_headers++] = (cb_http_header){ "Authorization", authz };
    }
    if (g_peer) cb_strlcpy(r.peer, g_peer, sizeof r.peer);
    r.body = body;
    r.body_len = n;
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&g_ctx, &r, &res);
    memset(&g_a, 0, sizeof g_a);
    g_a.status = res.status;
    g_a.to_pool = res.to_pool;
    cb_strlcpy(g_a.headers, cb_buf_cstr(&res.headers), sizeof g_a.headers);
    g_a.len = res.body.len < sizeof g_a.body ? res.body.len : 0;
    if (g_a.len) memcpy(g_a.body, res.body.p, g_a.len);
    cb_res_free(&res);
    return g_a.status;
}

typedef struct {
    uint8_t b[600];
    size_t  n;
} body;
static void put8(body *b, uint8_t v) { b->b[b->n++] = v; }
static void put_str8(body *b, const char *s, size_t n) {
    put8(b, (uint8_t)n);
    memcpy(b->b + b->n, s, n);
    b->n += n;
}
static body creds_n(uint8_t kind, const char *name, size_t nn, const char *pw, size_t pn) {
    body b = { .n = 0 };
    put8(&b, CBW_V);
    put8(&b, kind);
    put_str8(&b, name, nn);
    put_str8(&b, pw, pn);
    return b;
}
static body creds(uint8_t kind, const char *name, const char *pw) { return creds_n(kind, name, strlen(name), pw, strlen(pw)); }
static body one_str(uint8_t kind, const char *s, size_t n) {
    body b = { .n = 0 };
    put8(&b, CBW_V);
    put8(&b, kind);
    put_str8(&b, s, n);
    return b;
}
static body scope(uint8_t sc) {
    body b = { .n = 0 };
    put8(&b, CBW_V);
    put8(&b, CBW_SIGNOUT);
    put8(&b, sc);
    return b;
}

static int post(const char *path, const char *bearer, body b) { return send_req("POST", path, bearer, b.b, b.n); }

// The last answer is exactly the refusal (why, code, detail 0).
static bool refused(int why, int code) {
    uint8_t want[CBW_REFUSAL_BYTES] = { CBW_REFUSAL_FORMAT, (uint8_t)why };
    le_put_u32(want + 2, (uint32_t)code);
    return g_a.len == CBW_REFUSAL_BYTES && memcmp(g_a.body, want, CBW_REFUSAL_BYTES) == 0 &&
           strstr(g_a.headers, "Cache-Control: no-store") && strstr(g_a.headers, "application/octet-stream");
}

// The last answer as a session, read field by field from the header's layout.
typedef struct {
    char     access[CBW_ACCESS_MAX + 1], refresh[CBW_REFRESH_MAX + 1], user_id[CBW_ID_MAX + 1], name[CBW_NAME_MAX + 1];
    uint64_t expires_at_ms;
} session;
static bool read_session(session *s) {
    memset(s, 0, sizeof *s);
    const uint8_t *p = g_a.body;
    size_t n = g_a.len, at = 4;
    if (g_a.status != 200 || n < 4 || p[0] != CBW_V || p[1] != CBW_SESSION) return false;
    size_t an = le_get_u16(p + 2);
    if (an > CBW_ACCESS_MAX || at + an + 1 > n) return false;
    memcpy(s->access, p + at, an);
    at += an;
    size_t rn = p[at++];
    if (rn > CBW_REFRESH_MAX || at + rn + 6 + 1 > n) return false;
    memcpy(s->refresh, p + at, rn);
    at += rn;
    s->expires_at_ms = le_get_u48(p + at);
    at += 6;
    size_t un = p[at++];
    if (un > CBW_ID_MAX || at + un + 1 > n) return false;
    memcpy(s->user_id, p + at, un);
    at += un;
    size_t nn = p[at++];
    if (nn > CBW_NAME_MAX || at + nn != n) return false;
    memcpy(s->name, p + at, nn);
    return strstr(g_a.headers, "Cache-Control: no-store") != NULL;
}

#define SU "/v1/auth/signup"
#define SI "/v1/auth/signin"
#define RF "/v1/auth/refresh"
#define SO "/v1/auth/signout"
#define PW "/v1/auth/password"

// ---- the routes ---------------------------------------------------------------------------

static void sign_up_in_refresh_out(void) {
    setup();
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "ANNA", "correct-horse-1")), 200);
    session a;
    T_CHECK(read_session(&a));
    T_EQ_STR(a.name, "ANNA");
    T_EQ_INT(strlen(a.user_id), 36);
    T_EQ_INT(strlen(a.refresh), 43);
    int64_t left = (int64_t)a.expires_at_ms - cb_now_ms();
    T_CHECK(left > 3500 * 1000 && left <= 3600 * 1000);
    // The account's email is the name's, under the app's domain.
    cb_user_row row;
    T_EQ_INT(cb_auth_user_row(g_ctx.auth, a.user_id, &row), CB_OK);
    char em[CB_EMAIL_MAX + 1];
    cb_auth_name_email("accounts.example", "ANNA", 4, em);
    T_EQ_STR(row.email, em);
    // The case step: "anna" is ANNA, for sign-up (taken) and sign-in.
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "anna", "correct-horse-2")), 409);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_EXISTS));
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "anna", "correct-horse-1")), 200);
    session b;
    T_CHECK(read_session(&b));
    T_EQ_STR(b.user_id, a.user_id);
    T_EQ_STR(b.name, "ANNA");
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "ANNA", "wrong-horse-1")), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_BAD_LOGIN));
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "\xff", "wrong-horse-1")), 400);   // not UTF-8: no account
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_BAD_LOGIN));
    // Refresh: a new pair in the same session; an unknown token is the session's end.
    T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, b.refresh, strlen(b.refresh))), 200);
    session c;
    T_CHECK(read_session(&c));
    T_EQ_STR(c.user_id, a.user_id);
    T_CHECK(strcmp(c.refresh, b.refresh) != 0);
    T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, "nope", 4)), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_BAD_REFRESH));
    // Sign out this session: its token stops at once; the first session's still works.
    T_EQ_INT(post(SO, c.access, scope(CBW_SCOPE_LOCAL)), 204);
    T_EQ_INT(g_a.len, 0);
    T_EQ_INT(post(SO, c.access, scope(CBW_SCOPE_LOCAL)), 401);
    T_CHECK(refused(CBW_WHY_AUTH, 0));
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "ANNA", "correct-horse-1")), 200);
    session d;
    T_CHECK(read_session(&d));
    T_EQ_INT(post(SO, a.access, scope(CBW_SCOPE_GLOBAL)), 204);
    T_EQ_INT(post(SO, a.access, scope(CBW_SCOPE_LOCAL)), 401);
    T_EQ_INT(post(SO, d.access, scope(CBW_SCOPE_LOCAL)), 401);   // global: every session, not only the caller's
    teardown();
}

static void password_change(void) {
    setup();
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "BEA", "correct-horse-1")), 200);
    session a, b;
    T_CHECK(read_session(&a));
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "BEA", "correct-horse-1")), 200);
    T_CHECK(read_session(&b));
    body w = creds(CBW_PASSWORD, "wrong-current-1", "correct-horse-9");
    T_EQ_INT(post(PW, a.access, w), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_BAD_LOGIN));
    T_EQ_INT(post(PW, a.access, creds(CBW_PASSWORD, "correct-horse-1", "correct-horse-1")), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_SAME_PASSWORD));
    T_EQ_INT(post(PW, a.access, creds(CBW_PASSWORD, "correct-horse-1", "short")), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_WEAK_PASSWORD));
    T_EQ_INT(post(PW, a.access, creds_n(CBW_PASSWORD, "correct-horse-1", 15, "correct\0horse-9", 15)), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));
    T_EQ_INT(post(PW, a.access, creds_n(CBW_PASSWORD, "correct\0horse-1", 15, "correct-horse-9", 15)), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));
    T_EQ_INT(post(PW, NULL, creds(CBW_PASSWORD, "correct-horse-1", "correct-horse-9")), 401);
    T_CHECK(refused(CBW_WHY_AUTH, 0));
    cb_limits_reset();   // five changes an hour per account (below): those above count
    T_EQ_INT(post(PW, a.access, creds(CBW_PASSWORD, "correct-horse-1", "correct-horse-9")), 204);
    // Every OTHER session ended; this one stands.
    T_EQ_INT(post(SO, b.access, scope(CBW_SCOPE_LOCAL)), 401);
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "BEA", "correct-horse-9")), 200);
    // A password change per account is limited (5 an hour, rate_limits.c), counted before the body's read.
    for (int i = 0; i < 4; i++) T_EQ_INT(post(PW, a.access, creds(CBW_PASSWORD, "x", "y")), 400);
    T_EQ_INT(post(PW, a.access, creds(CBW_PASSWORD, "x", "y")), 429);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_TOO_MANY));
    teardown();
}

static void sign_up_rules(void) {
    setup();
    struct { const char *name, *pw; int code; } cases[] = {
        { "%EVE", "correct-horse-1", CBW_ACCOUNT_E_BAD_NAME },
        { "", "correct-horse-1", CBW_ACCOUNT_E_BAD_NAME },
        { "\xff\xfe", "correct-horse-1", CBW_ACCOUNT_E_BAD_NAME },
        { "A\xd0\x90", "correct-horse-1", CBW_ACCOUNT_E_BAD_NAME },
        { "CORA", "short", CBW_ACCOUNT_E_WEAK_PASSWORD },
        { "CORA", "password123", CBW_ACCOUNT_E_WEAK_PASSWORD },
        { "CORALINE", "coraline", CBW_ACCOUNT_E_WEAK_PASSWORD },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cb_limits_reset();
        T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, cases[i].name, cases[i].pw)), 400);
        T_CHECK(refused(CBW_WHY_ACCOUNT, cases[i].code));
    }
    // 64 bytes of name: in the field, past the 32 characters.
    char longname[CBW_NAME_MAX];
    memset(longname, 'A', sizeof longname);
    T_EQ_INT(post(SU, NULL, creds_n(CBW_SIGNUP, longname, sizeof longname, "correct-horse-1", 15)), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_BAD_NAME));
    teardown();
}

static void malformed(void) {
    setup();
    const char *paths[] = { SU, SI, RF, SO, PW };
    for (size_t i = 0; i < 5; i++) {
        T_EQ_INT(send_req("POST", paths[i], NULL, NULL, 0), i >= 3 ? 401 : 400);   // the bearer routes ask who first
        T_CHECK(refused(i >= 3 ? CBW_WHY_AUTH : CBW_WHY_MALFORMED, 0));
    }
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNIN, "ANNA", "correct-horse-1")), 400);   // another route's kind
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNUP, "ANNA", "correct-horse-1")), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));
    body b = creds(CBW_SIGNUP, "ANNA", "correct-horse-1");
    b.b[0] = 2;   // another version
    T_EQ_INT(post(SU, NULL, b), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));
    T_EQ_INT(post(SU, NULL, creds_n(CBW_SIGNUP, "AN\0A", 4, "correct-horse-1", 15)), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));
    T_EQ_INT(post(SU, NULL, creds_n(CBW_SIGNUP, "ANNA", 4, "correct\0horse-1", 15)), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));
    T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, "a\0", 2)), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));
    // A route that is not one, and a method this route does not take: the router's.
    T_EQ_INT(send_req("GET", SI, NULL, NULL, 0), 405);
    T_EQ_INT(post("/v1/auth/nothing", NULL, creds(CBW_SIGNIN, "A", "b")), 404);
    // A signed-in caller's malformed sign-out.
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "ANNA", "correct-horse-1")), 200);
    session a;
    T_CHECK(read_session(&a));
    T_EQ_INT(post(SO, a.access, scope(7)), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));
    T_EQ_INT(post(PW, a.access, scope(0)), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));
    T_EQ_INT(post(SO, "not-a-token", scope(0)), 401);
    T_CHECK(refused(CBW_WHY_AUTH, 0));
    teardown();
}

static void rate_limits(void) {
    setup();
    g_peer = "198.51.100.7";
    int st = 0, i;
    for (i = 0; i < 10 && (st = post(SU, NULL, creds(CBW_SIGNUP, "%BAD", "correct-horse-1"))) != 429; i++) {}
    T_EQ_INT(st, 429);
    T_EQ_INT(i, 5);   // signup per address: 5
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_TOO_MANY));
    T_CHECK(strstr(g_a.headers, "Retry-After: ") != NULL);
    for (i = 0; i < 30 && (st = post(SI, NULL, creds(CBW_SIGNIN, "DORA", "wrong-wrong-1"))) != 429; i++) {}
    T_EQ_INT(i, 10);   // signin per account: 10 before the address's 20
    for (i = 0; i < 70 && (st = post(RF, NULL, one_str(CBW_REFRESH, "x", 1))) != 429; i++) {}
    T_EQ_INT(i, 60);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_TOO_MANY));
    g_peer = NULL;
    // The account's own lock (auth.h CB_AUTH_FAIL_*): past five wrong passwords it is 429 whatever
    // the limits say.
    cb_limits_reset();
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "EVA", "correct-horse-1")), 200);
    for (i = 0; i < 6; i++) T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "EVA", "wrong-wrong-1")), 400);
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "EVA", "correct-horse-1")), 429);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_TOO_MANY));
    teardown();
}

// The name routes need the app's account domain; refresh does not.
static void no_account_domain(void) {
    setup_app(&NO_DOMAIN);
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "ANNA", "correct-horse-1")), 503);
    T_CHECK(refused(CBW_WHY_INTERNAL, 0));
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "ANNA", "correct-horse-1")), 503);
    T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, "x", 1)), 400);
    T_EQ_INT(send_req("POST", SU, NULL, NULL, 0), 503);   // before the body is read
    teardown();
    setup_app(NULL);
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "ANNA", "correct-horse-1")), 503);
    teardown();
    // No auth module at all (a server without a store): every route 503.
    setup();
    cb_auth_free(g_ctx.auth);
    g_ctx.auth = NULL;
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "ANNA", "correct-horse-1")), 503);
    T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, "x", 1)), 503);
    T_EQ_INT(post(SO, "t", scope(0)), 503);
    T_CHECK(refused(CBW_WHY_INTERNAL, 0));
    teardown();
}

// The store's failures, each as the server's answer.
static int g_fail_rc;
static int fail_by_email(cb_store *s, const char *email, cb_user_row *out) { return g_fail_rc; }
// The account gone between the token's check (its first read) and the route's own (the second).
static int g_user_gets;
static const cb_store_ops *g_real_ops;
static int gone_second(cb_store *s, const char *id, cb_user_row *out) {
    return ++g_user_gets >= 2 ? CB_E_NOT_FOUND : g_real_ops->user_get(s, id, out);
}
static void store_failures(void) {
    setup();
    cb_store_ops ops = *g_ctx.store->ops;
    const cb_store_ops *real = g_ctx.store->ops;
    ops.user_by_email = fail_by_email;
    g_ctx.store->ops = &ops;
    struct { int rc, status, why; } cases[] = {
        { CB_E_DB, 500, CBW_WHY_INTERNAL },
        { CB_E_BUSY, 503, CBW_WHY_BUSY },
        { CB_E_STATE, 503, CBW_WHY_INTERNAL },
    };
    for (size_t i = 0; i < 3; i++) {
        g_fail_rc = cases[i].rc;
        T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "ANNA", "correct-horse-1")), cases[i].status);
        T_CHECK(refused(cases[i].why, 0));
    }
    T_CHECK(strstr(g_a.headers, "Retry-After") == NULL);
    g_fail_rc = CB_E_BUSY;
    post(SI, NULL, creds(CBW_SIGNIN, "ANNA", "correct-horse-1"));
    T_CHECK(strstr(g_a.headers, "Retry-After: 1") != NULL);
    g_fail_rc = CB_E_WOULD_BLOCK;   // run on the loop: again on the pool
    post(SI, NULL, creds(CBW_SIGNIN, "ANNA", "correct-horse-1"));
    T_CHECK(g_a.to_pool);
    g_ctx.store->ops = real;
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "FAY", "correct-horse-1")), 200);
    session f;
    T_CHECK(read_session(&f));
    ops = *real;
    g_real_ops = real;
    ops.user_get = gone_second;
    g_user_gets = 0;
    g_ctx.store->ops = &ops;
    T_EQ_INT(post(PW, f.access, creds(CBW_PASSWORD, "correct-horse-1", "correct-horse-9")), 401);
    T_CHECK(refused(CBW_WHY_AUTH, 0));
    g_ctx.store->ops = real;
    teardown();
}

// ---- the pieces -----------------------------------------------------------------------------

static void name_email(void) {
    char e[CB_EMAIL_MAX + 1];
    T_EQ_INT(cb_auth_name_email("example.test", "ALICE", 5, e), 29);
    T_EQ_STR(e, "e7dcee3cc63d170b@example.test");   // hashlib.sha256(b"ALICE").hexdigest()[:16]
    T_EQ_INT(cb_auth_name_email(NULL, "ALICE", 5, e), -1);
    T_EQ_INT(cb_auth_name_email("", "ALICE", 5, e), -1);
    char big[CB_EMAIL_MAX + 8];
    memset(big, 'd', sizeof big - 1);
    big[sizeof big - 1] = 0;
    T_EQ_INT(cb_auth_name_email(big, "ALICE", 5, e), -1);
    T_EQ_STR(e, "");
    // The longest that fits: 16 + "@" + the domain is CB_EMAIL_MAX.
    big[CB_EMAIL_MAX - 17] = 0;
    T_EQ_INT(cb_auth_name_email(big, "ALICE", 5, e), CB_EMAIL_MAX);
    big[CB_EMAIL_MAX - 17] = 'd';
    big[CB_EMAIL_MAX - 16] = 0;
    T_EQ_INT(cb_auth_name_email(big, "ALICE", 5, e), -1);
}

static void refusal_bytes(void) {
    uint8_t b[CBW_REFUSAL_BYTES];
    T_EQ_INT(cb_refusal_encode(CBW_WHY_ACCOUNT, 9, -2, b), CBW_REFUSAL_BYTES);
    const uint8_t want[] = { 1, 8, 9, 0, 0, 0, 0xfe, 0xff, 0xff, 0xff };
    T_EQ_MEM(b, sizeof b, want, sizeof want);
    T_EQ_INT(cb_refusal_encode(0, 0, 0, b), -1);
    T_EQ_INT(cb_refusal_encode(256, 0, 0, b), -1);
    T_EQ_INT(cb_refusal_encode(-1, 0, 0, b), -1);
    T_EQ_INT(cb_refusal_encode(255, 0, 0, b), CBW_REFUSAL_BYTES);   // an app's own why passes
    cb_http_res res;
    cb_res_init(&res);
    cb_refuse_limited(&res, 0, false);
    T_EQ_INT(res.status, 429);
    T_CHECK(strstr(cb_buf_cstr(&res.headers), "Retry-After: 1\r\n") != NULL);
    T_EQ_INT(res.body.len, CBW_REFUSAL_BYTES);
    T_EQ_INT(res.body.p[1], CBW_WHY_BUSY);
    cb_res_free(&res);
    cb_res_init(&res);
    cb_refuse_limited(&res, 42, true);
    T_CHECK(strstr(cb_buf_cstr(&res.headers), "Retry-After: 42\r\n") != NULL);
    T_EQ_INT(res.body.p[1], CBW_WHY_ACCOUNT);
    T_EQ_INT(le_get_u32(res.body.p + 2), CBW_ACCOUNT_E_TOO_MANY);
    cb_res_free(&res);
    cb_res_init(&res);
    cb_refuse(&res, 418, 0, 0);   // why 0 is no refusal: an empty body
    T_EQ_INT(res.status, 418);
    T_EQ_INT(res.body.len, 0);
    cb_res_free(&res);
}

static void wire_decode(void) {
    cb_aw_request q;
    body b = creds(CBW_SIGNUP, "ANNA", "pw");
    T_EQ_INT(cb_aw_request_decode(b.b, b.n, CBW_SIGNUP, &q), 0);
    T_EQ_MEM(q.name, q.name_len, "ANNA", 4);
    T_EQ_MEM(q.password, q.password_len, "pw", 2);
    T_EQ_INT(cb_aw_request_decode(b.b, b.n, CBW_SIGNIN, &q), -1);   // another kind
    T_EQ_INT(q.name_len, 0);                                        // zeroed on a refusal
    T_EQ_INT(cb_aw_request_decode(b.b, b.n - 1, CBW_SIGNUP, &q), -1);
    b.b[b.n] = 0;
    T_EQ_INT(cb_aw_request_decode(b.b, b.n + 1, CBW_SIGNUP, &q), -1);
    T_EQ_INT(q.name_len + q.password_len, 0);   // read whole, then refused: nothing of it is left
    T_EQ_INT(cb_aw_request_decode(NULL, 5, CBW_SIGNUP, &q), -1);
    T_EQ_INT(cb_aw_request_decode(b.b, 1, CBW_SIGNUP, &q), -1);
    b = creds(CBW_PASSWORD, "cur", "new-one");
    T_EQ_INT(cb_aw_request_decode(b.b, b.n, CBW_PASSWORD, &q), 0);
    T_EQ_MEM(q.password, q.password_len, "cur", 3);
    T_EQ_MEM(q.new_password, q.new_password_len, "new-one", 7);
    // Each field's bound: at it decodes, one past it does not.
    char big[CBW_ACCESS_MAX];
    memset(big, 'x', sizeof big);
    b = creds_n(CBW_SIGNIN, big, CBW_NAME_MAX, big, CBW_PASSWORD_MAX);
    T_EQ_INT(cb_aw_request_decode(b.b, b.n, CBW_SIGNIN, &q), 0);
    b = creds_n(CBW_SIGNIN, big, CBW_NAME_MAX + 1, "p", 1);
    T_EQ_INT(cb_aw_request_decode(b.b, b.n, CBW_SIGNIN, &q), -1);
    b = creds_n(CBW_SIGNIN, "n", 1, big, CBW_PASSWORD_MAX + 1);
    T_EQ_INT(cb_aw_request_decode(b.b, b.n, CBW_SIGNIN, &q), -1);
    b = creds_n(CBW_PASSWORD, "c", 1, big, CBW_PASSWORD_MAX + 1);
    T_EQ_INT(cb_aw_request_decode(b.b, b.n, CBW_PASSWORD, &q), -1);
    b = one_str(CBW_REFRESH, big, CBW_REFRESH_MAX);
    T_EQ_INT(cb_aw_request_decode(b.b, b.n, CBW_REFRESH, &q), 0);
    T_EQ_INT(q.refresh_len, CBW_REFRESH_MAX);
    b = one_str(CBW_REFRESH, big, CBW_REFRESH_MAX + 1);
    T_EQ_INT(cb_aw_request_decode(b.b, b.n, CBW_REFRESH, &q), -1);
    // A str8 that says more than the body holds.
    const uint8_t over[] = { CBW_V, CBW_REFRESH, 9, 'a' };
    T_EQ_INT(cb_aw_request_decode(over, sizeof over, CBW_REFRESH, &q), -1);
    b = scope(CBW_SCOPE_GLOBAL);
    T_EQ_INT(cb_aw_request_decode(b.b, b.n, CBW_SIGNOUT, &q), 0);
    T_EQ_INT(q.scope, CBW_SCOPE_GLOBAL);
    b = scope(2);
    T_EQ_INT(cb_aw_request_decode(b.b, b.n, CBW_SIGNOUT, &q), -1);
    const uint8_t sess[] = { CBW_V, CBW_SESSION };   // not a request kind
    T_EQ_INT(cb_aw_request_decode(sess, sizeof sess, CBW_SESSION, &q), -1);
}

static void wire_session(void) {
    uint8_t out[CB_AW_SESSION_MAX];
    int n = cb_aw_session_encode("AT", 2, "RT", 2, 0x010203040506ull, "id", 2, "NM", 2, out, sizeof out);
    const uint8_t want[] = { CBW_V, CBW_SESSION, 2, 0, 'A', 'T', 2, 'R', 'T', 6, 5, 4, 3, 2, 1, 2, 'i', 'd', 2, 'N', 'M' };
    T_EQ_MEM(out, (size_t)n, want, sizeof want);
    T_EQ_INT(cb_aw_session_encode("AT", 2, "RT", 2, 1, "id", 2, "NM", 2, out, sizeof want - 1), -1);
    T_EQ_INT(cb_aw_session_encode("AT", 2, "RT", 2, 1, "id", 2, "NM", 2, NULL, 0), -1);
    char big[CBW_ACCESS_MAX + 1];
    memset(big, 'x', sizeof big);
    T_EQ_INT(cb_aw_session_encode(big, CBW_ACCESS_MAX, big, CBW_REFRESH_MAX, LE_U48_MAX, big, CBW_ID_MAX, big, CBW_NAME_MAX,
                                  out, sizeof out),
             CB_AW_SESSION_MAX);
    T_EQ_INT(cb_aw_session_encode(big, CBW_ACCESS_MAX + 1, "", 0, 0, "", 0, "", 0, out, sizeof out), -1);
    T_EQ_INT(cb_aw_session_encode("", 0, big, CBW_REFRESH_MAX + 1, 0, "", 0, "", 0, out, sizeof out), -1);
    T_EQ_INT(cb_aw_session_encode("", 0, "", 0, LE_U48_MAX + 1, "", 0, "", 0, out, sizeof out), -1);
    T_EQ_INT(cb_aw_session_encode("", 0, "", 0, 0, big, CBW_ID_MAX + 1, "", 0, out, sizeof out), -1);
    T_EQ_INT(cb_aw_session_encode("", 0, "", 0, 0, "", 0, big, CBW_NAME_MAX + 1, out, sizeof out), -1);
}

int main(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    T_RUN(sign_up_in_refresh_out);
    T_RUN(password_change);
    T_RUN(sign_up_rules);
    T_RUN(malformed);
    T_RUN(rate_limits);
    T_RUN(no_account_domain);
    T_RUN(store_failures);
    T_RUN(name_email);
    T_RUN(refusal_bytes);
    T_RUN(wire_decode);
    T_RUN(wire_session);
    return T_DONE();
}
