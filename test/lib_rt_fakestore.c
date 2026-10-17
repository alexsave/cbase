// lib_rt_fakestore.c - (lib_rt_fakestore.h).
#include "lib_rt_fakestore.h"

#include "aws/apigw_mgmt.h"
#include "realtime/rt_deps.h"
#include "util/str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

rtf_world rtf = { .mu = PTHREAD_MUTEX_INITIALIZER };

#define LOCK() pthread_mutex_lock(&rtf.mu)
#define UNLOCK() pthread_mutex_unlock(&rtf.mu)

static int f_is_member(cb_store *s, const char *game, const char *user) {
    LOCK();
    int r = rtf.is_member_rc;
    if (!r)
        for (int i = 0; i < rtf.n_members; i++)
            if (strcmp(rtf.members[i].game, game) == 0 && strcmp(rtf.members[i].user, user) == 0) r = 1;
    UNLOCK();
    return r;
}

static int f_chat_put(cb_store *s, const cb_chat_row *r) {
    LOCK();
    int rc = CB_OK;
    for (int i = 0; i < rtf.n_chat; i++)
        if (strcmp(rtf.chat[i].id, r->id) == 0) rc = CB_E_CONFLICT;
    if (rc == CB_OK && rtf.n_chat < RTF_MAX) {
        rtf.chat[rtf.n_chat] = *r;
        rtf.chat[rtf.n_chat].message = strdup(r->message ? r->message : "");
        rtf.n_chat++;
    }
    UNLOCK();
    return rc;
}

static int f_chat_list(cb_store *s, const char *game, int limit, cb_chat_fn fn, void *ud) {
    int n = 0;
    for (int i = 0; i < rtf.n_chat; i++)
        if (strcmp(rtf.chat[i].game_id, game) == 0) { n++; if (!fn(&rtf.chat[i], ud)) break; }
    return n;
}

static int f_bots_list(cb_store *s, cb_bot_fn fn, void *ud) {
    for (int i = 0; i < rtf.n_bots; i++)
        if (!fn(&rtf.bots[i], ud)) break;
    return rtf.n_bots;
}

static int f_ratings_list(cb_store *s, cb_rating_fn fn, void *ud) {
    for (int i = 0; i < rtf.n_ratings; i++)
        if (!fn(&rtf.ratings[i], ud)) break;
    return rtf.n_ratings;
}

static int f_snapshots_query(cb_store *s, const cb_snapshot_query *q, cb_snapshot_fn fn, void *ud) {
    // rtf.snaps are kept oldest first.
    int n = 0;
    for (int k = 0; k < rtf.n_snaps; k++) {
        const cb_snapshot_row *r = &rtf.snaps[q->newest_first ? rtf.n_snaps - 1 - k : k];
        bool mine = false;
        for (int p = 0; p < r->n_players; p++) mine |= strcmp(r->player_ids[p], q->viewer_id) == 0;
        if (!mine || (q->game_id && strcmp(q->game_id, r->game_id) != 0) || (q->id && strcmp(q->id, r->id) != 0)) continue;
        if (q->limit && n >= q->limit) break;
        n++;
        if (!fn(r, ud)) break;
    }
    return n;
}

static int f_conn_put(cb_store *s, const cb_conn_row *was, const cb_conn_row *c) {
    LOCK();
    rtf.conn_puts++;
    int i = 0;
    while (i < rtf.n_conns && strcmp(rtf.conns[i].conn_id, c->conn_id) != 0) i++;
    int64_t stored = i < rtf.n_conns ? rtf.conns[i].version : 0;
    if (stored != was->version) {   // store.h conn_put: the compare-and-set
        rtf.conn_conflicts++;
        UNLOCK();
        return CB_E_CONFLICT;
    }
    if (i == rtf.n_conns && rtf.n_conns < RTF_MAX) rtf.n_conns++;
    if (i < RTF_MAX) {
        rtf.conns[i] = *c;
        rtf.conns[i].version = was->version + 1;
    }
    UNLOCK();
    return CB_OK;
}

int rtf_conn_find(const char *conn_id) {
    for (int i = 0; i < rtf.n_conns; i++)
        if (strcmp(rtf.conns[i].conn_id, conn_id) == 0) return i;
    return -1;
}

