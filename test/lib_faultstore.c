// lib_faultstore.c - a store that fails one chosen call (lib_faultstore.h).
// The wrappers below are one per store.h operation, each the operation's own
// signature, forwarding to the store it wraps.
#include "lib_faultstore.h"

#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
    cb_store base;
    cb_store *inner;
} fault_store;

static atomic_long g_left = -1, g_calls;
static atomic_int g_rc, g_failed;

void fs_fail_at(long k, int rc) {
    atomic_store(&g_calls, 0);
    atomic_store(&g_failed, 0);
    atomic_store(&g_rc, rc);
    atomic_store(&g_left, k);
}
void fs_off(void) { atomic_store(&g_left, -1); }
long fs_calls(void) { return atomic_load(&g_calls); }
bool fs_failed(void) { return atomic_load(&g_failed) != 0; }

// true when this call is the one to fail.
static bool take(void) {
    atomic_fetch_add(&g_calls, 1);
    long left = atomic_load(&g_left);
    while (left >= 0) {
        if (atomic_compare_exchange_weak(&g_left, &left, left - 1)) {
            if (left == 0) { atomic_store(&g_failed, 1); return true; }
            return false;
        }
    }
    return false;
}

#define IN(s) (((fault_store *)(s))->inner)

static int w_user_get(cb_store *s, const char *id, cb_user_row *out) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->user_get ? IN(s)->ops->user_get(IN(s), id, out) : CB_E_NOTIMPL;
}
static int w_user_by_email(cb_store *s, const char *email, cb_user_row *out) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->user_by_email ? IN(s)->ops->user_by_email(IN(s), email, out) : CB_E_NOTIMPL;
}
static int w_user_create(cb_store *s, const cb_user_row *u) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->user_create ? IN(s)->ops->user_create(IN(s), u) : CB_E_NOTIMPL;
}
static int w_user_set_password(cb_store *s, const char *id, const char *password_hash, const char *keep) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->user_set_password ? IN(s)->ops->user_set_password(IN(s), id, password_hash, keep) : CB_E_NOTIMPL;
}
static int w_user_end_sessions(cb_store *s, const char *id) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->user_end_sessions ? IN(s)->ops->user_end_sessions(IN(s), id) : CB_E_NOTIMPL;
}
static int w_user_touch_sign_in(cb_store *s, const char *id, int64_t at) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->user_touch_sign_in ? IN(s)->ops->user_touch_sign_in(IN(s), id, at) : CB_E_NOTIMPL;
}
static int w_signin_fails_get(cb_store *s, const char *email, int64_t now, cb_signin_fails *out) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->signin_fails_get ? IN(s)->ops->signin_fails_get(IN(s), email, now, out) : CB_E_NOTIMPL;
}
static int w_signin_fails_put(cb_store *s, const char *email, const cb_signin_fails *f, int64_t now) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->signin_fails_put ? IN(s)->ops->signin_fails_put(IN(s), email, f, now) : CB_E_NOTIMPL;
}
static int w_signin_fails_clear(cb_store *s, const char *email) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->signin_fails_clear ? IN(s)->ops->signin_fails_clear(IN(s), email) : CB_E_NOTIMPL;
}
static int w_user_delete(cb_store *s, const char *id) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->user_delete ? IN(s)->ops->user_delete(IN(s), id) : CB_E_NOTIMPL;
}
static int w_users_list(cb_store *s, const char *search, int offset, int limit, cb_user_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->users_list ? IN(s)->ops->users_list(IN(s), search, offset, limit, fn, ud) : CB_E_NOTIMPL;
}
static int w_refresh_put(cb_store *s, const cb_refresh_row *r) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->refresh_put ? IN(s)->ops->refresh_put(IN(s), r) : CB_E_NOTIMPL;
}
static int w_refresh_take(cb_store *s, const char *token_hash, int64_t now_ms, int64_t spent_ms, cb_refresh_row *out) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->refresh_take ? IN(s)->ops->refresh_take(IN(s), token_hash, now_ms, spent_ms, out) : CB_E_NOTIMPL;
}
static int w_session_put(cb_store *s, const cb_session_row *r) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->session_put ? IN(s)->ops->session_put(IN(s), r) : CB_E_NOTIMPL;
}
static int w_session_get(cb_store *s, const char *id, int64_t now_ms, cb_session_row *out) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->session_get ? IN(s)->ops->session_get(IN(s), id, now_ms, out) : CB_E_NOTIMPL;
}
static int w_session_revoke(cb_store *s, const char *id, const char *user_id, int64_t expires_at) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->session_revoke ? IN(s)->ops->session_revoke(IN(s), id, user_id, expires_at) : CB_E_NOTIMPL;
}
static int w_game_get(cb_store *s, const char *id, cb_game_row *out) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->game_get ? IN(s)->ops->game_get(IN(s), id, out) : CB_E_NOTIMPL;
}
static int w_game_create(cb_store *s, const cb_game_row *g, const char *creator_id) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->game_create ? IN(s)->ops->game_create(IN(s), g, creator_id) : CB_E_NOTIMPL;
}
static int w_game_commit(cb_store *s, const cb_game_commit *c) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->game_commit ? IN(s)->ops->game_commit(IN(s), c) : CB_E_NOTIMPL;
}
static int w_game_delete(cb_store *s, const char *id, int64_t expected_version) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->game_delete ? IN(s)->ops->game_delete(IN(s), id, expected_version) : CB_E_NOTIMPL;
}
static int w_games_of_user(cb_store *s, const char *user_id, int limit, cb_member_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->games_of_user ? IN(s)->ops->games_of_user(IN(s), user_id, limit, fn, ud) : CB_E_NOTIMPL;
}
static int w_games_get_views(cb_store *s, const char *const *ids, int n, cb_game_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->games_get_views ? IN(s)->ops->games_get_views(IN(s), ids, n, fn, ud) : CB_E_NOTIMPL;
}
static int w_is_member(cb_store *s, const char *game_id, const char *user_id) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->is_member ? IN(s)->ops->is_member(IN(s), game_id, user_id) : CB_E_NOTIMPL;
}
static int w_game_members(cb_store *s, const char *game_id, cb_name_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->game_members ? IN(s)->ops->game_members(IN(s), game_id, fn, ud) : CB_E_NOTIMPL;
}
static int w_games_needing_bots(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->games_needing_bots ? IN(s)->ops->games_needing_bots(IN(s), stale_before, active_after, limit, fn, ud) : CB_E_NOTIMPL;
}
static int w_lease_acquire(cb_store *s, const char *game_id, int64_t now, int64_t ttl_ms, cb_uuid token_out) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->lease_acquire ? IN(s)->ops->lease_acquire(IN(s), game_id, now, ttl_ms, token_out) : CB_E_NOTIMPL;
}
static int w_lease_renew(cb_store *s, const char *game_id, const char *token, int64_t now, int64_t ttl_ms) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->lease_renew ? IN(s)->ops->lease_renew(IN(s), game_id, token, now, ttl_ms) : CB_E_NOTIMPL;
}
static int w_lease_release(cb_store *s, const char *game_id, const char *token) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->lease_release ? IN(s)->ops->lease_release(IN(s), game_id, token) : CB_E_NOTIMPL;
}
static int w_bots_list(cb_store *s, cb_bot_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->bots_list ? IN(s)->ops->bots_list(IN(s), fn, ud) : CB_E_NOTIMPL;
}
static int w_ratings_list(cb_store *s, cb_rating_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->ratings_list ? IN(s)->ops->ratings_list(IN(s), fn, ud) : CB_E_NOTIMPL;
}
static int w_ratings_top(cb_store *s, int limit, cb_rating_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->ratings_top ? IN(s)->ops->ratings_top(IN(s), limit, fn, ud) : CB_E_NOTIMPL;
}
static int w_ratings_for(cb_store *s, const char *const *ids, int n, int32_t *out) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->ratings_for ? IN(s)->ops->ratings_for(IN(s), ids, n, out) : CB_E_NOTIMPL;
}
static int w_rating_set_username(cb_store *s, const char *user_id, const char *username) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->rating_set_username ? IN(s)->ops->rating_set_username(IN(s), user_id, username) : CB_E_NOTIMPL;
}
static int w_snapshot_put(cb_store *s, const cb_snapshot_row *r) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->snapshot_put ? IN(s)->ops->snapshot_put(IN(s), r) : CB_E_NOTIMPL;
}
static int w_snapshots_query(cb_store *s, const cb_snapshot_query *q, cb_snapshot_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->snapshots_query ? IN(s)->ops->snapshots_query(IN(s), q, fn, ud) : CB_E_NOTIMPL;
}
static int w_snapshot_set_extras(cb_store *s, const char *id, const uint8_t *extras, size_t extras_len) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->snapshot_set_extras ? IN(s)->ops->snapshot_set_extras(IN(s), id, extras, extras_len) : CB_E_NOTIMPL;
}
static int w_chat_put(cb_store *s, const cb_chat_row *r) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->chat_put ? IN(s)->ops->chat_put(IN(s), r) : CB_E_NOTIMPL;
}
static int w_chat_list(cb_store *s, const char *game_id, int limit, cb_chat_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->chat_list ? IN(s)->ops->chat_list(IN(s), game_id, limit, fn, ud) : CB_E_NOTIMPL;
}
static int w_bots_idle(cb_store *s, const char *game_id, int64_t version) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->bots_idle ? IN(s)->ops->bots_idle(IN(s), game_id, version) : CB_E_NOTIMPL;
}
static int w_lease_held(cb_store *s, const char *game_id, int64_t now) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->lease_held ? IN(s)->ops->lease_held(IN(s), game_id, now) : CB_E_NOTIMPL;
}
static int w_admin_collections(cb_store *s, cb_name_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->admin_collections ? IN(s)->ops->admin_collections(IN(s), fn, ud) : CB_E_NOTIMPL;
}
static int w_admin_browse(cb_store *s, const char *collection, const char *prefix, const char *cursor, int limit, cb_admin_row_fn fn, void *ud, char *next_cursor, size_t next_cap) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->admin_browse ? IN(s)->ops->admin_browse(IN(s), collection, prefix, cursor, limit, fn, ud, next_cursor, next_cap) : CB_E_NOTIMPL;
}
static int w_admin_query(cb_store *s, const char *statement, bool allow_write, cb_admin_row_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->admin_query ? IN(s)->ops->admin_query(IN(s), statement, allow_write, fn, ud) : CB_E_NOTIMPL;
}
static int w_user_import(cb_store *s, const cb_user_row *u) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->user_import ? IN(s)->ops->user_import(IN(s), u) : CB_E_NOTIMPL;
}
static int w_rating_import(cb_store *s, const cb_rating_row *r) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->rating_import ? IN(s)->ops->rating_import(IN(s), r) : CB_E_NOTIMPL;
}
static int w_rating_reindex(cb_store *s, const cb_rating_row *r) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->rating_reindex ? IN(s)->ops->rating_reindex(IN(s), r) : CB_E_NOTIMPL;
}
static int w_bots_replace(cb_store *s, const cb_bot_row *rows, int n) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->bots_replace ? IN(s)->ops->bots_replace(IN(s), rows, n) : CB_E_NOTIMPL;
}
static int w_game_import(cb_store *s, const cb_game_row *g, const cb_uuid *humans, int n_humans, const cb_uuid *bots, int n_bots) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->game_import ? IN(s)->ops->game_import(IN(s), g, humans, n_humans, bots, n_bots) : CB_E_NOTIMPL;
}

