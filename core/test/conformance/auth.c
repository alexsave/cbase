// conformance/auth.c - the auth rows' conformance suite (conformance/auth.h).
#include "conformance/auth.h"

#include "t.h"

#include "util/err.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static cb_store *S;
static unsigned RUN;   // this run's tag: in every id, hash, email and name the suite writes
static unsigned SEQ;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void pause_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// A fresh canonical id, and a fresh token hash (64 lowercase hex), each this run's own.
static void new_id(cb_uuid out) { snprintf(out, sizeof(cb_uuid), "%08x-0000-4000-8000-%012x", RUN, ++SEQ); }
static void new_hash(char out[65]) { snprintf(out, 65, "%08x%056x", RUN, ++SEQ); }

#define HASH0 "$2b$10$abcdefghijklmnopqrstuuabcdefghijklmnopqrstuvwxyz01234"
#define HASH1 "$2b$10$abcdefghijklmnopqrstuuzyxwvutsrqponmlkjihgfedcba98765"

// A new account, made through user_create: its email "<tag>@<run>.ca.example", its name "<TAG><run>".
static void new_user(cb_user_row *u, const char *tag, int64_t created_at) {
    memset(u, 0, sizeof *u);
    new_id(u->id);
    snprintf(u->email, sizeof u->email, "%s%u@ca%08x.example", tag, SEQ, RUN);
    snprintf(u->username, sizeof u->username, "%s%u", tag, SEQ);
    strcpy(u->password_hash, HASH0);
    u->created_at = created_at;
    T_EQ_INT(CB_STORE_CALL(S, user_create, u), CB_OK);
}

static void new_token(char hash[65], const cb_user_row *u, const char *session, int64_t created, int64_t expires) {
    new_hash(hash);
    cb_refresh_row r;
    memset(&r, 0, sizeof r);
    strcpy(r.token_hash, hash);
    strcpy(r.user_id, u->id);
    strcpy(r.session_id, session);
    r.created_at = created;
    r.expires_at = expires;
    T_EQ_INT(CB_STORE_CALL(S, refresh_put, &r), CB_OK);
}

static int take(const char *hash, int64_t now, int64_t spent, cb_refresh_row *out) {
    return CB_STORE_CALL(S, refresh_take, hash, now, spent, out);
}

// ---- refresh tokens (store.h refresh_take) -------------------------------------------------

#define SPENT 60000

static void refresh_is_taken_once(void) {
    int64_t t0 = now_ms();
    cb_user_row u;
    new_user(&u, "rot", t0);
    cb_uuid sess;
    new_id(sess);
    char h[65];
    new_token(h, &u, sess, t0, t0 + 3600000);
    cb_refresh_row r;
    // The first take: the token as it was, unspent, every field back.
    T_EQ_INT(take(h, t0 + 10, SPENT, &r), CB_OK);
    T_EQ_STR(r.token_hash, h);
    T_EQ_STR(r.user_id, u.id);
    T_EQ_STR(r.session_id, sess);
    T_EQ_INT(r.created_at, t0);
    T_CHECK(!r.revoked);
    T_EQ_INT(r.revoked_at, 0);
    T_EQ_INT(r.expires_at, t0 + 3600000);
    // Every later take: spent, at the first take's time, kept SPENT from the first take.
    for (int i = 1; i <= 3; i++) {
        T_EQ_INT(take(h, t0 + 10 + i * 1000, SPENT, &r), CB_OK);
        T_CHECK(r.revoked);
        T_EQ_INT(r.revoked_at, t0 + 10);
        T_EQ_STR(r.user_id, u.id);
        T_EQ_STR(r.session_id, sess);
        T_EQ_INT(r.expires_at, t0 + 10 + SPENT);   // a later take never keeps it longer
    }
    // A hash that is none, or not a hash at all.
    char none[65];
    new_hash(none);
    T_EQ_INT(take(none, t0, SPENT, &r), CB_E_NOT_FOUND);
    T_EQ_INT(r.user_id[0], 0);
    T_EQ_INT(take("00", t0, SPENT, &r), CB_E_NOT_FOUND);
    T_EQ_INT(take("", t0, SPENT, &r), CB_E_NOT_FOUND);
}

