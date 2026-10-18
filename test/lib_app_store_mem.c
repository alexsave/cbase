// lib_app_store_mem.c - the in-RAM cb_store for the app's tests
// (lib_app_store_mem.h).
#include "lib_app_store_mem.h"

#include "table.h"
#include "util/uuid.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TM_GAMES 128
#define TM_USERS 64
#define TM_BOTS  64
#define TM_SNAPS 64

typedef struct {
    bool        used;
    cb_game_row row;
    int         n_humans, n_bots;
    cb_uuid     humans[CB_MAX_SEATS], bots[CB_MAX_SEATS];
    cb_uuid     lease_token;
    int64_t     lease_until;
} tm_game;

typedef struct {
    bool          used;
    cb_user_row   user;
    bool          has_rating;
    cb_rating_row rating;
} tm_user;

typedef struct {
    pthread_mutex_t mu;
    tm_hooks hooks;
    tm_game  games[TM_GAMES];
    tm_user  users[TM_USERS];
    int      n_bots;
    cb_bot_row bots[TM_BOTS];
    int      n_snaps;
    cb_snapshot_row snaps[TM_SNAPS];
} tm_impl;

#define IMPL(s) ((tm_impl *)(s)->impl)

static uint8_t *dupb(const uint8_t *p, size_t n) {
    if (!n) return NULL;
    uint8_t *d = malloc(n);
    memcpy(d, p, n);
    return d;
}

static void row_copy(cb_game_row *dst, const cb_game_row *src) {
    *dst = *src;
    dst->state = dupb(src->state, src->state_len);
    dst->roster = dupb(src->roster, src->roster_len);
    dst->logs = dupb(src->logs, src->logs_len);
}

static tm_game *find_game(tm_impl *m, const char *id) {
    for (int i = 0; i < TM_GAMES; i++) if (m->games[i].used && strcmp(m->games[i].row.id, id) == 0) return &m->games[i];
    return NULL;
}

static tm_user *find_user(tm_impl *m, const char *id) {
    for (int i = 0; i < TM_USERS; i++) if (m->users[i].used && strcmp(m->users[i].user.id, id) == 0) return &m->users[i];
    return NULL;
}

static cb_bot_row *find_bot(tm_impl *m, const char *id) {
    for (int i = 0; i < m->n_bots; i++) if (strcmp(m->bots[i].id, id) == 0) return &m->bots[i];
    return NULL;
}

// ---- users ---------------------------------------------------------------------------

static int m_user_get(cb_store *s, const char *id, cb_user_row *out) {
    tm_impl *m = IMPL(s);
    pthread_mutex_lock(&m->mu);
    tm_user *u = find_user(m, id);
    if (u) *out = u->user;
    pthread_mutex_unlock(&m->mu);
    return u ? CB_OK : CB_E_NOT_FOUND;
}

static int m_user_create(cb_store *s, const cb_user_row *row) {
    tm_impl *m = IMPL(s);
    int rc = CB_E_FULL;
    pthread_mutex_lock(&m->mu);
    if (find_user(m, row->id)) rc = CB_E_CONFLICT;
    else for (int i = 0; i < TM_USERS; i++) {
        if (m->users[i].used) continue;
        tm_user *u = &m->users[i];
        memset(u, 0, sizeof *u);
        u->used = true;
        u->user = *row;
        u->has_rating = true;
        snprintf(u->rating.user_id, sizeof u->rating.user_id, "%s", row->id);
        snprintf(u->rating.username, sizeof u->rating.username, "%s", row->username);
        u->rating.elo_rating = TABLE_RATING_START;
        u->rating.previous_elo = -1;
        rc = CB_OK;
        break;
    }
    pthread_mutex_unlock(&m->mu);
    return rc;
}

static int m_user_delete(cb_store *s, const char *id) {
    tm_impl *m = IMPL(s);
    pthread_mutex_lock(&m->mu);
    tm_user *u = find_user(m, id);
    if (u) memset(u, 0, sizeof *u);
    for (int i = 0; i < TM_GAMES; i++) {
        tm_game *g = &m->games[i];
        if (!g->used) continue;
        for (int j = 0; j < g->n_humans; j++) {
            if (strcmp(g->humans[j], id) != 0) continue;
            memmove(&g->humans[j], &g->humans[j + 1], (size_t)(g->n_humans - j - 1) * sizeof g->humans[0]);
            g->n_humans--;
            break;
        }
    }
    pthread_mutex_unlock(&m->mu);
    return u ? CB_OK : CB_E_NOT_FOUND;
}

