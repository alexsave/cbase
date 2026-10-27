// auth_http.c - the platform's session routes (auth_http.h).
#include "auth/auth_http.h"

#include "auth/auth.h"
#include "auth/auth_wire.h"
#include "auth/names.h"
#include "cbase/app.h"
#include "net/refuse.h"
#include "net/router.h"
#include "rate_limits.h"
#include "util/err.h"
#include "util/log.h"

#include <stdlib.h>

#include "../../../../shared/c/hex/hex.h"

#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

int cb_auth_name_email(const char *domain, const char *name, size_t n, char out[CB_EMAIL_MAX + 1]) {
    if (!domain || !domain[0]) return -1;
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)name, n, d);
    hex_encode(d, 8, out, CB_EMAIL_MAX + 1);
    int len = snprintf(out + 16, CB_EMAIL_MAX + 1 - 16, "@%s", domain);
    if (len < 0 || (size_t)len >= CB_EMAIL_MAX + 1 - 16) { out[0] = 0; return -1; }
    return 16 + len;
}

// ---- answers ----------------------------------------------------------------------------

// A store or server failure: a request that would wait on the loop runs again on the pool, a
// store throttled past its backoff is 503 BUSY (the client sends it again), and anything else
// is the server's.
static void failed(cb_http_res *res, int rc) {
    if (rc == CB_E_WOULD_BLOCK) res->to_pool = true;
    else if (rc == CB_E_NOT_FOUND) cb_refuse(res, 404, CBW_WHY_NOT_FOUND, 0);
    else if (rc == CB_E_BUSY) cb_refuse_busy(res);
    else if (rc == CB_E_STATE) cb_refuse(res, 503, CBW_WHY_INTERNAL, 0);
    else {
        CB_ERROR("auth", "request failed: %s", cb_strerror(rc));
        cb_refuse(res, 500, CBW_WHY_INTERNAL, 0);
    }
}

static int account_code(int rc) {
    switch (rc) {
    case CB_AUTH_E_EXISTS: return CBW_ACCOUNT_E_EXISTS;
    case CB_AUTH_E_BAD_LOGIN: return CBW_ACCOUNT_E_BAD_LOGIN;
    case CB_AUTH_E_WEAK_PASSWORD: return CBW_ACCOUNT_E_WEAK_PASSWORD;
    case CB_AUTH_E_LONG_PASSWORD: return CBW_ACCOUNT_E_LONG_PASSWORD;
    case CB_AUTH_E_BAD_USERNAME: return CBW_ACCOUNT_E_BAD_NAME;
    case CB_AUTH_E_BAD_EMAIL: return CBW_ACCOUNT_E_BAD_NAME;   // the email is the name's
    case CB_AUTH_E_BAD_REFRESH: return CBW_ACCOUNT_E_BAD_REFRESH;
    case CB_AUTH_E_REFRESH_USED: return CBW_ACCOUNT_E_REFRESH_USED;
    case CB_AUTH_E_SAME_PASSWORD: return CBW_ACCOUNT_E_SAME_PASSWORD;
    case CB_AUTH_E_TOO_MANY: return CBW_ACCOUNT_E_TOO_MANY;
    case CB_AUTH_E_BAD_IDENTITY: return CBW_ACCOUNT_E_BAD_IDENTITY;
    case CB_AUTH_E_NEEDS_NAME: return CBW_ACCOUNT_E_NEEDS_NAME;
    case CB_AUTH_E_LINKED: return CBW_ACCOUNT_E_LINKED;
    case CB_AUTH_E_NO_PROVIDER: return CBW_ACCOUNT_E_NO_PROVIDER;
    default: return 0;
    }
}

static void auth_failed(cb_http_res *res, int rc) {
    int code = account_code(rc);
    if (code)
        cb_refuse(res, code == CBW_ACCOUNT_E_EXISTS || code == CBW_ACCOUNT_E_LINKED ? 409 : code == CBW_ACCOUNT_E_TOO_MANY ? 429 : 400,
                  CBW_WHY_ACCOUNT, code);
    else if (rc == CB_E_NOT_FOUND) cb_refuse(res, 401, CBW_WHY_AUTH, 0);   // the account behind the token is gone
    else failed(res, rc);
}