static void refresh_expiry(void) {
    int64_t t0 = now_ms();
    cb_user_row u;
    new_user(&u, "exp", t0 - 7200000);
    cb_uuid sess;
    new_id(sess);
    // Past its time (not yet swept): gone, and presenting it again does not bring it back.
    char old[65];
    new_token(old, &u, sess, t0 - 7200000, t0 - 1000);
    cb_refresh_row r;
    T_EQ_INT(take(old, t0, SPENT, &r), CB_E_NOT_FOUND);
    T_EQ_INT(r.user_id[0], 0);
    T_EQ_INT(take(old, t0 + 1, SPENT, &r), CB_E_NOT_FOUND);
    T_EQ_INT(take(old, t0 + 2, SPENT, &r), CB_E_NOT_FOUND);
    // At its very end it is over (expires_at <= now).
    char edge[65];
    new_token(edge, &u, sess, t0, t0 + 500);
    T_EQ_INT(take(edge, t0 + 500, SPENT, &r), CB_E_NOT_FOUND);
    // Spent, it lasts SPENT after the first take and then is gone, however often it was presented.
    char h[65];
    new_token(h, &u, sess, t0, t0 + 3600000);
    T_EQ_INT(take(h, t0, SPENT, &r), CB_OK);
    T_CHECK(!r.revoked);
    T_EQ_INT(take(h, t0 + SPENT - 1, SPENT, &r), CB_OK);
    T_CHECK(r.revoked);
    T_EQ_INT(take(h, t0 + SPENT, SPENT, &r), CB_E_NOT_FOUND);
    T_EQ_INT(take(h, t0 + SPENT + 1, SPENT, &r), CB_E_NOT_FOUND);
    // A token with no end of its own (an old row): never expires by itself.
    char forever[65];
    new_token(forever, &u, sess, t0, 0);
    T_EQ_INT(take(forever, t0 + 1000, SPENT, &r), CB_OK);
    T_CHECK(!r.revoked);
    T_EQ_INT(r.expires_at, 0);
}

// THE DOUBLE TAKE: several takes of one token at once; exactly one sees it unspent.
#define TAKERS 8
#define ROUNDS 12
typedef struct {
    const char     *hash;
    int64_t         now;
    volatile int   *go;
    int             rc;
    cb_refresh_row  r;
} taker;

static void *take_main(void *ud) {
    taker *t = ud;
    while (!__atomic_load_n(t->go, __ATOMIC_ACQUIRE)) {}
    t->rc = take(t->hash, t->now, SPENT, &t->r);
    return NULL;
}

static void refresh_double_take(void) {
    int64_t t0 = now_ms();
    cb_user_row u;
    new_user(&u, "dbl", t0);
    cb_uuid sess;
    new_id(sess);
    int lone_wins = 0;
    for (int round = 0; round < ROUNDS; round++) {
        char h[65];
        new_token(h, &u, sess, t0, t0 + 3600000);
        volatile int go = 0;
        taker tk[TAKERS];
        pthread_t th[TAKERS];
        for (int i = 0; i < TAKERS; i++) {
            tk[i] = (taker){ .hash = h, .now = t0 + 100 + i, .go = &go };
            T_EQ_INT(pthread_create(&th[i], NULL, take_main, &tk[i]), 0);
        }
        __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
        int fresh = 0, winner = -1;
        for (int i = 0; i < TAKERS; i++) pthread_join(th[i], NULL);
        for (int i = 0; i < TAKERS; i++) {
            T_EQ_INT(tk[i].rc, CB_OK);
            T_EQ_STR(tk[i].r.user_id, u.id);
            if (!tk[i].r.revoked) { fresh++; winner = i; }
        }
        lone_wins += fresh == 1;
        T_EQ_INT(fresh, 1);
        // Every loser was told when the winner took it.
        for (int i = 0; winner >= 0 && i < TAKERS; i++)
            if (i != winner) T_EQ_INT(tk[i].r.revoked_at, tk[winner].now);
    }
    T_EQ_INT(lone_wins, ROUNDS);
}

