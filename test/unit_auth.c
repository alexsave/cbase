// unit_auth.c - accounts and sessions (auth/auth.h) against an in-RAM store
// (lib_auth_fakestore.c). The routes over them are unit_v1.c's.
#include "t.h"

#include "auth/auth.h"
#include "auth/bcrypt.h"
#include "lib_auth_fakestore.h"
#include "util/clock.h"
#include "util/err.h"

#include <stdlib.h>

static cb_config cfg;
static cb_ctx ctx;

static void setup(void) {
    memset(&cfg, 0, sizeof cfg);
    strcpy(cfg.jwt_secret, "a-unit-test-secret-with-enough-bytes");
    strcpy(cfg.allowed_origins, "*");
    strcpy(cfg.public_url, "https://api.example/");
    cfg.jwt_ttl_s = 3600;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.mode = CB_MODE_SERVE;
    ctx.store = auth_fakestore_new();
    T_EQ_INT(cb_auth_new(&ctx, &ctx.auth), CB_OK);
}

static void teardown(void) {
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

int main(void) {
    T_RUN(signup_then_signin);
    T_RUN(signup_refusals);
    T_RUN(refresh_rotates_and_reuse_revokes);
    T_RUN(logout_password_delete);
    T_RUN(verify_bearer_and_expiry);
    return T_DONE();
}
