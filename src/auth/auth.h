// auth.h - accounts and sessions: what GoTrue did, behind the routes in
// auth_http.h.
//
// Email + password only (the clients derive the email from the username:
// hex(SHA-256(UPPER(name)))[:16] + "@foolish.cards"). An access token is an
// HS256 JWT (jwt.h) valid CBASE_JWT_TTL seconds; a refresh token is 32 random
// bytes, base64url, stored only as its SHA-256 and rotated on every use. A
// username containing '%' is refused (bot nicknames carry that prefix).
#ifndef CB_AUTH_H
#define CB_AUTH_H

#include "ctx.h"
#include "jwt.h"
#include "net/http.h"
#include "store/store.h"

// Who a verified request is.
typedef struct {
    cb_uuid id;
    char    email[CB_EMAIL_MAX + 1];
    char    username[CB_USERNAME_MAX + 1];
    cb_uuid session_id;
} cb_user;

// What a sign-in, sign-up or refresh hands back (GoTrue's session object).
typedef struct {
    char    access_token[CB_JWT_MAX];
    char    refresh_token[64];      // base64url of 32 random bytes
    int     expires_in;             // seconds
    int64_t expires_at;             // seconds since the epoch
    cb_user user;
    int64_t user_created_at;        // ms, for the user object in the body
    int64_t last_sign_in_at;        // ms
} cb_session;

// Refusal codes beyond the common ones, for auth_http's error bodies.
#define CB_AUTH_E_EXISTS        (-101)  // signup: the email is registered ("user_already_exists")
#define CB_AUTH_E_BAD_LOGIN     (-102)  // signin: wrong email or password ("invalid_credentials")
#define CB_AUTH_E_WEAK_PASSWORD (-103)  // under 6 characters ("weak_password")
#define CB_AUTH_E_BAD_USERNAME  (-104)  // empty, too long, or containing '%'
#define CB_AUTH_E_BAD_REFRESH   (-105)  // unknown, revoked or reused refresh token

// Uses ctx->store and ctx->cfg (secret, TTL, public URL). CB_OK and *out.
int  cb_auth_new(cb_ctx *ctx, cb_auth **out);
void cb_auth_free(cb_auth *a);

// The user a request's "Authorization: Bearer <jwt>" names. CB_OK, or CB_E_AUTH
// (no token, a bad signature, expired). Never touches the store: a verified
// token is trusted until it expires, as GoTrue's were.
int cb_auth_verify_bearer(cb_auth *a, const cb_http_req *req, cb_user *out);
int cb_auth_verify_token(cb_auth *a, const char *jwt, cb_user *out);

int cb_auth_signup(cb_auth *a, const char *email, const char *password, const char *username, cb_session *out);
int cb_auth_signin(cb_auth *a, const char *email, const char *password, cb_session *out);
int cb_auth_refresh(cb_auth *a, const char *refresh_token, cb_session *out);
// Revokes the session's refresh tokens. The access token lives until it expires.
int cb_auth_logout(cb_auth *a, const cb_user *u);
int cb_auth_change_password(cb_auth *a, const cb_user *u, const char *new_password);
// Deletes the account (store user_delete). delete-account calls this AFTER the
// app layer has redacted the user from every game (functions_http.h).
int cb_auth_delete_user(cb_auth *a, const char *user_id);

// The user object GoTrue answers with ({"id","aud","role","email",
// "user_metadata":{"username"},"app_metadata",...,"created_at"}), written into
// an open JSON writer. Shared by auth_http.c and the admin Users panel.
struct cb_jw;
void cb_auth_write_user(struct cb_jw *w, const cb_user *u, int64_t created_at, int64_t last_sign_in_at);

#endif
