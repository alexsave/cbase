// unit_auth.c - accounts and sessions (auth/auth.h) against an in-RAM store
// (lib_auth_fakestore.c). The routes over them are unit_v1.c's.
#include "t.h"

#include "auth/auth.h"
#include "auth/bcrypt.h"
#include "lib_auth_fakestore.h"
#include "util/clock.h"
#include "util/err.h"

#include <openssl/sha.h>
#include <stdlib.h>

#include "util/hex.h"

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
    T_EQ_INT(cb_auth_signup(ctx.auth, "Anna@Foolish.Cards", "secret-11", "ANNA", &s), CB_OK);
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
    T_EQ_INT(cb_bcrypt_verify("secret-11", row.password_hash), 1);

    cb_session s2;
    T_EQ_INT(cb_auth_signin(ctx.auth, "ANNA@foolish.cards", "secret-11", &s2), CB_OK);
    T_EQ_STR(s2.user.id, s.user.id);
    T_CHECK(strcmp(s2.user.session_id, s.user.session_id) != 0);   // a new session
    T_CHECK(strcmp(s2.refresh_token, s.refresh_token) != 0);
    T_EQ_INT(cb_auth_signin(ctx.auth, "anna@foolish.cards", "secret-22", &s2), CB_AUTH_E_BAD_LOGIN);
    // An unknown email still spends a bcrypt verify, so a miss is not
    // measurably faster than a wrong password (cost 10 is ~60 ms here).
    int64_t m0 = cb_mono_ms();
    T_EQ_INT(cb_auth_signin(ctx.auth, "nobody@foolish.cards", "secret-11", &s2), CB_AUTH_E_BAD_LOGIN);
    T_CHECK(cb_mono_ms() - m0 >= 10);
    T_EQ_INT(cb_auth_signin(ctx.auth, "not an email", "secret-11", &s2), CB_AUTH_E_BAD_LOGIN);
    teardown();
}

static void signup_refusals(void) {
    setup();
    cb_session s;
    T_EQ_INT(cb_auth_signup(ctx.auth, "a@foolish.cards", "secret-11", "ANNA", &s), CB_OK);
    T_EQ_INT(cb_auth_signup(ctx.auth, "A@FOOLISH.CARDS", "secret-22", "ANNA2", &s), CB_AUTH_E_EXISTS);
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "12345", "B", &s), CB_AUTH_E_WEAK_PASSWORD);
    // Eight characters, not eight bytes: "\xc3\xa9" is one, so seven of them (14 bytes) is short.
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", "B", &s),
             CB_AUTH_E_WEAK_PASSWORD);
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "seven77", "B", &s), CB_AUTH_E_WEAK_PASSWORD);
    // The most common passwords, in any case, and the username itself.
    const char *common[] = { "password", "PASSWORD1", "12345678", "Qwerty123", "iloveyou", "1q2w3e4r", "trustno1" };
    for (size_t i = 0; i < sizeof common / sizeof common[0]; i++)
        T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", common[i], "B", &s), CB_AUTH_E_WEAK_PASSWORD);
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "bobby-the-great", "BOBBY-THE-GREAT", &s), CB_AUTH_E_WEAK_PASSWORD);
    char long_pw[80];
    memset(long_pw, 'p', 73);
    long_pw[73] = 0;
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", long_pw, "B", &s), CB_AUTH_E_LONG_PASSWORD);
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "secret-11", "%BOT", &s), CB_AUTH_E_BAD_USERNAME);
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "secret-11", "MID%DLE", &s), CB_AUTH_E_BAD_USERNAME);
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "secret-11", "", &s), CB_AUTH_E_BAD_USERNAME);
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "secret-11", NULL, &s), CB_AUTH_E_BAD_USERNAME);
    const char *bad[] = { "", "nobody", "@foolish.cards", "b@", "b@cards", "b@.cards", "b@x.", "b c@x.y", "a@b@x.y" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        T_EQ_INT(cb_auth_signup(ctx.auth, bad[i], "secret-11", "B", &s), CB_AUTH_E_BAD_EMAIL);
    // None of those created anything.
    T_EQ_INT(cb_auth_signup(ctx.auth, "b@foolish.cards", "secret-11", "B", &s), CB_OK);
    teardown();
}

static int64_t g_fake_now;
static int64_t fake_now(void) { return g_fake_now; }