// ---- the sessions' generation (store.h user_set_password, user_end_sessions) ---------------

static void generation_moves_in_the_same_write(void) {
    cb_user_row u, g;
    new_user(&u, "gen", now_ms());
    T_EQ_INT(CB_STORE_CALL(S, user_get, u.id, &g), CB_OK);
    T_EQ_INT(g.session_gen, 0);
    T_EQ_STR(g.kept_session, "");
    // A global sign-out: one up, none kept.
    T_EQ_INT(CB_STORE_CALL(S, user_end_sessions, u.id), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_get, u.id, &g), CB_OK);
    T_EQ_INT(g.session_gen, 1);
    T_EQ_STR(g.kept_session, "");
    T_EQ_STR(g.password_hash, HASH0);   // the password is not touched
    // A password change: the new hash, one up, the caller's session kept.
    cb_uuid keep;
    new_id(keep);
    T_EQ_INT(CB_STORE_CALL(S, user_set_password, u.id, HASH1, keep), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_get, u.id, &g), CB_OK);
    T_EQ_INT(g.session_gen, 2);
    T_EQ_STR(g.kept_session, keep);
    T_EQ_STR(g.password_hash, HASH1);
    // The next global sign-out keeps none, the kept one too.
    T_EQ_INT(CB_STORE_CALL(S, user_end_sessions, u.id), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_get, u.id, &g), CB_OK);
    T_EQ_INT(g.session_gen, 3);
    T_EQ_STR(g.kept_session, "");
    T_EQ_STR(g.password_hash, HASH1);
    // The rest of the account is as it was.
    T_EQ_STR(g.email, u.email);
    T_EQ_STR(g.username, u.username);
    T_EQ_INT(g.created_at, u.created_at);
    // Another account's generation did not move.
    cb_user_row v;
    new_user(&v, "gen", now_ms());
    T_EQ_INT(CB_STORE_CALL(S, user_end_sessions, u.id), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_get, v.id, &g), CB_OK);
    T_EQ_INT(g.session_gen, 0);
    // An account that is none: neither writes one.
    cb_uuid none;
    new_id(none);
    T_EQ_INT(CB_STORE_CALL(S, user_end_sessions, none), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(S, user_set_password, none, HASH1, ""), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(S, user_get, none, &g), CB_E_NOT_FOUND);
}

// ---- the account's delete (store.h user_delete) --------------------------------------------

typedef struct {
    char    hash[2][65];
    cb_uuid sess;
    char    subject[64];
} account_rows;

static void hang_rows(const cb_user_row *u, account_rows *a, int64_t t0) {
    new_id(a->sess);
    for (int i = 0; i < 2; i++) new_token(a->hash[i], u, a->sess, t0, t0 + 3600000);
    snprintf(a->subject, sizeof a->subject, "sub-%08x-%u", RUN, ++SEQ);
    cb_identity_row id;
    memset(&id, 0, sizeof id);
    id.idp = 1;
    strcpy(id.subject, a->subject);
    strcpy(id.user_id, u->id);
    strcpy(id.client_id, "com.example.web");
    strcpy(id.email, u->email);
    id.created_at = id.updated_at = t0;
    T_EQ_INT(CB_STORE_CALL(S, identity_put, &id, true), CB_OK);
}

static bool count_one(const cb_identity_row *r, void *ud) {
    (void)r;
    ++*(int *)ud;
    return true;
}

