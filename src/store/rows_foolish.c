// rows_foolish.c - foolish's rows and the accounts, written once over the
// item layer (items.h) for both backends (docs/PLATFORM.md stage 6). Every
// key, index key and attribute is the one the DynamoDB backend wrote before
// (json_surface.h "THE ITEM LAYOUT"); check-dynamo's key-capture proof holds
// them. Each backend's cb_store_ops points its row operations here.
//
// THE CAS. game_commit takes the stored seats from the caller when the row it
// is fenced on vouches for them (cb_game_row seats_exact; else it reads them
// with one Query), reads the rating rows of a rated commit, then writes one
// transaction whose item 0 is the game update conditioned on version =
// expected. Item 0 refused is CB_E_CONFLICT (or CB_E_NOT_FOUND when the
// refusal carries no item: the game is gone). Any other item refused means
// something the commit read moved underneath it (a rating another table
// changed, a seat's account deleted): it reads again and writes again, up to
// five times. A commit whose only item would be the row (most moves of a game
// between humans) is that update alone, one conditional write with the same
// fence (DESIGN.md "Store": about 2.4 ms at p50 on DynamoDB against the
// transaction's 17).
//
// THE LEASE is a conditional update of the game's SCAN marker, the small item
// that exists exactly while a bot can act on the game: one small write that
// also moves the marker's updated_at, the heartbeat's staleness clock.
//
// Not atomic across calls (each documented where it happens): user_delete's
// cascade and game_delete's chat and replay cleanup run after the row they
// hang off is gone, so a crash between the two leaves orphans no reader can
// reach.
#include "rows_foolish.h"

#include "store_priv.h"

#include "table.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/uuid.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MOD "store"
#define KB 400
#define ATTEMPTS 5

// ---- keys --------------------------------------------------------------------------------

static const char *k(char *out, size_t cap, const char *prefix, const char *id) {
    snprintf(out, cap, "%s%s", prefix, id ? id : "");
    return out;
}

// "<15-digit ms>#<id>": sorts as time, then id.
static const char *tsk(char *out, size_t cap, int64_t ms, const char *id) {
    snprintf(out, cap, "%015" PRId64 "#%s", ms < 0 ? (int64_t)0 : ms, id ? id : "");
    return out;
}

// A rating's GSI2 sort key: highest rating first, then the id.
static const char *rsk(char *out, size_t cap, int64_t elo, const char *id) {
    if (elo < 0) elo = 0;
    if (elo > INT32_MAX) elo = INT32_MAX;
    snprintf(out, cap, "E#%010" PRId64 "#%s", (int64_t)INT32_MAX - elo, id ? id : "");
    return out;
}

static bool listed(const cb_uuid *ids, int n, const char *id) {
    for (int i = 0; i < n; i++)
        if (strcmp(ids[i], id) == 0) return true;
    return false;
}

