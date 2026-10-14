// lib_rt_fakestore.h - one in-memory world for the realtime and PostgREST
// tests: a store with only the calls those modules make, a fake token check,
// fake view rows, and a recorder for API Gateway posts.
//
// rtf_install() points the store at `rtf.store` and swaps realtime's
// dependencies (realtime/rt_deps.h) for the fakes; rtf_reset() empties it all.
// Tokens: "tok-<uid>" verifies as <uid> (its username "U-<uid>"), anything
// else is CB_E_AUTH. Thread-safe: rt_local's test runs the protocol on a pool.
#ifndef CB_TEST_RT_FAKESTORE_H
#define CB_TEST_RT_FAKESTORE_H

#include "app/table_ops.h"
#include "store/store.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTF_MAX 64

typedef struct {
    char    game_id[CB_GAME_ID_MAX + 1];
    cb_uuid player_id;          // "" for the spectator row
    uint8_t view[16];
    size_t  view_len;
    int64_t version;
    char    status[16];
    int64_t updated_at;
} rtf_view;

typedef struct {
    char conn_id[64];
    char frame[2048];
} rtf_post;

typedef struct {
    cb_store store;
    pthread_mutex_t mu;
    int n_members;
    struct { char game[CB_GAME_ID_MAX + 1]; cb_uuid user; } members[RTF_MAX];
    int n_chat;
    cb_chat_row chat[RTF_MAX];          // message owned
    int n_bots;
    cb_bot_row bots[RTF_MAX];
    int n_ratings;
    cb_rating_row ratings[RTF_MAX];
    int n_snaps;
    cb_snapshot_row snaps[RTF_MAX];     // moves/extras owned
    int n_conns;
    cb_conn_row conns[RTF_MAX];
    int n_views;
    rtf_view views[RTF_MAX];
    // API Gateway: every post, and the connections that answer 410.
    int n_posts;
    rtf_post posts[RTF_MAX];
    int n_deletes;                      // cb_apigw_delete calls
    char gone[4][64];
    // Counters and forced failures.
    int conn_puts;
    int conn_conflicts;                 // conn_put calls refused by the version check
    void (*after_conn_get)(void *ud);   // run once, right after the next conn_get
    void *after_conn_get_ud;
    int is_member_rc;                   // 0: answer from `members`; else return this
} rtf_world;

extern rtf_world rtf;

void rtf_install(void);
void rtf_reset(void);

void rtf_member(const char *game, const char *user);
void rtf_add_view(const char *game, const char *player, const char *status, int64_t version, int64_t updated_at,
                  const uint8_t *view, size_t n);
void rtf_add_chat(const char *game, const char *user, const char *message, int64_t created_at);
void rtf_add_bot(const char *id, const char *nick, const char *key, int elo, int prev, int played, int64_t t);
void rtf_add_rating(const char *user, const char *name, int elo, int prev, int played, int64_t t);
void rtf_add_snap(const char *id, const char *game, const char *const *players, int n, const uint8_t *moves,
                  size_t mn, const uint8_t *extras, size_t en, int64_t created_at);
// The index of a stored connection, or -1.
int rtf_conn_find(const char *conn_id);

#endif