// ---- games ---------------------------------------------------------------------------

static int m_game_get(cb_store *s, const char *id, cb_game_row *out) {
    tm_impl *m = IMPL(s);
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, id);
    if (g) row_copy(out, &g->row);
    pthread_mutex_unlock(&m->mu);
    return g ? CB_OK : CB_E_NOT_FOUND;
}

static int m_game_create(cb_store *s, const cb_game_row *row, const char *creator) {
    tm_impl *m = IMPL(s);
    int rc = CB_E_FULL;
    pthread_mutex_lock(&m->mu);
    if (find_game(m, row->id)) rc = CB_E_CONFLICT;
    else for (int i = 0; i < TM_GAMES; i++) {
        tm_game *g = &m->games[i];
        if (g->used) continue;
        memset(g, 0, sizeof *g);
        g->used = true;
        row_copy(&g->row, row);
        g->row.version = 0;
        g->n_humans = 1;
        snprintf(g->humans[0], sizeof g->humans[0], "%s", creator);
        rc = CB_OK;
        break;
    }
    pthread_mutex_unlock(&m->mu);
    return rc;
}

static void apply_rating(tm_impl *m, const cb_game_commit *c, int i) {
    if (c->rated_is_bot[i]) {
        cb_bot_row *b = find_bot(m, c->rated_ids[i]);
        if (!b) return;
        b->previous_elo = b->elo_rating;
        b->elo_rating = b->elo_rating + c->rating_delta[i] < 0 ? 0 : b->elo_rating + c->rating_delta[i];
        b->games_played++;
        return;
    }
    tm_user *u = find_user(m, c->rated_ids[i]);
    if (!u) return;   // a deleted account has no row to rate
    if (!u->has_rating) {
        u->has_rating = true;
        snprintf(u->rating.user_id, sizeof u->rating.user_id, "%s", u->user.id);
        u->rating.elo_rating = TABLE_RATING_START;
        u->rating.previous_elo = -1;
    }
    u->rating.previous_elo = u->rating.elo_rating;
    int32_t r = u->rating.elo_rating + c->rating_delta[i];
    u->rating.elo_rating = r < 0 ? 0 : r;
    u->rating.games_played++;
    if (c->rated_names[i][0]) snprintf(u->rating.username, sizeof u->rating.username, "%s", c->rated_names[i]);
}

static bool listed_in(const cb_uuid *ids, int n, const char *id) {
    for (int i = 0; i < n; i++)
        if (strcmp(ids[i], id) == 0) return true;
    return false;
}

// The prev seats a commit hands over (cb_game_commit prev_known) must hold
// every seat stored: DynamoDB writes the difference from them without
// reading, and a stored seat they miss would never be deleted.
static bool stored_within_prev(const tm_game *g, const cb_game_commit *c) {
    for (int i = 0; i < g->n_humans; i++)
        if (!listed_in(c->prev_humans, c->n_prev_humans, g->humans[i])) return false;
    for (int i = 0; i < g->n_bots; i++)
        if (!listed_in(c->prev_bots, c->n_prev_bots, g->bots[i])) return false;
    return true;
}

// The seats as DynamoDB writes them from prev: the ones prev has and the
// commit does not are deleted, the ones the commit has and prev does not are
// put, the rest are left as stored. A wrong prev leaves the membership wrong,
// which the tests that read it see.
static void apply_seat_diff(cb_uuid *ids, int *n, const cb_uuid *prev, int n_prev, const cb_uuid *now, int n_now) {
    for (int i = 0; i < *n;)
        if (listed_in(prev, n_prev, ids[i]) && !listed_in(now, n_now, ids[i])) {
            memmove(&ids[i], &ids[i + 1], (size_t)(*n - i - 1) * sizeof ids[0]);
            (*n)--;
        } else {
            i++;
        }
    for (int i = 0; i < n_now; i++)
        if (!listed_in(prev, n_prev, now[i]) && !listed_in(ids, *n, now[i]) && *n < CB_MAX_SEATS)
            memcpy(ids[(*n)++], now[i], sizeof(cb_uuid));
}