static void nap(int64_t ms) {
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// ---- reading an item -----------------------------------------------------------------------

static bool s_of(const cb_item *it, const char *name, char *out, size_t cap) { return cb_item_s(it, name, out, cap); }

// A malloc'd copy of a B attribute (NULL/0 when absent). false: no memory, or another kind.
static bool b_of(const cb_item *it, const char *name, uint8_t **out, size_t *len) {
    cb_av v;
    *out = NULL;
    *len = 0;
    if (!cb_item_get(it, name, &v)) return true;
    if (v.kind != CB_AV_B) return false;
    if (!v.len) return true;
    *out = malloc(v.len);
    if (!*out) return false;
    memcpy(*out, v.bytes, v.len);
    *len = v.len;
    return true;
}

static void bool_of(const cb_item *it, const char *name, bool *out) { cb_item_bool(it, name, out); }

// ---- the attribute lists a write is made of -------------------------------------------------

typedef struct {
    cb_av a[CB_ITEM_ATTRS];
    int   n;
} attrs;

static void add(attrs *x, cb_av v) {
    if (x->n < CB_ITEM_ATTRS) x->a[x->n++] = v;
}

typedef struct {
    cb_iset a[CB_ITEM_ATTRS];
    int     n;
} sets;

static void set(sets *x, cb_av v, cb_set_how how) {
    if (x->n < CB_ITEM_ATTRS) x->a[x->n++] = (cb_iset){ v, how };
}

// The strings a write's attributes point at live here until the write is sent.
typedef struct {
    char s[16][KB];
    int  n;
} strs;

static const char *keep(strs *z, const char *v) {
    if (z->n >= 16) return "";
    snprintf(z->s[z->n], KB, "%s", v ? v : "");
    return z->s[z->n++];
}

// ---- users ---------------------------------------------------------------------------------

bool cb_rows_user_of(const cb_item *it, cb_user_row *u) {
    memset(u, 0, sizeof *u);
    if (!s_of(it, "id", u->id, sizeof u->id) || !s_of(it, "email", u->email, sizeof u->email) ||
        !s_of(it, "password_hash", u->password_hash, sizeof u->password_hash))
        return false;
    s_of(it, "username", u->username, sizeof u->username);
    u->created_at = cb_item_n_or(it, "created_at", 0);
    u->last_sign_in_at = cb_item_n_or(it, "last_sign_in_at", 0);
    u->session_gen = cb_item_n_or(it, "session_gen", 0);
    s_of(it, "kept_session", u->kept_session, sizeof u->kept_session);
    return true;
}

// USER#<id> PROFILE: listed on GSI2 USERS by creation.
static void profile_write(cb_iwrite *w, attrs *x, strs *z, const cb_user_row *u, bool fresh) {
    char gsk[KB];
    add(x, CB_AVS("gsi2pk", "USERS"));
    add(x, CB_AVS("gsi2sk", keep(z, tsk(gsk, sizeof gsk, u->created_at, u->id))));
    add(x, CB_AVS("id", u->id));
    add(x, CB_AVS("email", u->email));
    add(x, CB_AVS("password_hash", u->password_hash));
    if (u->username[0]) add(x, CB_AVS("username", u->username));
    add(x, CB_AVN("created_at", u->created_at));
    if (u->last_sign_in_at) add(x, CB_AVN("last_sign_in_at", u->last_sign_in_at));
    char pk[KB];
    *w = (cb_iwrite){ .op = CB_IW_PUT, .key = { keep(z, k(pk, sizeof pk, "USER#", u->id)), "PROFILE" }, .attrs = x->a,
                      .n_attrs = x->n };
    if (fresh) w->cond[w->n_cond++] = CB_IF_ABSENT;
}

// EMAIL#<email> USER: the id (signup's uniqueness is the condition on it).
static void email_write(cb_iwrite *w, attrs *x, strs *z, const cb_user_row *u, bool fresh) {
    char pk[KB];
    add(x, CB_AVS("user_id", u->id));
    *w = (cb_iwrite){ .op = CB_IW_PUT, .key = { keep(z, k(pk, sizeof pk, "EMAIL#", u->email)), "USER" }, .attrs = x->a,
                      .n_attrs = x->n };
    if (fresh) w->cond[w->n_cond++] = CB_IF_ABSENT;
}

// USER#<id> RATING: on GSI2 RATINGS (sparse: only a player who has played).
static void rating_attrs(attrs *x, strs *z, const cb_rating_row *rt) {
    if (rt->games_played > 0) {
        char gsk[KB];
        add(x, CB_AVS("gsi2pk", "RATINGS"));
        add(x, CB_AVS("gsi2sk", keep(z, rsk(gsk, sizeof gsk, rt->elo_rating, rt->user_id))));
    }
    add(x, CB_AVS("user_id", rt->user_id));
    if (rt->username[0]) add(x, CB_AVS("username", rt->username));
    add(x, CB_AVN("elo_rating", rt->elo_rating));
    if (rt->previous_elo >= 0) add(x, CB_AVN("previous_elo", rt->previous_elo));
    add(x, CB_AVN("games_played", rt->games_played));
    add(x, CB_AVN("updated_at", rt->updated_at));
}

static int get_item(cb_store *s, const char *pk, const char *sk, cb_item *it) {
    return s->ops->item_get(s, (cb_ikey){ pk, sk }, true, it);
}

int cb_rows_user_get(cb_store *s, const char *id, cb_user_row *out) {
    char pk[KB];
    cb_item it;
    cb_item_init(&it);
    int rc = get_item(s, k(pk, sizeof pk, "USER#", id), "PROFILE", &it);
    if (rc == CB_OK && !cb_rows_user_of(&it, out)) rc = CB_E_DB;
    cb_item_free(&it);
    return rc;
}

int cb_rows_user_by_email(cb_store *s, const char *email, cb_user_row *out) {
    char pk[KB];
    cb_uuid id;
    cb_item it;
    cb_item_init(&it);
    int rc = get_item(s, k(pk, sizeof pk, "EMAIL#", email), "USER", &it);
    if (rc == CB_OK && !s_of(&it, "user_id", id, sizeof id)) rc = CB_E_DB;
    cb_item_free(&it);
    return rc == CB_OK ? cb_rows_user_get(s, id, out) : rc;
}

int cb_rows_user_create(cb_store *s, const cb_user_row *u) {
    cb_iwrite w[3];
    attrs x[3] = { 0 };
    strs *z = calloc(1, sizeof *z);
    if (!z) return CB_E_NOMEM;
    email_write(&w[0], &x[0], z, u, true);
    profile_write(&w[1], &x[1], z, u, true);
    // The auth.users trigger's row: the start rating, previous_elo the
    // column default (the start too), the username.
    cb_rating_row rt = { .elo_rating = TABLE_RATING_START, .previous_elo = TABLE_RATING_START, .updated_at = u->created_at };
    memcpy(rt.user_id, u->id, sizeof rt.user_id);
    memcpy(rt.username, u->username, sizeof rt.username);
    rating_attrs(&x[2], z, &rt);
    char pk[KB];
    w[2] = (cb_iwrite){ .op = CB_IW_PUT, .key = { keep(z, k(pk, sizeof pk, "USER#", u->id)), "RATING" }, .attrs = x[2].a,
                        .n_attrs = x[2].n };
    int failed;
    int rc = s->ops->item_txn(s, w, 3, &failed, NULL);
    free(z);
    return rc == CB_E_CONFLICT ? (failed == 0 || failed == 1 ? CB_E_CONFLICT : CB_E_DB) : rc;
}

int cb_rows_user_import(cb_store *s, const cb_user_row *u) {
    cb_iwrite w[2];
    attrs x[2] = { 0 };
    strs *z = calloc(1, sizeof *z);
    if (!z) return CB_E_NOMEM;
    email_write(&w[0], &x[0], z, u, false);
    profile_write(&w[1], &x[1], z, u, false);
    int rc = s->ops->item_txn(s, w, 2, NULL, NULL);
    free(z);
    return rc == CB_E_CONFLICT ? CB_E_DB : rc;
}

// A present-only update of the profile (CB_E_NOT_FOUND when the user is gone).
static int profile_update(cb_store *s, const char *id, const cb_iset *st, int n) {
    char pk[KB];
    cb_iwrite w = { .op = CB_IW_UPDATE, .key = { k(pk, sizeof pk, "USER#", id), "PROFILE" }, .sets = st, .n_sets = n,
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
    char   (*text)[2][KB];
    int      n, cap;
    bool     oom;
} keylist;

static bool collect_key(const cb_item *it, void *ud) {
    keylist *l = ud;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        char (*t)[2][KB] = realloc(l->text, (size_t)cap * sizeof *t);
        if (!t) { l->oom = true; return false; }
        l->text = t;
        l->cap = cap;
    }
    if (!s_of(it, CB_ITEM_PK, l->text[l->n][0], KB) || !s_of(it, CB_ITEM_SK, l->text[l->n][1], KB)) return true;
    l->n++;
    return true;
}

// The keys as cb_ikey (after collecting: text no longer moves).
static int keylist_keys(keylist *l) {
    l->keys = malloc((size_t)(l->n ? l->n : 1) * sizeof *l->keys);
    if (!l->keys) return CB_E_NOMEM;
    for (int i = 0; i < l->n; i++) l->keys[i] = (cb_ikey){ l->text[i][0], l->text[i][1] };
    return CB_OK;
}

static void keylist_free(keylist *l) {
    free(l->keys);
    free(l->text);
    memset(l, 0, sizeof *l);
}

static int query(cb_store *s, cb_iquery q, cb_item_fn fn, void *ud) { return s->ops->item_query(s, &q, fn, ud); }

// The cascade auth.admin.deleteUser had, by hand: the account's seats, chat
// messages and refresh tokens (found through GSI1), then the account itself.
int cb_rows_user_delete(cb_store *s, const char *id) {
    cb_user_row u;
    int rc = cb_rows_user_get(s, id, &u);
    if (rc != CB_OK) return rc;
    keylist keys = { 0 };
    char g1[KB];
    rc = query(s, (cb_iquery){ .index = 1, .pk = k(g1, sizeof g1, "USERGAMES#", id) }, collect_key, &keys);
    if (rc == CB_OK) rc = query(s, (cb_iquery){ .index = 1, .pk = k(g1, sizeof g1, "USERCHAT#", id), .forward = true, .keys_only = true }, collect_key, &keys);
    if (rc == CB_OK) rc = query(s, (cb_iquery){ .index = 1, .pk = k(g1, sizeof g1, "USERREFRESH#", id), .forward = true, .keys_only = true }, collect_key, &keys);
    if (rc == CB_OK && keys.oom) rc = CB_E_NOMEM;
    if (rc == CB_OK) rc = keylist_keys(&keys);
    if (rc == CB_OK && keys.n) rc = s->ops->item_batch_delete(s, keys.keys, keys.n);
    keylist_free(&keys);
    if (rc != CB_OK) return rc;
    char pk[KB], epk[KB];
    k(pk, sizeof pk, "USER#", u.id);
    const cb_iwrite w[3] = {
        { .op = CB_IW_DELETE, .key = { pk, "PROFILE" } },
        { .op = CB_IW_DELETE, .key = { pk, "RATING" } },
        { .op = CB_IW_DELETE, .key = { k(epk, sizeof epk, "EMAIL#", u.email), "USER" } },
    };
    rc = s->ops->item_txn(s, w, 3, NULL, NULL);
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
    int rc = query(s, (cb_iquery){ .index = 2, .pk = "USERS" }, users_step, &w);
    return rc == CB_OK ? w.n : rc;
}

// ---- games --------------------------------------------------------------------------------

bool cb_rows_game_of(const cb_item *it, cb_game_row *g) {
    memset(g, 0, sizeof *g);
    int64_t status, version;
    if (!s_of(it, "id", g->id, sizeof g->id) || !cb_item_n(it, "status", &status) || !cb_item_n(it, "version", &version))
        return false;
    g->status = (int)status;
    g->version = version;
    uint8_t *seed;
    size_t seed_len;
    if (!b_of(it, "state", &g->state, &g->state_len) || !b_of(it, "roster", &g->roster, &g->roster_len) ||
        !b_of(it, "logs_packed", &g->logs, &g->logs_len) || !b_of(it, "game_seed", &seed, &seed_len)) {
        cb_game_row_free(g);
        return false;
    }
    if (seed && seed_len == CB_SEED_LEN) {
        g->has_seed = true;
        memcpy(g->seed, seed, CB_SEED_LEN);
    }
    free(seed);
    bool_of(it, "needs_bots", &g->needs_bots);
    bool_of(it, "seats_exact", &g->seats_exact);
    g->round_epoch = cb_item_n_or(it, "round_epoch", 0);
    g->created_at = cb_item_n_or(it, "created_at", 0);
    g->updated_at = cb_item_n_or(it, "updated_at", 0);
    g->last_commit_at = cb_item_n_or(it, "last_commit_at", 0);
    return true;
}

// GAME#<id> ROW, whole.
static void game_attrs(attrs *x, const cb_game_row *g) {
    add(x, CB_AVS("id", g->id));
    add(x, CB_AVN("status", g->status));
    add(x, CB_AVB("state", g->state, g->state_len));
    add(x, CB_AVB("roster", g->roster, g->roster_len));
    if (g->logs_len) add(x, CB_AVB("logs_packed", g->logs, g->logs_len));
    if (g->has_seed) add(x, CB_AVB("game_seed", g->seed, CB_SEED_LEN));
    add(x, CB_AVBOOL("needs_bots", g->needs_bots));
    add(x, CB_AVN("version", g->version));
    add(x, CB_AVN("round_epoch", g->round_epoch));
    add(x, CB_AVN("created_at", g->created_at));
    add(x, CB_AVN("updated_at", g->updated_at));
    add(x, CB_AVN("last_commit_at", g->last_commit_at));
    if (g->seats_exact) add(x, CB_AVBOOL("seats_exact", true));
}

int cb_rows_game_get(cb_store *s, const char *id, cb_game_row *out) {
    char pk[KB];
    cb_item it;
    cb_item_init(&it);
    int rc = get_item(s, k(pk, sizeof pk, "GAME#", id), "ROW", &it);
    if (rc == CB_OK && !cb_rows_game_of(&it, out)) rc = CB_E_DB;
    cb_item_free(&it);
    return rc;
}

typedef struct {
    cb_game_row *got;
    bool        *have;
    bool         bad;
} views_get;

static bool view_at(int i, const cb_item *it, void *ud) {
    views_get *v = ud;
    if (!cb_rows_game_of(it, &v->got[i])) { v->bad = true; return false; }
    v->have[i] = true;
    return true;
}

// The game rows of a list of views, every column but the session log (the
// bulk of a game's bytes, which a view never reads).
int cb_rows_games_get_views(cb_store *s, const char *const *ids, int n, cb_game_fn fn, void *ud) {
    static const char *const COLS[] = { "id", "status", "version", "state", "roster", "game_seed", "needs_bots",
                                        "round_epoch", "created_at", "updated_at", "last_commit_at" };
    if (n <= 0) return 0;
    cb_ikey *keys = calloc((size_t)n, sizeof *keys);
    char (*pks)[KB] = calloc((size_t)n, KB);
    cb_game_row *got = calloc((size_t)n, sizeof *got);
    bool *have = calloc((size_t)n, sizeof *have);
    int rc = keys && pks && got && have ? CB_OK : CB_E_NOMEM;
    for (int i = 0; rc == CB_OK && i < n; i++) keys[i] = (cb_ikey){ k(pks[i], KB, "GAME#", ids[i]), "ROW" };
    views_get v = { got, have, false };
    if (rc == CB_OK) rc = s->ops->item_batch_get(s, keys, n, COLS, (int)(sizeof COLS / sizeof COLS[0]), view_at, &v);
    if (rc == CB_OK && v.bad) rc = CB_E_DB;
    int count = 0;
    bool more = true;
    for (int i = 0; have && i < n; i++) {
        if (!have[i]) continue;
        if (rc == CB_OK && more) {
            count++;
            more = fn(&got[i], ud);
        }
        cb_game_row_free(&got[i]);
    }
    free(keys);
    free(pks);
    free(got);
    free(have);
    return rc == CB_OK ? count : rc;
}

// A seated human's item: the dashboard's GSI1 entry, ordered by when the seat
// was written (taken). joined_at is kept from the first time.
static void member_write(cb_iwrite *w, sets *x, strs *z, const char *game, const char *user, int64_t at) {
    char pk[KB], sk[KB], g1[KB], g1s[KB];
    set(x, CB_AVS("game_id", game), CB_SET_TO);
    set(x, CB_AVS("user_id", user), CB_SET_TO);
    set(x, CB_AVN("updated_at", at), CB_SET_TO);
    set(x, CB_AVS("gsi1pk", keep(z, k(g1, sizeof g1, "USERGAMES#", user))), CB_SET_TO);
    set(x, CB_AVS("gsi1sk", keep(z, tsk(g1s, sizeof g1s, at, game))), CB_SET_TO);
    set(x, CB_AVN("joined_at", at), CB_SET_IF_ABSENT);
    *w = (cb_iwrite){ .op = CB_IW_UPDATE, .key = { keep(z, k(pk, sizeof pk, "GAME#", game)), keep(z, k(sk, sizeof sk, "MEMBER#", user)) },
                      .sets = x->a, .n_sets = x->n };
}

// The SCAN marker: present (with the times the scan filters on) while a bot
// can act on the game (the row's needs_bots, table.h table_bots_due), absent
// otherwise. It carries the row's version (row_version), which fences a
// drive's bots_idle. A commit updates it in place (`keep_lease`), leaving a
// drive's lease on it as it is; a new or imported game puts it whole.
static void scan_write(cb_iwrite *w, attrs *ax, sets *sx, strs *z, const cb_game_row *g, bool keep_lease) {
    char sk[KB];
    const char *skk = keep(z, k(sk, sizeof sk, "GAME#", g->id));
    if (!g->needs_bots) {
        *w = (cb_iwrite){ .op = CB_IW_DELETE, .key = { "SCAN", skk } };
        return;
    }
    if (keep_lease) {
        set(sx, CB_AVS("game_id", g->id), CB_SET_TO);
        set(sx, CB_AVN("updated_at", g->updated_at), CB_SET_TO);
        set(sx, CB_AVN("last_commit_at", g->last_commit_at), CB_SET_TO);
        set(sx, CB_AVN("row_version", g->version), CB_SET_TO);
        *w = (cb_iwrite){ .op = CB_IW_UPDATE, .key = { "SCAN", skk }, .sets = sx->a, .n_sets = sx->n };
        return;
    }
    add(ax, CB_AVS("game_id", g->id));
    add(ax, CB_AVN("updated_at", g->updated_at));
    add(ax, CB_AVN("last_commit_at", g->last_commit_at));
    add(ax, CB_AVN("row_version", g->version));
    *w = (cb_iwrite){ .op = CB_IW_PUT, .key = { "SCAN", skk }, .attrs = ax->a, .n_attrs = ax->n };
}

int cb_rows_game_create(cb_store *s, const cb_game_row *g, const char *creator) {
    cb_game_row row = *g;
    row.last_commit_at = g->updated_at;   // a new row's last commit is its creation
    strs *z = calloc(1, sizeof *z);
    if (!z) return CB_E_NOMEM;
    cb_iwrite w[3];
    attrs ga = { 0 }, sa = { 0 };
    sets ms = { 0 }, ss = { 0 };
    char pk[KB];
    game_attrs(&ga, &row);
    w[0] = (cb_iwrite){ .op = CB_IW_PUT, .key = { keep(z, k(pk, sizeof pk, "GAME#", g->id)), "ROW" }, .attrs = ga.a,
                        .n_attrs = ga.n, .cond = { CB_IF_ABSENT }, .n_cond = 1 };
    member_write(&w[1], &ms, z, g->id, creator, g->updated_at);
    int n = 2;
    if (g->needs_bots) scan_write(&w[n++], &sa, &ss, z, &row, false);
    int failed;
    int rc = s->ops->item_txn(s, w, n, &failed, NULL);
    free(z);
    return rc == CB_E_CONFLICT ? (failed == 0 ? CB_E_CONFLICT : CB_E_DB) : rc;
}

// The seats of a game, humans and bots.
typedef struct {
    int     n_humans, n_bots;
    cb_uuid humans[64], bots[64];
} seats_t;

static bool seat_step(const cb_item *it, void *ud) {
    seats_t *st = ud;
    char sk[KB];
    if (!s_of(it, CB_ITEM_SK, sk, sizeof sk)) return true;
    const char *rest = NULL;
    bool bot = false;
    if (strncmp(sk, "MEMBER#", 7) == 0) rest = sk + 7;
    else if (strncmp(sk, "BOT#", 4) == 0) rest = sk + 4, bot = true;
    if (!rest || strlen(rest) >= sizeof(cb_uuid)) return true;
    int *n = bot ? &st->n_bots : &st->n_humans;
    cb_uuid *ids = bot ? st->bots : st->humans;
    if (*n < 64) memcpy(ids[(*n)++], rest, strlen(rest) + 1);
    return true;
}

// The stored seats, humans and bots, in one strongly consistent Query: every
// seat item sorts before ROW ("BOT#..." < "MEMBER#..." < "ROW").
static int read_seats(cb_store *s, const char *game, seats_t *out) {
    char pk[KB];
    memset(out, 0, sizeof *out);
    return query(s, (cb_iquery){ .pk = k(pk, sizeof pk, "GAME#", game), .sk_below = "ROW", .forward = true, .keys_only = true },
                 seat_step, out);
}

// The seats the caller says the row at expected_version holds (cb_game_commit
// prev_known), as read_seats would have answered when the commit lands.
static void seats_given(const cb_game_commit *c, seats_t *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < c->n_prev_humans && i < CB_MAX_SEATS; i++) memcpy(out->humans[out->n_humans++], c->prev_humans[i], sizeof(cb_uuid));
    for (int i = 0; i < c->n_prev_bots && i < CB_MAX_SEATS; i++) memcpy(out->bots[out->n_bots++], c->prev_bots[i], sizeof(cb_uuid));
}

