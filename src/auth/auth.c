// auth.c - accounts and sessions (auth.h): what GoTrue did for foolish.
//
// A session is a session_id (a UUID minted at sign-up or sign-in) plus a chain
// of refresh tokens. Each refresh token is 32 CSPRNG bytes in base64url; only
// its SHA-256 (lowercase hex) is stored. A refresh consumes the token it was
// given (store refresh_take marks it revoked atomically) and issues a new pair
// in the same session. Presenting a consumed token again within
// CB_AUTH_REUSE_GRACE_MS of its first use is a retry of a refresh whose
// response was lost (a phone changing networks), and is answered with another
// new pair in the same session, as GoTrue's 10 s reuse interval does (GoTrue
// answers the same child; a hash-only store cannot, so this is a sibling).
// Later, it is reuse: the whole session is revoked, so a stolen token and its
// legitimate holder are both signed out.
//
// The session itself is a row (store.h cb_session_row) that every refresh
// reads strongly consistent before anything else: ending a session (a
// logout, a detected reuse) is one write to it, and ending all of a user's
// but one (a global sign-out, a password change) is one write to the user
// (session_gen one up, kept_session). Nothing is listed through an index to be
// revoked, so revocation is complete the moment it is written (findings M2,
// M3). Each session has an absolute end (CB_AUTH_SESSION_MS), and each token
// row a TTL: the session's end while live, CB_AUTH_SPENT_TOKEN_MS once spent.
//
// Access tokens are verified locally and never against the store (jwt.h).
#include "auth.h"

#include "bcrypt.h"
#include "names.h"
#include "util/b64.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/hex.h"
#include "util/log.h"
#include "util/str.h"
#include "util/uuid.h"

#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define REFRESH_RAW 32
#define REFRESH_B64 43   // base64url of 32 bytes, unpadded
#define PASSWORD_MAX 72  // bcrypt reads no further

int64_t (*cb_auth_now)(void) = cb_now_ms;

struct cb_auth {
    cb_store *store;    // NULL while the store is not built: only verification works
    char secret[sizeof ((cb_config *)0)->jwt_secret];
    char issuer[600];
    int  ttl_s;
};

int cb_auth_new(cb_ctx *ctx, cb_auth **out) {
    *out = NULL;
    const cb_config *cfg = ctx->cfg;
    if (!cfg->jwt_secret[0]) return CB_E_INVALID;
    cb_auth *a = calloc(1, sizeof *a);
    if (!a) return CB_E_NOMEM;
    a->store = ctx->store;
    cb_strlcpy(a->secret, cfg->jwt_secret, sizeof a->secret);
    if (cfg->public_url[0]) {
        size_t n = cb_strlcpy(a->issuer, cfg->public_url, sizeof a->issuer - 9);
        if (n >= sizeof a->issuer - 9) n = sizeof a->issuer - 10;
        while (n > 0 && a->issuer[n - 1] == '/') a->issuer[--n] = 0;
        cb_strlcpy(a->issuer + n, "/auth/v1", sizeof a->issuer - n);
    }
    a->ttl_s = cfg->jwt_ttl_s > 0 ? cfg->jwt_ttl_s : 3600;
    *out = a;
    return CB_OK;
}

void cb_auth_free(cb_auth *a) {
    if (!a) return;
    OPENSSL_cleanse(a->secret, sizeof a->secret);
    free(a);
}

// ---- verification ----------------------------------------------------------------

int cb_auth_verify_token(cb_auth *a, const char *jwt, cb_user *out) {
    if (!a || !jwt) return CB_E_AUTH;
    cb_jwt_claims c;
    int rc = cb_jwt_verify(a->secret, jwt, cb_auth_now() / 1000, &c);
    if (rc != CB_OK) return rc == CB_E_NOMEM ? rc : CB_E_AUTH;
    memset(out, 0, sizeof *out);
    memcpy(out->id, c.sub, sizeof out->id);
    memcpy(out->email, c.email, sizeof out->email);
    memcpy(out->username, c.username, sizeof out->username);
    memcpy(out->session_id, c.session_id, sizeof out->session_id);
    return CB_OK;
}

