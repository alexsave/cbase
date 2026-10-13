// unit_auth.c - accounts and sessions (auth/auth.h) against an in-RAM store
// (lib_auth_fakestore.c), then the GoTrue routes (auth/auth_http.h) through
// cb_handle exactly as both mains call it.
#include "t.h"

#include "auth/auth.h"
#include "auth/auth_http.h"
#include "auth/bcrypt.h"
#include "handler.h"
#include "lib_auth_fakestore.h"
#include "net/router.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/json.h"

#include <stdlib.h>

static const char ANON[] = "anon-key-for-tests";
static cb_config cfg;
static cb_ctx ctx;

static void setup(void) {
    memset(&cfg, 0, sizeof cfg);
    strcpy(cfg.jwt_secret, "a-unit-test-secret-with-enough-bytes");
    strcpy(cfg.anon_key, ANON);
    strcpy(cfg.allowed_origins, "*");
    strcpy(cfg.public_url, "https://api.example/");
    cfg.jwt_ttl_s = 3600;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.mode = CB_MODE_SERVE;
    ctx.store = auth_fakestore_new();
    T_EQ_INT(cb_auth_new(&ctx, &ctx.auth), CB_OK);
    ctx.router = cb_router_new();
    T_EQ_INT(cb_auth_routes(&ctx), CB_OK);
}

static void teardown(void) {
    cb_router_free(ctx.router);
    cb_auth_free(ctx.auth);
    auth_fakestore_free(ctx.store);
    memset(&ctx, 0, sizeof ctx);
}

// ---- the library ----------------------------------------------------------------

static void signup_then_signin(void) {
    setup();
    cb_session s;
    int64_t t0 = cb_now_ms();
    T_EQ_INT(cb_auth_signup(ctx.auth, "Anna@Foolish.Cards", "secret1", "ANNA", &s), CB_OK);
    T_EQ_STR(s.user.email, "anna@foolish.cards");
    T_EQ_STR(s.user.username, "ANNA");
    T_CHECK(cb_uuid_valid(s.user.id, strlen(s.user.id)));
    T_CHECK(cb_uuid_valid(s.user.session_id, strlen(s.user.session_id)));
    T_EQ_INT(s.expires_in, 3600);
    T_CHECK(s.expires_at >= t0 / 1000 + 3600 && s.expires_at <= cb_now_ms() / 1000 + 3600);
    T_EQ_INT(strlen(s.refresh_token), 43);
    T_CHECK(s.user_created_at >= t0 && s.last_sign_in_at >= t0);

    cb_user u;
    T_EQ_INT(cb_auth_verify_token(ctx.auth, s.access_token, &u), CB_OK);
    T_EQ_STR(u.id, s.user.id);
    T_EQ_STR(u.username, "ANNA");
    T_EQ_STR(u.session_id, s.user.session_id);

    // The stored hash is bcrypt at GoTrue's cost, and not the password.
    cb_user_row row;
    T_EQ_INT(cb_auth_user_row(ctx.auth, u.id, &row), CB_OK);
    T_CHECK(strncmp(row.password_hash, "$2b$10$", 7) == 0);
    T_EQ_INT(cb_bcrypt_verify("secret1", row.password_hash), 1);

    cb_session s2;
    T_EQ_INT(cb_auth_signin(ctx.auth, "ANNA@foolish.cards", "secret1", &s2), CB_OK);
    T_EQ_STR(s2.user.id, s.user.id);
    T_CHECK(strcmp(s2.user.session_id, s.user.session_id) != 0);   // a new session
    T_CHECK(strcmp(s2.refresh_token, s.refresh_token) != 0);
    T_EQ_INT(cb_auth_signin(ctx.auth, "anna@foolish.cards", "secret2", &s2), CB_AUTH_E_BAD_LOGIN);
    // An unknown email still spends a bcrypt verify, so a miss is not
    // measurably faster than a wrong password (cost 10 is ~60 ms here).
    int64_t m0 = cb_mono_ms();
    T_EQ_INT(cb_auth_signin(ctx.auth, "nobody@foolish.cards", "secret1", &s2), CB_AUTH_E_BAD_LOGIN);
    T_CHECK(cb_mono_ms() - m0 >= 10);
    T_EQ_INT(cb_auth_signin(ctx.auth, "not an email", "secret1", &s2), CB_AUTH_E_BAD_LOGIN);
    teardown();
}