// What a rating read found for one id.
typedef struct {
    bool    exists;    // a rating row
    int32_t elo;
    bool    account;   // a human's profile (asked when !both)
} rating_read;

typedef struct {
    const char *const *ids;
    const bool        *is_bot;
    int                n;
    bool               both;
    rating_read       *out;
    bool               from_bot[64];
} ratings_walk;

// Both kinds asked: a bot's row wins over a user's, whichever came first
// (seat_ratings' COALESCE(b.elo_rating, u.elo_rating) order).
static bool rating_at(int i, const cb_item *it, void *ud) {
    ratings_walk *w = ud;
    char pk[KB], sk[16];
    if (!s_of(it, CB_ITEM_PK, pk, sizeof pk) || !s_of(it, CB_ITEM_SK, sk, sizeof sk)) return true;
    bool bot = strncmp(pk, "BOT#", 4) == 0;
    int64_t elo;
    bool has_elo = cb_item_n(it, "elo_rating", &elo);
    const char *id = strchr(pk, '#');
    if (!id) return true;
    id++;
    for (int j = 0; j < w->n && j < 64; j++) {
        if (strcmp(w->ids[j], id) != 0 || (!w->both && bot != w->is_bot[j])) continue;
        if (strcmp(sk, "PROFILE") == 0) {
            w->out[j].account = true;
            continue;
        }
        if (!has_elo || (w->both && w->from_bot[j] && !bot)) continue;
        w->out[j].exists = true;
        w->out[j].elo = (int32_t)elo;
        w->from_bot[j] = bot;
    }
    return true;
}

// The rating rows (and, for humans unless `both`, the accounts) of ids, in
// one batch read: both asks a bot's row and a user's for every id.
static int read_ratings(cb_store *s, const char *const *ids, const bool *is_bot, int n, bool both, rating_read *out) {
    static const char *const PROJ[] = { "elo_rating" };
    cb_ikey keys[100];
    char pks[100][KB];
    int nk = 0;
    for (int i = 0; i < n; i++) memset(&out[i], 0, sizeof out[i]);
    for (int i = 0; i < n && nk + 2 <= 100; i++) {
        if (both || is_bot[i]) keys[nk] = (cb_ikey){ k(pks[nk], KB, "BOT#", ids[i]), "BOT" }, nk++;
        if (both || !is_bot[i]) {
            keys[nk] = (cb_ikey){ k(pks[nk], KB, "USER#", ids[i]), "RATING" }, nk++;
            if (!both) keys[nk] = (cb_ikey){ k(pks[nk], KB, "USER#", ids[i]), "PROFILE" }, nk++;
        }
    }
    ratings_walk w = { ids, is_bot, n, both, out, { false } };
    return nk ? s->ops->item_batch_get(s, keys, nk, PROJ, 1, rating_at, &w) : CB_OK;
}

// The commit as it will be written: a human newly seated whose account is
// gone is left out.
static int drop_missing_accounts(cb_store *s, const cb_game_commit *c, const seats_t *stored, cb_game_commit *out) {
    *out = *c;
    const char *ids[CB_MAX_SEATS];
    bool bots[CB_MAX_SEATS] = { false };
    int n = 0;
    for (int i = 0; i < c->n_humans; i++)
        if (!listed(stored->humans, stored->n_humans, c->humans[i])) ids[n++] = c->humans[i];
    if (!n) return CB_OK;
    rating_read rd[CB_MAX_SEATS];
    int rc = read_ratings(s, ids, bots, n, false, rd);
    if (rc != CB_OK) return rc;
    out->n_humans = 0;
    for (int i = 0; i < c->n_humans; i++) {
        bool kept = true;
        for (int j = 0; j < n; j++)
            if (strcmp(ids[j], c->humans[i]) == 0) kept = rd[j].account;
        if (kept) memcpy(out->humans[out->n_humans++], c->humans[i], sizeof(cb_uuid));
    }
    return CB_OK;
}

// Everything one commit writes, and the storage its writes point into.
typedef struct {
    cb_iwrite w[CB_ITEM_TXN_MAX + 8];
    int       n;
    attrs     a[CB_ITEM_TXN_MAX + 8];
    sets      x[CB_ITEM_TXN_MAX + 8];
    strs      z[40];
    int       nz;
    cb_rows_write cas;
} batch;

// A string store with room for the next write's few strings (each write
// keeps at most four at a time between two calls).
static strs *bz(batch *b) { return &b->z[b->nz < 40 ? b->nz : 39]; }
static strs *bz_next(batch *b) {
    if (bz(b)->n >= 12 && b->nz < 39) b->nz++;
    return bz(b);
}
static cb_iwrite *bw(batch *b) { return b->n < CB_ITEM_TXN_MAX + 8 ? &b->w[b->n] : NULL; }