static void delete_takes_its_own_and_only_its_own(void) {
    int64_t t0 = now_ms();
    cb_user_row a, b, g;
    new_user(&a, "del", t0);
    new_user(&b, "del", t0);
    account_rows ra, rb;
    hang_rows(&a, &ra, t0);
    hang_rows(&b, &rb, t0);
    // One of a's tokens already spent: it goes too.
    cb_refresh_row r;
    T_EQ_INT(take(ra.hash[0], t0 + 1, SPENT, &r), CB_OK);
    // GSI1 may lag (conformance/store.h): wait until a's tokens and identity are listed under a.
    int n = 0;
    for (int64_t until = now_ms() + CS_INDEX_WAIT_MS; now_ms() < until; pause_ms(50)) {
        n = 0;
        if (CB_STORE_CALL(S, identities_of, a.id, count_one, &n) == 1) break;
    }
    T_EQ_INT(n, 1);
    T_EQ_INT(CB_STORE_CALL(S, user_delete, a.id), CB_OK);
    // a is gone: the account, its email, its tokens (spent or not), its identity.
    T_EQ_INT(CB_STORE_CALL(S, user_get, a.id, &g), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(S, user_by_email, a.email, &g), CB_E_NOT_FOUND);
    for (int i = 0; i < 2; i++) T_EQ_INT(take(ra.hash[i], t0 + 2, SPENT, &r), CB_E_NOT_FOUND);
    cb_identity_row id;
    T_EQ_INT(CB_STORE_CALL(S, identity_get, 1, ra.subject, &id), CB_E_NOT_FOUND);
    n = 0;
    T_EQ_INT(CB_STORE_CALL(S, identities_of, a.id, count_one, &n), 0);
    // b is as it was.
    T_EQ_INT(CB_STORE_CALL(S, user_get, b.id, &g), CB_OK);
    T_EQ_STR(g.email, b.email);
    T_EQ_INT(CB_STORE_CALL(S, user_by_email, b.email, &g), CB_OK);
    T_EQ_STR(g.id, b.id);
    T_EQ_INT(CB_STORE_CALL(S, identity_get, 1, rb.subject, &id), CB_OK);
    T_EQ_STR(id.user_id, b.id);
    for (int i = 0; i < 2; i++) {
        T_EQ_INT(take(rb.hash[i], t0 + 2, SPENT, &r), CB_OK);
        T_CHECK(!r.revoked);
        T_EQ_STR(r.user_id, b.id);
    }
    // A second delete finds none; the email is free for a new account.
    T_EQ_INT(CB_STORE_CALL(S, user_delete, a.id), CB_E_NOT_FOUND);
    cb_user_row again = a;
    new_id(again.id);
    T_EQ_INT(CB_STORE_CALL(S, user_create, &again), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_by_email, a.email, &g), CB_OK);
    T_EQ_STR(g.id, again.id);
}

// ---- the import (store.h user_import) ----------------------------------------------------