static void signup_refusals(void) {
    setup();
    cb_session s;
    T_EQ_INT(cb_auth_signup(ctx.auth, "a@foolish.cards", "secret1", "ANNA", &s), CB_OK);
    T_EQ_INT(cb_auth_signup(ctx.auth, "A@FOOLISH.CARDS", "secret2", "ANNA2", &s), CB_AUTH_E_EXISTS);
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "12345", "B", &s), CB_AUTH_E_WEAK_PASSWORD);
    // Six characters, not six bytes: "\xc3\xa9" is one.
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", "B", &s),
             CB_AUTH_E_WEAK_PASSWORD);
    char long_pw[80];
    memset(long_pw, 'p', 73);
    long_pw[73] = 0;
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", long_pw, "B", &s), CB_AUTH_E_LONG_PASSWORD);
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "secret1", "%BOT", &s), CB_AUTH_E_BAD_USERNAME);
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "secret1", "MID%DLE", &s), CB_AUTH_E_BAD_USERNAME);
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "secret1", "", &s), CB_AUTH_E_BAD_USERNAME);
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "secret1", NULL, &s), CB_AUTH_E_BAD_USERNAME);
    const char *bad[] = { "", "nobody", "@foolish.cards", "b@", "b@cards", "b@.cards", "b@x.", "b c@x.y", "a@b@x.y" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        T_EQ_INT(cb_auth_signup(ctx.auth, bad[i], "secret1", "B", &s), CB_AUTH_E_BAD_EMAIL);
    // None of those created anything.
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "secret1", "B", &s), CB_OK);
    teardown();
}

static int64_t g_fake_now;
static int64_t fake_now(void) { return g_fake_now; }