// THE CAS: every scalar and blob of the row, fenced on the version read.
void cb_rows_cas_write(const cb_game_commit *c, int64_t last_commit_at, cb_rows_write *out) {
    const cb_game_row *g = c->row;
    sets x = { 0 };
    set(&x, CB_AVN("status", g->status), CB_SET_TO);
    set(&x, CB_AVB("state", g->state, g->state_len), CB_SET_TO);
    set(&x, CB_AVB("roster", g->roster, g->roster_len), CB_SET_TO);
    if (g->logs_len) set(&x, CB_AVB("logs_packed", g->logs, g->logs_len), CB_SET_TO);
    else set(&x, CB_AVB("logs_packed", NULL, 0), CB_SET_REMOVE);
    if (g->has_seed) set(&x, CB_AVB("game_seed", g->seed, CB_SEED_LEN), CB_SET_TO);
    else set(&x, CB_AVB("game_seed", NULL, 0), CB_SET_REMOVE);
    set(&x, CB_AVBOOL("needs_bots", g->needs_bots), CB_SET_TO);
    set(&x, CB_AVN("version", g->version), CB_SET_TO);
    set(&x, CB_AVN("round_epoch", g->round_epoch), CB_SET_TO);
    set(&x, CB_AVN("updated_at", g->updated_at), CB_SET_TO);
    set(&x, CB_AVN("last_commit_at", last_commit_at), CB_SET_TO);
    // The seats this commit writes make the items the new roster's
    // (cb_game_row seats_exact), whether they were read or given.
    set(&x, CB_AVBOOL("seats_exact", true), CB_SET_TO);
    memcpy(out->sets, x.a, (size_t)x.n * sizeof x.a[0]);
    k(out->pk, sizeof out->pk, "GAME#", g->id);
    out->w = (cb_iwrite){ .op = CB_IW_UPDATE, .key = { out->pk, "ROW" }, .sets = out->sets, .n_sets = x.n,
                          .cond = { { CB_C_EQ, CB_AVN("version", c->expected_version) } }, .n_cond = 1, .old_on_fail = true };
}

void cb_rows_game_put_write(const cb_game_row *g, cb_rows_write *out) {
    attrs x = { 0 };
    game_attrs(&x, g);
    memcpy(out->attrs, x.a, (size_t)x.n * sizeof x.a[0]);
    k(out->pk, sizeof out->pk, "GAME#", g->id);
    out->w = (cb_iwrite){ .op = CB_IW_PUT, .key = { out->pk, "ROW" }, .attrs = out->attrs, .n_attrs = x.n };
}

static void row_cas(batch *b, const cb_game_commit *c, int64_t last_commit_at) {
    cb_rows_write *rw = &b->cas;
    cb_rows_cas_write(c, last_commit_at, rw);
    b->w[b->n++] = rw->w;
}

// The seat items a commit writes: the humans and bots it seats that are not
// stored, and the stored ones it no longer seats. b NULL: only count.
static int seat_writes(batch *b, const cb_game_commit *c, const seats_t *stored) {
    const cb_game_row *g = c->row;
    char pk[KB], sk[KB];
    int items = 0;
    // A seat is written when it is taken, not on every commit: a human's
    // seat is a transactional write plus its GSI1 entry moving, every commit,
    // for the dashboard's order alone, which nothing reads.
    for (int i = 0; i < c->n_humans; i++)
        if (!stored || !listed(stored->humans, stored->n_humans, c->humans[i])) {
            if (b && bw(b)) {
                member_write(&b->w[b->n], &b->x[b->n], bz_next(b), g->id, c->humans[i], g->updated_at);
                b->n++;
            }
            items++;
        }
    for (int i = 0; stored && i < stored->n_humans; i++)
        if (!listed(c->humans, c->n_humans, stored->humans[i])) {
            if (b && bw(b)) {
                strs *z = bz_next(b);
                b->w[b->n++] = (cb_iwrite){ .op = CB_IW_DELETE, .key = { keep(z, k(pk, sizeof pk, "GAME#", g->id)),
                                                                          keep(z, k(sk, sizeof sk, "MEMBER#", stored->humans[i])) } };
            }
            items++;
        }
    for (int i = 0; i < c->n_bots; i++)
        if (!stored || !listed(stored->bots, stored->n_bots, c->bots[i])) {
            if (b && bw(b)) {
                strs *z = bz_next(b);
                attrs *a = &b->a[b->n];
                add(a, CB_AVS("game_id", keep(z, g->id)));
                add(a, CB_AVS("bot_id", keep(z, c->bots[i])));
                b->w[b->n] = (cb_iwrite){ .op = CB_IW_PUT, .key = { keep(z, k(pk, sizeof pk, "GAME#", g->id)),
                                                                     keep(z, k(sk, sizeof sk, "BOT#", c->bots[i])) },
                                          .attrs = a->a, .n_attrs = a->n };
                b->n++;
            }
            items++;
        }
    for (int i = 0; stored && i < stored->n_bots; i++)
        if (!listed(c->bots, c->n_bots, stored->bots[i])) {
            if (b && bw(b)) {
                strs *z = bz_next(b);
                b->w[b->n++] = (cb_iwrite){ .op = CB_IW_DELETE, .key = { keep(z, k(pk, sizeof pk, "GAME#", g->id)),
                                                                          keep(z, k(sk, sizeof sk, "BOT#", stored->bots[i])) } };
            }
            items++;
        }
    return items;
}

// One rated seat's write, fenced on the rating it read (nothing for a seat
// with nothing to rate: a bot or an account that is gone).
static void rating_write(batch *b, const cb_game_commit *c, int i, const rating_read *rd, int64_t at) {
    char pk[KB], gsk[KB];
    const char *id = c->rated_ids[i];
    bool bot = c->rated_is_bot[i];
    strs *z = bz_next(b);
    const char *pkk = keep(z, k(pk, sizeof pk, bot ? "BOT#" : "USER#", id));
    int32_t cur = rd->elo, delta = c->rating_delta[i];
    if (!rd->exists) {
        if (bot || !rd->account || b->n + 2 > CB_ITEM_TXN_MAX + 8) return;
        // A human with no row yet starts from the start rating, if the account
        // is still there (checked in the same transaction).
        int64_t v = (int64_t)TABLE_RATING_START + delta;
        cb_rating_row rt = { .elo_rating = (int32_t)(v < 0 ? 0 : v), .previous_elo = TABLE_RATING_START,
                             .games_played = 1, .updated_at = at };
        snprintf(rt.user_id, sizeof rt.user_id, "%s", id);
        snprintf(rt.username, sizeof rt.username, "%s", c->rated_names[i]);
        attrs *a = &b->a[b->n];
        rating_attrs(a, z, &rt);
        // The strings rating_attrs read are the commit's own (rt is local): copy them.
        for (int j = 0; j < a->n; j++)
            if (a->a[j].kind == CB_AV_S) a->a[j].s = keep(bz_next(b), a->a[j].s);
        b->w[b->n++] = (cb_iwrite){ .op = CB_IW_PUT, .key = { pkk, "RATING" }, .attrs = a->a, .n_attrs = a->n,
                                    .cond = { CB_IF_ABSENT }, .n_cond = 1 };
        b->w[b->n++] = (cb_iwrite){ .op = CB_IW_CHECK, .key = { pkk, "PROFILE" }, .cond = { CB_IF_EXISTS }, .n_cond = 1 };
        return;
    }
    if (!bw(b)) return;
    int64_t v = (int64_t)cur + delta;
    if (v < 0) v = 0;
    sets *x = &b->x[b->n];
    set(x, CB_AVN("elo_rating", v), CB_SET_TO);
    if (!bot) {
        // The row joins (or moves in) the leaderboard index: it has played now.
        set(x, CB_AVS("gsi2pk", "RATINGS"), CB_SET_TO);
        set(x, CB_AVS("gsi2sk", keep(bz_next(b), rsk(gsk, sizeof gsk, v, id))), CB_SET_TO);
    }
    set(x, CB_AVN("previous_elo", cur), CB_SET_TO);
    set(x, CB_AVN("updated_at", at), CB_SET_TO);
    set(x, CB_AVN("games_played", 1), CB_SET_INCR);
    b->w[b->n++] = (cb_iwrite){ .op = CB_IW_UPDATE, .key = { pkk, bot ? "BOT" : "RATING" }, .sets = x->a, .n_sets = x->n,
                                .cond = { { CB_C_EQ, CB_AVN("elo_rating", cur) } }, .n_cond = 1 };
}

// THE CAS (and, with `import`, the import's unconditional write of a row and
// its seats). See the top of the file.
static int commit_or_import(cb_store *s, const cb_game_commit *c0, bool import) {
    if (!c0 || !c0->row || c0->n_humans < 0 || c0->n_humans > CB_MAX_SEATS || c0->n_bots < 0 ||
        c0->n_bots > CB_MAX_SEATS || c0->n_rated < 0 || c0->n_rated > CB_MAX_SEATS)
        return CB_E_INVALID;
    seats_t *stored = malloc(sizeof *stored);
    batch *b = malloc(sizeof *b);
    if (!stored || !b) { free(stored); free(b); return CB_E_NOMEM; }
    int rc = CB_E_CONFLICT;
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        memset(b, 0, sizeof *b);
        cb_game_commit c;
        rating_read ratings[CB_MAX_SEATS];
        if (!import && c0->prev_known) seats_given(c0, stored);
        else if ((rc = read_seats(s, c0->row->id, stored)) != CB_OK) break;
        if ((rc = drop_missing_accounts(s, c0, stored, &c)) != CB_OK) break;
        if (c.n_rated) {
            const char *ids[CB_MAX_SEATS];
            for (int i = 0; i < c.n_rated; i++) ids[i] = c.rated_ids[i];
            if ((rc = read_ratings(s, ids, c.rated_is_bot, c.n_rated, false, ratings)) != CB_OK) break;
        }
        const cb_game_row *g = c.row;
        cb_game_row row = *g;
        row.last_commit_at = import ? g->last_commit_at : g->updated_at;   // a commit's time is its last commit
        // The SCAN marker: none to write when the commit leaves the game
        // without bot work and the row it is fenced on had none (prev_known):
        // no marker exists then (DESIGN.md "Store").
        bool no_marker = c.prev_known && !c.prev_needs_bots && !row.needs_bots;
        if (!import && no_marker && c.n_rated == 0 && seat_writes(NULL, &c, stored) == 0) {
            // Only the row changes: one conditional write, the same fence,
            // and a refusal brings the stored item back unless the game is gone.
            row_cas(b, &c, row.last_commit_at);
            cb_item old;
            cb_item_init(&old);
            rc = s->ops->item_write(s, &b->w[0], &old);
            if (rc == CB_E_CONFLICT && old.n == 0) rc = CB_E_NOT_FOUND;
            cb_item_free(&old);
            break;
        }
        char pk[KB];
        if (import) {
            game_attrs(&b->a[0], &row);
            b->w[0] = (cb_iwrite){ .op = CB_IW_PUT, .key = { keep(bz_next(b), k(pk, sizeof pk, "GAME#", g->id)), "ROW" },
                                   .attrs = b->a[0].a, .n_attrs = b->a[0].n };
            b->n = 1;
        } else {
            row_cas(b, &c, row.last_commit_at);   // item 0
        }
        seat_writes(b, &c, stored);
        for (int i = 0; c.n_rated && i < c.n_rated; i++) {
            bool dup = false;   // one write per item: a repeated id is rated once
            for (int j = 0; j < i; j++) dup |= strcmp(c.rated_ids[j], c.rated_ids[i]) == 0;
            if (!dup) rating_write(b, &c, i, &ratings[i], g->updated_at);
        }
        if (bw(b)) {
            scan_write(&b->w[b->n], &b->a[b->n], &b->x[b->n], bz_next(b), &row, !import);
            b->n++;
        }
        if (b->n > CB_ITEM_TXN_MAX) {
            CB_ERROR(MOD, "commit %s: more than %d items in one transaction", g->id, CB_ITEM_TXN_MAX);
            rc = CB_E_FULL;
            break;
        }
        int failed;
        bool had;
        rc = s->ops->item_txn(s, b->w, b->n, &failed, &had);
        if (rc != CB_E_CONFLICT) break;
        if (!import && failed == 0) {   // the fence: moved on, or gone
            rc = had ? CB_E_CONFLICT : CB_E_NOT_FOUND;
            break;
        }
        // Something else the commit read moved (a rating, an account): read again.
        rc = CB_E_CONFLICT;
        nap(10LL << attempt);
    }
    free(b);
    free(stored);
    return rc;
}