int cb_auth_verify_bearer(cb_auth *a, const cb_http_req *req, cb_user *out) {
    const char *tok = cb_http_bearer(req);
    if (!tok || !tok[0]) return CB_E_AUTH;
    return cb_auth_verify_token(a, tok, out);
}

// ---- rules -----------------------------------------------------------------------

// Lowercased into out. A plain shape check, as GoTrue's: one '@' with
// something on both sides, a dot in the domain, no blanks or control bytes.
static int normalize_email(const char *in, char out[CB_EMAIL_MAX + 1]) {
    if (!in) return CB_AUTH_E_BAD_EMAIL;
    size_t n = strlen(in);
    if (n == 0 || n > CB_EMAIL_MAX) return CB_AUTH_E_BAD_EMAIL;
    int at = -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c <= ' ' || c == 0x7f) return CB_AUTH_E_BAD_EMAIL;
        if (c == '@') {
            if (at >= 0) return CB_AUTH_E_BAD_EMAIL;
            at = (int)i;
        }
        out[i] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    out[n] = 0;
    if (at <= 0 || (size_t)at == n - 1) return CB_AUTH_E_BAD_EMAIL;
    const char *dot = strchr(out + at + 1, '.');
    if (!dot || dot == out + at + 1 || dot[1] == 0) return CB_AUTH_E_BAD_EMAIL;
    return CB_OK;
}

// auth.h "Passwords". `username` NULL: not compared (a change has the stored one).
static int check_password(const char *pw, const char *username) {
    if (!pw) return CB_AUTH_E_WEAK_PASSWORD;
    size_t n = strlen(pw);
    if (n > PASSWORD_MAX) return CB_AUTH_E_LONG_PASSWORD;
    // Characters, not bytes: a UTF-8 continuation byte is not one.
    size_t chars = 0;
    for (size_t i = 0; i < n; i++) chars += ((unsigned char)pw[i] & 0xC0) != 0x80;
    if (chars < CB_AUTH_PASSWORD_MIN || cb_auth_password_common(pw)) return CB_AUTH_E_WEAK_PASSWORD;
    if (username && strcasecmp(pw, username) == 0) return CB_AUTH_E_WEAK_PASSWORD;
    return CB_OK;
}

// A new name's rule (names.h; '%', a bot's prefix, among it).
static int check_username(const char *u) { return u ? cb_name_check(u, strlen(u)) : CB_AUTH_E_BAD_USERNAME; }

static void token_hash(const char *token, char out[65]) {
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)token, strlen(token), d);
    cb_hex_encode(d, sizeof d, out, 65);
}

// ---- sessions --------------------------------------------------------------------

// Mints an access token and a fresh refresh token for `u` in session `sess`,
// storing the refresh token's hash, kept until the session's end. CB_OK and *out.
static int issue(cb_auth *a, const cb_user_row *u, const cb_session_row *sess, cb_session *out) {
    const char *session_id = sess->id;
    memset(out, 0, sizeof *out);
    int64_t now_s = cb_auth_now() / 1000;
    cb_jwt_claims c;
    memset(&c, 0, sizeof c);
    memcpy(c.sub, u->id, sizeof c.sub);
    memcpy(c.email, u->email, sizeof c.email);
    memcpy(c.username, u->username, sizeof c.username);
    cb_strlcpy(c.session_id, session_id, sizeof c.session_id);
    c.iat = now_s;
    c.exp = now_s + a->ttl_s;
    int n = cb_jwt_mint(a->secret, &c, a->issuer, out->access_token, sizeof out->access_token);
    if (n < 0) return n;

    uint8_t raw[REFRESH_RAW];
    cb_random_bytes(raw, sizeof raw);
    cb_b64url_encode(raw, sizeof raw, out->refresh_token, sizeof out->refresh_token);
    OPENSSL_cleanse(raw, sizeof raw);
    cb_refresh_row r;
    memset(&r, 0, sizeof r);
    token_hash(out->refresh_token, r.token_hash);
    memcpy(r.user_id, u->id, sizeof r.user_id);
    cb_strlcpy(r.session_id, session_id, sizeof r.session_id);
    r.created_at = cb_auth_now();
    r.expires_at = sess->expires_at;
    int rc = CB_STORE_CALL(a->store, refresh_put, &r);
    if (rc != CB_OK) { memset(out, 0, sizeof *out); return rc; }

    out->expires_in = a->ttl_s;
    out->expires_at = c.exp;
    memcpy(out->user.id, u->id, sizeof out->user.id);
    memcpy(out->user.email, u->email, sizeof out->user.email);
    memcpy(out->user.username, u->username, sizeof out->user.username);
    cb_strlcpy(out->user.session_id, session_id, sizeof out->user.session_id);
    out->user_created_at = u->created_at;
    out->last_sign_in_at = u->last_sign_in_at;
    return CB_OK;
}

