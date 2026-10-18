// rows_foolish.h - foolish's rows and the accounts over the item layer
// (rows_foolish.c), the operations each backend's cb_store_ops points at.
// Their contracts are store.h's, operation by operation.
#ifndef CB_ROWS_FOOLISH_H
#define CB_ROWS_FOOLISH_H

#include "store.h"

// One write and the storage it points into (the bench and the tests build
// the requests a commit sends without a store).
typedef struct {
    cb_iwrite w;
    cb_iset   sets[16];
    cb_av     attrs[20];
    char      pk[400];
} cb_rows_write;

// The lone commit's write: the CAS on the row (rows_foolish.c "THE CAS").
void cb_rows_cas_write(const cb_game_commit *c, int64_t last_commit_at, cb_rows_write *out);
// The game row, put whole (as a create or an import writes it).
void cb_rows_game_put_write(const cb_game_row *g, cb_rows_write *out);
// An item read into its row (byte fields and a chat's message malloc'd):
// false when it is not one (a required attribute missing or mistyped). The
// store's reads use them; the fuzz targets reach them directly.
bool cb_rows_game_of(const cb_item *it, cb_game_row *g);       // GAME#<id> ROW
bool cb_rows_user_of(const cb_item *it, cb_user_row *u);       // USER#<id> PROFILE
bool cb_rows_bot_of(const cb_item *it, cb_bot_row *b);         // BOT#<id> BOT
bool cb_rows_rating_of(const cb_item *it, cb_rating_row *r);   // USER#<id> RATING
bool cb_rows_snapshot_of(const cb_item *it, cb_snapshot_row *s);   // SNAP#<id> SNAP
bool cb_rows_chat_of(const cb_item *it, cb_chat_row *c);       // CHAT#<game> <created>#<id>

int cb_rows_user_get(cb_store *s, const char *id, cb_user_row *out);
int cb_rows_user_by_email(cb_store *s, const char *email, cb_user_row *out);
int cb_rows_user_create(cb_store *s, const cb_user_row *u);
int cb_rows_user_import(cb_store *s, const cb_user_row *u);
int cb_rows_user_set_password(cb_store *s, const char *id, const char *hash, const char *keep_session);
int cb_rows_user_end_sessions(cb_store *s, const char *id);
int cb_rows_user_touch_sign_in(cb_store *s, const char *id, int64_t at);
int cb_rows_user_delete(cb_store *s, const char *id);
int cb_rows_users_list(cb_store *s, const char *search, int offset, int limit, cb_user_fn fn, void *ud);
int cb_rows_game_get(cb_store *s, const char *id, cb_game_row *out);
int cb_rows_games_get_views(cb_store *s, const char *const *ids, int n, cb_game_fn fn, void *ud);
int cb_rows_game_create(cb_store *s, const cb_game_row *g, const char *creator);
int cb_rows_game_commit(cb_store *s, const cb_game_commit *c);
int cb_rows_game_import(cb_store *s, const cb_game_row *g, const cb_uuid *humans, int n_humans, const cb_uuid *bots,
                        int n_bots);
int cb_rows_game_delete(cb_store *s, const char *id, int64_t expected);
int cb_rows_games_of_user(cb_store *s, const char *user, int limit, cb_member_fn fn, void *ud);
int cb_rows_is_member(cb_store *s, const char *game, const char *user);
int cb_rows_game_members(cb_store *s, const char *game, cb_name_fn fn, void *ud);
int cb_rows_games_needing_bots(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn,
                               void *ud);
int cb_rows_lease_acquire(cb_store *s, const char *game, int64_t now, int64_t ttl, cb_uuid token_out);
int cb_rows_lease_renew(cb_store *s, const char *game, const char *token, int64_t now, int64_t ttl);
int cb_rows_lease_release(cb_store *s, const char *game, const char *token);
int cb_rows_bots_idle(cb_store *s, const char *game, int64_t version);
int cb_rows_lease_held(cb_store *s, const char *game, int64_t now);
int cb_rows_bots_list(cb_store *s, cb_bot_fn fn, void *ud);
int cb_rows_ratings_list(cb_store *s, cb_rating_fn fn, void *ud);
int cb_rows_ratings_top(cb_store *s, int limit, cb_rating_fn fn, void *ud);
int cb_rows_ratings_for(cb_store *s, const char *const *ids, int n, int32_t *out);
int cb_rows_rating_set_username(cb_store *s, const char *user, const char *username);
int cb_rows_rating_import(cb_store *s, const cb_rating_row *rt);
int cb_rows_rating_reindex(cb_store *s, const cb_rating_row *rt);
int cb_rows_bots_replace(cb_store *s, const cb_bot_row *rows, int n);
int cb_rows_snapshot_put(cb_store *s, const cb_snapshot_row *sn);
int cb_rows_snapshots_query(cb_store *s, const cb_snapshot_query *q, cb_snapshot_fn fn, void *ud);
int cb_rows_snapshot_set_extras(cb_store *s, const char *id, const uint8_t *extras, size_t extras_len);
int cb_rows_chat_put(cb_store *s, const cb_chat_row *c);
int cb_rows_chat_list(cb_store *s, const char *game, int limit, cb_chat_fn fn, void *ud);

// Every operation above, for a backend's cb_store_ops (with rows_auth.h's).
#define CB_ROWS_FOOLISH_OPS \
    .user_get = cb_rows_user_get, \
    .user_by_email = cb_rows_user_by_email, \
    .user_create = cb_rows_user_create, \
    .user_import = cb_rows_user_import, \
    .user_set_password = cb_rows_user_set_password, \
    .user_end_sessions = cb_rows_user_end_sessions, \
    .user_touch_sign_in = cb_rows_user_touch_sign_in, \
    .user_delete = cb_rows_user_delete, \
    .users_list = cb_rows_users_list, \
    .game_get = cb_rows_game_get, \
    .games_get_views = cb_rows_games_get_views, \
    .game_create = cb_rows_game_create, \
    .game_commit = cb_rows_game_commit, \
    .game_import = cb_rows_game_import, \
    .game_delete = cb_rows_game_delete, \
    .games_of_user = cb_rows_games_of_user, \
    .is_member = cb_rows_is_member, \
    .game_members = cb_rows_game_members, \
    .games_needing_bots = cb_rows_games_needing_bots, \
    .lease_acquire = cb_rows_lease_acquire, \
    .lease_renew = cb_rows_lease_renew, \
    .lease_release = cb_rows_lease_release, \
    .bots_idle = cb_rows_bots_idle, \
    .lease_held = cb_rows_lease_held, \
    .bots_list = cb_rows_bots_list, \
    .ratings_list = cb_rows_ratings_list, \
    .ratings_top = cb_rows_ratings_top, \
    .ratings_for = cb_rows_ratings_for, \
    .rating_set_username = cb_rows_rating_set_username, \
    .rating_import = cb_rows_rating_import, \
    .rating_reindex = cb_rows_rating_reindex, \
    .bots_replace = cb_rows_bots_replace, \
    .snapshot_put = cb_rows_snapshot_put, \
    .snapshots_query = cb_rows_snapshots_query, \
    .snapshot_set_extras = cb_rows_snapshot_set_extras, \
    .chat_put = cb_rows_chat_put, \
    .chat_list = cb_rows_chat_list,

#endif