static int m_game_commit(cb_store *s, const cb_game_commit *c) {
    tm_impl *m = IMPL(s);
    int rc;
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, c->row->id);
    if (g && m->hooks.steal_commits > 0) {
        // Another writer committed first.
        m->hooks.steal_commits--;
        g->row.version++;
    }
    if (!g) rc = CB_E_NOT_FOUND;
    else if (g->row.version != c->expected_version) { rc = CB_E_CONFLICT; m->hooks.conflicts++; }
    else if (c->prev_known && !stored_within_prev(g, c)) { rc = CB_E_STATE; m->hooks.prev_wrong++; }
    else {
        cb_game_row_free(&g->row);
        row_copy(&g->row, c->row);
        if (c->prev_known) {
            apply_seat_diff(g->humans, &g->n_humans, c->prev_humans, c->n_prev_humans, c->humans, c->n_humans);
            apply_seat_diff(g->bots, &g->n_bots, c->prev_bots, c->n_prev_bots, c->bots, c->n_bots);
        } else {
            g->n_humans = c->n_humans;
            g->n_bots = c->n_bots;
            memcpy(g->humans, c->humans, sizeof g->humans);
            memcpy(g->bots, c->bots, sizeof g->bots);
        }
        for (int i = 0; i < c->n_rated; i++) apply_rating(m, c, i);
        m->hooks.commits++;
        if (c->prev_known) m->hooks.prev_given++;
        rc = CB_OK;
    }
    pthread_mutex_unlock(&m->mu);
    return rc;
}

static int m_game_delete(cb_store *s, const char *id, int64_t expected) {
    tm_impl *m = IMPL(s);
    int rc;
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, id);
    if (!g) rc = CB_E_NOT_FOUND;
    else if (g->row.version != expected) rc = CB_E_CONFLICT;
    else { cb_game_row_free(&g->row); memset(g, 0, sizeof *g); rc = CB_OK; }
    for (int i = 0; rc == CB_OK && i < m->n_snaps; i++)
        if (strcmp(m->snaps[i].game_id, id) == 0) m->snaps[i].game_id[0] = 0;
    pthread_mutex_unlock(&m->mu);
    return rc;
}

static int m_games_of_user(cb_store *s, const char *user, int limit, cb_member_fn fn, void *ud) {
    tm_impl *m = IMPL(s);
    cb_member_row rows[TM_GAMES];
    int n = 0;
    pthread_mutex_lock(&m->mu);
    for (int i = 0; i < TM_GAMES; i++) {
        tm_game *g = &m->games[i];
        if (!g->used) continue;
        for (int j = 0; j < g->n_humans; j++) {
            if (strcmp(g->humans[j], user) != 0) continue;
            snprintf(rows[n].game_id, sizeof rows[n].game_id, "%s", g->row.id);
            rows[n++].updated_at = g->row.updated_at;
        }
    }
    pthread_mutex_unlock(&m->mu);
    // Most recently updated first.
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && rows[j].updated_at > rows[j - 1].updated_at; j--) {
            cb_member_row t = rows[j]; rows[j] = rows[j - 1]; rows[j - 1] = t;
        }
    int k = 0;
    for (; k < n && (limit <= 0 || k < limit); k++) if (!fn(&rows[k], ud)) { k++; break; }
    return k;
}

static int m_is_member(cb_store *s, const char *game, const char *user) {
    tm_impl *m = IMPL(s);
    int r = 0;
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, game);
    for (int j = 0; g && j < g->n_humans; j++) if (strcmp(g->humans[j], user) == 0) r = 1;
    pthread_mutex_unlock(&m->mu);
    return r;
}

static int m_game_members(cb_store *s, const char *game, cb_name_fn fn, void *ud) {
    tm_impl *m = IMPL(s);
    cb_uuid ids[CB_MAX_SEATS];
    int n = 0;
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, game);
    for (int j = 0; g && j < g->n_humans && n < CB_MAX_SEATS; j++) memcpy(ids[n++], g->humans[j], sizeof ids[0]);
    pthread_mutex_unlock(&m->mu);
    int k = 0;
    while (k < n) {
        bool more = fn(ids[k], ud);
        k++;
        if (!more) break;
    }
    return k;
}