// A new session for `u`, stored, then its first pair.
static int begin_session(cb_auth *a, const cb_user_row *u, cb_session *out) {
    cb_session_row sess;
    memset(&sess, 0, sizeof sess);
    cb_uuid_v4(sess.id);
    memcpy(sess.user_id, u->id, sizeof sess.user_id);
    sess.created_at = cb_auth_now();
    sess.expires_at = sess.created_at + CB_AUTH_SESSION_MS;
    sess.gen = u->session_gen;
    int rc = CB_STORE_CALL(a->store, session_put, &sess);
    return rc == CB_OK ? issue(a, u, &sess, out) : rc;
}

int cb_auth_signup(cb_auth *a, const char *email, const char *password, const char *username, cb_session *out) {
    if (!a->store) return CB_E_STATE;
    cb_user_row u;
    memset(&u, 0, sizeof u);
    int rc = normalize_email(email, u.email);
    if (rc == CB_OK) rc = check_username(username);
    if (rc == CB_OK) rc = check_password(password, username);
    if (rc != CB_OK) return rc;
    cb_strlcpy(u.username, username, sizeof u.username);
    if ((rc = cb_bcrypt_hash(password, CB_BCRYPT_COST, u.password_hash)) != CB_OK) return rc;
    cb_uuid_v4(u.id);
    u.created_at = u.last_sign_in_at = cb_auth_now();   // confirmations are off: signed in at once
    rc = CB_STORE_CALL(a->store, user_create, &u);
    if (rc == CB_E_CONFLICT) return CB_AUTH_E_EXISTS;
    if (rc != CB_OK) return rc;
    return begin_session(a, &u, out);
}

// Spent on an unknown email, so a miss costs what a wrong password does. A
// hash of a random password nobody kept (htpasswd -B, respelled $2b$).
static const char DUMMY_HASH[] = "$2b$10$OJvZ7hLjzyMHI.C85EGMDOs4pIvjMhJwQ1XaHEDVepxMCI.baQEUO";

// The count after one more failure at `now` (auth.h CB_AUTH_FAIL_*).
static void count_failure(cb_signin_fails *f, int64_t now) {
    if (f->n <= 0 || now - f->last_at > CB_AUTH_FAIL_WINDOW_MS) {
        f->n = 0;
        f->locked_until = 0;
    }
    f->n++;
    f->last_at = now;
    if (f->n > CB_AUTH_FAIL_FREE) {
        int over = f->n - CB_AUTH_FAIL_FREE - 1;   // 0 for the first failure past the free ones
        int64_t lock = CB_AUTH_LOCK_BASE_MS << (over < 20 ? over : 20);   // the shift bounded, the cap is the rule
        if (lock > CB_AUTH_LOCK_MAX_MS) lock = CB_AUTH_LOCK_MAX_MS;
        f->locked_until = now + lock;
    }
}