int cb_rows_game_commit(cb_store *s, const cb_game_commit *c) { return commit_or_import(s, c, false); }

int cb_rows_game_import(cb_store *s, const cb_game_row *g, const cb_uuid *humans, int n_humans, const cb_uuid *bots,
                        int n_bots) {
    if (n_humans < 0 || n_humans > CB_MAX_SEATS || n_bots < 0 || n_bots > CB_MAX_SEATS) return CB_E_INVALID;
    cb_game_commit c;
    memset(&c, 0, sizeof c);
    c.row = g;
    c.n_humans = n_humans;
    c.n_bots = n_bots;
    c.expected_version = g->version;
    for (int i = 0; i < n_humans; i++) memcpy(c.humans[i], humans[i], sizeof(cb_uuid));
    for (int i = 0; i < n_bots; i++) memcpy(c.bots[i], bots[i], sizeof(cb_uuid));
    return commit_or_import(s, &c, true);
}

int cb_rows_game_delete(cb_store *s, const char *id, int64_t expected) {
    seats_t *st = malloc(sizeof *st);
    if (!st) return CB_E_NOMEM;
    int rc = read_seats(s, id, st);
    int n_seats = st->n_humans + st->n_bots;
    if (rc == CB_OK && n_seats + 2 > CB_ITEM_TXN_MAX) rc = CB_E_FULL;
    if (rc == CB_OK) {
        cb_iwrite w[CB_ITEM_TXN_MAX];
        static _Thread_local char sks[CB_ITEM_TXN_MAX][KB];
        char pk[KB], scan[KB];
        k(pk, sizeof pk, "GAME#", id);
        int n = 0;
        // Item 0: the row, fenced on the version (the item back tells gone from moved).
        w[n++] = (cb_iwrite){ .op = CB_IW_DELETE, .key = { pk, "ROW" }, .cond = { { CB_C_EQ, CB_AVN("version", expected) } },
                              .n_cond = 1, .old_on_fail = true };
        for (int i = 0; i < st->n_humans; i++) w[n] = (cb_iwrite){ .op = CB_IW_DELETE, .key = { pk, k(sks[n], KB, "MEMBER#", st->humans[i]) } }, n++;
        for (int i = 0; i < st->n_bots; i++) w[n] = (cb_iwrite){ .op = CB_IW_DELETE, .key = { pk, k(sks[n], KB, "BOT#", st->bots[i]) } }, n++;
        w[n++] = (cb_iwrite){ .op = CB_IW_DELETE, .key = { "SCAN", k(scan, sizeof scan, "GAME#", id) } };
        int failed;
        bool had;
        rc = s->ops->item_txn(s, w, n, &failed, &had);
        if (rc == CB_E_CONFLICT) rc = failed == 0 ? (had ? CB_E_CONFLICT : CB_E_NOT_FOUND) : CB_E_DB;
    }
    free(st);
    if (rc != CB_OK) return rc;

    // After the row: its chat goes (ON DELETE CASCADE) and its replays lose
    // their game id (ON DELETE SET NULL). Logged, not returned: the game is gone.
    char cpk[KB], g1[KB];
    keylist chat = { 0 }, snaps = { 0 };
    if (query(s, (cb_iquery){ .pk = k(cpk, sizeof cpk, "CHAT#", id) }, collect_key, &chat) != CB_OK || chat.oom ||
        keylist_keys(&chat) != CB_OK || (chat.n && s->ops->item_batch_delete(s, chat.keys, chat.n) != CB_OK))
        CB_WARN(MOD, "game %s deleted, its chat not fully", id);
    if (query(s, (cb_iquery){ .index = 1, .pk = k(g1, sizeof g1, "GAMESNAPS#", id), .forward = true, .keys_only = true },
              collect_key, &snaps) == CB_OK && !snaps.oom)
        for (int i = 0; i < snaps.n; i++) {
            const cb_iset st2[] = { { CB_AVS("game_id", ""), CB_SET_TO },
                                    { CB_AVS(CB_ITEM_GSI1PK, ""), CB_SET_REMOVE },
                                    { CB_AVS(CB_ITEM_GSI1SK, ""), CB_SET_REMOVE } };
            cb_iwrite w = { .op = CB_IW_UPDATE, .key = { snaps.text[i][0], snaps.text[i][1] }, .sets = st2, .n_sets = 3,
                            .cond = { CB_IF_EXISTS }, .n_cond = 1 };
            int r = s->ops->item_write(s, &w, NULL);
            if (r != CB_OK && r != CB_E_CONFLICT) CB_WARN(MOD, "game %s deleted, a replay still names it", id);
        }
    keylist_free(&chat);
    keylist_free(&snaps);
    return CB_OK;
}

// ---- membership and the scan ------------------------------------------------------------------

typedef struct {
    int          limit, n;
    cb_member_fn fn;
    void        *ud;
} members_walk;

static bool members_step(const cb_item *it, void *ud) {
    members_walk *w = ud;
    cb_member_row m;
    memset(&m, 0, sizeof m);
    m.updated_at = cb_item_n_or(it, "updated_at", 0);
    if (!s_of(it, "game_id", m.game_id, sizeof m.game_id)) return true;
    w->n++;
    return w->fn(&m, w->ud) && (w->limit <= 0 || w->n < w->limit);
}

int cb_rows_games_of_user(cb_store *s, const char *user, int limit, cb_member_fn fn, void *ud) {
    char g1[KB];
    members_walk w = { .limit = limit, .fn = fn, .ud = ud };
    int rc = query(s, (cb_iquery){ .index = 1, .pk = k(g1, sizeof g1, "USERGAMES#", user), .page = limit > 0 ? limit : 0 },
                   members_step, &w);
    return rc == CB_OK ? w.n : rc;
}

int cb_rows_is_member(cb_store *s, const char *game, const char *user) {
    char pk[KB], sk[KB];
    cb_item it;
    cb_item_init(&it);
    int rc = get_item(s, k(pk, sizeof pk, "GAME#", game), k(sk, sizeof sk, "MEMBER#", user), &it);
    cb_item_free(&it);
    return rc == CB_OK ? 1 : rc == CB_E_NOT_FOUND ? 0 : rc;
}

int cb_rows_game_members(cb_store *s, const char *game, cb_name_fn fn, void *ud) {
    seats_t *st = malloc(sizeof *st);
    if (!st) return CB_E_NOMEM;
    int rc = read_seats(s, game, st), n = 0;
    while (rc == CB_OK && n < st->n_humans) {
        bool more = fn(st->humans[n], ud);
        n++;
        if (!more) break;
    }
    free(st);
    return rc == CB_OK ? n : rc;
}

typedef struct {
    char    id[CB_GAME_ID_MAX + 1];
    int64_t updated_at;
} due;

typedef struct {
    int64_t stale_before, active_after;
    due    *rows;
    int     n, cap;
    bool    oom;
} scan_walk;

static bool scan_step(const cb_item *it, void *ud) {
    scan_walk *w = ud;
    due e;
    if (!s_of(it, "game_id", e.id, sizeof e.id)) return true;
    e.updated_at = cb_item_n_or(it, "updated_at", 0);
    int64_t last = cb_item_n_or(it, "last_commit_at", 0);
    if (!(e.updated_at < w->stale_before && last > w->active_after)) return true;
    if (w->n == w->cap) {
        int cap = w->cap ? w->cap * 2 : 32;
        due *p = realloc(w->rows, (size_t)cap * sizeof *p);
        if (!p) { w->oom = true; return false; }
        w->rows = p;
        w->cap = cap;
    }
    w->rows[w->n++] = e;
    return true;
}

static int due_cmp(const void *pa, const void *pb) {
    const due *a = pa, *b = pb;
    if (a->updated_at != b->updated_at) return a->updated_at < b->updated_at ? -1 : 1;
    return strcmp(a->id, b->id);
}

// The heartbeat: the SCAN partition holds exactly the games that need bots;
// the windows (strict, as the SQL's < and >) are applied here.
int cb_rows_games_needing_bots(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn,
                               void *ud) {
    scan_walk w = { .stale_before = stale_before, .active_after = active_after };
    int rc = query(s, (cb_iquery){ .pk = "SCAN", .forward = true }, scan_step, &w);
    if (rc == CB_OK && w.oom) rc = CB_E_NOMEM;
    int n = 0;
    if (rc == CB_OK) {
        if (w.n > 1) qsort(w.rows, (size_t)w.n, sizeof *w.rows, due_cmp);
        for (int i = 0; i < w.n && (limit <= 0 || n < limit); i++) {
            n++;
            if (!fn(w.rows[i].id, ud)) break;
        }
    }
    free(w.rows);
    return rc == CB_OK ? n : rc;
}