static void send_session(cb_http_res *res, const cb_session *s) {
    uint8_t body[CB_AW_SESSION_MAX];
    int n = cb_aw_session_encode(s->access_token, strlen(s->access_token), s->refresh_token, strlen(s->refresh_token),
                                 s->expires_at > 0 ? (uint64_t)s->expires_at * 1000 : 0, s->user.id, strlen(s->user.id),
                                 s->user.username, strlen(s->user.username), body, sizeof body);
    if (n < 0) { failed(res, CB_E_NOMEM); return; }
    cb_res_header(res, "Cache-Control", "no-store");
    cb_res_send(res, 200, "application/octet-stream", body, (size_t)n);
}

// ---- checks -----------------------------------------------------------------------------

// One from key's bucket of `kind`, or false with the 429 answered. On the auth clock
// (auth.h cb_auth_now), as the lockout and the tokens are: a test that holds it still is
// not refilled by however long its bcrypts take.
static bool within(cb_http_res *res, cb_limit_kind kind, const char *key) {
    int retry;
    if (cb_limit_take(kind, key, cb_auth_now(), &retry)) return true;
    CB_INFO("auth", "rate limit %s: %s", cb_limit_rules[kind].name, key);
    cb_refuse_limited(res, retry, true);
    return false;
}

// The verified caller, or false with the refusal answered.
static bool caller(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, cb_user *u) {
    if (!ctx->auth || !ctx->store) { cb_refuse(res, 503, CBW_WHY_INTERNAL, 0); return false; }
    memset(u, 0, sizeof *u);
    int rc = cb_auth_verify_bearer(ctx->auth, req, u);
    if (rc == CB_E_AUTH) { cb_refuse(res, 401, CBW_WHY_AUTH, 0); return false; }
    if (rc != CB_OK) { failed(res, rc); return false; }   // the check could not run: the server's failure
    return true;
}

// The body as a request of `kind`, or false with 400 MALFORMED.
static bool request_of(const cb_http_req *req, cb_http_res *res, uint8_t kind, cb_aw_request *q) {
    if (cb_aw_request_decode((const uint8_t *)req->body, req->body_len, kind, q) != 0) {
        cb_refuse(res, 400, CBW_WHY_MALFORMED, 0);
        return false;
    }
    return true;
}