static int f_conn_get(cb_store *s, const char *id, cb_conn_row *out) {
    rtf.conn_gets++;
    int i = rtf_conn_find(id);
    int rc = CB_E_NOT_FOUND;
    if (i >= 0) {
        *out = rtf.conns[i];
        rc = CB_OK;
    }
    // A frame of the same connection that runs between this read and its
    // write (API Gateway's concurrent invocations), once.
    void (*between)(void *) = rtf.after_conn_get;
    rtf.after_conn_get = NULL;
    if (between) between(rtf.after_conn_get_ud);
    return rc;
}

static int f_conn_delete(cb_store *s, const char *id) {
    rtf.conn_deletes++;
    int i = rtf_conn_find(id);
    if (i < 0) return CB_E_NOT_FOUND;
    rtf.conns[i] = rtf.conns[--rtf.n_conns];
    return CB_OK;
}

static int f_conns_by_topic(cb_store *s, const char *topic, cb_conn_fn fn, void *ud) {
    int n = 0;
    for (int i = 0; i < rtf.n_conns; i++)
        for (int k = 0; k < rtf.conns[i].n_topics; k++)
            if (strcmp(rtf.conns[i].topics[k], topic) == 0) { n++; if (!fn(&rtf.conns[i], ud)) return n; }
    return n;
}

static const cb_store_ops FAKE_OPS = {
    .name = "rtf",
    .is_member = f_is_member,
    .chat_put = f_chat_put,
    .chat_list = f_chat_list,
    .bots_list = f_bots_list,
    .ratings_list = f_ratings_list,
    .snapshots_query = f_snapshots_query,
    .conn_put = f_conn_put,
    .conn_get = f_conn_get,
    .conn_delete = f_conn_delete,
    .conns_by_topic = f_conns_by_topic,
};

// ---- the fake dependencies ------------------------------------------------------------

static int d_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    memset(out, 0, sizeof *out);
    if (!cb_starts_with(jwt, "tok-") || strlen(jwt + 4) > CB_UUID_LEN) return CB_E_AUTH;
    cb_strlcpy(out->id, jwt + 4, sizeof out->id);
    snprintf(out->username, sizeof out->username, "U-%s", out->id);
    return CB_OK;
}

static void fill_view(const rtf_view *v, cb_view_row *out) {
    memset(out, 0, sizeof *out);
    cb_strlcpy(out->game_id, v->game_id, sizeof out->game_id);
    cb_strlcpy(out->player_id, v->player_id, sizeof out->player_id);
    out->view = malloc(v->view_len ? v->view_len : 1);
    memcpy(out->view, v->view, v->view_len);
    out->view_len = v->view_len;
    out->version = v->version;
    cb_strlcpy(out->status, v->status, sizeof out->status);
    out->updated_at = v->updated_at;
}

static int d_view(cb_ctx *ctx, const char *game, const char *viewer, cb_view_row *out) {
    for (int i = 0; i < rtf.n_views; i++)
        if (strcmp(rtf.views[i].game_id, game) == 0 && strcmp(rtf.views[i].player_id, viewer ? viewer : "") == 0) {
            fill_view(&rtf.views[i], out);
            return CB_OK;
        }
    return CB_E_NOT_FOUND;
}

static int d_post(const char *endpoint, const char *conn, const void *data, size_t len) {
    LOCK();
    int st = 200;
    for (int i = 0; i < 4; i++)
        if (strcmp(rtf.gone[i], conn) == 0) st = CB_APIGW_GONE;
    if (st == 200 && rtf.n_posts < RTF_MAX) {
        rtf_post *p = &rtf.posts[rtf.n_posts++];
        cb_strlcpy(p->conn_id, conn, sizeof p->conn_id);
        cb_strncpy0(p->frame, sizeof p->frame, data, len < sizeof p->frame ? len : 0);
    }
    UNLOCK();
    return st;
}

static int d_delete(const char *endpoint, const char *conn) {
    LOCK();
    rtf.n_deletes++;
    UNLOCK();
    return 200;
}

