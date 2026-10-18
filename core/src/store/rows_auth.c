// rows_auth.c - the accounts and the auth rows over the item layer
// (rows_auth.h). An account's product items join its signup and its delete
// through the store's cb_store_app (store.h).
#include "rows_auth.h"

#include "store_priv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAILS_TTL_MS (3600LL * 1000)   // store.h cb_signin_fails: an hour after the last write
#define KEY_MAX 400

static const char *key(char *out, size_t cap, const char *prefix, const char *id) {
    snprintf(out, cap, "%s%s", prefix, id ? id : "");
    return out;
}

static int put(cb_store *s, cb_ikey k, const cb_av *a, int n, int64_t ttl_ms, bool absent) {
    cb_iwrite w = { .op = CB_IW_PUT, .key = k, .attrs = a, .n_attrs = n, .ttl_ms = ttl_ms };
    if (absent) w.cond[w.n_cond++] = CB_IF_ABSENT;
    return s->ops->item_write(s, &w, NULL);
}

// A time-ordered sort key: the ms, 15 digits, then the id.
static const char *tsk(char *out, size_t cap, int64_t ms, const char *id) {
    snprintf(out, cap, "%015" PRId64 "#%s", ms < 0 ? (int64_t)0 : ms, id ? id : "");
    return out;
}

// ---- accounts ------------------------------------------------------------------------------

bool cb_rows_user_of(const cb_item *it, cb_user_row *u) {
    memset(u, 0, sizeof *u);
    if (!cb_item_s(it, "id", u->id, sizeof u->id) || !cb_item_s(it, "email", u->email, sizeof u->email) ||
        !cb_item_s(it, "password_hash", u->password_hash, sizeof u->password_hash))
        return false;
    cb_item_s(it, "username", u->username, sizeof u->username);
    u->created_at = cb_item_n_or(it, "created_at", 0);
    u->last_sign_in_at = cb_item_n_or(it, "last_sign_in_at", 0);
    u->session_gen = cb_item_n_or(it, "session_gen", 0);
    cb_item_s(it, "kept_session", u->kept_session, sizeof u->kept_session);
    return true;
}

// The account's two items, as a signup (fresh: each must be absent) or an
// import writes them: EMAIL#<email> USER (the id; signup's uniqueness is
// the condition on it), and USER#<id> PROFILE (listed on GSI2 USERS by
// creation). The strings they point at live in `a`.
typedef struct {
    char  pk[2][KEY_MAX], gsk[KEY_MAX];
    cb_av email[1], profile[8];
} acct_items;

static void acct_writes(cb_iwrite w[2], acct_items *a, const cb_user_row *u, bool fresh) {
    a->email[0] = CB_AVS("user_id", u->id);
    w[0] = (cb_iwrite){ .op = CB_IW_PUT, .key = { key(a->pk[0], KEY_MAX, "EMAIL#", u->email), "USER" }, .attrs = a->email,
                        .n_attrs = 1 };
    int n = 0;
    a->profile[n++] = CB_AVS("gsi2pk", "USERS");
    a->profile[n++] = CB_AVS("gsi2sk", tsk(a->gsk, sizeof a->gsk, u->created_at, u->id));
    a->profile[n++] = CB_AVS("id", u->id);
    a->profile[n++] = CB_AVS("email", u->email);
    a->profile[n++] = CB_AVS("password_hash", u->password_hash);
    if (u->username[0]) a->profile[n++] = CB_AVS("username", u->username);
    a->profile[n++] = CB_AVN("created_at", u->created_at);
    if (u->last_sign_in_at) a->profile[n++] = CB_AVN("last_sign_in_at", u->last_sign_in_at);
    w[1] = (cb_iwrite){ .op = CB_IW_PUT, .key = { key(a->pk[1], KEY_MAX, "USER#", u->id), "PROFILE" }, .attrs = a->profile,
                        .n_attrs = n };
    if (fresh) {
        w[0].cond[w[0].n_cond++] = CB_IF_ABSENT;
        w[1].cond[w[1].n_cond++] = CB_IF_ABSENT;
    }
}

