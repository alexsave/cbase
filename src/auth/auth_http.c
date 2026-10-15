// auth_http.c - the GoTrue routes (auth_http.h).
//
// What the two SDKs read, and so what is written (checked against
// @supabase/auth-js 2.106 lib/fetch.js and supabase-swift's Auth/Types.swift
// and Internal/APIClient.swift):
//
//   * A session: access_token, refresh_token and expires_in must all be truthy
//     or auth-js drops the session (hasSession); expires_at and token_type are
//     read too; `user` is decoded by supabase-swift into User, whose id must
//     be a UUID and whose created_at and updated_at are required dates.
//   * Dates are ISO-8601 with a fractional part and 'Z' (supabase-swift parses
//     with Date.ISO8601FormatStyle and nothing else).
//   * An error: both SDKs send X-Supabase-Api-Version: 2024-01-01. GoTrue then
//     answers {"code":"<error code>","msg":".."} and echoes the header, and
//     both SDKs take the error code from `code`. A request without the header
//     gets GoTrue's older shape {"code":<status>,"error_code":"..","msg":".."}.
//     The two shapes are not interchangeable: supabase-swift decodes `code` as
//     a String, and an integer there fails the whole decode ("Unexpected
//     error"), so the integer form is sent only to a client that did not ask
//     for the new one. A weak password also carries weak_password.reasons.
//   * The refresh error codes refresh_token_not_found and
//     refresh_token_already_used are the ones supabase-swift treats as "this
//     session is gone" and clears.
#include "auth_http.h"

#include "auth.h"
#include "json_surface.h"
#include "net/router.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#include <string.h>

#define API_VERSION_HEADER "X-Supabase-Api-Version"
#define API_VERSION_2024   "2024-01-01"

// The request asked for the 2024-01-01 API (or later): a YYYY-MM-DD at or after it.
static bool api_2024(const cb_http_req *req) {
    const char *v = cb_http_header_get(req, API_VERSION_HEADER);
    if (!v || strlen(v) != 10) return false;
    for (int i = 0; i < 10; i++)
        if (i == 4 || i == 7 ? v[i] != '-' : (v[i] < '0' || v[i] > '9')) return false;
    return strcmp(v, API_VERSION_2024) >= 0;
}

static void send_json(const cb_http_req *req, cb_http_res *res, int status, cb_buf *b) {
    if (api_2024(req)) cb_res_header(res, API_VERSION_HEADER, API_VERSION_2024);
    cb_res_header(res, "Cache-Control", "no-store");
    cb_res_send(res, status, "application/json", b->p ? (const void *)b->p : "", b->len);
}

static void fail(const cb_http_req *req, cb_http_res *res, int status, const char *code, const char *msg) {
    cb_buf b;
    cb_buf_init(&b);
    cb_js_gotrue_error(&b, api_2024(req), status, code, msg);
    send_json(req, res, status, &b);
    cb_buf_free(&b);
}

// An auth.c or store refusal as GoTrue would have answered it.
static void fail_rc(const cb_http_req *req, cb_http_res *res, int rc) {
    switch (rc) {
    case CB_AUTH_E_EXISTS: fail(req, res, 422, "user_already_exists", "User already registered"); return;
    case CB_AUTH_E_BAD_LOGIN: fail(req, res, 400, "invalid_credentials", "Invalid login credentials"); return;
    case CB_AUTH_E_WEAK_PASSWORD: fail(req, res, 422, "weak_password", "Password should be at least 6 characters."); return;
    case CB_AUTH_E_LONG_PASSWORD: fail(req, res, 422, "validation_failed", "Password cannot be longer than 72 characters"); return;
    case CB_AUTH_E_BAD_USERNAME:
        fail(req, res, 422, "validation_failed", "Username must be 1 to 64 characters and must not contain '%'");
        return;
    case CB_AUTH_E_BAD_EMAIL: fail(req, res, 400, "email_address_invalid", "Unable to validate email address: invalid format"); return;
    case CB_AUTH_E_BAD_REFRESH: fail(req, res, 400, "refresh_token_not_found", "Invalid Refresh Token: Refresh Token Not Found"); return;
    case CB_AUTH_E_REFRESH_USED: fail(req, res, 400, "refresh_token_already_used", "Invalid Refresh Token: Already Used"); return;
    case CB_AUTH_E_SAME_PASSWORD:
        fail(req, res, 422, "same_password", "New password should be different from the old password.");
        return;
    case CB_E_NOT_FOUND: fail(req, res, 403, "user_not_found", "User from sub claim in JWT does not exist"); return;
    case CB_E_STATE: cb_router_error(res, 503, "auth is not available"); return;
    default:
        CB_ERROR("auth", "request failed: %s", cb_strerror(rc));
        fail(req, res, 500, "unexpected_failure", "Database error");
        return;
    }
}

// The `want` fields of the body, or false with a 400 already answered.
static bool body(const cb_http_req *req, cb_http_res *res, unsigned want, cb_js_gotrue_body *out) {
    switch (cb_js_gotrue_body_decode((const char *)req->body, req->body_len, want, out)) {
    case CB_JS_GT_OK: return true;
    case CB_JS_GT_NOT_OBJECT: fail(req, res, 400, "bad_json", "Could not parse request body as JSON"); return false;
    default: fail(req, res, 400, "validation_failed", "A field is not a string, or is too long"); return false;
    }
}

static void send_session(const cb_http_req *req, cb_http_res *res, const cb_session *s) {
    cb_buf b;
    cb_buf_init(&b);
    if (cb_js_gotrue_session(&b, s) == CB_OK) send_json(req, res, 200, &b);
    else fail(req, res, 500, "unexpected_failure", "response too large");
    cb_buf_free(&b);
}