void rtf_reset(void) {
    LOCK();
    for (int i = 0; i < rtf.n_chat; i++) cb_chat_row_free(&rtf.chat[i]);
    for (int i = 0; i < rtf.n_snaps; i++) cb_snapshot_row_free(&rtf.snaps[i]);
    pthread_mutex_t mu = rtf.mu;
    memset(&rtf, 0, sizeof rtf);
    rtf.mu = mu;
    rtf.store.ops = &FAKE_OPS;
    UNLOCK();
}

void rtf_install(void) {
    rtf_reset();
    cb_rt_deps_g.verify_token = d_verify;
    cb_rt_deps_g.table_view = d_view;
    cb_rt_deps_g.apigw_post = d_post;
    cb_rt_deps_g.apigw_delete = d_delete;
}

void rtf_member(const char *game, const char *user) {
    cb_strlcpy(rtf.members[rtf.n_members].game, game, sizeof rtf.members[0].game);
    cb_strlcpy(rtf.members[rtf.n_members].user, user, sizeof rtf.members[0].user);
    rtf.n_members++;
}

void rtf_add_view(const char *game, const char *player, const char *status, int64_t version, int64_t updated_at,
                  const uint8_t *view, size_t n) {
    rtf_view *v = &rtf.views[rtf.n_views++];
    cb_strlcpy(v->game_id, game, sizeof v->game_id);
    cb_strlcpy(v->player_id, player ? player : "", sizeof v->player_id);
    cb_strlcpy(v->status, status, sizeof v->status);
    v->version = version;
    v->updated_at = updated_at;
    memcpy(v->view, view, n);
    v->view_len = n;
}

void rtf_add_chat(const char *game, const char *user, const char *message, int64_t created_at) {
    cb_chat_row r;
    memset(&r, 0, sizeof r);
    snprintf(r.id, sizeof r.id, "00000000-0000-4000-8000-%012d", rtf.n_chat + 1);
    cb_strlcpy(r.game_id, game, sizeof r.game_id);
    cb_strlcpy(r.user_id, user, sizeof r.user_id);
    r.message = (char *)message;
    r.created_at = created_at;
    f_chat_put(&rtf.store, &r);
}

void rtf_add_bot(const char *id, const char *nick, const char *key, int elo, int prev, int played, int64_t t) {
    cb_bot_row *b = &rtf.bots[rtf.n_bots++];
    memset(b, 0, sizeof *b);
    cb_strlcpy(b->id, id, sizeof b->id);
    cb_strlcpy(b->nickname, nick, sizeof b->nickname);
    cb_strlcpy(b->strategy_key, key, sizeof b->strategy_key);
    b->elo_rating = elo;
    b->previous_elo = prev;
    b->games_played = played;
    b->created_at = b->updated_at = t;
}

void rtf_add_rating(const char *user, const char *name, int elo, int prev, int played, int64_t t) {
    cb_rating_row *r = &rtf.ratings[rtf.n_ratings++];
    memset(r, 0, sizeof *r);
    cb_strlcpy(r->user_id, user, sizeof r->user_id);
    cb_strlcpy(r->username, name, sizeof r->username);
    r->elo_rating = elo;
    r->previous_elo = prev;
    r->games_played = played;
    r->updated_at = t;
}

void rtf_add_snap(const char *id, const char *game, const char *const *players, int n, const uint8_t *moves,
                  size_t mn, const uint8_t *extras, size_t en, int64_t created_at) {
    cb_snapshot_row *s = &rtf.snaps[rtf.n_snaps++];
    memset(s, 0, sizeof *s);
    cb_strlcpy(s->id, id, sizeof s->id);
    cb_strlcpy(s->game_id, game, sizeof s->game_id);
    for (int i = 0; i < n; i++) cb_strlcpy(s->player_ids[i], players[i], sizeof s->player_ids[i]);
    s->n_players = n;
    s->moves = malloc(mn ? mn : 1);
    memcpy(s->moves, moves, mn);
    s->moves_len = mn;
    if (extras) {
        s->extras = malloc(en ? en : 1);
        memcpy(s->extras, extras, en);
        s->extras_len = en;
    }
    s->created_at = created_at;
}