static int m_games_needing_bots(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn, void *ud) {
    tm_impl *m = IMPL(s);
    char ids[TM_GAMES][CB_GAME_ID_MAX + 1];
    int n = 0;
    pthread_mutex_lock(&m->mu);
    for (int i = 0; i < TM_GAMES && n < limit; i++) {
        const cb_game_row *r = &m->games[i].row;
        if (m->games[i].used && r->needs_bots && r->updated_at < stale_before && r->last_commit_at > active_after)
            snprintf(ids[n++], sizeof ids[0], "%s", r->id);
    }
    pthread_mutex_unlock(&m->mu);
    for (int i = 0; i < n; i++) if (!fn(ids[i], ud)) return i + 1;
    return n;
}

static int m_lease_acquire(cb_store *s, const char *id, int64_t now, int64_t ttl, cb_uuid token) {
    tm_impl *m = IMPL(s);
    int rc;
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, id);
    if (!g || !g->row.needs_bots) rc = CB_E_NOT_FOUND;
    else if (g->lease_until > now) rc = CB_E_CONFLICT;
    else {
        cb_uuid_v4(g->lease_token);
        g->lease_until = now + ttl;
        g->row.updated_at = now;
        snprintf(token, sizeof(cb_uuid), "%s", g->lease_token);
        rc = CB_OK;
    }
    pthread_mutex_unlock(&m->mu);
    return rc;
}

static int m_lease_renew(cb_store *s, const char *id, const char *token, int64_t now, int64_t ttl) {
    tm_impl *m = IMPL(s);
    int rc;
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, id);
    if (!g || strcmp(g->lease_token, token) != 0) rc = CB_E_CONFLICT;
    else { g->lease_until = now + ttl; g->row.updated_at = now; rc = CB_OK; }
    pthread_mutex_unlock(&m->mu);
    return rc;
}

void (*tm_on_release)(const char *game_id);

static int m_lease_release(cb_store *s, const char *id, const char *token) {
    if (tm_on_release) tm_on_release(id);
    tm_impl *m = IMPL(s);
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, id);
    if (g && strcmp(g->lease_token, token) == 0) g->lease_until = 0;
    pthread_mutex_unlock(&m->mu);
    return CB_OK;
}

static int m_bots_idle(cb_store *s, const char *id, int64_t version) {
    tm_impl *m = IMPL(s);
    int rc = CB_E_CONFLICT;
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, id);
    if (g && g->row.version == version) { g->row.needs_bots = false; rc = CB_OK; }
    if (g && rc == CB_OK) m->hooks.bots_idle++;
    pthread_mutex_unlock(&m->mu);
    return rc;
}

static int m_lease_held(cb_store *s, const char *id, int64_t now) {
    tm_impl *m = IMPL(s);
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, id);
    int held = g && g->row.needs_bots && g->lease_until > now ? 1 : 0;   // as m_lease_acquire refuses
    pthread_mutex_unlock(&m->mu);
    return held;
}

// ---- bots and ratings ----------------------------------------------------------------------

static int m_bots_list(cb_store *s, cb_bot_fn fn, void *ud) {
    tm_impl *m = IMPL(s);
    cb_bot_row rows[TM_BOTS];
    pthread_mutex_lock(&m->mu);
    int n = m->n_bots;
    memcpy(rows, m->bots, sizeof rows);
    pthread_mutex_unlock(&m->mu);
    for (int i = 0; i < n; i++) if (!fn(&rows[i], ud)) return i + 1;
    return n;
}

static int m_ratings_for(cb_store *s, const char *const *ids, int n, int32_t *out) {
    tm_impl *m = IMPL(s);
    if (m->hooks.fail_ratings) return CB_E_DB;
    pthread_mutex_lock(&m->mu);
    for (int i = 0; i < n; i++) {
        cb_bot_row *b = find_bot(m, ids[i]);
        tm_user *u = b ? NULL : find_user(m, ids[i]);
        out[i] = b ? b->elo_rating : (u && u->has_rating) ? u->rating.elo_rating : TABLE_RATING_NONE;
    }
    pthread_mutex_unlock(&m->mu);
    return CB_OK;
}