// The verified caller, or false with 401 (no token) or 403 (a bad one) answered.
static bool caller(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, cb_user *u) {
    const char *tok = cb_http_bearer(req);
    if (!tok || !tok[0]) {
        fail(req, res, 401, "no_authorization", "This endpoint requires a valid Bearer token");
        return false;
    }
    if (cb_auth_verify_token(ctx->auth, tok, u) != CB_OK) {
        fail(req, res, 403, "bad_jwt", "invalid JWT: unable to parse or verify signature, or the token is expired");
        return false;
    }
    return true;
}

static void send_user(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, const cb_user *u) {
    cb_user_row row;
    int rc = cb_auth_user_row(ctx->auth, u->id, &row);
    if (rc != CB_OK) { fail_rc(req, res, rc); return; }
    cb_user cur = *u;
    memcpy(cur.email, row.email, sizeof cur.email);
    memcpy(cur.username, row.username, sizeof cur.username);
    cb_buf b;
    cb_buf_init(&b);
    if (cb_js_gotrue_user(&b, &cur, row.created_at, row.last_sign_in_at) == CB_OK) send_json(req, res, 200, &b);
    else fail(req, res, 500, "unexpected_failure", "response too large");
    cb_buf_free(&b);
}

// ---- the routes ------------------------------------------------------------------

#define NEED_AUTH(ctx, res) \
    do { if (!(ctx)->auth) { cb_router_error((res), 503, "auth is not available"); return; } } while (0)

// POST /auth/v1/signup {email, password, data:{username}}
static void h_signup(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    NEED_AUTH(ctx, res);
    cb_js_gotrue_body f;
    if (!body(req, res, CB_JS_GT_EMAIL | CB_JS_GT_PASSWORD | CB_JS_GT_USERNAME, &f)) return;
    if (!f.email[0]) { fail(req, res, 400, "validation_failed", "Signup requires an email"); return; }
    if (!f.password[0]) { fail(req, res, 400, "validation_failed", "Signup requires a valid password"); return; }
    cb_session s;
    int rc = cb_auth_signup(ctx->auth, f.email, f.password, f.username, &s);
    if (rc != CB_OK) fail_rc(req, res, rc);
    else send_session(req, res, &s);
}

// POST /auth/v1/token?grant_type=password|refresh_token
static void h_token(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    NEED_AUTH(ctx, res);
    char grant[32];
    if (!cb_http_query_get(req, "grant_type", grant, sizeof grant)) grant[0] = 0;
    bool password = strcmp(grant, "password") == 0, refresh = strcmp(grant, "refresh_token") == 0;
    cb_js_gotrue_body f;
    if (!body(req, res, password ? CB_JS_GT_EMAIL | CB_JS_GT_PASSWORD : refresh ? CB_JS_GT_REFRESH_TOKEN : 0, &f)) return;
    cb_session s;
    int rc;
    if (password) {
        rc = cb_auth_signin(ctx->auth, f.email, f.password, &s);
    } else if (refresh) {
        rc = cb_auth_refresh(ctx->auth, f.refresh_token, &s);
    } else {
        fail(req, res, 400, "validation_failed", "unsupported_grant_type");
        return;
    }
    if (rc != CB_OK) fail_rc(req, res, rc);
    else send_session(req, res, &s);
}

// GET /auth/v1/user
static void h_user_get(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    NEED_AUTH(ctx, res);
    cb_user u;
    if (caller(ctx, req, res, &u)) send_user(ctx, req, res, &u);
}

// PUT /auth/v1/user {password}. Other attributes (email, data) are not
// changed: the clients send only a password, and a username change would have
// to reach the leaderboard and every roster.
static void h_user_put(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    NEED_AUTH(ctx, res);
    cb_user u;
    if (!caller(ctx, req, res, &u)) return;
    cb_js_gotrue_body f;
    if (!body(req, res, CB_JS_GT_PASSWORD, &f)) return;
    if (f.has_password) {
        int rc = cb_auth_change_password(ctx->auth, &u, f.password);
        if (rc != CB_OK) { fail_rc(req, res, rc); return; }
    }
    send_user(ctx, req, res, &u);
}

// POST /auth/v1/logout[?scope=global|local|others]: 204.
static void h_logout(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    NEED_AUTH(ctx, res);
    cb_user u;
    if (!caller(ctx, req, res, &u)) return;
    // GoTrue's default scope is global, and supabase-js sends it explicitly.
    char scope[16];
    if (!cb_http_query_get(req, "scope", scope, sizeof scope)) cb_strlcpy(scope, "global", sizeof scope);
    if (strcmp(scope, "others") == 0) {
        // "Sign out my other devices": not served (auth.h); no client sends it.
        cb_res_status(res, 204);
        return;
    }
    int rc = cb_auth_logout(ctx->auth, &u, strcmp(scope, "local") == 0 ? CB_LOGOUT_LOCAL : CB_LOGOUT_GLOBAL);
    if (rc != CB_OK) { fail_rc(req, res, rc); return; }
    cb_res_status(res, 204);
}

int cb_auth_routes(cb_ctx *ctx) {
    static const cb_route routes[] = {
        { "POST", "/auth/v1/signup", CB_ROUTE_APIKEY, h_signup },
        { "POST", "/auth/v1/token", CB_ROUTE_APIKEY, h_token },
        { "GET", "/auth/v1/user", CB_ROUTE_APIKEY, h_user_get },
        { "PUT", "/auth/v1/user", CB_ROUTE_APIKEY, h_user_put },
        { "POST", "/auth/v1/logout", CB_ROUTE_APIKEY, h_logout },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        int rc = cb_router_add(ctx->router, routes[i].method, routes[i].pattern, routes[i].flags, routes[i].fn);
        if (rc != CB_OK) return rc;
    }
    return CB_OK;
}