static void refresh_rotates_and_reuse_revokes(void) {
    setup();
    g_fake_now = cb_now_ms();
    cb_auth_now = fake_now;
    cb_session s0, s1, s2, x;
    T_EQ_INT(cb_auth_signup(ctx.auth, "r@foolish.cards", "secret-11", "R", &s0), CB_OK);
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
    T_EQ_INT(cb_auth_signin(ctx.auth, "r@foolish.cards", "secret-11", &other), CB_OK);

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
    int sessions = auth_fakestore_session_rows(ctx.store);
    T_EQ_INT(cb_auth_refresh(ctx.auth, s1.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    // Ended by one write to the session, not one per token (findings M2, M3):
    // the live tokens are still unspent rows, and refuse all the same.
    T_EQ_INT(auth_fakestore_session_tokens(ctx.store, s0.user.session_id, &revoked), 4);
    T_EQ_INT(revoked, 2);
    T_EQ_INT(auth_fakestore_session_rows(ctx.store), sessions);
    T_EQ_INT(cb_auth_refresh(ctx.auth, s2.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    T_EQ_INT(cb_auth_refresh(ctx.auth, g.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
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
    T_EQ_INT(cb_auth_signup(ctx.auth, "l@foolish.cards", "secret-11", "L", &s), CB_OK);
    T_EQ_INT(cb_auth_verify_token(ctx.auth, s.access_token, &u), CB_OK);
    // A second device: local logout ends only this session, global both.
    cb_session d2, d2b, d2c;
    T_EQ_INT(cb_auth_signin(ctx.auth, "l@foolish.cards", "secret-11", &d2), CB_OK);
    T_EQ_INT(cb_auth_refresh(ctx.auth, d2.refresh_token, &d2b), CB_OK);
    T_EQ_INT(cb_auth_logout(ctx.auth, &u, CB_LOGOUT_LOCAL), CB_OK);
    T_EQ_INT(cb_auth_refresh(ctx.auth, s.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    T_EQ_INT(cb_auth_refresh(ctx.auth, d2b.refresh_token, &d2c), CB_OK);
    // d2b was rotated a moment ago, but the session's end decides first: a
    // retry of it after the global logout is refused, not a sibling.
    T_EQ_INT(cb_auth_logout(ctx.auth, &u, CB_LOGOUT_GLOBAL), CB_OK);
    T_EQ_INT(cb_auth_refresh(ctx.auth, d2c.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    T_EQ_INT(cb_auth_refresh(ctx.auth, d2b.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    // The access token lives until it expires, as GoTrue's did.
    T_EQ_INT(cb_auth_verify_token(ctx.auth, s.access_token, &u), CB_OK);

    T_EQ_INT(cb_auth_change_password(ctx.auth, &u, "secret-11", "secret-11"), CB_AUTH_E_SAME_PASSWORD);
    T_EQ_INT(cb_auth_change_password(ctx.auth, &u, "secret-11", "short"), CB_AUTH_E_WEAK_PASSWORD);
    T_EQ_INT(cb_auth_change_password(ctx.auth, &u, "secret-11", "Password1"), CB_AUTH_E_WEAK_PASSWORD);
    T_EQ_INT(cb_auth_change_password(ctx.auth, &u, "secret-11", "secret-22"), CB_OK);
    T_EQ_INT(cb_auth_signin(ctx.auth, "l@foolish.cards", "secret-11", &x), CB_AUTH_E_BAD_LOGIN);
    T_EQ_INT(cb_auth_signin(ctx.auth, "l@foolish.cards", "secret-22", &s2), CB_OK);

    T_EQ_INT(cb_auth_delete_user(ctx.auth, u.id), CB_OK);
    T_EQ_INT(cb_auth_signin(ctx.auth, "l@foolish.cards", "secret-22", &x), CB_AUTH_E_BAD_LOGIN);
    T_EQ_INT(cb_auth_refresh(ctx.auth, s2.refresh_token, &x), CB_AUTH_E_BAD_REFRESH);
    cb_user_row row;
    T_EQ_INT(cb_auth_user_row(ctx.auth, u.id, &row), CB_E_NOT_FOUND);
    T_EQ_INT(cb_auth_delete_user(ctx.auth, u.id), CB_E_NOT_FOUND);
    // The email is free again.
    T_EQ_INT(cb_auth_signup(ctx.auth, "l@foolish.cards", "secret-33", "L", &x), CB_OK);
    teardown();
}

// A password change (finding M1): the current password is required (and
// counted as a sign-in is, so a stolen access token cannot guess it), and
// every OTHER session ends in the same write; the caller's goes on.
static void password_change_needs_the_current_one_and_ends_the_rest(void) {
    setup();
    g_fake_now = 1700000000000LL;
    cb_auth_now = fake_now;
    cb_session mine, thief, x;
    cb_user u;
    T_EQ_INT(cb_auth_signup(ctx.auth, "p@foolish.cards", "first-pass-1", "P", &mine), CB_OK);
    T_EQ_INT(cb_auth_signin(ctx.auth, "p@foolish.cards", "first-pass-1", &thief), CB_OK);
    // Holding only the thief's access token, without the password: refused.
    T_EQ_INT(cb_auth_verify_token(ctx.auth, thief.access_token, &u), CB_OK);
    T_EQ_INT(cb_auth_change_password(ctx.auth, &u, "a-guess-123", "thief-pass-1"), CB_AUTH_E_BAD_LOGIN);
    T_EQ_INT(cb_auth_change_password(ctx.auth, &u, NULL, "thief-pass-1"), CB_AUTH_E_BAD_LOGIN);
    // The owner changes it from their own session.
    g_fake_now += 1000;
    T_EQ_INT(cb_auth_verify_token(ctx.auth, mine.access_token, &u), CB_OK);
    T_EQ_INT(cb_auth_change_password(ctx.auth, &u, "first-pass-1", "second-pass-2"), CB_OK);
    // The other session is over; the caller's refreshes on.
    T_EQ_INT(cb_auth_refresh(ctx.auth, thief.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    cb_session mine2;
    T_EQ_INT(cb_auth_refresh(ctx.auth, mine.refresh_token, &mine2), CB_OK);
    T_EQ_STR(mine2.user.session_id, mine.user.session_id);
    // A session begun after the change is not touched by it.
    g_fake_now += 1000;
    cb_session later, later2;
    T_EQ_INT(cb_auth_signin(ctx.auth, "p@foolish.cards", "second-pass-2", &later), CB_OK);
    T_EQ_INT(cb_auth_refresh(ctx.auth, later.refresh_token, &later2), CB_OK);
    // Guessing the current password counts toward the lock, as sign-ins do.
    T_EQ_INT(cb_auth_verify_token(ctx.auth, later2.access_token, &u), CB_OK);
    for (int i = 0; i <= CB_AUTH_FAIL_FREE; i++)
        T_EQ_INT(cb_auth_change_password(ctx.auth, &u, "a-guess-123", "third-pass-3"), CB_AUTH_E_BAD_LOGIN);
    T_EQ_INT(cb_auth_change_password(ctx.auth, &u, "second-pass-2", "third-pass-3"), CB_AUTH_E_TOO_MANY);
    T_EQ_INT(cb_auth_signin(ctx.auth, "p@foolish.cards", "second-pass-2", &x), CB_AUTH_E_TOO_MANY);
    cb_auth_now = cb_now_ms;
    teardown();
}

// Sessions end, and the rows they leave behind are bounded (finding M2): a
// live token lives as long as its session, a spent one a week, and a session
// ends CB_AUTH_SESSION_MS after it began however often it is refreshed.
static void sessions_and_tokens_expire(void) {
    setup();
    g_fake_now = 1700000000000LL;
    cb_auth_now = fake_now;
    cb_session s0, s1, x;
    T_EQ_INT(cb_auth_signup(ctx.auth, "e@foolish.cards", "expire-pass-1", "E", &s0), CB_OK);
    const int64_t began = g_fake_now;
    int64_t lo, hi;
    auth_fakestore_token_expires(ctx.store, s0.user.session_id, &lo, &hi);
    T_EQ_INT(lo, began + CB_AUTH_SESSION_MS);
    g_fake_now += 1000;
    T_EQ_INT(cb_auth_refresh(ctx.auth, s0.refresh_token, &s1), CB_OK);
    auth_fakestore_token_expires(ctx.store, s0.user.session_id, &lo, &hi);
    T_EQ_INT(lo, g_fake_now + CB_AUTH_SPENT_TOKEN_MS);   // the spent one: a week
    T_EQ_INT(hi, began + CB_AUTH_SESSION_MS);            // the live one: the session's end
    // A spent token past its week is unknown, not reuse: nothing more is revoked.
    g_fake_now += CB_AUTH_SPENT_TOKEN_MS;
    T_EQ_INT(cb_auth_refresh(ctx.auth, s0.refresh_token, &x), CB_AUTH_E_BAD_REFRESH);
    T_EQ_INT(cb_auth_refresh(ctx.auth, s1.refresh_token, &s1), CB_OK);
    // Refreshed every day, the session still ends when its lifetime does.
    while (g_fake_now + 24 * 3600 * 1000LL < began + CB_AUTH_SESSION_MS) {
        g_fake_now += 24 * 3600 * 1000LL;
        T_EQ_INT(cb_auth_refresh(ctx.auth, s1.refresh_token, &s1), CB_OK);
    }
    g_fake_now = began + CB_AUTH_SESSION_MS;
    int rc = cb_auth_refresh(ctx.auth, s1.refresh_token, &x);
    T_CHECK(rc == CB_AUTH_E_BAD_REFRESH || rc == CB_AUTH_E_REFRESH_USED);
    cb_auth_now = cb_now_ms;
    teardown();
}

// A session begun before sessions were stored has token rows and no session
// row: it refreshes (nobody is signed out by the change), gets its row on
// the way, and can then be ended like any other.
static void a_session_from_before_sessions(void) {
    setup();
    cb_session s0, s1, x;
    T_EQ_INT(cb_auth_signup(ctx.auth, "o@foolish.cards", "older-pass-1", "O", &s0), CB_OK);
    // A token as the old code wrote it: no expiry, a session nobody stored.
    cb_refresh_row r;
    memset(&r, 0, sizeof r);
    unsigned char d[32];
    SHA256((const unsigned char *)"OLDTOKEN0000000000000000000000000000000000x", 43, d);
    cb_hex_encode(d, 32, r.token_hash, sizeof r.token_hash);
    cb_strlcpy(r.user_id, s0.user.id, sizeof r.user_id);
    cb_strlcpy(r.session_id, "99999999-9999-4999-8999-999999999999", sizeof r.session_id);
    r.created_at = cb_now_ms() - 1000;
    T_EQ_INT(CB_STORE_CALL(ctx.store, refresh_put, &r), CB_OK);
    int rows = auth_fakestore_session_rows(ctx.store);
    T_EQ_INT(cb_auth_refresh(ctx.auth, "OLDTOKEN0000000000000000000000000000000000x", &s1), CB_OK);
    T_EQ_STR(s1.user.session_id, "99999999-9999-4999-8999-999999999999");
    T_EQ_INT(auth_fakestore_session_rows(ctx.store), rows + 1);
    cb_user u;
    T_EQ_INT(cb_auth_verify_token(ctx.auth, s1.access_token, &u), CB_OK);
    T_EQ_INT(cb_auth_logout(ctx.auth, &u, CB_LOGOUT_LOCAL), CB_OK);
    T_EQ_INT(cb_auth_refresh(ctx.auth, s1.refresh_token, &x), CB_AUTH_E_REFRESH_USED);
    // The other session (the sign-up's) is untouched by that local logout.
    T_EQ_INT(cb_auth_refresh(ctx.auth, s0.refresh_token, &x), CB_OK);
    // An old token whose session would have ended by now (it carries no end
    // of its own): refused, and no session is made for it.
    SHA256((const unsigned char *)"OLDTOKEN1111111111111111111111111111111111x", 43, d);
    cb_hex_encode(d, 32, r.token_hash, sizeof r.token_hash);
    cb_strlcpy(r.session_id, "88888888-8888-4888-8888-888888888888", sizeof r.session_id);
    r.created_at = cb_now_ms() - CB_AUTH_SESSION_MS - 1;
    T_EQ_INT(CB_STORE_CALL(ctx.store, refresh_put, &r), CB_OK);
    rows = auth_fakestore_session_rows(ctx.store);
    T_EQ_INT(cb_auth_refresh(ctx.auth, "OLDTOKEN1111111111111111111111111111111111x", &x), CB_AUTH_E_BAD_REFRESH);
    T_EQ_INT(auth_fakestore_session_rows(ctx.store), rows);
    teardown();
}

static void verify_bearer_and_expiry(void) {
    setup();
    cb_session s;
    T_EQ_INT(cb_auth_signup(ctx.auth, "v@foolish.cards", "secret-11", "V", &s), CB_OK);
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

// Guessing one account's password (auth.h CB_AUTH_FAIL_*): five free
// failures in the window, then a lock that doubles; a locked account refuses
// even the right password, with no bcrypt and no write; a name with no
// account is never counted; a success clears the count.
static void signin_lockout(void) {
    setup();
    cb_session s, x;
    g_fake_now = 1700000000000LL;
    cb_auth_now = fake_now;
    T_EQ_INT(cb_auth_signup(ctx.auth, "g@foolish.cards", "right-pass-1", "G", &s), CB_OK);
    for (int i = 0; i < CB_AUTH_FAIL_FREE; i++) {
        g_fake_now += 1000;
        T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "wrong-pass-1", &x), CB_AUTH_E_BAD_LOGIN);
    }
    T_EQ_INT(auth_fakestore_fail_writes(ctx.store), CB_AUTH_FAIL_FREE);
    // Not locked yet: the right password still works, and clears the count.
    g_fake_now += 1000;
    T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "right-pass-1", &x), CB_OK);
    for (int i = 0; i < CB_AUTH_FAIL_FREE; i++) {
        g_fake_now += 1000;
        T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "wrong-pass-1", &x), CB_AUTH_E_BAD_LOGIN);
    }
    // The sixth failure locks for 30 s.
    g_fake_now += 1000;
    T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "wrong-pass-1", &x), CB_AUTH_E_BAD_LOGIN);
    int writes = auth_fakestore_fail_writes(ctx.store);
    g_fake_now += 1000;
    int64_t m0 = cb_mono_ms();
    T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "right-pass-1", &x), CB_AUTH_E_TOO_MANY);
    T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "wrong-pass-1", &x), CB_AUTH_E_TOO_MANY);
    T_CHECK(cb_mono_ms() - m0 < 10);                                  // no bcrypt while locked
    T_EQ_INT(auth_fakestore_fail_writes(ctx.store), writes);           // and no write
    // 30 s after the failure that locked it: open again.
    g_fake_now += CB_AUTH_LOCK_BASE_MS - 1000;
    // Each lock doubles (60 s, 120 s, ...) and never passes CB_AUTH_LOCK_MAX_MS.
    int64_t lock = 2 * CB_AUTH_LOCK_BASE_MS;
    for (int i = 0; i < 8; i++, lock = lock * 2 > CB_AUTH_LOCK_MAX_MS ? CB_AUTH_LOCK_MAX_MS : lock * 2) {
        T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "wrong-pass-1", &x), CB_AUTH_E_BAD_LOGIN);
        g_fake_now += lock - 1;
        T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "right-pass-1", &x), CB_AUTH_E_TOO_MANY);
        g_fake_now += 1;
    }
    T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "right-pass-1", &x), CB_OK);
    // Cleared: five free failures again.
    for (int i = 0; i < CB_AUTH_FAIL_FREE; i++) T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "wrong", &x), CB_AUTH_E_BAD_LOGIN);
    T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "right-pass-1", &x), CB_OK);
    // The window: a quiet one starts the count again.
    for (int i = 0; i < CB_AUTH_FAIL_FREE; i++) T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "wrong", &x), CB_AUTH_E_BAD_LOGIN);
    g_fake_now += CB_AUTH_FAIL_WINDOW_MS + 1;
    T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "wrong", &x), CB_AUTH_E_BAD_LOGIN);
    T_EQ_INT(cb_auth_signin(ctx.auth, "g@foolish.cards", "right-pass-1", &x), CB_OK);
    // A name with no account: never counted, never written, never locked.
    writes = auth_fakestore_fail_writes(ctx.store);
    for (int i = 0; i < 3 * CB_AUTH_FAIL_FREE; i++)
        T_EQ_INT(cb_auth_signin(ctx.auth, "ghost@foolish.cards", "wrong", &x), CB_AUTH_E_BAD_LOGIN);
    T_EQ_INT(auth_fakestore_fail_writes(ctx.store), writes);
    cb_auth_now = cb_now_ms;
    teardown();
}

int main(void) {
    T_RUN(signup_then_signin);
    T_RUN(signup_refusals);
    T_RUN(signin_lockout);
    T_RUN(refresh_rotates_and_reuse_revokes);
    T_RUN(logout_password_delete);
    T_RUN(password_change_needs_the_current_one_and_ends_the_rest);
    T_RUN(sessions_and_tokens_expire);
    T_RUN(a_session_from_before_sessions);
    T_RUN(verify_bearer_and_expiry);
    return T_DONE();
}