// ---- the lease (on the SCAN marker: see the top of the file) -----------------------------------

static cb_iwrite lease_update(const char *sk, const cb_iset *st, int n) {
    return (cb_iwrite){ .op = CB_IW_UPDATE, .key = { "SCAN", sk }, .sets = st, .n_sets = n };
}

int cb_rows_lease_acquire(cb_store *s, const char *game, int64_t now, int64_t ttl, cb_uuid token_out) {
    char sk[KB];
    cb_uuid tok;
    cb_uuid_v4(tok);
    const cb_iset st[] = { { CB_AVS("lease_token", tok), CB_SET_TO },
                           { CB_AVN("lease_until", now + ttl), CB_SET_TO },
                           { CB_AVN("updated_at", now), CB_SET_TO } };
    cb_iwrite w = lease_update(k(sk, sizeof sk, "GAME#", game), st, 3);
    // Free when never taken, released (removed) or run out: lease_until < now.
    w.cond[0] = CB_IF_EXISTS;
    w.cond[1] = (cb_iclause){ CB_C_ABSENT_OR_LT, CB_AVN("lease_until", now) };
    w.n_cond = 2;
    cb_item old;
    cb_item_init(&old);
    int rc = s->ops->item_write(s, &w, &old);
    if (rc == CB_E_CONFLICT && old.n == 0) rc = CB_E_NOT_FOUND;   // no marker: gone, or no bot work
    cb_item_free(&old);
    if (rc == CB_OK) memcpy(token_out, tok, sizeof tok);
    return rc;
}

int cb_rows_lease_renew(cb_store *s, const char *game, const char *token, int64_t now, int64_t ttl) {
    char sk[KB];
    const cb_iset st[] = { { CB_AVN("lease_until", now + ttl), CB_SET_TO }, { CB_AVN("updated_at", now), CB_SET_TO } };
    cb_iwrite w = lease_update(k(sk, sizeof sk, "GAME#", game), st, 2);
    w.cond[0] = (cb_iclause){ CB_C_EQ, CB_AVS("lease_token", token) };
    w.n_cond = 1;
    return s->ops->item_write(s, &w, NULL);
}

int cb_rows_lease_release(cb_store *s, const char *game, const char *token) {
    char sk[KB];
    const cb_iset st[] = { { CB_AVN("lease_until", 0), CB_SET_REMOVE } };
    cb_iwrite w = lease_update(k(sk, sizeof sk, "GAME#", game), st, 1);
    w.cond[0] = (cb_iclause){ CB_C_EQ, CB_AVS("lease_token", token) };
    w.n_cond = 1;
    int rc = s->ops->item_write(s, &w, NULL);
    return rc == CB_E_CONFLICT ? CB_OK : rc;   // a lease that is not the token's is left alone
}

// A drive found no bot can act on the row at `version`: the marker goes (and
// any lease on it) unless a commit has moved it past that version. A marker
// written before the marker carried row_version passes too. Deleting a marker
// that is already gone is no failure.
int cb_rows_bots_idle(cb_store *s, const char *game, int64_t version) {
    char sk[KB];
    cb_iwrite w = { .op = CB_IW_DELETE, .key = { "SCAN", k(sk, sizeof sk, "GAME#", game) },
                    .cond = { { CB_C_ABSENT_OR_EQ, CB_AVN("row_version", version) } }, .n_cond = 1 };
    return s->ops->item_write(s, &w, NULL);
}

int cb_rows_lease_held(cb_store *s, const char *game, int64_t now) {
    char sk[KB];
    cb_item it;
    cb_item_init(&it);
    int rc = get_item(s, "SCAN", k(sk, sizeof sk, "GAME#", game), &it);
    // No marker, no lease; held as the acquire's condition refuses it:
    // lease_until not yet before now (a lease never taken, or released: none).
    if (rc == CB_OK) rc = cb_item_n_or(&it, "lease_until", INT64_MIN) >= now ? 1 : 0;
    else if (rc == CB_E_NOT_FOUND) rc = 0;
    cb_item_free(&it);
    return rc;
}

// ---- bots and ratings -----------------------------------------------------------------------

bool cb_rows_bot_of(const cb_item *it, cb_bot_row *b) {
    memset(b, 0, sizeof *b);
    if (!s_of(it, "id", b->id, sizeof b->id) || !s_of(it, "nickname", b->nickname, sizeof b->nickname) ||
        !s_of(it, "strategy_key", b->strategy_key, sizeof b->strategy_key))
        return false;
    b->elo_rating = (int32_t)cb_item_n_or(it, "elo_rating", 1000);
    b->previous_elo = (int32_t)cb_item_n_or(it, "previous_elo", -1);
    b->games_played = (int32_t)cb_item_n_or(it, "games_played", 0);
    b->created_at = cb_item_n_or(it, "created_at", 0);
    b->updated_at = cb_item_n_or(it, "updated_at", 0);
    return true;
}

bool cb_rows_rating_of(const cb_item *it, cb_rating_row *r) {
    memset(r, 0, sizeof *r);
    if (!s_of(it, "user_id", r->user_id, sizeof r->user_id)) return false;
    s_of(it, "username", r->username, sizeof r->username);
    r->elo_rating = (int32_t)cb_item_n_or(it, "elo_rating", 1000);
    r->previous_elo = (int32_t)cb_item_n_or(it, "previous_elo", -1);
    r->games_played = (int32_t)cb_item_n_or(it, "games_played", 0);
    r->updated_at = cb_item_n_or(it, "updated_at", 0);
    return true;
}

typedef struct {
    void  *rows;
    int    n, cap, limit;
    size_t size;
    bool   oom, bots;
} vec;

static void *vec_push(vec *v) {
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 32;
        void *p = realloc(v->rows, (size_t)cap * v->size);
        if (!p) { v->oom = true; return NULL; }
        v->rows = p;
        v->cap = cap;
    }
    return (char *)v->rows + (size_t)v->n++ * v->size;
}

static bool row_step(const cb_item *it, void *ud) {
    vec *v = ud;
    if (v->bots) {
        cb_bot_row b;
        if (!cb_rows_bot_of(it, &b)) return true;
        cb_bot_row *e = vec_push(v);
        if (e) *e = b;
        return e != NULL;
    }
    cb_rating_row r;
    if (!cb_rows_rating_of(it, &r)) return true;
    cb_rating_row *e = vec_push(v);
    if (e) *e = r;
    return e != NULL && (v->limit <= 0 || v->n < v->limit);
}

int cb_rows_bots_list(cb_store *s, cb_bot_fn fn, void *ud) {
    vec v = { .size = sizeof(cb_bot_row), .bots = true };
    int rc = query(s, (cb_iquery){ .index = 2, .pk = "BOTS", .forward = true }, row_step, &v);
    if (rc == CB_OK && v.oom) rc = CB_E_NOMEM;
    int n = 0;
    if (rc == CB_OK) {
        cb_store_bots_sort(v.rows, v.n);
        for (int i = 0; i < v.n; i++) {
            n++;
            if (!fn(&((cb_bot_row *)v.rows)[i], ud)) break;
        }
    }
    free(v.rows);
    return rc == CB_OK ? n : rc;
}

int cb_rows_ratings_list(cb_store *s, cb_rating_fn fn, void *ud) {
    vec v = { .size = sizeof(cb_rating_row) };
    int rc = query(s, (cb_iquery){ .index = 2, .pk = "RATINGS", .forward = true }, row_step, &v);
    if (rc == CB_OK && v.oom) rc = CB_E_NOMEM;
    int n = 0;
    if (rc == CB_OK) {
        cb_store_ratings_sort(v.rows, v.n);
        for (int i = 0; i < v.n; i++) {
            n++;
            if (!fn(&((cb_rating_row *)v.rows)[i], ud)) break;
        }
    }
    free(v.rows);
    return rc == CB_OK ? n : rc;
}

// One Query of at most `limit` items: the index holds only played rows, in
// leaderboard order, so nothing past the limit is read.
int cb_rows_ratings_top(cb_store *s, int limit, cb_rating_fn fn, void *ud) {
    if (limit <= 0) return 0;
    vec v = { .size = sizeof(cb_rating_row), .limit = limit };
    int rc = query(s, (cb_iquery){ .index = 2, .pk = "RATINGS", .sk_prefix = "E#", .forward = true, .page = limit }, row_step, &v);
    if (rc == CB_OK && v.oom) rc = CB_E_NOMEM;
    int n = 0;
    for (int i = 0; rc == CB_OK && i < v.n; i++) {
        const cb_rating_row *r = &((cb_rating_row *)v.rows)[i];
        if (r->games_played <= 0) continue;   // never in the index; a hand edit could put one there
        n++;
        if (!fn(r, ud)) break;
    }
    free(v.rows);
    return rc == CB_OK ? n : rc;
}

int cb_rows_ratings_for(cb_store *s, const char *const *ids, int n, int32_t *out) {
    if (n < 0 || n > 50) return CB_E_INVALID;   // two keys an id, BatchGetItem's 100
    if (n == 0) return CB_OK;
    rating_read rd[50];
    bool bots[50] = { false };
    int rc = read_ratings(s, ids, bots, n, true, rd);
    for (int i = 0; rc == CB_OK && i < n; i++) out[i] = rd[i].exists ? rd[i].elo : TABLE_RATING_NONE;
    return rc;
}

int cb_rows_rating_set_username(cb_store *s, const char *user, const char *username) {
    char pk[KB];
    const cb_iset st[] = { { CB_AVS("username", username ? username : ""), username && *username ? CB_SET_TO : CB_SET_REMOVE } };
    cb_iwrite w = { .op = CB_IW_UPDATE, .key = { k(pk, sizeof pk, "USER#", user), "RATING" }, .sets = st, .n_sets = 1,
                    .cond = { CB_IF_EXISTS }, .n_cond = 1 };
    int rc = s->ops->item_write(s, &w, NULL);
    return rc == CB_E_CONFLICT ? CB_OK : rc;   // no row: nothing to rename, as the SQL UPDATE
}