// The password checked against u's hash, counted (auth.h CB_AUTH_FAIL_*):
// CB_OK, CB_AUTH_E_BAD_LOGIN, CB_AUTH_E_TOO_MANY (locked: not even checked),
// or a store error. A sign-in and a password change's current password both
// come through here, so a stolen access token cannot guess the password
// faster than a sign-in can.
static int verify_counted(cb_auth *a, const cb_user_row *u, const char *password, int64_t now) {
    cb_signin_fails f;
    memset(&f, 0, sizeof f);
    int rc = CB_STORE_CALL(a->store, signin_fails_get, u->email, now, &f);
    if (rc != CB_OK && rc != CB_E_NOT_FOUND) return rc;
    bool counted = rc == CB_OK && f.n > 0;
    if (counted && f.locked_until > now) return CB_AUTH_E_TOO_MANY;   // no bcrypt, no write
    int ok = cb_bcrypt_verify(password, u->password_hash);
    if (ok < 0) CB_WARN("auth", "user %s: stored hash does not parse as bcrypt", u->id);
    if (ok != 1) {
        count_failure(&f, now);
        if (f.locked_until) CB_WARN("auth", "user %s: %d failed sign-ins; locked for %lld s", u->id, f.n,
                                    (long long)((f.locked_until - now) / 1000));
        rc = CB_STORE_CALL(a->store, signin_fails_put, u->email, &f, now);
        if (rc != CB_OK) CB_WARN("auth", "user %s: the failed sign-in was not counted: %s", u->id, cb_strerror(rc));
        return CB_AUTH_E_BAD_LOGIN;
    }
    if (counted && (rc = CB_STORE_CALL(a->store, signin_fails_clear, u->email)) != CB_OK)
        CB_WARN("auth", "user %s: the failed sign-ins were not cleared: %s", u->id, cb_strerror(rc));
    return CB_OK;
}

int cb_auth_signin(cb_auth *a, const char *email, const char *password, cb_session *out) {
    if (!a->store) return CB_E_STATE;
    char em[CB_EMAIL_MAX + 1];
    if (!password || normalize_email(email, em) != CB_OK) return CB_AUTH_E_BAD_LOGIN;
    cb_user_row u;
    int rc = CB_STORE_CALL(a->store, user_by_email, em, &u);
    if (rc == CB_E_NOT_FOUND) {
        (void)cb_bcrypt_verify(password, DUMMY_HASH);
        return CB_AUTH_E_BAD_LOGIN;
    }
    if (rc != CB_OK) return rc;
    int64_t now = cb_auth_now();
    if ((rc = verify_counted(a, &u, password, now)) != CB_OK) return rc;
    if ((rc = CB_STORE_CALL(a->store, user_touch_sign_in, u.id, now)) != CB_OK) return rc;
    u.last_sign_in_at = now;
    return begin_session(a, &u, out);
}

// Is the session over for u (auth.h): revoked, or begun at a generation the
// user has since ended (all but kept_session). Its end needs no check here:
// session_get answers none past it, and a token never outlives its session.
static bool session_over(const cb_session_row *s, const cb_user_row *u) {
    if (s->revoked) return true;
    return s->gen < u->session_gen && strcmp(s->id, u->kept_session) != 0;
}