int cb_rows_user_get(cb_store *s, const char *id, cb_user_row *out) {
    char pk[KEY_MAX];
    cb_item it;
    cb_item_init(&it);
    int rc = s->ops->item_get(s, (cb_ikey){ key(pk, sizeof pk, "USER#", id), "PROFILE" }, true, &it);
    if (rc == CB_OK && !cb_rows_user_of(&it, out)) rc = CB_E_DB;
    cb_item_free(&it);
    return rc;
}

int cb_rows_user_by_email(cb_store *s, const char *email, cb_user_row *out) {
    char pk[KEY_MAX];
    cb_uuid id;
    cb_item it;
    cb_item_init(&it);
    int rc = s->ops->item_get(s, (cb_ikey){ key(pk, sizeof pk, "EMAIL#", email), "USER" }, true, &it);
    if (rc == CB_OK && !cb_item_s(&it, "user_id", id, sizeof id)) rc = CB_E_DB;
    cb_item_free(&it);
    return rc == CB_OK ? cb_rows_user_get(s, id, out) : rc;
}

// One transaction: the account's two items, and the product's after them.
int cb_rows_user_create(cb_store *s, const cb_user_row *u) {
    cb_iwrite w[2];
    acct_items *a = calloc(1, sizeof *a);
    if (!a) return CB_E_NOMEM;
    acct_writes(w, a, u, true);
    int failed = -1;
    int rc = s->app && s->app->signup_txn ? s->app->signup_txn(s, u, w, 2, &failed)
                                          : s->ops->item_txn(s, w, 2, &failed, NULL);
    free(a);
    // A refused account item is the email taken (or the id, a UUID: never);
    // a refused product item is the store's inconsistency.
    return rc == CB_E_CONFLICT ? (failed == 0 || failed == 1 ? CB_E_CONFLICT : CB_E_DB) : rc;
}

int cb_rows_user_import(cb_store *s, const cb_user_row *u) {
    cb_iwrite w[2];
    acct_items *a = calloc(1, sizeof *a);
    if (!a) return CB_E_NOMEM;
    acct_writes(w, a, u, false);
    int rc = s->ops->item_txn(s, w, 2, NULL, NULL);
    free(a);
    return rc == CB_E_CONFLICT ? CB_E_DB : rc;
}

// A present-only update of the profile (CB_E_NOT_FOUND when the user is gone).
static int profile_update(cb_store *s, const char *id, const cb_iset *st, int n) {
    char pk[KEY_MAX];
    cb_iwrite w = { .op = CB_IW_UPDATE, .key = { key(pk, sizeof pk, "USER#", id), "PROFILE" }, .sets = st, .n_sets = n,
                    .cond = { CB_IF_EXISTS }, .n_cond = 1 };
    int rc = s->ops->item_write(s, &w, NULL);
    return rc == CB_E_CONFLICT ? CB_E_NOT_FOUND : rc;
}

// The new hash and, in the same write, every session so far ended but `keep`
// (session_gen one up; a missing one counts as 0).
int cb_rows_user_set_password(cb_store *s, const char *id, const char *hash, const char *keep_session) {
    const cb_iset st[] = {
        { CB_AVS("password_hash", hash), CB_SET_TO },
        { CB_AVN("session_gen", 1), CB_SET_INCR },
        { CB_AVS("kept_session", keep_session ? keep_session : ""), CB_SET_TO },
    };
    return profile_update(s, id, st, 3);
}

int cb_rows_user_end_sessions(cb_store *s, const char *id) {
    const cb_iset st[] = { { CB_AVN("session_gen", 1), CB_SET_INCR }, { CB_AVS("kept_session", ""), CB_SET_TO } };
    return profile_update(s, id, st, 2);
}

int cb_rows_user_touch_sign_in(cb_store *s, const char *id, int64_t at) {
    const cb_iset st[] = { { CB_AVN("last_sign_in_at", at), CB_SET_TO } };
    return profile_update(s, id, st, 1);
}

