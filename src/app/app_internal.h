// app_internal.h - what table_ops.c shares with bots.c and functions_http.c,
// and the seams the app's unit tests drive. Nothing outside src/app/ and its
// tests includes this.
//
// THE SECTION. One kernel section's scratch: the Game, the hook snapshots, the
// Table, the commit products and their arena, about 300 KB, heap-allocated per
// operation so any thread may run one (DESIGN.md "Kernel thread safety"). A
// section is loaded from a stored row, an operation runs on section->t, and
// cb_section_commit turns what it did into one CAS commit plus everything a
// winning commit owes: the cache, the end of the game, the fan-out.
#ifndef CB_APP_INTERNAL_H
#define CB_APP_INTERNAL_H

#include "store/store.h"
#include "table_ops.h"

#include <stdbool.h>
#include <stdint.h>

// ---- seams ------------------------------------------------------------------------

// The host clock every kernel call is handed, and the sleep a bot drive paces
// with. cb_now_ms and cb_sleep_ms; a test points both at a fake clock so the
// kernel's pacing costs no wall time.
extern int64_t (*cb_app_now)(void);
extern void (*cb_app_sleep)(int64_t ms);

// ---- the per-process row cache (game_cache.ts) ------------------------------------

#define CB_ROW_CACHE_CAP 256

// A row: this process's cache when allowed and warm (*cached set), else the
// store. CB_OK, CB_E_NOT_FOUND, or the store's error.
int  cb_row_load(cb_ctx *ctx, const char *id, bool allow_cache, cb_game_row *out, bool *cached);
// After a winning commit: a PLAYING row is remembered, any other is evicted.
void cb_row_cache_note(const cb_game_row *row);
void cb_row_cache_drop(const char *id);
void cb_row_cache_clear(void);
int  cb_row_cache_size(void);

// ---- the section ------------------------------------------------------------------

#define CB_ARENA_BYTES (128 * 1024)

typedef struct cb_section {
    Game        g;
    TableSnaps  snaps;
    Table       t;
    TableCommit c;
    char        gid[CB_GAME_ID_MAX + 1];
    // The humans the loaded roster seated before the operation (membership diff).
    int         n_old;
    cb_uuid     old_humans[CB_MAX_SEATS];
    uint8_t     arena[CB_ARENA_BYTES];
} cb_section;

cb_section *cb_section_new(void);
void cb_section_free(cb_section *s);

// table_init + table_load + table_set_deal_seed (the row's seed as lowercase
// hex, "" for none), and the old humans noted. The kernel's result (< 0 a
// stored row the kernel refuses).
int cb_section_load(cb_section *s, const cb_game_row *row);

// One viewer's envelope of the loaded table at `version` (viewer -1: the
// spectator), malloc'd. CB_OK or CB_E_STATE.
int cb_section_envelope(cb_section *s, int viewer, uint32_t version, uint8_t **out, size_t *out_len);

// What a commit attempt came to.
typedef struct {
    bool     lost;          // the CAS lost: nothing was written; reload and run again
    int64_t  version;       // the committed version (row->version + 1)
    bool     needs_bots;
    bool     ended;
    uint8_t *actor_env;     // the actor's envelope (seat, else spectator); NULL when no actor_id
    size_t   actor_env_len;
} cb_commit_out;

// Commits the operation that just ran on s->t against `row` (the version it
// was loaded at): the products (rated when the kernel wants ratings, from
// `ratings` when *have_ratings, else read from the store into it), the new row
// (logs appended or reset, round_epoch, the deal seed when `dealt_seed` and the
// operation dealt), the CAS. On a win: the cache, the snapshot when the game
// ended, the fan-out. CB_OK (look at out->lost), or CB_E_STATE / the store's
// error.
int cb_section_commit(cb_ctx *ctx, cb_section *s, const cb_game_row *row, const uint8_t *dealt_seed,
                      const char *actor_id, int32_t ratings[CB_MAX_SEATS], bool *have_ratings,
                      cb_commit_out *out);

#endif