int cb_auth_refresh(cb_auth *a, const char *refresh_token, cb_session *out) {
    if (!a->store) return CB_E_STATE;
    if (!refresh_token || strlen(refresh_token) != REFRESH_B64) return CB_AUTH_E_BAD_REFRESH;
    char h[65];
    token_hash(refresh_token, h);
    cb_refresh_row r;
    int64_t now = cb_auth_now();
    int rc = CB_STORE_CALL(a->store, refresh_take, h, now, CB_AUTH_SPENT_TOKEN_MS, &r);
    if (rc == CB_E_NOT_FOUND) return CB_AUTH_E_BAD_REFRESH;
    if (rc != CB_OK) return rc;
    cb_user_row u;
    rc = CB_STORE_CALL(a->store, user_get, r.user_id, &u);
    if (rc == CB_E_NOT_FOUND) return CB_AUTH_E_BAD_REFRESH;
    if (rc != CB_OK) return rc;
    // The session decides first, read strongly consistent (finding M3): a
    // session ended by a logout, a global sign-out, a password change or a
    // detected reuse never refreshes again, whatever its tokens say.
    cb_session_row sess;
    rc = CB_STORE_CALL(a->store, session_get, r.session_id, now, &sess);
    if (rc == CB_E_NOT_FOUND) {
        // A session begun before sessions were stored: its row is made now,
        // from the token, at generation 0 (it began before any generation
        // was ended), so it can be ended like any other. One past its end
        // never gets here with a live token, but an old token that carries no
        // end of its own does: refused by its session's end.
        memset(&sess, 0, sizeof sess);
        cb_strlcpy(sess.id, r.session_id, sizeof sess.id);
        memcpy(sess.user_id, r.user_id, sizeof sess.user_id);
        sess.created_at = r.created_at;
        sess.expires_at = r.created_at + CB_AUTH_SESSION_MS;
        if (sess.expires_at <= now) return CB_AUTH_E_BAD_REFRESH;
        if ((rc = CB_STORE_CALL(a->store, session_put, &sess)) != CB_OK) return rc;
    } else if (rc != CB_OK) {
        return rc;
    }
    if (session_over(&sess, &u)) return CB_AUTH_E_REFRESH_USED;
    if (r.revoked && now - r.revoked_at <= CB_AUTH_REUSE_GRACE_MS) {
        CB_INFO("auth", "session %s: a refresh token used %lld ms ago was presented again; within the grace, a new pair",
                r.session_id, (long long)(now - r.revoked_at));
    } else if (r.revoked) {
        // Reuse: end the whole session, so a stolen token and its legitimate
        // holder are both signed out. One write.
        CB_WARN("auth", "session %s: a consumed refresh token was presented; session revoked", r.session_id);
        rc = CB_STORE_CALL(a->store, session_revoke, r.session_id, r.user_id, sess.expires_at);
        return rc == CB_OK ? CB_AUTH_E_REFRESH_USED : rc;
    }
    return issue(a, &u, &sess, out);
}

int cb_auth_logout(cb_auth *a, const cb_user *u, cb_logout_scope scope) {
    if (!a->store) return CB_E_STATE;
    if (scope == CB_LOGOUT_GLOBAL) return CB_STORE_CALL(a->store, user_end_sessions, u->id);
    return CB_STORE_CALL(a->store, session_revoke, u->session_id, u->id, cb_auth_now() + CB_AUTH_SESSION_MS);
}

int cb_auth_change_password(cb_auth *a, const cb_user *u, const char *current, const char *new_password) {
    if (!a->store) return CB_E_STATE;
    if (!current) return CB_AUTH_E_BAD_LOGIN;
    cb_user_row row;
    int rc = CB_STORE_CALL(a->store, user_get, u->id, &row);
    if (rc != CB_OK) return rc;
    int64_t now = cb_auth_now();
    // The current password first (finding M1): an access token alone does
    // not prove the caller owns the account.
    if ((rc = verify_counted(a, &row, current, now)) != CB_OK) return rc;
    if ((rc = check_password(new_password, row.username)) != CB_OK) return rc;
    if (strcmp(current, new_password) == 0) return CB_AUTH_E_SAME_PASSWORD;
    char hash[CB_BCRYPT_HASH_LEN + 1];
    if ((rc = cb_bcrypt_hash(new_password, CB_BCRYPT_COST, hash)) != CB_OK) return rc;
    // The new hash and the end of every other session, in one write: whoever
    // held a stolen session is out, the caller stays in.
    return CB_STORE_CALL(a->store, user_set_password, u->id, hash, u->session_id);
}

int cb_auth_delete_user(cb_auth *a, const char *user_id) {
    if (!a->store) return CB_E_STATE;
    return CB_STORE_CALL(a->store, user_delete, user_id);
}

int cb_auth_user_row(cb_auth *a, const char *user_id, cb_user_row *out) {
    if (!a->store) return CB_E_STATE;
    return CB_STORE_CALL(a->store, user_get, user_id, out);
}
