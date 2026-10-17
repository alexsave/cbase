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
#define CB_AUTH_E_BAD_REFRESH   (-105)  // unknown refresh token, or its user is gone ("refresh_token_not_found")
#define CB_AUTH_E_REFRESH_USED  (-106)  // a consumed or revoked token: the session is now revoked ("refresh_token_already_used")
#define CB_AUTH_E_SAME_PASSWORD (-107)  // change password: the new one is the old one ("same_password")
#define CB_AUTH_E_BAD_EMAIL     (-108)  // signup: not an address ("email_address_invalid")
#define CB_AUTH_E_LONG_PASSWORD (-109)  // over 72 bytes, which bcrypt would silently cut ("validation_failed")

// Uses ctx->store and ctx->cfg (secret, TTL, public URL). CB_OK and *out.
int  cb_auth_new(cb_ctx *ctx, cb_auth **out);
void cb_auth_free(cb_auth *a);

// The user a request's "Authorization: Bearer <jwt>" names. CB_OK, CB_E_AUTH
// (no token, a bad signature, expired), or CB_E_NOMEM when the check itself
// could not run (jwt.h): a 5xx, never "signed out". Never touches the store:
// a verified token is trusted until it expires, as GoTrue's were.
int cb_auth_verify_bearer(cb_auth *a, const cb_http_req *req, cb_user *out);
int cb_auth_verify_token(cb_auth *a, const char *jwt, cb_user *out);

int cb_auth_signup(cb_auth *a, const char *email, const char *password, const char *username, cb_session *out);
int cb_auth_signin(cb_auth *a, const char *email, const char *password, cb_session *out);
// A consumed token presented again within this window of its first use is a
// retry, answered with a new pair (GoTrue's REFRESH_TOKEN_REUSE_INTERVAL, 10 s).
#define CB_AUTH_REUSE_GRACE_MS 10000
int cb_auth_refresh(cb_auth *a, const char *refresh_token, cb_session *out);
// The clock auth reads (cb_now_ms); a test seam.
extern int64_t (*cb_auth_now)(void);
// Logout (NW_SIGNOUT's scope): LOCAL revokes this session's refresh tokens,
// GLOBAL every session of the user. Access tokens live until they expire, as
// GoTrue's did.
typedef enum { CB_LOGOUT_LOCAL, CB_LOGOUT_GLOBAL } cb_logout_scope;
int cb_auth_logout(cb_auth *a, const cb_user *u, cb_logout_scope scope);
int cb_auth_change_password(cb_auth *a, const cb_user *u, const char *new_password);
// Deletes the account (store user_delete). delete-account calls this AFTER the
// app layer has redacted the user from every game (functions_http.h).
int cb_auth_delete_user(cb_auth *a, const char *user_id);
// The stored account behind a verified user (GET /user answers from it, so a
// deleted account stops answering while its token lives). CB_E_NOT_FOUND.
int cb_auth_user_row(cb_auth *a, const char *user_id, cb_user_row *out);

#endif