// A NUL-terminated copy of a length-counted field; false when it holds a NUL (a C string cannot
// say it, and no password or name has one).
static bool cstr(const char *p, size_t n, char *out, size_t cap) {
    if (n >= cap || memchr(p, 0, n)) return false;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static const char *account_domain(const cb_ctx *ctx) { return ctx->app ? ctx->app->account_domain : NULL; }

// ---- routes -----------------------------------------------------------------------------

static void h_credentials(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, uint8_t kind) {
    if (!ctx->auth || !account_domain(ctx)) { cb_refuse(res, 503, CBW_WHY_INTERNAL, 0); return; }
    cb_aw_request q;
    if (!request_of(req, res, kind, &q)) return;
    char name[CBW_NAME_MAX + 1], pw[CBW_PASSWORD_MAX + 1], email[CB_EMAIL_MAX + 1];
    if (!cstr(q.name, q.name_len, name, sizeof name) || !cstr(q.password, q.password_len, pw, sizeof pw)) {
        OPENSSL_cleanse(&q, sizeof q);
        cb_refuse(res, 400, CBW_WHY_MALFORMED, 0);
        return;
    }
    OPENSSL_cleanse(q.password, sizeof q.password);
    // The case step of the canonical name (auth/names.h) for both routes: the account is the one
    // the name's bytes derive, so "alice" is "ALICE" as every client has always sent it. Not
    // UTF-8: no account has that name.
    char canon[CBW_NAME_MAX * 2 + 8];
    int cn = cb_name_case(q.name, q.name_len, canon, sizeof canon);
    if (cn < 0 || cn > CBW_NAME_MAX) {
        OPENSSL_cleanse(pw, sizeof pw);
        auth_failed(res, kind == CBW_SIGNUP ? CB_AUTH_E_BAD_USERNAME : CB_AUTH_E_BAD_LOGIN);
        return;
    }
    if (cb_auth_name_email(account_domain(ctx), canon, (size_t)cn, email) < 0) {
        OPENSSL_cleanse(pw, sizeof pw);
        failed(res, CB_E_STATE);
        return;
    }
    // The limits before any work: an address's attempts, an account's guesses, and every sign-up
    // this process makes (rate_limits.h).
    char ip[96];
    cb_viewer_key(ctx, req, ip, sizeof ip);
    if (kind == CBW_SIGNUP ? !within(res, CB_LIMIT_SIGNUP_IP, ip) || !within(res, CB_LIMIT_SIGNUP_ALL, "*")
                           : !within(res, CB_LIMIT_SIGNIN_IP, ip) || !within(res, CB_LIMIT_SIGNIN_NAME, email)) {
        OPENSSL_cleanse(pw, sizeof pw);
        return;
    }
    cb_session s;
    int rc = kind == CBW_SIGNUP ? cb_auth_signup(ctx->auth, email, pw, canon, &s) : cb_auth_signin(ctx->auth, email, pw, &s);
    OPENSSL_cleanse(pw, sizeof pw);
    if (rc != CB_OK) auth_failed(res, rc);
    else send_session(res, &s);
    OPENSSL_cleanse(&s, sizeof s);
}

static void h_signup(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { h_credentials(ctx, req, res, CBW_SIGNUP); }
static void h_signin(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { h_credentials(ctx, req, res, CBW_SIGNIN); }

static void h_refresh(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ctx->auth) { cb_refuse(res, 503, CBW_WHY_INTERNAL, 0); return; }
    cb_aw_request q;
    if (!request_of(req, res, CBW_REFRESH, &q)) return;
    char tok[CBW_REFRESH_MAX + 1];
    if (!cstr(q.refresh, q.refresh_len, tok, sizeof tok)) { cb_refuse(res, 400, CBW_WHY_MALFORMED, 0); return; }
    char ip[96];
    cb_viewer_key(ctx, req, ip, sizeof ip);
    if (!within(res, CB_LIMIT_REFRESH_IP, ip)) return;
    cb_session s;
    int rc = cb_auth_refresh(ctx->auth, tok, &s);
    OPENSSL_cleanse(tok, sizeof tok);
    if (rc != CB_OK) auth_failed(res, rc);
    else send_session(res, &s);
    OPENSSL_cleanse(&s, sizeof s);
}

static void h_signout(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!caller(ctx, req, res, &u)) return;
    cb_aw_request q;
    if (!request_of(req, res, CBW_SIGNOUT, &q)) return;
    int rc = cb_auth_logout(ctx->auth, &u, q.scope == CBW_SCOPE_GLOBAL ? CB_LOGOUT_GLOBAL : CB_LOGOUT_LOCAL);
    if (rc != CB_OK) auth_failed(res, rc);
    else cb_res_status(res, 204);
}

static void h_password(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!caller(ctx, req, res, &u)) return;
    cb_aw_request q;
    if (!request_of(req, res, CBW_PASSWORD, &q)) return;
    if (!within(res, CB_LIMIT_PASSWORD_USER, u.id)) { OPENSSL_cleanse(&q, sizeof q); return; }
    char cur[CBW_PASSWORD_MAX + 1], pw[CBW_PASSWORD_MAX + 1];
    bool ok = cstr(q.password, q.password_len, cur, sizeof cur) && cstr(q.new_password, q.new_password_len, pw, sizeof pw);
    OPENSSL_cleanse(&q, sizeof q);
    if (!ok) {
        OPENSSL_cleanse(cur, sizeof cur);
        cb_refuse(res, 400, CBW_WHY_MALFORMED, 0);
        return;
    }
    int rc = cb_auth_change_password(ctx->auth, &u, cur, pw);
    OPENSSL_cleanse(cur, sizeof cur);
    OPENSSL_cleanse(pw, sizeof pw);
    if (rc != CB_OK) auth_failed(res, rc);
    else cb_res_status(res, 204);
}