int cb_rows_rating_import(cb_store *s, const cb_rating_row *rt) {
    char pk[KB];
    attrs x = { 0 };
    strs *z = calloc(1, sizeof *z);
    if (!z) return CB_E_NOMEM;
    rating_attrs(&x, z, rt);
    cb_iwrite w = { .op = CB_IW_PUT, .key = { k(pk, sizeof pk, "USER#", rt->user_id), "RATING" }, .attrs = x.a, .n_attrs = x.n };
    int rc = s->ops->item_write(s, &w, NULL);
    free(z);
    return rc;
}

// Only a row whose key is not already the one it should have: a second run
// rewrites nothing (and so invalidates nothing).
int cb_rows_rating_reindex(cb_store *s, const cb_rating_row *rt) {
    char pk[KB], gsk[KB];
    cb_iwrite w = { .op = CB_IW_UPDATE, .key = { k(pk, sizeof pk, "USER#", rt->user_id), "RATING" } };
    cb_iset st[2];
    w.cond[0] = (cb_iclause){ CB_C_EQ, CB_AVN("elo_rating", rt->elo_rating) };
    w.cond[1] = (cb_iclause){ CB_C_EQ, CB_AVN("games_played", rt->games_played) };
    if (rt->games_played > 0) {
        rsk(gsk, sizeof gsk, rt->elo_rating, rt->user_id);
        st[0] = (cb_iset){ CB_AVS("gsi2pk", "RATINGS"), CB_SET_TO };
        st[1] = (cb_iset){ CB_AVS("gsi2sk", gsk), CB_SET_TO };
        w.cond[2] = (cb_iclause){ CB_C_NE, CB_AVS("gsi2sk", gsk) };
    } else {
        st[0] = (cb_iset){ CB_AVS("gsi2pk", ""), CB_SET_REMOVE };
        st[1] = (cb_iset){ CB_AVS("gsi2sk", ""), CB_SET_REMOVE };
        w.cond[2] = (cb_iclause){ CB_C_ATTR_EXISTS, CB_AVS("gsi2pk", "") };
    }
    w.n_cond = 3;
    w.sets = st;
    w.n_sets = 2;
    return s->ops->item_write(s, &w, NULL);
}

static bool bot_key_step(const cb_item *it, void *ud) {
    vec *v = ud;
    cb_uuid *e = vec_push(v);
    if (!e) return false;
    if (!s_of(it, "id", *e, sizeof *e)) v->n--;
    return true;
}

// Every bot row replaced by these n, atomically.
int cb_rows_bots_replace(cb_store *s, const cb_bot_row *rows, int n) {
    vec stored = { .size = sizeof(cb_uuid) };
    int rc = query(s, (cb_iquery){ .index = 2, .pk = "BOTS", .forward = true }, bot_key_step, &stored);
    if (rc == CB_OK && stored.oom) rc = CB_E_NOMEM;
    batch *b = rc == CB_OK ? calloc(1, sizeof *b) : NULL;
    if (rc == CB_OK && !b) rc = CB_E_NOMEM;
    for (int i = 0; rc == CB_OK && i < stored.n; i++) {
        const char *id = ((cb_uuid *)stored.rows)[i];
        bool kept = false;
        for (int j = 0; j < n && !kept; j++) kept = strcmp(rows[j].id, id) == 0;
        if (kept) continue;
        if (!bw(b)) { rc = CB_E_FULL; break; }
        char pk[KB];
        b->w[b->n++] = (cb_iwrite){ .op = CB_IW_DELETE, .key = { keep(bz_next(b), k(pk, sizeof pk, "BOT#", id)), "BOT" } };
    }
    for (int i = 0; rc == CB_OK && i < n; i++) {
        if (!bw(b)) { rc = CB_E_FULL; break; }
        const cb_bot_row *r = &rows[i];
        attrs *a = &b->a[b->n];
        char pk[KB];
        add(a, CB_AVS("gsi2pk", "BOTS"));
        add(a, CB_AVS("gsi2sk", r->id));
        add(a, CB_AVS("id", r->id));
        add(a, CB_AVS("nickname", r->nickname));
        add(a, CB_AVS("strategy_key", r->strategy_key));
        add(a, CB_AVN("elo_rating", r->elo_rating));
        if (r->previous_elo >= 0) add(a, CB_AVN("previous_elo", r->previous_elo));
        add(a, CB_AVN("games_played", r->games_played));
        add(a, CB_AVN("created_at", r->created_at));
        add(a, CB_AVN("updated_at", r->updated_at));
        b->w[b->n] = (cb_iwrite){ .op = CB_IW_PUT, .key = { keep(bz_next(b), k(pk, sizeof pk, "BOT#", r->id)), "BOT" },
                                  .attrs = a->a, .n_attrs = a->n };
        b->n++;
    }
    if (rc == CB_OK && b->n > CB_ITEM_TXN_MAX) rc = CB_E_FULL;
    if (rc == CB_OK && b->n) {
        rc = s->ops->item_txn(s, b->w, b->n, NULL, NULL);
        if (rc == CB_E_CONFLICT) rc = CB_E_DB;
    }
    free(b);
    free(stored.rows);
    return rc;
}

// ---- snapshots and chat -------------------------------------------------------------------------

bool cb_rows_snapshot_of(const cb_item *it, cb_snapshot_row *s) {
    memset(s, 0, sizeof *s);
    if (!s_of(it, "id", s->id, sizeof s->id)) return false;
    s_of(it, "game_id", s->game_id, sizeof s->game_id);
    cb_av l;
    if (cb_item_get(it, "player_ids", &l) && l.kind == CB_AV_SL)
        for (int i = 0; i < l.n_list && s->n_players < CB_MAX_SEATS; i++) {
            if (strlen(l.list[i]) >= sizeof(cb_uuid)) return false;
            snprintf(s->player_ids[s->n_players++], sizeof(cb_uuid), "%s", l.list[i]);
        }
    if (!b_of(it, "moves", &s->moves, &s->moves_len) || !b_of(it, "extras", &s->extras, &s->extras_len)) {
        cb_snapshot_row_free(s);
        return false;
    }
    s->created_at = cb_item_n_or(it, "created_at", 0);
    return true;
}

static void snapshot_attrs(attrs *x, strs *z, const cb_snapshot_row *s, const char *const *players) {
    char g1[KB], ts[KB];
    if (s->game_id[0]) {
        add(x, CB_AVS("gsi1pk", keep(z, k(g1, sizeof g1, "GAMESNAPS#", s->game_id))));
        add(x, CB_AVS("gsi1sk", keep(z, tsk(ts, sizeof ts, s->created_at, s->id))));
    }
    add(x, CB_AVS("id", s->id));
    add(x, CB_AVS("game_id", s->game_id));
    add(x, CB_AVSL("player_ids", players, s->n_players));
    add(x, CB_AVB("moves", s->moves, s->moves_len));
    if (s->extras_len) add(x, CB_AVB("extras", s->extras, s->extras_len));
    add(x, CB_AVN("created_at", s->created_at));
}

// THE REPLAY AND ITS POINTERS (Alex, 2026-10-18). SNAP#<id> SNAP is the
// replay: the only item holding the moves, the extras (the seats' names) and
// player_ids, listed on GSI1 GAMESNAPS#<game>. Each player's SNAP#<id>
// PLAYER#<user> is a pointer listed on GSI2 PLAYER#<user> by date: the id,
// the game and the date, nothing else, so a game end writes the replay once
// and N small items, and no copy ever holds a name. "My replays" is a Query
// of the pointers and one BatchGetItem of the replays (snapshots_query).
// Copies written before the change hold the whole replay; a reader ignores
// what a pointer should not have, and redaction slims them (set_extras).

// The replay and a pointer per player, in one transaction (so a reader never
// sees half of one), the replay refusing a reused id.
int cb_rows_snapshot_put(cb_store *s, const cb_snapshot_row *sn) {
    if (sn->n_players < 0 || sn->n_players > CB_MAX_SEATS) return CB_E_INVALID;
    batch *b = calloc(1, sizeof *b);
    if (!b) return CB_E_NOMEM;
    const char *players[CB_MAX_SEATS];
    for (int i = 0; i < sn->n_players; i++) players[i] = sn->player_ids[i];
    char pk[KB], sk[KB], g2[KB], ts[KB];
    const char *pkk = keep(bz_next(b), k(pk, sizeof pk, "SNAP#", sn->id));
    snapshot_attrs(&b->a[0], bz_next(b), sn, players);
    b->w[b->n++] = (cb_iwrite){ .op = CB_IW_PUT, .key = { pkk, "SNAP" }, .attrs = b->a[0].a, .n_attrs = b->a[0].n,
                                .cond = { CB_IF_ABSENT }, .n_cond = 1 };
    for (int i = 0; i < sn->n_players; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) dup |= strcmp(sn->player_ids[j], sn->player_ids[i]) == 0;
        if (dup) continue;
        attrs *a = &b->a[b->n];
        strs *z = bz_next(b);
        add(a, CB_AVS("gsi2pk", keep(z, k(g2, sizeof g2, "PLAYER#", sn->player_ids[i]))));
        add(a, CB_AVS("gsi2sk", keep(z, tsk(ts, sizeof ts, sn->created_at, sn->id))));
        add(a, CB_AVS("id", sn->id));
        add(a, CB_AVS("game_id", sn->game_id));
        add(a, CB_AVN("created_at", sn->created_at));
        b->w[b->n] = (cb_iwrite){ .op = CB_IW_PUT, .key = { pkk, keep(bz_next(b), k(sk, sizeof sk, "PLAYER#", sn->player_ids[i])) },
                                  .attrs = a->a, .n_attrs = a->n };
        b->n++;
    }
    int failed;
    int rc = s->ops->item_txn(s, b->w, b->n, &failed, NULL);
    free(b);
    return rc == CB_E_CONFLICT ? (failed == 0 ? CB_E_CONFLICT : CB_E_DB) : rc;
}