static int m_rating_set_username(cb_store *s, const char *id, const char *name) {
    tm_impl *m = IMPL(s);
    pthread_mutex_lock(&m->mu);
    tm_user *u = find_user(m, id);
    if (u && u->has_rating) snprintf(u->rating.username, sizeof u->rating.username, "%s", name ? name : "");
    pthread_mutex_unlock(&m->mu);
    return u ? CB_OK : CB_E_NOT_FOUND;
}

// ---- snapshots ---------------------------------------------------------------------------

static int m_snapshot_put(cb_store *s, const cb_snapshot_row *r) {
    tm_impl *m = IMPL(s);
    int rc = CB_E_FULL;
    pthread_mutex_lock(&m->mu);
    m->hooks.snapshots++;
    if (m->n_snaps < TM_SNAPS) {
        cb_snapshot_row *d = &m->snaps[m->n_snaps++];
        *d = *r;
        d->moves = dupb(r->moves, r->moves_len);
        d->extras = dupb(r->extras, r->extras_len);
        rc = CB_OK;
    }
    pthread_mutex_unlock(&m->mu);
    return rc;
}

static int m_snapshots_query(cb_store *s, const cb_snapshot_query *q, cb_snapshot_fn fn, void *ud) {
    tm_impl *m = IMPL(s);
    int n = 0;
    pthread_mutex_lock(&m->mu);
    for (int i = 0; i < m->n_snaps; i++) {
        const cb_snapshot_row *r = &m->snaps[q->newest_first ? m->n_snaps - 1 - i : i];
        bool mine = false;
        for (int j = 0; j < r->n_players; j++) if (strcmp(r->player_ids[j], q->viewer_id) == 0) mine = true;
        if (!mine || (q->game_id && strcmp(q->game_id, r->game_id) != 0) || (q->id && strcmp(q->id, r->id) != 0)) continue;
        n++;
        if (!fn(r, ud) || (q->limit && n >= q->limit)) break;
    }
    pthread_mutex_unlock(&m->mu);
    return n;
}

static int m_snapshot_set_extras(cb_store *s, const char *id, const uint8_t *extras, size_t extras_len) {
    tm_impl *m = IMPL(s);
    int rc = CB_E_NOT_FOUND;
    pthread_mutex_lock(&m->mu);
    for (int i = 0; i < m->n_snaps; i++) {
        cb_snapshot_row *r = &m->snaps[i];
        if (strcmp(r->id, id) != 0) continue;
        free(r->extras);
        r->extras = dupb(extras, extras_len);
        r->extras_len = extras_len;
        rc = CB_OK;
    }
    pthread_mutex_unlock(&m->mu);
    return rc;
}

// ---- the store ----------------------------------------------------------------------------

static void m_close(cb_store *s) {
    tm_impl *m = IMPL(s);
    for (int i = 0; i < TM_GAMES; i++) if (m->games[i].used) cb_game_row_free(&m->games[i].row);
    for (int i = 0; i < m->n_snaps; i++) cb_snapshot_row_free(&m->snaps[i]);
    pthread_mutex_destroy(&m->mu);
    free(m);
    free(s);
}

static const cb_store_ops tm_ops = {
    .name = "mem",
    .close = m_close,
    .user_get = m_user_get,
    .user_create = m_user_create,
    .user_delete = m_user_delete,
    .game_get = m_game_get,
    .games_get_views = cb_store_games_get_each,
    .game_create = m_game_create,
    .game_commit = m_game_commit,
    .game_delete = m_game_delete,
    .games_of_user = m_games_of_user,
    .is_member = m_is_member,
    .game_members = m_game_members,
    .games_needing_bots = m_games_needing_bots,
    .lease_acquire = m_lease_acquire,
    .lease_renew = m_lease_renew,
    .lease_release = m_lease_release,
    .bots_idle = m_bots_idle,
    .lease_held = m_lease_held,
    .bots_list = m_bots_list,
    .ratings_for = m_ratings_for,
    .rating_set_username = m_rating_set_username,
    .snapshot_put = m_snapshot_put,
    .snapshots_query = m_snapshots_query,
    .snapshot_set_extras = m_snapshot_set_extras,
};