static int w_item_get(cb_store *s, cb_ikey key, bool consistent, cb_item *out) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->item_get ? IN(s)->ops->item_get(IN(s), key, consistent, out) : CB_E_NOTIMPL;
}
static int w_item_write(cb_store *s, const cb_iwrite *w, cb_item *old) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->item_write ? IN(s)->ops->item_write(IN(s), w, old) : CB_E_NOTIMPL;
}
static int w_item_txn(cb_store *s, const cb_iwrite *w, int n, int *failed, bool *had) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->item_txn ? IN(s)->ops->item_txn(IN(s), w, n, failed, had) : CB_E_NOTIMPL;
}
static int w_item_query(cb_store *s, const cb_iquery *q, cb_item_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->item_query ? IN(s)->ops->item_query(IN(s), q, fn, ud) : CB_E_NOTIMPL;
}
static int w_item_batch_get(cb_store *s, const cb_ikey *keys, int n, const char *const *proj, int n_proj, cb_item_at_fn fn, void *ud) {
    if (take()) return atomic_load(&g_rc);
    return IN(s)->ops->item_batch_get ? IN(s)->ops->item_batch_get(IN(s), keys, n, proj, n_proj, fn, ud) : CB_E_NOTIMPL;
}

static void w_close(cb_store *s) {
    cb_store_close(IN(s));
    free(s);
}