// Keys collected from a query, for a batch delete.
typedef struct {
    cb_ikey *keys;
    char   (*text)[2][KEY_MAX];
    int      n, cap;
    bool     oom;
} keylist;

static bool collect_key(const cb_item *it, void *ud) {
    keylist *l = ud;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        char (*t)[2][KEY_MAX] = realloc(l->text, (size_t)cap * sizeof *t);
        if (!t) { l->oom = true; return false; }
        l->text = t;
        l->cap = cap;
    }
    if (!cb_item_s(it, CB_ITEM_PK, l->text[l->n][0], KEY_MAX) || !cb_item_s(it, CB_ITEM_SK, l->text[l->n][1], KEY_MAX))
        return true;
    l->n++;
    return true;
}

// The cascade auth.admin.deleteUser had, by hand: what the product hangs off
// the account and the account's refresh tokens (found through GSI1), in one
// batch delete; then the account itself with the product's own account
// items, in one transaction. Not atomic across the two: a crash between
// them leaves orphans no reader can reach, and a retry finishes the job.
int cb_rows_user_delete(cb_store *s, const char *id) {
    cb_user_row u;
    int rc = cb_rows_user_get(s, id, &u);
    if (rc != CB_OK) return rc;
    keylist keys = { 0 };
    char g1[KEY_MAX];
    if (s->app && s->app->delete_keys) rc = s->app->delete_keys(s, id, collect_key, &keys);
    if (rc == CB_OK) {
        cb_iquery q = { .index = 1, .pk = key(g1, sizeof g1, "USERREFRESH#", id), .forward = true, .keys_only = true };
        rc = s->ops->item_query(s, &q, collect_key, &keys);
    }
    if (rc == CB_OK && keys.oom) rc = CB_E_NOMEM;
    if (rc == CB_OK && keys.n) {
        keys.keys = malloc((size_t)keys.n * sizeof *keys.keys);
        if (!keys.keys) rc = CB_E_NOMEM;
        for (int i = 0; rc == CB_OK && i < keys.n; i++) keys.keys[i] = (cb_ikey){ keys.text[i][0], keys.text[i][1] };
        if (rc == CB_OK) rc = s->ops->item_batch_delete(s, keys.keys, keys.n);
    }
    free(keys.keys);
    free(keys.text);
    if (rc != CB_OK) return rc;
    char pk[KEY_MAX], epk[KEY_MAX];
    const cb_iwrite w[2] = {
        { .op = CB_IW_DELETE, .key = { key(pk, sizeof pk, "USER#", u.id), "PROFILE" } },
        { .op = CB_IW_DELETE, .key = { key(epk, sizeof epk, "EMAIL#", u.email), "USER" } },
    };
    rc = s->app && s->app->delete_txn ? s->app->delete_txn(s, &u, w, 2) : s->ops->item_txn(s, w, 2, NULL, NULL);
    return rc == CB_E_CONFLICT ? CB_E_DB : rc;
}

typedef struct {
    const char *search;
    int         skip, limit, n;
    cb_user_fn  fn;
    void       *ud;
} users_walk;

static bool users_step(const cb_item *it, void *ud) {
    users_walk *w = ud;
    cb_user_row u;
    if (!cb_rows_user_of(it, &u)) return true;
    if (!cb_store_user_matches(&u, w->search)) return true;
    if (w->skip > 0) { w->skip--; return true; }
    w->n++;
    if (!w->fn(&u, w->ud)) return false;
    return w->limit <= 0 || w->n < w->limit;
}

// Newest first (GSI2 USERS by creation).
int cb_rows_users_list(cb_store *s, const char *search, int offset, int limit, cb_user_fn fn, void *ud) {
    users_walk w = { .search = search, .skip = offset > 0 ? offset : 0, .limit = limit, .fn = fn, .ud = ud };
    cb_iquery q = { .index = 2, .pk = "USERS" };
    int rc = s->ops->item_query(s, &q, users_step, &w);
    return rc == CB_OK ? w.n : rc;
}