// The viewer's pointers, in the query's order, narrowed by the pointer's own
// id and game (the replay confirms both), up to the query's limit.
typedef struct {
    const cb_snapshot_query *q;
    char (*key)[KB];   // each pointer's replay pk, SNAP#<id>
    int    n, cap;
    bool   oom;
} pointers;

static bool pointer_step(const cb_item *it, void *ud) {
    pointers *p = ud;
    char id[KB], gid[CB_GAME_ID_MAX + 1] = "";
    if (!s_of(it, "id", id, sizeof id)) return true;
    s_of(it, "game_id", gid, sizeof gid);
    if ((p->q->id && strcmp(id, p->q->id) != 0) || (p->q->game_id && strcmp(gid, p->q->game_id) != 0)) return true;
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 16;
        char (*t)[KB] = realloc(p->key, (size_t)cap * sizeof *t);
        if (!t) { p->oom = true; return false; }
        p->key = t;
        p->cap = cap;
    }
    k(p->key[p->n++], KB, "SNAP#", id);
    return p->q->limit <= 0 || p->n < p->q->limit;
}

typedef struct {
    cb_snapshot_row *rows;
    bool            *got;
    bool             bad;
} replays;

static bool replay_step(int i, const cb_item *it, void *ud) {
    replays *r = ud;
    if (!cb_rows_snapshot_of(it, &r->rows[i])) { r->bad = true; return true; }
    r->got[i] = true;
    return true;
}

int cb_rows_snapshots_query(cb_store *s, const cb_snapshot_query *q, cb_snapshot_fn fn, void *ud) {
    if (!q || !q->viewer_id || !*q->viewer_id) return CB_E_INVALID;
    char g2[KB];
    pointers p = { .q = q };
    int rc = query(s, (cb_iquery){ .index = 2, .pk = k(g2, sizeof g2, "PLAYER#", q->viewer_id), .forward = !q->newest_first },
                   pointer_step, &p);
    if (rc == CB_OK && p.oom) rc = CB_E_NOMEM;
    cb_ikey *keys = rc == CB_OK ? malloc((size_t)(p.n ? p.n : 1) * sizeof *keys) : NULL;
    replays r = { .rows = rc == CB_OK ? calloc((size_t)(p.n ? p.n : 1), sizeof *r.rows) : NULL,
                  .got = rc == CB_OK ? calloc((size_t)(p.n ? p.n : 1), sizeof *r.got) : NULL };
    if (rc == CB_OK && (!keys || !r.rows || !r.got)) rc = CB_E_NOMEM;
    for (int i = 0; rc == CB_OK && i < p.n; i++) keys[i] = (cb_ikey){ p.key[i], "SNAP" };
    if (rc == CB_OK && p.n) rc = s->ops->item_batch_get(s, keys, p.n, NULL, 0, replay_step, &r);
    // In the pointers' order; a pointer whose replay is gone, or whose game
    // the replay no longer names (its game deleted), hands out nothing.
    int n = 0;
    bool more = true;
    for (int i = 0; rc == CB_OK && more && i < p.n; i++) {
        if (!r.got[i]) continue;
        const cb_snapshot_row *sn = &r.rows[i];
        if ((q->id && strcmp(sn->id, q->id) != 0) || (q->game_id && strcmp(sn->game_id, q->game_id) != 0)) continue;
        n++;
        more = fn(sn, ud);
    }
    for (int i = 0; r.rows && i < p.n; i++)
        if (r.got[i]) cb_snapshot_row_free(&r.rows[i]);
    free(r.rows);
    free(r.got);
    free(keys);
    free(p.key);
    if (rc == CB_OK && r.bad) CB_WARN(MOD, "a replay of %s does not decode", q->viewer_id);
    return rc == CB_OK ? n : rc;
}

// The replay's extras replaced: one conditional UpdateItem of the replay
// alone, since a pointer holds no name. A copy written before the pointers
// (THE REPLAY AND ITS POINTERS) still holds the whole replay, names and all,
// so when the replay has one, every such copy is slimmed to a pointer in the
// same transaction: a reader never sees the replay redacted and a copy not.
static bool fat_step(const cb_item *it, void *ud) {
    cb_av v;
    char sk[KB];
    if (!s_of(it, CB_ITEM_SK, sk, sizeof sk) || strcmp(sk, "SNAP") == 0) return true;
    if (!cb_item_get(it, "extras", &v) && !cb_item_get(it, "moves", &v) && !cb_item_get(it, "player_ids", &v)) return true;
    return collect_key(it, ud);
}

int cb_rows_snapshot_set_extras(cb_store *s, const char *id, const uint8_t *extras, size_t extras_len) {
    char pk[KB];
    k(pk, sizeof pk, "SNAP#", id);
    keylist fat = { 0 };
    int rc = query(s, (cb_iquery){ .pk = pk, .sk_prefix = "PLAYER#", .forward = true }, fat_step, &fat);
    if (rc == CB_OK && fat.oom) rc = CB_E_NOMEM;
    if (rc == CB_OK && fat.n + 1 > CB_ITEM_TXN_MAX) rc = CB_E_FULL;
    if (rc == CB_OK) {
        cb_iwrite w[CB_ITEM_TXN_MAX];
        const cb_iset st[] = { { CB_AVB("extras", extras, extras_len), extras_len ? CB_SET_TO : CB_SET_REMOVE } };
        w[0] = (cb_iwrite){ .op = CB_IW_UPDATE, .key = { pk, "SNAP" }, .sets = st, .n_sets = 1, .cond = { CB_IF_EXISTS }, .n_cond = 1 };
        static const cb_iset slim[] = { { CB_AVS("extras", ""), CB_SET_REMOVE },
                                        { CB_AVS("moves", ""), CB_SET_REMOVE },
                                        { CB_AVS("player_ids", ""), CB_SET_REMOVE },
                                        { CB_AVS(CB_ITEM_GSI1PK, ""), CB_SET_REMOVE },
                                        { CB_AVS(CB_ITEM_GSI1SK, ""), CB_SET_REMOVE } };
        for (int i = 0; i < fat.n; i++)
            w[1 + i] = (cb_iwrite){ .op = CB_IW_UPDATE, .key = { fat.text[i][0], fat.text[i][1] }, .sets = slim, .n_sets = 5,
                                    .cond = { CB_IF_EXISTS }, .n_cond = 1 };
        rc = fat.n ? s->ops->item_txn(s, w, 1 + fat.n, NULL, NULL) : s->ops->item_write(s, &w[0], NULL);
        if (rc == CB_E_CONFLICT) rc = CB_E_NOT_FOUND;
    }
    keylist_free(&fat);
    return rc;
}

bool cb_rows_chat_of(const cb_item *it, cb_chat_row *c) {
    memset(c, 0, sizeof *c);
    cb_av m;
    if (!s_of(it, "id", c->id, sizeof c->id) || !s_of(it, "game_id", c->game_id, sizeof c->game_id) ||
        !s_of(it, "user_id", c->user_id, sizeof c->user_id) || !cb_item_get(it, "message", &m) || m.kind != CB_AV_S)
        return false;
    c->message = strdup(m.s);
    if (!c->message) return false;
    bool_of(it, "is_system", &c->is_system);
    c->created_at = cb_item_n_or(it, "created_at", 0);
    return true;
}

int cb_rows_chat_put(cb_store *s, const cb_chat_row *c) {
    char pk[KB], sk[KB], g1[KB], gpk[KB], upk[KB];
    tsk(sk, sizeof sk, c->created_at, c->id);
    const cb_av a[] = {
        CB_AVS("gsi1pk", k(g1, sizeof g1, "USERCHAT#", c->user_id)), CB_AVS("gsi1sk", sk), CB_AVS("id", c->id),
        CB_AVS("game_id", c->game_id), CB_AVS("user_id", c->user_id), CB_AVS("message", c->message ? c->message : ""),
        CB_AVBOOL("is_system", c->is_system), CB_AVN("created_at", c->created_at),
    };
    const cb_iwrite w[3] = {
        { .op = CB_IW_PUT, .key = { k(pk, sizeof pk, "CHAT#", c->game_id), sk }, .attrs = a, .n_attrs = 8,
          .cond = { CB_IF_ABSENT }, .n_cond = 1 },
        { .op = CB_IW_CHECK, .key = { k(gpk, sizeof gpk, "GAME#", c->game_id), "ROW" }, .cond = { CB_IF_EXISTS }, .n_cond = 1 },
        { .op = CB_IW_CHECK, .key = { k(upk, sizeof upk, "USER#", c->user_id), "PROFILE" }, .cond = { CB_IF_EXISTS }, .n_cond = 1 },
    };
    int failed;
    int rc = s->ops->item_txn(s, w, 3, &failed, NULL);
    if (rc == CB_E_CONFLICT) rc = failed == 0 ? CB_E_CONFLICT : failed == 1 || failed == 2 ? CB_E_NOT_FOUND : CB_E_DB;
    return rc;
}

typedef struct {
    cb_chat_row *rows;
    int          n, cap, limit;
    bool         oom;
} chats;

static bool chat_step(const cb_item *it, void *ud) {
    chats *v = ud;
    cb_chat_row c;
    if (!cb_rows_chat_of(it, &c)) return true;
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 32;
        cb_chat_row *p = realloc(v->rows, (size_t)cap * sizeof *p);
        if (!p) { v->oom = true; free(c.message); return false; }
        v->rows = p;
        v->cap = cap;
    }
    v->rows[v->n++] = c;
    return v->limit <= 0 || v->n < v->limit;
}

// Newest `limit` first from the store, handed out oldest first.
int cb_rows_chat_list(cb_store *s, const char *game, int limit, cb_chat_fn fn, void *ud) {
    char pk[KB];
    chats v = { .limit = limit };
    int rc = query(s, (cb_iquery){ .pk = k(pk, sizeof pk, "CHAT#", game), .page = limit > 0 ? limit : 0 }, chat_step, &v);
    if (rc == CB_OK && v.oom) rc = CB_E_NOMEM;
    int n = 0;
    bool more = rc == CB_OK;
    for (int i = v.n - 1; i >= 0; i--) {
        if (more) {
            n++;
            more = fn(&v.rows[i], ud);
        }
        cb_chat_row_free(&v.rows[i]);
    }
    free(v.rows);
    return rc == CB_OK ? n : rc;
}
