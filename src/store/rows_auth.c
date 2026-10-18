// rows_auth.c - the auth rows over the item layer (rows_auth.h).
#include "rows_auth.h"

#include <stdio.h>
#include <string.h>

#define FAILS_TTL_MS (3600LL * 1000)   // store.h cb_signin_fails: an hour after the last write
#define KEY_MAX 400

static const char *key(char *out, size_t cap, const char *prefix, const char *id) {
    snprintf(out, cap, "%s%s", prefix, id ? id : "");
    return out;
}

static int put(cb_store *s, cb_ikey k, const cb_av *a, int n, int64_t ttl_ms, cb_icond cond) {
    cb_iwrite w = { .op = CB_IW_PUT, .key = k, .attrs = a, .n_attrs = n, .ttl_ms = ttl_ms, .cond = cond };
    return s->ops->item_write(s, &w, NULL);
}

// ---- sessions --------------------------------------------------------------------------

int cb_rows_session_put(cb_store *s, const cb_session_row *r) {
    char pk[KEY_MAX];
    const cb_av a[] = {
        CB_AVS("id", r->id), CB_AVS("user_id", r->user_id), CB_AVN("created_at", r->created_at),
        CB_AVBOOL("revoked", r->revoked), CB_AVN("gen", r->gen),
    };
    return put(s, (cb_ikey){ key(pk, sizeof pk, "SESSION#", r->id), "SESSION" }, a, 5, r->expires_at, CB_IF_ANY);
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
               r->expires_at > 0 ? r->expires_at : 0, CB_IF_ABSENT);
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
                    .n_sets = 2, .ttl_ms = now_ms + spent_ms, .ttl_how = CB_SET_TO, .cond = CB_IF_EXISTS };
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

typedef struct {
    cb_store *s;
    int       rc;
} forget;

static bool forget_one(const cb_item *it, void *ud) {
    forget *f = ud;
    char pk[KEY_MAX], sk[64];
    if (!cb_item_s(it, CB_ITEM_PK, pk, sizeof pk) || !cb_item_s(it, CB_ITEM_SK, sk, sizeof sk)) return true;
    cb_iwrite w = { .op = CB_IW_DELETE, .key = { pk, sk } };
    int rc = f->s->ops->item_write(f->s, &w, NULL);
    if (rc != CB_OK) f->rc = rc;
    return rc == CB_OK;
}

int cb_rows_refresh_forget_user(cb_store *s, const char *user_id) {
    char g1[KEY_MAX];
    forget f = { s, CB_OK };
    int rc = s->ops->item_query(s, 1, key(g1, sizeof g1, "USERREFRESH#", user_id), NULL, true, 0, forget_one, &f);
    return rc != CB_OK ? rc : f.rc;
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
    int rc = put(s, (cb_ikey){ key(pk, sizeof pk, "AUTHFAIL#", email), "FAILS" }, a, 3, now_ms + FAILS_TTL_MS, CB_IF_ANY);
    return rc == CB_E_CONFLICT ? CB_E_DB : rc;
}

int cb_rows_signin_fails_clear(cb_store *s, const char *email) {
    char pk[KEY_MAX];
    cb_iwrite w = { .op = CB_IW_DELETE, .key = { key(pk, sizeof pk, "AUTHFAIL#", email), "FAILS" } };
    int rc = s->ops->item_write(s, &w, NULL);
    return rc == CB_E_CONFLICT ? CB_E_DB : rc;
}