cb_store *tm_store_new(int n_bots, const char *brain) {
    cb_store *s = calloc(1, sizeof *s);
    tm_impl *m = calloc(1, sizeof *m);
    pthread_mutex_init(&m->mu, NULL);
    s->ops = &tm_ops;
    s->impl = m;
    for (int i = 0; i < n_bots && i < TM_BOTS; i++) {
        cb_bot_row *b = &m->bots[m->n_bots++];
        snprintf(b->id, sizeof b->id, "b0000000-0000-4000-8000-%012d", i + 1);
        snprintf(b->nickname, sizeof b->nickname, "%%Bot %d", i + 1);
        snprintf(b->strategy_key, sizeof b->strategy_key, "%s", brain);
        b->elo_rating = TABLE_RATING_START;
        b->previous_elo = -1;
    }
    return s;
}

tm_hooks *tm_hooks_of(cb_store *s) { return &IMPL(s)->hooks; }

const char *tm_bot_id(cb_store *s, int i) {
    tm_impl *m = IMPL(s);
    return i >= 0 && i < m->n_bots ? m->bots[i].id : NULL;
}

int tm_add_user(cb_store *s, const char *id, const char *username) {
    cb_user_row u;
    memset(&u, 0, sizeof u);
    snprintf(u.id, sizeof u.id, "%s", id);
    snprintf(u.email, sizeof u.email, "%s@test", username);
    snprintf(u.username, sizeof u.username, "%s", username);
    return m_user_create(s, &u);
}

void tm_set_user_created(cb_store *s, const char *id, int64_t created_at) {
    tm_impl *m = IMPL(s);
    pthread_mutex_lock(&m->mu);
    tm_user *u = find_user(m, id);
    if (u) u->user.created_at = created_at;
    pthread_mutex_unlock(&m->mu);
}

int tm_rating(cb_store *s, const char *id, cb_rating_row *out) {
    tm_impl *m = IMPL(s);
    int rc = CB_E_NOT_FOUND;
    pthread_mutex_lock(&m->mu);
    cb_bot_row *b = find_bot(m, id);
    tm_user *u = b ? NULL : find_user(m, id);
    memset(out, 0, sizeof *out);
    if (b) {
        snprintf(out->user_id, sizeof out->user_id, "%s", b->id);
        out->elo_rating = b->elo_rating;
        out->previous_elo = b->previous_elo;
        out->games_played = b->games_played;
        rc = CB_OK;
    } else if (u && u->has_rating) {
        *out = u->rating;
        rc = CB_OK;
    }
    pthread_mutex_unlock(&m->mu);
    return rc;
}

int tm_lease(cb_store *s, const char *game_id, char token[CB_UUID_LEN + 1], int64_t *until) {
    tm_impl *m = IMPL(s);
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, game_id);
    if (g) { snprintf(token, CB_UUID_LEN + 1, "%s", g->lease_token); *until = g->lease_until; }
    pthread_mutex_unlock(&m->mu);
    return g ? CB_OK : CB_E_NOT_FOUND;
}

int tm_hold_lease(cb_store *s, const char *game_id, int64_t until) {
    tm_impl *m = IMPL(s);
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, game_id);
    if (g) { snprintf(g->lease_token, sizeof g->lease_token, "00000000-0000-4000-8000-00000000dead"); g->lease_until = until; }
    pthread_mutex_unlock(&m->mu);
    return g ? CB_OK : CB_E_NOT_FOUND;
}

int tm_forget_seats(cb_store *s, const char *game_id) {
    tm_impl *m = IMPL(s);
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, game_id);
    if (g) g->row.seats_exact = false;
    pthread_mutex_unlock(&m->mu);
    return g ? CB_OK : CB_E_NOT_FOUND;
}

int tm_set_scan(cb_store *s, const char *game_id, bool needs_bots, int64_t updated_at, int64_t last_commit_at) {
    tm_impl *m = IMPL(s);
    pthread_mutex_lock(&m->mu);
    tm_game *g = find_game(m, game_id);
    if (g) { g->row.needs_bots = needs_bots; g->row.updated_at = updated_at; g->row.last_commit_at = last_commit_at; }
    pthread_mutex_unlock(&m->mu);
    return g ? CB_OK : CB_E_NOT_FOUND;
}
