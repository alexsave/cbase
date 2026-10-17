// lib_app_store_mem.h - an in-RAM cb_store for the app's tests.
//
// Implements the parts of store.h the app layer calls (users, games and their
// CAS, membership, bots, ratings, snapshots, the bot lease, the scan) with the
// semantics store.h documents, behind one mutex. Chat, connections and the
// admin calls are left NULL (CB_E_NOTIMPL). It is a test double, not a backend:
// fixed capacities, linear scans.
//
// Hooks a test drives:
//   steal_commits  the next N game_commit calls find the stored version moved
//                  on by "another writer" (the stored version is bumped first),
//                  so the CAS loses exactly as a real race would.
//   fail_ratings   ratings_for answers CB_E_DB.
#ifndef CB_TEST_APP_STORE_MEM_H
#define CB_TEST_APP_STORE_MEM_H

#include "store/store.h"

typedef struct {
    int steal_commits;     // see above
    int fail_ratings;      // see above
    int commits;           // game_commit calls that wrote
    int conflicts;         // game_commit calls that lost the CAS
    int snapshots;         // snapshot_put calls
    int bots_idle;         // bots_idle calls that took a game out of the scan
} tm_hooks;

// A fresh store with `n_bots` bots seated in the bots table, each with
// strategy key `brain` ("random" plays instantly).
cb_store *tm_store_new(int n_bots, const char *brain);
tm_hooks *tm_hooks_of(cb_store *s);
// The i-th bot's id (NULL past the end).
const char *tm_bot_id(cb_store *s, int i);
// A user row (email "<name>@test", no password), its rating row with it.
int tm_add_user(cb_store *s, const char *id, const char *username);
// A user's account age, for the rated-age rule (table_ops.c, M6).
void tm_set_user_created(cb_store *s, const char *id, int64_t created_at);
// A user's or a bot's rating row: CB_OK and the row, or CB_E_NOT_FOUND.
int tm_rating(cb_store *s, const char *id, cb_rating_row *out);
// The game's lease holder token ("" for none) and until (ms).
int tm_lease(cb_store *s, const char *game_id, char token[CB_UUID_LEN + 1], int64_t *until);
// Rewrites what the heartbeat's scan reads of a stored game.
// Puts a lease on the game by hand (a drive elsewhere holding it), until `until`; 0 frees it.
int tm_hold_lease(cb_store *s, const char *game_id, int64_t until);
// Called (outside the store's lock) by lease_release before it frees the
// lease: a move made there lands while the drive still holds it. NULL: none.
extern void (*tm_on_release)(const char *game_id);
int tm_set_scan(cb_store *s, const char *game_id, bool needs_bots, int64_t updated_at, int64_t last_commit_at);

#endif