static void refresh_rotates_and_reuse_revokes(void) {
    setup();
    g_fake_now = cb_now_ms();
    cb_auth_now = fake_now;
    cb_session s0, s1, s2, x;
    T_EQ_INT(cb_auth_signup(ctx.auth, "r@foolish.cards", "secret1", "R", &s0), CB_OK);
    T_EQ_INT(cb_auth_refresh(ctx.auth, s0.refresh_token, &s1), CB_OK);
    T_CHECK(strcmp(s1.refresh_token, s0.refresh_token) != 0);
    T_EQ_STR(s1.user.session_id, s0.user.session_id);   // same session, new pair
    T_EQ_STR(s1.user.id, s0.user.id);
    cb_user u;
    T_EQ_INT(cb_auth_verify_token(ctx.auth, s1.access_token, &u), CB_OK);
    T_EQ_INT(cb_auth_refresh(ctx.auth, s1.refresh_token, &s2), CB_OK);
    int revoked = 0;
    T_EQ_INT(auth_fakestore_session_tokens(ctx.store, s0.user.session_id, &revoked), 3);
    T_EQ_INT(revoked, 2);   // s0's and s1's consumed, s2's live

    // An unrelated session, to prove reuse revokes only its own.
    cb_session other, o1;
    T_EQ_INT(cb_auth_signin(ctx.auth, "r@foolish.cards", "secret1", &other), CB_OK);

    // s1 again within the grace: a retry of a lost response gets a sibling
    // pair in the same session, and nothing is revoked beyond s1 itself.
    g_fake_now += CB_AUTH_REUSE_GRACE_MS;
    cb_session g;
    T_EQ_INT(cb_auth_refresh(ctx.auth, s1.refresh_token, &g), CB_OK);
    T_EQ_STR(g.user.session_id, s0.user.session_id);
    T_CHECK(strcmp(g.refresh_token, s2.refresh_token) != 0);
    T_EQ_INT(auth_fakestore_session_tokens(ctx.store, s0.user.session_id, &revoked), 4);
    T_EQ_INT(revoked, 2);
    // The window is anchored at the FIRST use: a retry inside it did not
    // stretch it, so 1 ms later s1 is reuse, and the session dies, so even
    // the newest token fails.
    g_fake_now += 1;
    T_EQ_INT(cb_auth_refresh(ctx.auth, s1.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    T_EQ_INT(auth_fakestore_session_tokens(ctx.store, s0.user.session_id, &revoked), 4);
    T_EQ_INT(revoked, 4);
    T_EQ_INT(cb_auth_refresh(ctx.auth, s2.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    T_EQ_INT(cb_auth_refresh(ctx.auth, s0.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    T_EQ_INT(cb_auth_refresh(ctx.auth, other.refresh_token, &o1), CB_OK);

    // Unknown and malformed tokens.
    char fake[44];
    memset(fake, 'A', 43);
    fake[43] = 0;
    T_EQ_INT(cb_auth_refresh(ctx.auth, fake, &x), CB_AUTH_E_BAD_REFRESH);
    T_EQ_INT(cb_auth_refresh(ctx.auth, "short", &x), CB_AUTH_E_BAD_REFRESH);
    T_EQ_INT(cb_auth_refresh(ctx.auth, NULL, &x), CB_AUTH_E_BAD_REFRESH);
    cb_auth_now = cb_now_ms;
    teardown();
}

static void logout_password_delete(void) {
    setup();
    cb_session s, s2, x;
    cb_user u;
    T_EQ_INT(cb_auth_signup(ctx.auth, "l@foolish.cards", "secret1", "L", &s), CB_OK);
    T_EQ_INT(cb_auth_verify_token(ctx.auth, s.access_token, &u), CB_OK);
    // A second device: local logout ends only this session, global both.
    cb_session d2, d2b, d2c;
    T_EQ_INT(cb_auth_signin(ctx.auth, "l@foolish.cards", "secret1", &d2), CB_OK);
    T_EQ_INT(cb_auth_refresh(ctx.auth, d2.refresh_token, &d2b), CB_OK);
    T_EQ_INT(cb_auth_logout(ctx.auth, &u, CB_LOGOUT_LOCAL), CB_OK);
    T_EQ_INT(cb_auth_refresh(ctx.auth, s.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    T_EQ_INT(cb_auth_refresh(ctx.auth, d2b.refresh_token, &d2c), CB_OK);
    // d2b was rotated a moment ago, but a logout zeroes the grace: a retry of
    // it after the global logout is reuse, not a sibling.
    T_EQ_INT(cb_auth_logout(ctx.auth, &u, CB_LOGOUT_GLOBAL), CB_OK);
    T_EQ_INT(cb_auth_refresh(ctx.auth, d2c.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    T_EQ_INT(cb_auth_refresh(ctx.auth, d2b.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    // The access token lives until it expires, as GoTrue's did.
    T_EQ_INT(cb_auth_verify_token(ctx.auth, s.access_token, &u), CB_OK);

    T_EQ_INT(cb_auth_change_password(ctx.auth, &u, "secret1"), CB_AUTH_E_SAME_PASSWORD);
    T_EQ_INT(cb_auth_change_password(ctx.auth, &u, "short"), CB_AUTH_E_WEAK_PASSWORD);
    T_EQ_INT(cb_auth_change_password(ctx.auth, &u, "secret2"), CB_OK);
    T_EQ_INT(cb_auth_signin(ctx.auth, "l@foolish.cards", "secret1", &x), CB_AUTH_E_BAD_LOGIN);
    T_EQ_INT(cb_auth_signin(ctx.auth, "l@foolish.cards", "secret2", &s2), CB_OK);

    T_EQ_INT(cb_auth_delete_user(ctx.auth, u.id), CB_OK);
    T_EQ_INT(cb_auth_signin(ctx.auth, "l@foolish.cards", "secret2", &x), CB_AUTH_E_BAD_LOGIN);
    T_EQ_INT(cb_auth_refresh(ctx.auth, s2.refresh_token, &x), CB_AUTH_E_BAD_REFRESH);
    cb_user_row row;
    T_EQ_INT(cb_auth_user_row(ctx.auth, u.id, &row), CB_E_NOT_FOUND);
    T_EQ_INT(cb_auth_delete_user(ctx.auth, u.id), CB_E_NOT_FOUND);
    // The email is free again.
    T_EQ_INT(cb_auth_signup(ctx.auth, "l@foolish.cards", "secret3", "L", &x), CB_OK);
    teardown();
}

static void verify_bearer_and_expiry(void) {
    setup();
    cb_session s;
    T_EQ_INT(cb_auth_signup(ctx.auth, "v@foolish.cards", "secret1", "V", &s), CB_OK);
    char raw[4096];
    snprintf(raw, sizeof raw, "GET /x HTTP/1.1\r\nAuthorization: Bearer %s\r\n\r\n", s.access_token);
    cb_http_req req;
    T_CHECK(cb_http_parse(raw, strlen(raw), 1024, &req) > 0);
    cb_user u;
    T_EQ_INT(cb_auth_verify_bearer(ctx.auth, &req, &u), CB_OK);
    T_EQ_STR(u.id, s.user.id);
    char raw2[] = "GET /x HTTP/1.1\r\n\r\n";
    T_CHECK(cb_http_parse(raw2, strlen(raw2), 1024, &req) > 0);
    T_EQ_INT(cb_auth_verify_bearer(ctx.auth, &req, &u), CB_E_AUTH);

    // Expired an hour ago, signed with the right secret.
    cb_jwt_claims c;
    memset(&c, 0, sizeof c);
    memcpy(c.sub, s.user.id, sizeof c.sub);
    memcpy(c.session_id, s.user.session_id, sizeof c.session_id);
    c.iat = cb_now_ms() / 1000 - 7200;
    c.exp = c.iat + 3600;
    char tok[CB_JWT_MAX];
    T_CHECK(cb_jwt_mint(cfg.jwt_secret, &c, NULL, tok, sizeof tok) > 0);
    T_EQ_INT(cb_auth_verify_token(ctx.auth, tok, &u), CB_E_AUTH);
    // The same claims signed by another secret.
    c.exp = c.iat + 100000;
    T_CHECK(cb_jwt_mint("not-the-secret", &c, NULL, tok, sizeof tok) > 0);
    T_EQ_INT(cb_auth_verify_token(ctx.auth, tok, &u), CB_E_AUTH);
    teardown();
}

// ---- the routes, through cb_handle ------------------------------------------------

typedef struct {
    int status;
    char body[8192];
    size_t len;
    char headers[2048];
} reply;

// One request through cb_handle. `hdrs` are extra header lines ("A: b\r\n").
static reply call(const char *method, const char *target, const char *hdrs, const char *body) {
    static char raw[16384];
    snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: t\r\n%sContent-Length: %zu\r\n\r\n%s", method, target, hdrs,
             body ? strlen(body) : 0, body ? body : "");
    cb_http_req req;
    reply r;
    memset(&r, 0, sizeof r);
    if (cb_http_parse(raw, strlen(raw), 1 << 16, &req) <= 0) { r.status = -1; return r; }
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&ctx, &req, &res);
    r.status = res.status;
    r.len = res.body.len < sizeof r.body - 1 ? res.body.len : sizeof r.body - 1;
    if (r.len) memcpy(r.body, res.body.p, r.len);
    snprintf(r.headers, sizeof r.headers, "%s", cb_buf_cstr(&res.headers));
    cb_res_free(&res);
    return r;
}

#define APIKEY "apikey: anon-key-for-tests\r\n"
#define V2024  "X-Supabase-Api-Version: 2024-01-01\r\n"

static bool jstr(const reply *r, const char *path, char *out, size_t cap) {
    return cb_json_get_str(r->body, r->len, path, out, cap);
}
static bool jstr_is(const reply *r, const char *path, const char *want) {
    char v[2048];
    return jstr(r, path, v, sizeof v) && strcmp(v, want) == 0;
}
static int64_t jint(const reply *r, const char *path) {
    int64_t v = -1;
    return cb_json_get_i64(r->body, r->len, path, &v) ? v : -1;
}
// A GoTrue / supabase-swift date: "YYYY-MM-DDTHH:MM:SS.ffffffZ".
static bool jdate(const reply *r, const char *path) {
    char v[64];
    if (!jstr(r, path, v, sizeof v) || strlen(v) != 27 || v[10] != 'T' || v[19] != '.' || v[26] != 'Z') return false;
    int64_t ms;
    return cb_iso8601_parse(v, strlen(v), &ms) == CB_OK && ms > 0;
}

// Every field the two SDKs read from a session, present and well formed.
static void check_session_body(const reply *r, const char *email, const char *username) {
    T_EQ_INT(r->status, 200);
    T_CHECK(cb_json_valid(r->body, r->len));
    char at[CB_JWT_MAX], rt[64], id[64];
    T_CHECK(jstr(r, "access_token", at, sizeof at));
    T_CHECK(jstr_is(r, "token_type", "bearer"));
    T_EQ_INT(jint(r, "expires_in"), 3600);
    T_CHECK(jint(r, "expires_at") >= cb_now_ms() / 1000 + 3590);
    T_CHECK(jstr(r, "refresh_token", rt, sizeof rt) && strlen(rt) == 43);
    T_CHECK(jstr(r, "user.id", id, sizeof id) && cb_uuid_valid(id, strlen(id)));
    T_CHECK(jstr_is(r, "user.aud", "authenticated"));
    T_CHECK(jstr_is(r, "user.role", "authenticated"));
    T_CHECK(jstr_is(r, "user.email", email));
    T_CHECK(jstr_is(r, "user.phone", ""));
    T_CHECK(jdate(r, "user.email_confirmed_at"));
    T_CHECK(jdate(r, "user.confirmed_at"));
    T_CHECK(jdate(r, "user.last_sign_in_at"));
    T_CHECK(jdate(r, "user.created_at"));
    T_CHECK(jdate(r, "user.updated_at"));
    T_CHECK(jstr_is(r, "user.app_metadata.provider", "email"));
    T_CHECK(jstr_is(r, "user.app_metadata.providers.0", "email"));
    T_CHECK(jstr_is(r, "user.user_metadata.username", username));
    T_EQ_INT(cb_json_array_len(r->body, r->len, "user.identities"), 0);
    bool anon = true;
    T_CHECK(cb_json_get_bool(r->body, r->len, "user.is_anonymous", &anon) && !anon);
    cb_user u;
    T_EQ_INT(cb_auth_verify_token(ctx.auth, at, &u), CB_OK);
    T_EQ_STR(u.id, id);
}

// An error in the shape the request's API version asks for.
static void check_error(const reply *r, int status, const char *code, bool v2024) {
    T_EQ_INT(r->status, status);
    T_CHECK(cb_json_valid(r->body, r->len));
    char m[256];
    T_CHECK(jstr(r, "msg", m, sizeof m) && m[0]);
    if (v2024) {
        T_CHECK(jstr_is(r, "code", code));
        T_CHECK(strstr(r->headers, "X-Supabase-Api-Version: 2024-01-01\r\n") != NULL);
    } else {
        T_EQ_INT(jint(r, "code"), status);
        T_CHECK(jstr_is(r, "error_code", code));
    }
}

static void http_signup_signin_refresh(void) {
    setup();
    reply r = call("POST", "/auth/v1/signup", APIKEY V2024,
                   "{\"email\":\"Anna@Foolish.Cards\",\"password\":\"secret1\",\"data\":{\"username\":\"ANNA\"},"
                   "\"gotrue_meta_security\":{},\"code_challenge\":null,\"code_challenge_method\":null}");
    check_session_body(&r, "anna@foolish.cards", "ANNA");
    T_CHECK(strstr(r.headers, "Access-Control-Allow-Origin: *\r\n") != NULL);

    // The same email again, both error dialects.
    r = call("POST", "/auth/v1/signup", APIKEY V2024,
             "{\"email\":\"anna@foolish.cards\",\"password\":\"secret1\",\"data\":{\"username\":\"ANNA\"}}");
    check_error(&r, 422, "user_already_exists", true);
    r = call("POST", "/auth/v1/signup", APIKEY,
             "{\"email\":\"anna@foolish.cards\",\"password\":\"secret1\",\"data\":{\"username\":\"ANNA\"}}");
    check_error(&r, 422, "user_already_exists", false);

    // Password grant, right and wrong.
    r = call("POST", "/auth/v1/token?grant_type=password", APIKEY V2024,
             "{\"email\":\"anna@foolish.cards\",\"password\":\"secret1\",\"gotrue_meta_security\":{}}");
    check_session_body(&r, "anna@foolish.cards", "ANNA");
    char rt0[64];
    T_CHECK(jstr(&r, "refresh_token", rt0, sizeof rt0));
    reply bad = call("POST", "/auth/v1/token?grant_type=password", APIKEY V2024,
                     "{\"email\":\"anna@foolish.cards\",\"password\":\"secret2\"}");
    check_error(&bad, 400, "invalid_credentials", true);

    // Refresh: rotation, then the consumed token again.
    char body[256];
    snprintf(body, sizeof body, "{\"refresh_token\":\"%s\"}", rt0);
    r = call("POST", "/auth/v1/token?grant_type=refresh_token", APIKEY V2024, body);
    check_session_body(&r, "anna@foolish.cards", "ANNA");
    char rt1[64];
    T_CHECK(jstr(&r, "refresh_token", rt1, sizeof rt1));
    T_CHECK(strcmp(rt1, rt0) != 0);
    // A retry at once (its response lost): within the grace, a session.
    r = call("POST", "/auth/v1/token?grant_type=refresh_token", APIKEY V2024, body);
    check_session_body(&r, "anna@foolish.cards", "ANNA");
    // Past the grace: reuse.
    g_fake_now = cb_now_ms() + CB_AUTH_REUSE_GRACE_MS + 1000;
    cb_auth_now = fake_now;
    r = call("POST", "/auth/v1/token?grant_type=refresh_token", APIKEY V2024, body);
    check_error(&r, 400, "refresh_token_already_used", true);
    snprintf(body, sizeof body, "{\"refresh_token\":\"%s\"}", rt1);
    r = call("POST", "/auth/v1/token?grant_type=refresh_token", APIKEY V2024, body);
    check_error(&r, 400, "refresh_token_already_used", true);   // the reuse ended the session
    cb_auth_now = cb_now_ms;
    r = call("POST", "/auth/v1/token?grant_type=refresh_token", APIKEY V2024, "{\"refresh_token\":\"nope\"}");
    check_error(&r, 400, "refresh_token_not_found", true);

    r = call("POST", "/auth/v1/token?grant_type=magic", APIKEY V2024, "{}");
    check_error(&r, 400, "validation_failed", true);
    teardown();
}

static void http_refusals(void) {
    setup();
    const char *ok_body = "{\"email\":\"b@foolish.cards\",\"password\":\"secret1\",\"data\":{\"username\":\"B\"}}";
    // The anon key: missing, wrong, and in the query (Realtime's habit).
    reply r = call("POST", "/auth/v1/signup", V2024, ok_body);
    T_EQ_INT(r.status, 401);
    r = call("POST", "/auth/v1/signup", "apikey: anon-key-for-testz\r\n" V2024, ok_body);
    T_EQ_INT(r.status, 401);
    r = call("GET", "/auth/v1/user", "apikey: \r\n", NULL);
    T_EQ_INT(r.status, 401);

    r = call("POST", "/auth/v1/signup", APIKEY V2024,
             "{\"email\":\"b@foolish.cards\",\"password\":\"12345\",\"data\":{\"username\":\"B\"}}");
    check_error(&r, 422, "weak_password", true);
    T_CHECK(jstr_is(&r, "weak_password.reasons.0", "length"));
    r = call("POST", "/auth/v1/signup", APIKEY V2024,
             "{\"email\":\"b@foolish.cards\",\"password\":\"secret1\",\"data\":{\"username\":\"%B\"}}");
    check_error(&r, 422, "validation_failed", true);
    r = call("POST", "/auth/v1/signup", APIKEY V2024,
             "{\"email\":\"b-at-foolish\",\"password\":\"secret1\",\"data\":{\"username\":\"B\"}}");
    check_error(&r, 400, "email_address_invalid", true);
    r = call("POST", "/auth/v1/signup", APIKEY V2024, "{\"email\":\"b@foolish.cards\"");
    check_error(&r, 400, "bad_json", true);
    r = call("POST", "/auth/v1/signup", APIKEY V2024, "{\"email\":7,\"password\":\"secret1\"}");
    check_error(&r, 400, "validation_failed", true);
    r = call("POST", "/auth/v1/signup", APIKEY V2024, "{\"email\":\"b@foolish.cards\",\"data\":{\"username\":\"B\"}}");
    check_error(&r, 400, "validation_failed", true);

    // None of those created the account; this does.
    r = call("POST", "/auth/v1/signup?apikey=anon-key-for-tests", V2024, ok_body);
    check_session_body(&r, "b@foolish.cards", "B");
    teardown();
}

static void http_user_and_logout(void) {
    setup();
    reply r = call("POST", "/auth/v1/signup", APIKEY V2024,
                   "{\"email\":\"c@foolish.cards\",\"password\":\"secret1\",\"data\":{\"username\":\"CAT\"}}");
    T_EQ_INT(r.status, 200);
    char at[CB_JWT_MAX], rt[64], id[64], h[CB_JWT_MAX + 64];
    T_CHECK(jstr(&r, "access_token", at, sizeof at));
    T_CHECK(jstr(&r, "refresh_token", rt, sizeof rt));
    T_CHECK(jstr(&r, "user.id", id, sizeof id));
    snprintf(h, sizeof h, APIKEY V2024 "Authorization: Bearer %s\r\n", at);

    r = call("GET", "/auth/v1/user", h, NULL);
    T_EQ_INT(r.status, 200);
    T_CHECK(jstr_is(&r, "id", id));
    T_CHECK(jstr_is(&r, "email", "c@foolish.cards"));
    T_CHECK(jstr_is(&r, "user_metadata.username", "CAT"));
    T_CHECK(jdate(&r, "created_at") && jdate(&r, "updated_at"));

    r = call("GET", "/auth/v1/user", APIKEY V2024, NULL);
    check_error(&r, 401, "no_authorization", true);

    // An expired token, correctly signed.
    cb_jwt_claims c;
    memset(&c, 0, sizeof c);
    memcpy(c.sub, id, sizeof c.sub);
    cb_uuid_v4(c.session_id);
    c.iat = cb_now_ms() / 1000 - 4000;
    c.exp = c.iat + 3600;
    char old[CB_JWT_MAX], h2[CB_JWT_MAX + 64];
    T_CHECK(cb_jwt_mint(cfg.jwt_secret, &c, NULL, old, sizeof old) > 0);
    snprintf(h2, sizeof h2, APIKEY V2024 "Authorization: Bearer %s\r\n", old);
    r = call("GET", "/auth/v1/user", h2, NULL);
    check_error(&r, 403, "bad_jwt", true);

    // Change the password; same password refused; the new one signs in.
    r = call("PUT", "/auth/v1/user", h, "{\"password\":\"secret1\"}");
    check_error(&r, 422, "same_password", true);
    r = call("PUT", "/auth/v1/user", h, "{\"password\":\"secret2\",\"code_challenge\":null}");
    T_EQ_INT(r.status, 200);
    T_CHECK(jstr_is(&r, "id", id));
    r = call("POST", "/auth/v1/token?grant_type=password", APIKEY V2024,
             "{\"email\":\"c@foolish.cards\",\"password\":\"secret2\"}");
    T_EQ_INT(r.status, 200);
    char rt2[64];   // a second session (another device)
    T_CHECK(jstr(&r, "refresh_token", rt2, sizeof rt2));

    // Logout with no scope is GoTrue's default, global: 204, no body, and
    // the session's refresh token is dead.
    r = call("POST", "/auth/v1/logout", h, NULL);
    T_EQ_INT(r.status, 204);
    T_EQ_INT(r.len, 0);
    char body[128];
    snprintf(body, sizeof body, "{\"refresh_token\":\"%s\"}", rt);
    r = call("POST", "/auth/v1/token?grant_type=refresh_token", APIKEY V2024, body);
    T_EQ_INT(r.status, 400);
    // Global is every session of the user, the other device's included.
    snprintf(body, sizeof body, "{\"refresh_token\":\"%s\"}", rt2);
    r = call("POST", "/auth/v1/token?grant_type=refresh_token", APIKEY V2024, body);
    T_EQ_INT(r.status, 400);
    r = call("POST", "/auth/v1/logout", APIKEY V2024, NULL);
    check_error(&r, 401, "no_authorization", true);

    // A deleted account stops answering GET /user while its token lives.
    T_EQ_INT(cb_auth_delete_user(ctx.auth, id), CB_OK);
    r = call("GET", "/auth/v1/user", h, NULL);
    check_error(&r, 403, "user_not_found", true);
    teardown();
}

// The module with no store (a backend not built yet) answers 503, never crashes.
static void http_without_a_store(void) {
    setup();
    cb_store *keep = ctx.store;
    cb_auth_free(ctx.auth);
    ctx.store = NULL;
    T_EQ_INT(cb_auth_new(&ctx, &ctx.auth), CB_OK);
    reply r = call("POST", "/auth/v1/signup", APIKEY V2024,
                   "{\"email\":\"d@foolish.cards\",\"password\":\"secret1\",\"data\":{\"username\":\"D\"}}");
    T_EQ_INT(r.status, 503);
    cb_auth_free(ctx.auth);
    ctx.auth = NULL;
    r = call("GET", "/auth/v1/user", APIKEY V2024, NULL);
    T_EQ_INT(r.status, 503);
    ctx.store = keep;
    teardown();
}

int main(void) {
    T_RUN(signup_then_signin);
    T_RUN(signup_refusals);
    T_RUN(refresh_rotates_and_reuse_revokes);
    T_RUN(logout_password_delete);
    T_RUN(verify_bearer_and_expiry);
    T_RUN(http_signup_signin_refresh);
    T_RUN(http_refusals);
    T_RUN(http_user_and_logout);
    T_RUN(http_without_a_store);
    return T_DONE();
}