// ---- sessions --------------------------------------------------------------------------

int cb_rows_session_put(cb_store *s, const cb_session_row *r) {
    char pk[KEY_MAX];
    const cb_av a[] = {
        CB_AVS("id", r->id), CB_AVS("user_id", r->user_id), CB_AVN("created_at", r->created_at),
        CB_AVBOOL("revoked", r->revoked), CB_AVN("gen", r->gen),
    };
    return put(s, (cb_ikey){ key(pk, sizeof pk, "SESSION#", r->id), "SESSION" }, a, 5, r->expires_at, false);
}

int cb_rows_session_get(cb_store *s, const char *id, int64_t now_ms, cb_session_row *out) {
    char pk[KEY_MAX];
    memset(out, 0, sizeof *out);
    cb_item it;
    cb_item_init(&it);
    // Consistent: a revoke just written is seen.
    int rc = s->ops->item_get(s, (cb_ikey){ key(pk, sizeof pk, "SESSION#", id), "SESSION" }, true, &it);
    if (rc == CB_OK) {
        if (!cb_item_s(&it, "id", out->id, sizeof out->id)) rc = CB_E_NOT_FOUND;
        cb_item_s(&it, "user_id", out->user_id, sizeof out->user_id);
        out->created_at = cb_item_n_or(&it, "created_at", 0);
        out->expires_at = cb_item_n_or(&it, CB_ITEM_TTL_MS, 0);
        out->gen = cb_item_n_or(&it, "gen", 0);
        cb_item_bool(&it, "revoked", &out->revoked);
        if (out->expires_at <= now_ms) rc = CB_E_NOT_FOUND;   // TTL deletion lags by up to days
        if (rc != CB_OK) memset(out, 0, sizeof *out);
    }
    cb_item_free(&it);
    return rc;
}

// Ended in one write; a session with no item yet gets one (store.h session_revoke).
int cb_rows_session_revoke(cb_store *s, const char *id, const char *user_id, int64_t expires_at) {
    char pk[KEY_MAX];
    const cb_iset sets[] = {
        { CB_AVBOOL("revoked", true), CB_SET_TO },
        { CB_AVS("id", id), CB_SET_IF_ABSENT },
        { CB_AVS("user_id", user_id), CB_SET_IF_ABSENT },
    };
    cb_iwrite w = { .op = CB_IW_UPDATE, .key = { key(pk, sizeof pk, "SESSION#", id), "SESSION" }, .sets = sets,
                    .n_sets = 3, .ttl_ms = expires_at, .ttl_how = CB_SET_IF_ABSENT };
    int rc = s->ops->item_write(s, &w, NULL);
    return rc == CB_E_CONFLICT ? CB_E_DB : rc;
}

// ---- refresh tokens --------------------------------------------------------------------------

int cb_rows_refresh_put(cb_store *s, const cb_refresh_row *r) {
    char pk[KEY_MAX], g1[KEY_MAX];
    const cb_av a[] = {
        CB_AVS(CB_ITEM_GSI1PK, key(g1, sizeof g1, "USERREFRESH#", r->user_id)), CB_AVS(CB_ITEM_GSI1SK, r->token_hash),
        CB_AVS("token_hash", r->token_hash), CB_AVS("user_id", r->user_id), CB_AVS("session_id", r->session_id),
        CB_AVN("created_at", r->created_at), CB_AVBOOL("revoked", r->revoked),
    };
    return put(s, (cb_ikey){ key(pk, sizeof pk, "REFRESH#", r->token_hash), "TOKEN" }, a, 7,
               r->expires_at > 0 ? r->expires_at : 0, true);
}

