// lib_auth_fakestore.c - the auth tests' in-RAM store (lib_auth_fakestore.h).
#include "lib_auth_fakestore.h"

#include "util/str.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_USERS  64
#define FAKE_TOKENS 256

typedef struct {
    pthread_mutex_t mu;
    cb_user_row users[FAKE_USERS];
    bool user_live[FAKE_USERS];
    cb_refresh_row tokens[FAKE_TOKENS];
    int n_tokens;
} fake;

static fake *F(cb_store *s) { return (fake *)s->impl; }

static int find_user(fake *f, const char *id) {
    for (int i = 0; i < FAKE_USERS; i++)
        if (f->user_live[i] && strcmp(f->users[i].id, id) == 0) return i;
    return -1;
}

static int f_user_get(cb_store *s, const char *id, cb_user_row *out) {
    fake *f = F(s);
    pthread_mutex_lock(&f->mu);
    int i = find_user(f, id);
    if (i >= 0) *out = f->users[i];
    pthread_mutex_unlock(&f->mu);
    return i >= 0 ? CB_OK : CB_E_NOT_FOUND;
}

static int f_user_by_email(cb_store *s, const char *email, cb_user_row *out) {
    fake *f = F(s);
    int rc = CB_E_NOT_FOUND;
    pthread_mutex_lock(&f->mu);
    for (int i = 0; i < FAKE_USERS; i++)
        if (f->user_live[i] && strcmp(f->users[i].email, email) == 0) { *out = f->users[i]; rc = CB_OK; break; }
    pthread_mutex_unlock(&f->mu);
    return rc;
}

static int f_user_create(cb_store *s, const cb_user_row *u) {
    fake *f = F(s);
    int rc = CB_E_FULL;
    pthread_mutex_lock(&f->mu);
    for (int i = 0; i < FAKE_USERS; i++)
        if (f->user_live[i] && (strcmp(f->users[i].email, u->email) == 0 || strcmp(f->users[i].id, u->id) == 0)) {
            rc = CB_E_CONFLICT;
            goto out;
        }
    for (int i = 0; i < FAKE_USERS; i++)
        if (!f->user_live[i]) { f->users[i] = *u; f->user_live[i] = true; rc = CB_OK; break; }
out:
    pthread_mutex_unlock(&f->mu);
    return rc;
}

static int f_user_set_password(cb_store *s, const char *id, const char *hash) {
    fake *f = F(s);
    pthread_mutex_lock(&f->mu);
    int i = find_user(f, id);
    if (i >= 0) cb_strlcpy(f->users[i].password_hash, hash, sizeof f->users[i].password_hash);
    pthread_mutex_unlock(&f->mu);
    return i >= 0 ? CB_OK : CB_E_NOT_FOUND;
}

static int f_user_touch(cb_store *s, const char *id, int64_t at) {
    fake *f = F(s);
    pthread_mutex_lock(&f->mu);
    int i = find_user(f, id);
    if (i >= 0) f->users[i].last_sign_in_at = at;
    pthread_mutex_unlock(&f->mu);
    return i >= 0 ? CB_OK : CB_E_NOT_FOUND;
}

static int f_user_delete(cb_store *s, const char *id) {
    fake *f = F(s);
    pthread_mutex_lock(&f->mu);
    int i = find_user(f, id);
    if (i >= 0) {
        f->user_live[i] = false;
        int k = 0;
        for (int j = 0; j < f->n_tokens; j++)
            if (strcmp(f->tokens[j].user_id, id) != 0) f->tokens[k++] = f->tokens[j];
        f->n_tokens = k;
    }
    pthread_mutex_unlock(&f->mu);
    return i >= 0 ? CB_OK : CB_E_NOT_FOUND;
}

static int f_refresh_put(cb_store *s, const cb_refresh_row *r) {
    fake *f = F(s);
    int rc = CB_OK;
    pthread_mutex_lock(&f->mu);
    for (int j = 0; j < f->n_tokens; j++)
        if (strcmp(f->tokens[j].token_hash, r->token_hash) == 0) rc = CB_E_CONFLICT;
    if (rc == CB_OK && f->n_tokens == FAKE_TOKENS) rc = CB_E_FULL;
    if (rc == CB_OK) f->tokens[f->n_tokens++] = *r;
    pthread_mutex_unlock(&f->mu);
    return rc;
}

static int f_refresh_take(cb_store *s, const char *hash, int64_t now_ms, cb_refresh_row *out) {
    fake *f = F(s);
    int rc = CB_E_NOT_FOUND;
    pthread_mutex_lock(&f->mu);
    for (int j = 0; j < f->n_tokens; j++)
        if (strcmp(f->tokens[j].token_hash, hash) == 0) {
            *out = f->tokens[j];
            if (!f->tokens[j].revoked) f->tokens[j].revoked_at = now_ms;
            f->tokens[j].revoked = true;
            rc = CB_OK;
            break;
        }
    pthread_mutex_unlock(&f->mu);
    return rc;
}

static int f_refresh_revoke_session(cb_store *s, const char *sid) {
    fake *f = F(s);
    pthread_mutex_lock(&f->mu);
    for (int j = 0; j < f->n_tokens; j++)
        if (strcmp(f->tokens[j].session_id, sid) == 0) {
            f->tokens[j].revoked = true;
            f->tokens[j].revoked_at = 0;
        }
    pthread_mutex_unlock(&f->mu);
    return CB_OK;
}

static int f_refresh_revoke_user(cb_store *s, const char *uid) {
    fake *f = F(s);
    pthread_mutex_lock(&f->mu);
    for (int j = 0; j < f->n_tokens; j++)
        if (strcmp(f->tokens[j].user_id, uid) == 0) {
            f->tokens[j].revoked = true;
            f->tokens[j].revoked_at = 0;
        }
    pthread_mutex_unlock(&f->mu);
    return CB_OK;
}

static void f_close(cb_store *s) { auth_fakestore_free(s); }

static const cb_store_ops FAKE_OPS = {
    .name = "auth-fake",
    .close = f_close,
    .user_get = f_user_get,
    .user_by_email = f_user_by_email,
    .user_create = f_user_create,
    .user_set_password = f_user_set_password,
    .user_touch_sign_in = f_user_touch,
    .user_delete = f_user_delete,
    .refresh_put = f_refresh_put,
    .refresh_take = f_refresh_take,
    .refresh_revoke_session = f_refresh_revoke_session,
    .refresh_revoke_user = f_refresh_revoke_user,
};

cb_store *auth_fakestore_new(void) {
    cb_store *s = calloc(1, sizeof *s);
    fake *f = calloc(1, sizeof *f);
    if (!s || !f) { free(s); free(f); return NULL; }
    pthread_mutex_init(&f->mu, NULL);
    s->ops = &FAKE_OPS;
    s->impl = f;
    return s;
}

void auth_fakestore_free(cb_store *s) {
    if (!s) return;
    fake *f = F(s);
    pthread_mutex_destroy(&f->mu);
    free(f);
    free(s);
}

int auth_fakestore_session_tokens(cb_store *s, const char *session_id, int *revoked) {
    fake *f = F(s);
    int n = 0, r = 0;
    pthread_mutex_lock(&f->mu);
    for (int j = 0; j < f->n_tokens; j++)
        if (strcmp(f->tokens[j].session_id, session_id) == 0) { n++; r += f->tokens[j].revoked; }
    pthread_mutex_unlock(&f->mu);
    *revoked = r;
    return n;
}