static const cb_store_ops FAULT_OPS = {
    .name = "fault",
    .close = w_close,
    .user_get = w_user_get,
    .user_by_email = w_user_by_email,
    .user_create = w_user_create,
    .user_set_password = w_user_set_password,
    .user_touch_sign_in = w_user_touch_sign_in,
    .signin_fails_get = w_signin_fails_get,
    .signin_fails_put = w_signin_fails_put,
    .signin_fails_clear = w_signin_fails_clear,
    .user_delete = w_user_delete,
    .users_list = w_users_list,
    .refresh_put = w_refresh_put,
    .refresh_take = w_refresh_take,
    .session_put = w_session_put,
    .session_get = w_session_get,
    .session_revoke = w_session_revoke,
    .user_end_sessions = w_user_end_sessions,
    .game_get = w_game_get,
    .game_create = w_game_create,
    .game_commit = w_game_commit,
    .game_delete = w_game_delete,
    .games_of_user = w_games_of_user,
    .games_get_views = w_games_get_views,
    .is_member = w_is_member,
    .game_members = w_game_members,
    .games_needing_bots = w_games_needing_bots,
    .lease_acquire = w_lease_acquire,
    .lease_renew = w_lease_renew,
    .lease_release = w_lease_release,
    .bots_list = w_bots_list,
    .ratings_list = w_ratings_list,
    .ratings_top = w_ratings_top,
    .ratings_for = w_ratings_for,
    .rating_set_username = w_rating_set_username,
    .snapshot_put = w_snapshot_put,
    .snapshots_query = w_snapshots_query,
    .snapshot_set_extras = w_snapshot_set_extras,
    .chat_put = w_chat_put,
    .chat_list = w_chat_list,
    .bots_idle = w_bots_idle,
    .lease_held = w_lease_held,
    .admin_collections = w_admin_collections,
    .admin_browse = w_admin_browse,
    .admin_query = w_admin_query,
    .user_import = w_user_import,
    .rating_import = w_rating_import,
    .rating_reindex = w_rating_reindex,
    .bots_replace = w_bots_replace,
    .game_import = w_game_import,
    .item_get = w_item_get,
    .item_write = w_item_write,
    .item_txn = w_item_txn,
    .item_query = w_item_query,
    .item_batch_get = w_item_batch_get,
};

// Every operation of store.h is wrapped above. An operation added there and
// not here would be left NULL (CB_E_NOTIMPL to whatever calls it through this
// store), so the table's size is held to the count: the name, close, and 54 - 1.
_Static_assert(sizeof(cb_store_ops) == (size_t)(54 + 1) * sizeof(void *),
               "store.h has an operation lib_faultstore.c does not wrap: add its w_ wrapper and its line in FAULT_OPS");

cb_store *fs_wrap(cb_store *inner) {
    fault_store *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->base.ops = &FAULT_OPS;
    f->inner = inner;
    return &f->base;
}