int cb_rows_refresh_take(cb_store *s, const char *hash, int64_t now_ms, int64_t spent_ms, cb_refresh_row *out) {
    char pk[KEY_MAX];
    memset(out, 0, sizeof *out);
    // Spent in one conditional write: the first take stamps revoked_at, a
    // later one keeps it; a spent token is kept spent_ms more, for reuse
    // detection, then the TTL takes it. The token as it was comes back.
    const cb_iset sets[] = {
        { CB_AVBOOL("revoked", true), CB_SET_TO },
        { CB_AVN("revoked_at", now_ms), CB_SET_IF_ABSENT },
    };
    cb_iwrite w = { .op = CB_IW_UPDATE, .key = { key(pk, sizeof pk, "REFRESH#", hash), "TOKEN" }, .sets = sets,
                    .n_sets = 2, .ttl_ms = now_ms + spent_ms, .ttl_how = CB_SET_TO, .cond = { CB_IF_EXISTS }, .n_cond = 1 };
    cb_item old;
    cb_item_init(&old);
    int rc = s->ops->item_write(s, &w, &old);
    if (rc == CB_E_CONFLICT) rc = CB_E_NOT_FOUND;   // no such token
    if (rc == CB_OK) {
        if (!cb_item_s(&old, "token_hash", out->token_hash, sizeof out->token_hash) ||
            !cb_item_s(&old, "user_id", out->user_id, sizeof out->user_id))
            rc = CB_E_DB;
        cb_item_s(&old, "session_id", out->session_id, sizeof out->session_id);
        out->created_at = cb_item_n_or(&old, "created_at", 0);
        cb_item_bool(&old, "revoked", &out->revoked);
        out->revoked_at = cb_item_n_or(&old, "revoked_at", 0);
        out->expires_at = cb_item_n_or(&old, CB_ITEM_TTL_MS, 0);
        // Past its time but not yet swept by the TTL: as good as gone.
        if (rc == CB_OK && out->expires_at > 0 && out->expires_at <= now_ms) rc = CB_E_NOT_FOUND;
    }
    cb_item_free(&old);
    return rc;
}

// ---- failed sign-ins ------------------------------------------------------------------------

int cb_rows_signin_fails_get(cb_store *s, const char *email, int64_t now_ms, cb_signin_fails *out) {
    char pk[KEY_MAX];
    memset(out, 0, sizeof *out);
    cb_item it;
    cb_item_init(&it);
    int rc = s->ops->item_get(s, (cb_ikey){ key(pk, sizeof pk, "AUTHFAIL#", email), "FAILS" }, true, &it);
    int64_t n, expires;
    // TTL deletion lags by up to days: a row past its time reads as none.
    if (rc == CB_OK && (!cb_item_n(&it, "n", &n) || !cb_item_n(&it, CB_ITEM_TTL_MS, &expires) || expires <= now_ms))
        rc = CB_E_NOT_FOUND;
    if (rc == CB_OK) {
        out->n = n < 0 ? 0 : n > INT32_MAX ? INT32_MAX : (int32_t)n;
        out->last_at = cb_item_n_or(&it, "last_at", 0);
        out->locked_until = cb_item_n_or(&it, "locked_until", 0);
    }
    cb_item_free(&it);
    return rc;
}

int cb_rows_signin_fails_put(cb_store *s, const char *email, const cb_signin_fails *f, int64_t now_ms) {
    char pk[KEY_MAX];
    const cb_av a[] = { CB_AVN("n", f->n), CB_AVN("last_at", f->last_at), CB_AVN("locked_until", f->locked_until) };
    int rc = put(s, (cb_ikey){ key(pk, sizeof pk, "AUTHFAIL#", email), "FAILS" }, a, 3, now_ms + FAILS_TTL_MS, false);
    return rc == CB_E_CONFLICT ? CB_E_DB : rc;
}

int cb_rows_signin_fails_clear(cb_store *s, const char *email) {
    char pk[KEY_MAX];
    cb_iwrite w = { .op = CB_IW_DELETE, .key = { key(pk, sizeof pk, "AUTHFAIL#", email), "FAILS" } };
    int rc = s->ops->item_write(s, &w, NULL);
    return rc == CB_E_CONFLICT ? CB_E_DB : rc;
}