static void import_rules(void) {
    int64_t t0 = now_ms();
    cb_user_row u, g;
    memset(&u, 0, sizeof u);
    new_id(u.id);
    snprintf(u.email, sizeof u.email, "imp%u@ca%08x.example", SEQ, RUN);
    snprintf(u.username, sizeof u.username, "imp%u", SEQ);
    strcpy(u.password_hash, HASH0);
    u.created_at = t0 - 5000;
    u.last_sign_in_at = t0 - 100;
    // A new account, as given, every field.
    T_EQ_INT(CB_STORE_CALL(S, user_import, &u), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_get, u.id, &g), CB_OK);
    T_EQ_STR(g.email, u.email);
    T_EQ_STR(g.username, u.username);
    T_EQ_STR(g.password_hash, HASH0);
    T_EQ_INT(g.created_at, u.created_at);
    T_EQ_INT(g.last_sign_in_at, u.last_sign_in_at);
    T_EQ_INT(g.session_gen, 0);
    T_EQ_INT(CB_STORE_CALL(S, user_by_email, u.email, &g), CB_OK);
    T_EQ_STR(g.id, u.id);
    // Signed out everywhere since: a re-import of the older row never lowers the generation.
    T_EQ_INT(CB_STORE_CALL(S, user_end_sessions, u.id), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_end_sessions, u.id), CB_OK);
    strcpy(u.password_hash, HASH1);
    T_EQ_INT(CB_STORE_CALL(S, user_import, &u), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_get, u.id, &g), CB_OK);
    T_EQ_INT(g.session_gen, 2);
    T_EQ_STR(g.kept_session, "");
    T_EQ_STR(g.password_hash, HASH1);   // the rest is replaced
    // A row exported after more sign-outs raises it, and its kept session goes with it.
    u.session_gen = 5;
    new_id(u.kept_session);
    T_EQ_INT(CB_STORE_CALL(S, user_import, &u), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_get, u.id, &g), CB_OK);
    T_EQ_INT(g.session_gen, 5);
    T_EQ_STR(g.kept_session, u.kept_session);
    // A lower one again: the stored generation and its kept session stay.
    cb_user_row low = u;
    low.session_gen = 1;
    new_id(low.kept_session);
    T_EQ_INT(CB_STORE_CALL(S, user_import, &low), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_get, u.id, &g), CB_OK);
    T_EQ_INT(g.session_gen, 5);
    T_EQ_STR(g.kept_session, u.kept_session);
    // A changed email: the account answers to the new one only.
    char old_email[CB_EMAIL_MAX + 1];
    strcpy(old_email, u.email);
    snprintf(u.email, sizeof u.email, "moved%u@ca%08x.example", ++SEQ, RUN);
    T_EQ_INT(CB_STORE_CALL(S, user_import, &u), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_by_email, u.email, &g), CB_OK);
    T_EQ_STR(g.id, u.id);
    T_EQ_INT(CB_STORE_CALL(S, user_by_email, old_email, &g), CB_E_NOT_FOUND);
    // Another account's email: refused, and that email still signs in to its own account.
    cb_user_row other;
    new_user(&other, "imp", t0);
    cb_user_row thief = u;
    new_id(thief.id);
    snprintf(thief.username, sizeof thief.username, "thief%u", ++SEQ);   // only the email is another's
    strcpy(thief.email, other.email);
    int rc = CB_STORE_CALL(S, user_import, &thief);
    T_CHECK(rc != CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_get, thief.id, &g), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(S, user_by_email, other.email, &g), CB_OK);
    T_EQ_STR(g.id, other.id);
    T_EQ_STR(g.password_hash, HASH0);
    // The same for an existing account moved onto another's email.
    cb_user_row onto = u;
    strcpy(onto.email, other.email);
    T_CHECK(CB_STORE_CALL(S, user_import, &onto) != CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, user_by_email, other.email, &g), CB_OK);
    T_EQ_STR(g.id, other.id);
    T_EQ_INT(CB_STORE_CALL(S, user_by_email, u.email, &g), CB_OK);
    T_EQ_STR(g.id, u.id);
    // A malformed id: refused, nothing written.
    cb_user_row bad = u;
    strcpy(bad.id, "not-a-uuid");
    snprintf(bad.email, sizeof bad.email, "bad%u@ca%08x.example", ++SEQ, RUN);
    T_EQ_INT(CB_STORE_CALL(S, user_import, &bad), CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(S, user_by_email, bad.email, &g), CB_E_NOT_FOUND);
    strcpy(bad.id, "AAAAAAAA-0000-4000-8000-000000000001");   // not the canonical lowercase
    T_EQ_INT(CB_STORE_CALL(S, user_import, &bad), CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(S, user_by_email, bad.email, &g), CB_E_NOT_FOUND);
}

// ---- the admin's list (store.h users_list) ------------------------------------------------

typedef struct {
    cb_uuid ids[8];
    int     n, stop_at;
} seen;

static bool see(const cb_user_row *u, void *ud) {
    seen *s = ud;
    if (s->n < 8) strcpy(s->ids[s->n], u->id);
    s->n++;
    return s->stop_at <= 0 || s->n < s->stop_at;
}