// POST /v1/auth/oidc (auth_http.h): a provider's identity signed in, made into an account, or
// linked to the bearer's.
static void h_oidc(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ctx->auth) { cb_refuse(res, 503, CBW_WHY_INTERNAL, 0); return; }
    cb_aw_oidc *q = malloc(sizeof *q);
    if (!q) { failed(res, CB_E_NOMEM); return; }
    cb_user u;
    char canon[CBW_NAME_MAX * 2 + 8] = "", email[CB_EMAIL_MAX + 1] = "", ip[96];
    bool link = false;
    if (cb_aw_oidc_decode((const uint8_t *)req->body, req->body_len, q) != 0 || memchr(q->token, 0, q->token_len) ||
        memchr(q->nonce, 0, q->nonce_len) || memchr(q->code, 0, q->code_len) || memchr(q->name, 0, q->name_len)) {
        cb_refuse(res, 400, CBW_WHY_MALFORMED, 0);
        goto out;
    }
    link = (q->flags & CBW_OIDC_LINK) != 0;
    if (link && !caller(ctx, req, res, &u)) goto out;
    cb_viewer_key(ctx, req, ip, sizeof ip);
    if (!within(res, CB_LIMIT_SIGNIN_IP, ip)) goto out;
    if (q->name_len && !link) {
        // A new account's name: the case step and its email, as sign-up's; and sign-up's limits.
        int cn = cb_name_case(q->name, q->name_len, canon, sizeof canon);
        if (cn < 0 || cn > CBW_NAME_MAX) { auth_failed(res, CB_AUTH_E_BAD_USERNAME); goto out; }
        if (!account_domain(ctx)) { cb_refuse(res, 503, CBW_WHY_INTERNAL, 0); goto out; }
        if (cb_auth_name_email(account_domain(ctx), canon, (size_t)cn, email) < 0) { failed(res, CB_E_STATE); goto out; }
        if (!within(res, CB_LIMIT_SIGNUP_IP, ip) || !within(res, CB_LIMIT_SIGNUP_ALL, "*")) goto out;
    }
    cb_oidc_ask ask = { .idp = q->idp, .id_token = q->token, .id_token_len = q->token_len, .nonce = q->nonce,
                        .nonce_len = q->nonce_len, .code = q->code, .code_len = q->code_len,
                        .name = canon[0] ? canon : NULL, .email = canon[0] ? email : NULL };
    if (link) {
        int rc = cb_auth_oidc_link(ctx->auth, &u, &ask);
        if (rc != CB_OK) auth_failed(res, rc);
        else cb_res_status(res, 204);
    } else {
        cb_session s;
        int rc = cb_auth_oidc(ctx->auth, &ask, &s);
        if (rc != CB_OK) auth_failed(res, rc);
        else send_session(res, &s);
        OPENSSL_cleanse(&s, sizeof s);
    }
out:
    OPENSSL_cleanse(q, sizeof *q);
    free(q);
}

static const cb_route AUTH_ROUTES[] = {
    { "POST", "/v1/auth/oidc", CB_ROUTE_POOL, h_oidc, 0 },
    { "POST", "/v1/auth/signup", CB_ROUTE_POOL, h_signup, 0 },
    { "POST", "/v1/auth/signin", CB_ROUTE_POOL, h_signin, 0 },
    { "POST", "/v1/auth/refresh", CB_ROUTE_POOL, h_refresh, 0 },
    { "POST", "/v1/auth/signout", CB_ROUTE_POOL, h_signout, 0 },
    { "POST", "/v1/auth/password", CB_ROUTE_POOL, h_password, 0 },
};

int cb_auth_routes(cb_ctx *ctx) {
    return cb_router_add_all(ctx->router, AUTH_ROUTES, (int)(sizeof AUTH_ROUTES / sizeof AUTH_ROUTES[0]));
}