static void users_list_newest_first(void) {
    int64_t t0 = now_ms();
    cb_user_row u[3];
    char tag[32];
    snprintf(tag, sizeof tag, "lst%08x", RUN);
    for (int i = 0; i < 3; i++) new_user(&u[i], tag, t0 + i);
    char needle[32];
    snprintf(needle, sizeof needle, "LST%08X", RUN);   // case-blind
    // GSI2 may lag: wait until all three are listed.
    seen s = { 0 };
    for (int64_t until = now_ms() + CS_INDEX_WAIT_MS; now_ms() < until; pause_ms(50)) {
        memset(&s, 0, sizeof s);
        if (CB_STORE_CALL(S, users_list, needle, 0, 0, see, &s) == 3) break;
    }
    T_EQ_INT(s.n, 3);
    T_EQ_STR(s.ids[0], u[2].id);
    T_EQ_STR(s.ids[1], u[1].id);
    T_EQ_STR(s.ids[2], u[0].id);
    // From an offset, to a limit.
    memset(&s, 0, sizeof s);
    T_EQ_INT(CB_STORE_CALL(S, users_list, needle, 1, 1, see, &s), 1);
    T_EQ_STR(s.ids[0], u[1].id);
    // Stopped by the callback.
    memset(&s, 0, sizeof s);
    s.stop_at = 2;
    T_EQ_INT(CB_STORE_CALL(S, users_list, needle, 0, 0, see, &s), 2);
    // By one account's name (contained, case-blind), and by none.
    memset(&s, 0, sizeof s);
    T_EQ_INT(CB_STORE_CALL(S, users_list, u[1].email, 0, 0, see, &s), 1);
    T_EQ_STR(s.ids[0], u[1].id);
    memset(&s, 0, sizeof s);
    T_EQ_INT(CB_STORE_CALL(S, users_list, "no-such-account-anywhere", 0, 0, see, &s), 0);
}

// ---- failed sign-ins and readiness ---------------------------------------------------------

static void signin_fails_clear(void) {
    int64_t t0 = now_ms();
    char email[64];
    snprintf(email, sizeof email, "fails%u@ca%08x.example", ++SEQ, RUN);
    cb_signin_fails f = { .n = 3, .last_at = t0, .locked_until = t0 + 1000 }, g;
    T_EQ_INT(CB_STORE_CALL(S, signin_fails_put, email, &f, t0), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, signin_fails_get, email, t0, &g), CB_OK);
    T_EQ_INT(g.n, 3);
    T_EQ_INT(g.locked_until, t0 + 1000);
    T_EQ_INT(CB_STORE_CALL(S, signin_fails_clear, email), CB_OK);
    T_EQ_INT(CB_STORE_CALL(S, signin_fails_get, email, t0, &g), CB_E_NOT_FOUND);
    T_EQ_INT(g.n, 0);
    T_EQ_INT(CB_STORE_CALL(S, signin_fails_clear, email), CB_OK);   // none: still CB_OK
}

static void ready_answers(void) { T_EQ_INT(S->ops->ready ? S->ops->ready(S) : CB_E_NOTIMPL, CB_OK); }   // GET /ready's call

int ca_auth_run(const cs_backend *b, int *checks) {
    RUN = ((unsigned)getpid() << 8) ^ (unsigned)SEQ ^ 0xca000000u;
    S = b->open ? b->open(b->ud) : NULL;
    T_CHECK(S != NULL);
    if (S) {
        fprintf(stderr, "conformance/auth: %s, run %08x\n", b->name, RUN);
        T_RUN(refresh_is_taken_once);
        T_RUN(refresh_expiry);
        T_RUN(refresh_double_take);
        T_RUN(generation_moves_in_the_same_write);
        T_RUN(delete_takes_its_own_and_only_its_own);
        T_RUN(import_rules);
        T_RUN(users_list_newest_first);
        T_RUN(signin_fails_clear);
        T_RUN(ready_answers);
        cb_store_close(S);
        S = NULL;
    }
    if (checks) *checks = t_checks;
    fprintf(stderr, "conformance/auth: %s, %d cases, %d checks, %d failed\n", b->name, t_tests, t_checks, t_failed);
    return t_failed;
}
