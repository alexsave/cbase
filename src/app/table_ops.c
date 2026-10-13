// table_ops.c - one game operation per request (table_ops.h), and the section
// and row cache the bot loop shares (app_internal.h).
//
// The port of table_io.ts (runTableOp, commitProducts, ratingsFor), finalize.ts,
// game_cache.ts and the row arithmetic commit_table did in plpgsql, onto the C
// kernel called directly. What a blob, a seat or a push means stays the
// kernel's; what is here is which row to read, which results commit, the CAS
// loop, and the new row's bookkeeping columns.
#include "app_internal.h"
#include "fanout.h"

#include "util/clock.h"
#include "util/err.h"
#include "util/hex.h"
#include "util/log.h"
#include "util/str.h"
#include "util/uuid.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A pickup can carry every card in play, and the session log must record it
// whole, or table_replay_code's gate refuses the finished game and it gets no
// replay. The kernel's compact default (16) is its arena's; the Makefile builds
// with the shipped human-game capacities (KERNEL_CAPS), and this holds it there.
#include "game.h"
_Static_assert(MAX_LOG_PAIRS >= MAX_DECK,
               "the kernel must be built with the human-game capacities (c_base/Makefile KERNEL_CAPS)");

int64_t (*cb_app_now)(void) = cb_now_ms;
void (*cb_app_sleep)(int64_t ms) = cb_sleep_ms;

// ---- byte helpers ------------------------------------------------------------------

static uint8_t *dup_bytes(const void *p, size_t n) {
    uint8_t *d = malloc(n ? n : 1);
    if (d && n) memcpy(d, p, n);
    return d;
}

// A deep copy of a row: every byte field owned by the copy.
static int row_copy(cb_game_row *dst, const cb_game_row *src) {
    *dst = *src;
    dst->state = dup_bytes(src->state, src->state_len);
    dst->roster = dup_bytes(src->roster, src->roster_len);
    dst->logs = src->logs_len ? dup_bytes(src->logs, src->logs_len) : NULL;
    if (!dst->state || !dst->roster || (src->logs_len && !dst->logs)) {
        cb_game_row_free(dst);
        return CB_E_NOMEM;
    }
    return CB_OK;
}

// ---- the row cache (game_cache.ts) ---------------------------------------------------
//
// Rows this process just committed, PLAYING games only, least recently noted
// evicted at CB_ROW_CACHE_CAP. Correctness never depends on it: every commit is
// fenced on the version, so a stale entry costs one conflict and a reload, and
// a refusal computed from an entry is re-checked fresh (cb_table_run). Unlike
// the TypeScript cache it also holds the session log, because the C store
// writes the whole row and the log is appended here, not in SQL.

typedef struct {
    bool        used;
    uint64_t    tick;
    cb_game_row row;
} cache_entry;

static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;
static cache_entry g_cache[CB_ROW_CACHE_CAP];
static uint64_t g_cache_tick;

static cache_entry *cache_find(const char *id) {
    for (int i = 0; i < CB_ROW_CACHE_CAP; i++)
        if (g_cache[i].used && strcmp(g_cache[i].row.id, id) == 0) return &g_cache[i];
    return NULL;
}

static void cache_evict(cache_entry *e) {
    cb_game_row_free(&e->row);
    memset(e, 0, sizeof *e);
}

void cb_row_cache_drop(const char *id) {
    pthread_mutex_lock(&g_cache_mu);
    cache_entry *e = cache_find(id);
    if (e) cache_evict(e);
    pthread_mutex_unlock(&g_cache_mu);
}

void cb_row_cache_clear(void) {
    pthread_mutex_lock(&g_cache_mu);
    for (int i = 0; i < CB_ROW_CACHE_CAP; i++)
        if (g_cache[i].used) cache_evict(&g_cache[i]);
    pthread_mutex_unlock(&g_cache_mu);
}

int cb_row_cache_size(void) {
    int n = 0;
    pthread_mutex_lock(&g_cache_mu);
    for (int i = 0; i < CB_ROW_CACHE_CAP; i++) n += g_cache[i].used;
    pthread_mutex_unlock(&g_cache_mu);
    return n;
}

void cb_row_cache_note(const cb_game_row *row) {
    // A lobby or a finished game is edited from everywhere: read it fresh.
    if (row->status != GAME_STATUS_PLAYING) { cb_row_cache_drop(row->id); return; }
    cb_game_row copy;
    if (row_copy(&copy, row) != CB_OK) { cb_row_cache_drop(row->id); return; }
    pthread_mutex_lock(&g_cache_mu);
    cache_entry *e = cache_find(row->id);
    if (!e) {
        cache_entry *oldest = NULL;
        for (int i = 0; i < CB_ROW_CACHE_CAP && !e; i++)
            if (!g_cache[i].used) e = &g_cache[i];
        if (!e) {
            for (int i = 0; i < CB_ROW_CACHE_CAP; i++)
                if (!oldest || g_cache[i].tick < oldest->tick) oldest = &g_cache[i];
            e = oldest;
        }
    }
    if (e->used) cache_evict(e);
    e->used = true;
    e->tick = ++g_cache_tick;
    e->row = copy;
    pthread_mutex_unlock(&g_cache_mu);
}

int cb_row_load(cb_ctx *ctx, const char *id, bool allow_cache, cb_game_row *out, bool *cached) {
    *cached = false;
    if (allow_cache) {
        int rc = CB_E_NOT_FOUND;
        pthread_mutex_lock(&g_cache_mu);
        cache_entry *e = cache_find(id);
        if (e) rc = row_copy(out, &e->row);
        pthread_mutex_unlock(&g_cache_mu);
        if (rc == CB_OK) { *cached = true; return CB_OK; }
    }
    memset(out, 0, sizeof *out);
    return CB_STORE_CALL(ctx->store, game_get, id, out);
}

// ---- the section -----------------------------------------------------------------------

cb_section *cb_section_new(void) {
    return calloc(1, sizeof(cb_section));
}

void cb_section_free(cb_section *s) {
    free(s);
}

int cb_section_load(cb_section *s, const cb_game_row *row) {
    snprintf(s->gid, sizeof s->gid, "%s", row->id);
    table_init(&s->t, &s->g, &s->snaps);
    int rc = table_load(&s->t, row->state, (int)row->state_len, row->roster, (int)row->roster_len);
    if (rc < 0) return rc;
    // Every mid-game draw and every bot decision is seeded from this game's
    // deal seed, as its hex text exactly as table_io.ts's bareHex wrote it.
    char hex[2 * CB_SEED_LEN + 1] = "";
    if (row->has_seed) cb_hex_encode(row->seed, CB_SEED_LEN, hex, sizeof hex);
    rc = table_set_deal_seed(&s->t, hex, (int)strlen(hex));
    if (rc < 0) return rc;
    s->n_old = 0;
    for (int i = 0; i < s->t.r.n; i++) {
        const RosterSeat *seat = &s->t.r.seats[i];
        // memcpy, not snprintf: the seat and the list live in the same struct,
        // and gcc's -Wrestrict (fortified, under ASan) cannot prove they do not overlap.
        if (seat->brain_len == 0 && s->n_old < CB_MAX_SEATS) {
            size_t n = seat->id_len < CB_UUID_LEN ? (size_t)seat->id_len : CB_UUID_LEN;
            cb_strncpy0(s->old_humans[s->n_old++], sizeof(cb_uuid), (const char *)seat->id, n);
        }
    }
    return TABLE_OK;
}

int cb_section_envelope(cb_section *s, int viewer, uint32_t version, uint8_t **out, size_t *out_len) {
    uint8_t buf[CB_ENVELOPE_MAX];
    int n = table_envelope(&s->t, s->gid, (int)strlen(s->gid), viewer, version, buf, sizeof buf);
    if (n < 0) {
        CB_ERROR("table", "game %s: envelope for viewer %d refused (%d)", s->gid, viewer, n);
        return CB_E_STATE;
    }
    *out = dup_bytes(buf, (size_t)n);
    *out_len = (size_t)n;
    return *out ? CB_OK : CB_E_NOMEM;
}

// One viewer's cast for the last operation, malloc'd. The buffer is generous:
// a deal's push carries every snapshot's events.
static int section_cast(cb_section *s, int viewer, uint32_t version, uint8_t **out, size_t *out_len) {
    enum { CAST_CAP = 64 * 1024 };
    uint8_t *buf = malloc(CAST_CAP);
    if (!buf) return CB_E_NOMEM;
    int n = table_cast(&s->t, s->gid, (int)strlen(s->gid), viewer, version, buf, CAST_CAP);
    if (n < 0) {
        free(buf);
        CB_ERROR("table", "game %s: cast for viewer %d refused (%d)", s->gid, viewer, n);
        return CB_E_STATE;
    }
    *out = buf;
    *out_len = (size_t)n;
    return CB_OK;
}

static bool seat_is_human(const RosterSeat *seat) { return seat->brain_len == 0; }

static void seat_id(const RosterSeat *seat, cb_uuid out) {
    snprintf(out, sizeof(cb_uuid), "%.*s", (int)seat->id_len, seat->id);
}

// ---- the end of a game (finalize.ts) ------------------------------------------------------
//
// After the winning commit only, so it happens once: the verified v6 replay
// code and the extras, from the committed row's deal seed and session log, as
// one snapshot row. A failure is logged and swallowed: the game has ended
// either way, and the session log stays as the record until the next session
// replaces it. The TypeScript host then cleared logs_packed with a second,
// unfenced UPDATE; the C store has no such write, so the log is left for the
// next commit that leaves the finished table (commit_table's rule: a lobby
// commit empties it, a deal replaces it).
static void finalize_game(cb_ctx *ctx, cb_section *s, const cb_game_row *nrow) {
    enum { CODE_CAP = 2 * 1024 * 1024, SCRATCH_CAP = 1024 * 1024, EXTRAS_CAP = 256 * 1024 };
    uint8_t *code = NULL, *scratch = NULL, *extras = NULL;
    const char *why = NULL;
    int rc = 0;
    if (!nrow->has_seed) { why = "no deal seed: nothing to encode a replay from"; goto out; }
    if (nrow->logs_len == 0) { why = "no session log: nothing to encode a replay from"; goto out; }
    code = malloc(CODE_CAP);
    scratch = malloc(SCRATCH_CAP);
    extras = malloc(EXTRAS_CAP);
    if (!code || !scratch || !extras) { why = "out of memory"; goto out; }
    table_init(&s->t, &s->g, &s->snaps);
    if ((rc = table_load(&s->t, nrow->state, (int)nrow->state_len, nrow->roster, (int)nrow->roster_len)) < 0) {
        why = "does not load";
        goto out;
    }
    int n_extras = table_replay_extras(&s->t, nrow->logs, (int)nrow->logs_len, extras, EXTRAS_CAP);
    if (n_extras < 0) { rc = n_extras; why = "replay extras refused"; goto out; }
    if ((rc = table_load(&s->t, nrow->state, (int)nrow->state_len, nrow->roster, (int)nrow->roster_len)) < 0) {
        why = "does not load";
        goto out;
    }
    int n_code = table_replay_code(&s->t, nrow->seed, CB_SEED_LEN, nrow->logs, (int)nrow->logs_len,
                                   code, CODE_CAP, scratch, SCRATCH_CAP);
    if (n_code < 0) { rc = n_code; why = "replay code refused"; goto out; }
    CB_INFO("table", "game %s: replay encoded (v6) to %d+%d bytes", nrow->id, n_code, n_extras);

    cb_snapshot_row snap;
    memset(&snap, 0, sizeof snap);
    cb_uuid_v4(snap.id);
    snprintf(snap.game_id, sizeof snap.game_id, "%s", nrow->id);
    // Every seat, bots included, in seat order: what finalize.ts wrote.
    for (int i = 0; i < s->t.r.n && i < CB_MAX_SEATS; i++) seat_id(&s->t.r.seats[i], snap.player_ids[snap.n_players++]);
    snap.moves = code;
    snap.moves_len = (size_t)n_code;
    snap.extras = extras;
    snap.extras_len = (size_t)n_extras;
    snap.created_at = cb_app_now();
    if ((rc = CB_STORE_CALL(ctx->store, snapshot_put, &snap)) != CB_OK) why = "snapshot_put failed";
out:
    if (why) CB_ERROR("table", "game %s: snapshot failed, keeping the log: %s (%d)", nrow->id, why, rc);
    free(code);
    free(scratch);
    free(extras);
}

// ---- the commit ---------------------------------------------------------------------------

// The seats' ratings into `ratings` (TABLE_RATING_NONE for none). false when
// they cannot be read: the game then ends unrated, as ratingsFor's null did.
static bool read_ratings(cb_ctx *ctx, const Table *t, int32_t ratings[CB_MAX_SEATS]) {
    cb_uuid ids[CB_MAX_SEATS];
    const char *ptrs[CB_MAX_SEATS];
    for (int i = 0; i < t->r.n; i++) { seat_id(&t->r.seats[i], ids[i]); ptrs[i] = ids[i]; }
    int rc = CB_STORE_CALL(ctx->store, ratings_for, ptrs, t->r.n, ratings);
    if (rc != CB_OK) CB_WARN("table", "seat ratings unreadable (%s): the game ends unrated", cb_strerror(rc));
    return rc == CB_OK;
}

static bool in_list(const cb_uuid *list, int n, const char *id) {
    for (int i = 0; i < n; i++) if (strcmp(list[i], id) == 0) return true;
    return false;
}

int cb_section_commit(cb_ctx *ctx, cb_section *s, const cb_game_row *row, const uint8_t *dealt_seed,
                      const char *actor_id, int32_t ratings[CB_MAX_SEATS], bool *have_ratings,
                      cb_commit_out *out) {
    memset(out, 0, sizeof *out);
    Table *t = &s->t;
    TableCommit *c = &s->c;
    const int gid_len = (int)strlen(s->gid);
    const int64_t now = cb_app_now();
    const uint32_t next = (uint32_t)(row->version + 1);
    int rc = table_commit_products(t, s->gid, gid_len, next, now, c, s->arena, sizeof s->arena);
    if (rc >= 0 && c->wants_ratings) {
        // The kernel rates the game inside the section that ends it. Its products
        // are const, so the C host rates and asks for them again: no re-run.
        if (!*have_ratings) *have_ratings = read_ratings(ctx, t, ratings);
        if (*have_ratings) {
            table_rate(t, ratings, t->r.n);
            rc = table_commit_products(t, s->gid, gid_len, next, now, c, s->arena, sizeof s->arena);
        }
    }
    if (rc < 0) {
        CB_ERROR("table", "game %s: no commit products (%d)", s->gid, rc);
        return CB_E_STATE;
    }

    // ---- the new row: commit_table's arithmetic, moved here -------------------
    const uint8_t *A = s->arena;
    const bool waiting = c->status == GAME_STATUS_WAITING;
    cb_game_row nrow;
    memset(&nrow, 0, sizeof nrow);
    snprintf(nrow.id, sizeof nrow.id, "%s", row->id);
    nrow.status = c->status;
    nrow.state = (uint8_t *)A + c->state.off;
    nrow.state_len = (size_t)c->state.len;
    nrow.roster = (uint8_t *)A + c->roster.off;
    nrow.roster_len = (size_t)c->roster.len;
    nrow.needs_bots = c->needs_bots;
    nrow.version = row->version + 1;
    nrow.created_at = row->created_at;
    nrow.updated_at = now;
    nrow.last_commit_at = now;
    // The deal seed: none in a lobby; the fresh one when this operation dealt;
    // else the stored one.
    if (!waiting && c->dealt_now && dealt_seed) {
        nrow.has_seed = true;
        memcpy(nrow.seed, dealt_seed, CB_SEED_LEN);
    } else if (!waiting && row->has_seed) {
        nrow.has_seed = true;
        memcpy(nrow.seed, row->seed, CB_SEED_LEN);
    }
    // The session log: a deal replaces it, a lobby empties it, anything else
    // appends this operation's records.
    uint8_t *logs = NULL;
    if (c->logs_reset) {
        nrow.logs = (uint8_t *)A + c->logs.off;
        nrow.logs_len = (size_t)c->logs.len;
    } else if (!waiting) {
        size_t n = row->logs_len + (size_t)c->logs.len;
        if (n) {
            logs = malloc(n);
            if (!logs) return CB_E_NOMEM;
            if (row->logs_len) memcpy(logs, row->logs, row->logs_len);
            if (c->logs.len) memcpy(logs + row->logs_len, A + c->logs.off, (size_t)c->logs.len);
        }
        nrow.logs = logs;
        nrow.logs_len = n;
    }
    // The round guard's epoch: 0 on a deal or in a lobby, the new version when
    // this operation closed a round, else unchanged.
    nrow.round_epoch = (c->logs_reset || waiting) ? 0 : c->closed_round ? row->version + 1 : row->round_epoch;

    cb_game_commit gc;
    memset(&gc, 0, sizeof gc);
    gc.row = &nrow;
    gc.expected_version = row->version;
    for (int i = 0; i < t->r.n; i++) {
        const RosterSeat *seat = &t->r.seats[i];
        if (seat_is_human(seat)) seat_id(seat, gc.humans[gc.n_humans++]);
        else seat_id(seat, gc.bots[gc.n_bots++]);
    }
    if (c->n_rated > 0) {
        gc.n_rated = t->r.n;
        for (int i = 0; i < t->r.n; i++) {
            seat_id(&t->r.seats[i], gc.rated_ids[i]);
            gc.rated_is_bot[i] = !seat_is_human(&t->r.seats[i]);
            gc.rating_delta[i] = c->rating_delta[i];
            snprintf(gc.rated_names[i], sizeof gc.rated_names[i], "%.*s",
                     (int)t->r.seats[i].name_len, t->r.seats[i].name);
        }
    }

    // ---- everything the aftermath needs, copied out of the section ----------------
    const bool broadcast = c->n_events > 0 || c->goods_changed;
    cb_fanout f;
    memset(&f, 0, sizeof f);
    f.game_id = row->id;
    f.version = nrow.version;
    f.status = cb_table_status_label(c->status);
    f.updated_at = now;
    f.broadcast = broadcast;
    f.n_seats = t->r.n;
    rc = CB_OK;
    for (int i = 0; i < t->r.n && rc == CB_OK; i++) {
        const RosterSeat *seat = &t->r.seats[i];
        if (!seat_is_human(seat)) continue;
        seat_id(seat, f.seats[i].user_id);
        f.seats[i].envelope = dup_bytes(A + c->views[i].off, (size_t)c->views[i].len);
        f.seats[i].envelope_len = (size_t)c->views[i].len;
        if (!f.seats[i].envelope) rc = CB_E_NOMEM;
        if (rc == CB_OK && broadcast) {
            uint8_t *cast = NULL;
            rc = section_cast(s, i, next, &cast, &f.seats[i].cast_len);
            f.seats[i].cast = cast;
        }
    }
    if (rc == CB_OK && broadcast) {
        uint8_t *cast = NULL;
        rc = section_cast(s, -1, next, &cast, &f.spectator_cast_len);
        f.spectator_cast = cast;
    }
    for (int i = 0; i < f.n_seats; i++) {
        if (!f.seats[i].user_id[0]) continue;
        if (!in_list(s->old_humans, s->n_old, f.seats[i].user_id)) cb_strlcpy(f.joined[f.n_joined++], f.seats[i].user_id, sizeof(cb_uuid));
    }
    for (int i = 0; i < s->n_old; i++) {
        bool still = false;
        for (int j = 0; j < f.n_seats; j++) if (strcmp(f.seats[j].user_id, s->old_humans[i]) == 0) still = true;
        if (!still) snprintf(f.left[f.n_left++], sizeof(cb_uuid), "%s", s->old_humans[i]);
    }
    if (rc == CB_OK && actor_id) {
        int seat = table_seat_of(t, actor_id, (int)strlen(actor_id));
        Span e = seat >= 0 && c->views[seat].len > 0 ? c->views[seat] : c->spectator;
        out->actor_env = dup_bytes(A + e.off, (size_t)e.len);
        out->actor_env_len = (size_t)e.len;
        if (!out->actor_env) rc = CB_E_NOMEM;
    }
    // The committed row survives the section: finalize reloads the table.
    cb_game_row kept;
    memset(&kept, 0, sizeof kept);
    if (rc == CB_OK) rc = row_copy(&kept, &nrow);

    if (rc == CB_OK) {
        rc = CB_STORE_CALL(ctx->store, game_commit, &gc);
        if (rc == CB_E_CONFLICT) {
            // Somebody committed first: nothing was written.
            cb_row_cache_drop(row->id);
            out->lost = true;
            rc = CB_OK;
        } else if (rc == CB_OK) {
            out->version = nrow.version;
            out->needs_bots = c->needs_bots;
            out->ended = c->ended;
            cb_row_cache_note(&kept);
            if (c->ended) finalize_game(ctx, s, &kept);
            cb_fanout_commit(ctx, &f);
        } else {
            CB_ERROR("table", "game %s: commit failed (%s)", row->id, cb_strerror(rc));
        }
    }

    cb_game_row_free(&kept);
    free(logs);
    for (int i = 0; i < CB_MAX_SEATS; i++) {
        free((void *)f.seats[i].envelope);
        free((void *)f.seats[i].cast);
    }
    free((void *)f.spectator_cast);
    if ((rc != CB_OK || out->lost) && out->actor_env) {
        free(out->actor_env);
        out->actor_env = NULL;
        out->actor_env_len = 0;
    }
    return rc;
}

// ---- one operation (runTableOp) ---------------------------------------------------------

static bool commits_nothing(int rc) {
    return rc == TABLE_MOOT || rc == TABLE_STALE_ROUND || rc == TABLE_REJECTED;
}

// A refusal of the move is only authoritative against a fresh row: an apply
// from a stale cache corrects itself through the CAS, a refusal never reaches it.
static bool fresh_only(int rc) {
    return rc == TABLE_STALE_ROUND || rc == TABLE_REJECTED;
}

int cb_table_run(cb_ctx *ctx, const cb_table_op *op, cb_table_result *out) {
    memset(out, 0, sizeof *out);
    if (!ctx->store) return CB_E_STATE;
    cb_section *s = cb_section_new();
    if (!s) return CB_E_NOMEM;
    const char *actor = op->actor ? op->actor->id : NULL;
    int32_t ratings[CB_MAX_SEATS];
    bool have_ratings = false;
    int ret = CB_E_CONFLICT;

    for (int attempt = 1; attempt <= CB_TABLE_ATTEMPTS; attempt++) {
        cb_game_row row;
        bool cached = false;
        int rc = cb_row_load(ctx, op->game_id, !op->fresh && attempt == 1, &row, &cached);
        if (rc != CB_OK) { ret = rc; break; }
        uint8_t seed[CB_SEED_LEN];
        cb_random_bytes(seed, sizeof seed);

        // ---- the kernel section ----
        int lr = cb_section_load(s, &row);
        if (lr < 0) {
            cb_game_row_free(&row);
            if (cached) { cb_row_cache_drop(op->game_id); continue; }
            // A stored row the kernel refuses: never silently repaired or played on.
            CB_ERROR("table", "game %s does not load (%d, detail %d)", op->game_id, lr, s->t.detail);
            ret = CB_E_STATE;
            break;
        }
        rc = op->run(&s->t, &row, seed, op->ud);
        if (rc < 0 || (cached && fresh_only(rc))) {
            if (cached) { cb_row_cache_drop(op->game_id); cb_game_row_free(&row); continue; }
            out->result = rc;
            out->detail = s->t.detail;
            out->version = row.version;
            cb_game_row_free(&row);
            ret = CB_OK;
            break;
        }
        out->result = rc;
        out->reject = rc == TABLE_REJECTED ? s->t.reject : 0;

        if (rc == TABLE_EMPTY) {
            // The last seat left: the row goes, fenced on the version like a commit.
            ret = cb_section_envelope(s, -1, (uint32_t)row.version, &out->envelope, &out->envelope_len);
            if (ret == CB_OK) ret = CB_STORE_CALL(ctx->store, game_delete, op->game_id, row.version);
            cb_row_cache_drop(op->game_id);
            if (ret == CB_E_CONFLICT) {
                free(out->envelope);
                out->envelope = NULL;
                cb_game_row_free(&row);
                continue;
            }
            if (ret == CB_OK) {
                out->committed = true;
                out->version = row.version;
                cb_fanout f;
                memset(&f, 0, sizeof f);
                f.game_id = op->game_id;
                f.version = row.version;
                f.status = cb_table_status_label(GAME_STATUS_WAITING);
                f.updated_at = cb_app_now();
                for (int i = 0; i < s->n_old; i++) snprintf(f.left[f.n_left++], sizeof(cb_uuid), "%s", s->old_humans[i]);
                cb_fanout_commit(ctx, &f);
            }
            cb_game_row_free(&row);
            break;
        }

        if (commits_nothing(rc)) {
            int seat = actor ? table_seat_of(&s->t, actor, (int)strlen(actor)) : -1;
            out->needs_bots = table_needs_bots(&s->t);
            out->version = row.version;
            ret = cb_section_envelope(s, seat >= 0 ? seat : -1, (uint32_t)row.version, &out->envelope, &out->envelope_len);
            cb_game_row_free(&row);
            break;
        }

        cb_commit_out co;
        ret = cb_section_commit(ctx, s, &row, seed, actor, ratings, &have_ratings, &co);
        cb_game_row_free(&row);
        if (ret != CB_OK) break;
        if (co.lost) { ret = CB_E_CONFLICT; continue; }
        out->committed = true;
        out->version = co.version;
        out->needs_bots = co.needs_bots;
        out->envelope = co.actor_env;
        out->envelope_len = co.actor_env_len;
        break;
    }
    if (ret == CB_E_CONFLICT)
        CB_ERROR("table", "could not commit game %s after %d attempts: write contention", op->game_id, CB_TABLE_ATTEMPTS);
    if (ret != CB_OK) {
        free(out->envelope);
        out->envelope = NULL;
        out->envelope_len = 0;
    }
    cb_section_free(s);
    return ret;
}

// ---- create ---------------------------------------------------------------------------------

int cb_table_create(cb_ctx *ctx, const cb_user *u, cb_table_result *out) {
    memset(out, 0, sizeof *out);
    if (!ctx->store) return CB_E_STATE;
    cb_section *s = cb_section_new();
    if (!s) return CB_E_NOMEM;
    int ret = CB_E_CONFLICT;
    for (int draw = 1; draw <= CB_TABLE_ID_DRAWS; draw++) {
        // Six characters of a random UUID: 24 bits, so a draw can name a game
        // that exists; the store refuses it and we draw again.
        cb_uuid id;
        cb_uuid_v4(id);
        snprintf(s->gid, sizeof s->gid, "%.6s", id);
        table_init(&s->t, &s->g, &s->snaps);
        int rc = table_create(&s->t, u->id, (int)strlen(u->id), u->username, (int)strlen(u->username));
        if (rc < 0) {
            CB_ERROR("table", "create refused (%d, detail %d)", rc, s->t.detail);
            ret = CB_E_STATE;
            break;
        }
        const int64_t now = cb_app_now();
        rc = table_commit_products(&s->t, s->gid, (int)strlen(s->gid), 0, now, &s->c, s->arena, sizeof s->arena);
        if (rc < 0 || s->c.views[0].len <= 0) {
            CB_ERROR("table", "create: no products (%d)", rc);
            ret = CB_E_STATE;
            break;
        }
        const TableCommit *c = &s->c;
        cb_game_row row;
        memset(&row, 0, sizeof row);
        snprintf(row.id, sizeof row.id, "%s", s->gid);
        row.status = c->status;
        row.state = s->arena + c->state.off;
        row.state_len = (size_t)c->state.len;
        row.roster = s->arena + c->roster.off;
        row.roster_len = (size_t)c->roster.len;
        row.created_at = row.updated_at = row.last_commit_at = now;
        // Stored BEFORE the answer: the envelope names the game id.
        ret = CB_STORE_CALL(ctx->store, game_create, &row, u->id);
        if (ret == CB_E_CONFLICT) {
            CB_WARN("table", "game id %s is taken (draw %d/%d), drawing again", s->gid, draw, CB_TABLE_ID_DRAWS);
            continue;
        }
        if (ret != CB_OK) break;
        out->committed = true;
        out->version = 0;
        out->envelope = dup_bytes(s->arena + c->views[0].off, (size_t)c->views[0].len);
        out->envelope_len = (size_t)c->views[0].len;
        if (!out->envelope) { ret = CB_E_NOMEM; break; }
        // The creator's view row appears on their dashboard (pv- INSERT).
        cb_fanout f;
        memset(&f, 0, sizeof f);
        f.game_id = s->gid;
        f.status = cb_table_status_label(c->status);
        f.updated_at = now;
        f.n_seats = 1;
        snprintf(f.seats[0].user_id, sizeof(cb_uuid), "%s", u->id);
        f.seats[0].envelope = out->envelope;
        f.seats[0].envelope_len = out->envelope_len;
        f.n_joined = 1;
        snprintf(f.joined[0], sizeof(cb_uuid), "%s", u->id);
        cb_fanout_commit(ctx, &f);
        break;
    }
    if (ret == CB_E_CONFLICT) CB_ERROR("table", "create: no free game id in %d draws", CB_TABLE_ID_DRAWS);
    cb_section_free(s);
    return ret;
}

// ---- views on demand --------------------------------------------------------------------------

void cb_view_row_free(cb_view_row *v) {
    free(v->view);
    v->view = NULL;
    v->view_len = 0;
}

const char *cb_table_status_label(int status) {
    switch (status) {
    case GAME_STATUS_WAITING:   return "waiting";
    case GAME_STATUS_PLAYING:   return "playing";
    case GAME_STATUS_GAME_OVER: return "game_over";
    default:                    return "unknown";
    }
}

static int view_of_row(cb_section *s, const cb_game_row *row, const char *viewer_id, cb_view_row *out) {
    memset(out, 0, sizeof *out);
    int lr = cb_section_load(s, row);
    if (lr < 0) {
        CB_ERROR("table", "game %s does not load (%d)", row->id, lr);
        return CB_E_STATE;
    }
    int seat = -1;
    if (viewer_id) {
        seat = table_seat_of(&s->t, viewer_id, (int)strlen(viewer_id));
        if (seat < 0) return CB_E_NOT_FOUND;
        snprintf(out->player_id, sizeof out->player_id, "%s", viewer_id);
    }
    snprintf(out->game_id, sizeof out->game_id, "%s", row->id);
    out->version = row->version;
    out->updated_at = row->updated_at;
    snprintf(out->status, sizeof out->status, "%s", cb_table_status_label(row->status));
    return cb_section_envelope(s, seat, (uint32_t)row->version, &out->view, &out->view_len);
}

int cb_table_view(cb_ctx *ctx, const char *game_id, const char *viewer_id, cb_view_row *out) {
    memset(out, 0, sizeof *out);
    if (!ctx->store) return CB_E_STATE;
    cb_game_row row;
    memset(&row, 0, sizeof row);
    int rc = CB_STORE_CALL(ctx->store, game_get, game_id, &row);
    if (rc != CB_OK) return rc;
    cb_section *s = cb_section_new();
    rc = s ? view_of_row(s, &row, viewer_id, out) : CB_E_NOMEM;
    cb_section_free(s);
    cb_game_row_free(&row);
    if (rc != CB_OK) cb_view_row_free(out);
    return rc;
}

typedef struct {
    int n, cap;
    char (*ids)[CB_GAME_ID_MAX + 1];
} id_list;

static bool collect_member(const cb_member_row *r, void *ud) {
    id_list *l = ud;
    if (l->n == l->cap) {
        int cap = l->cap ? 2 * l->cap : 16;
        void *p = realloc(l->ids, (size_t)cap * sizeof *l->ids);
        if (!p) return false;
        l->ids = p;
        l->cap = cap;
    }
    snprintf(l->ids[l->n++], sizeof *l->ids, "%s", r->game_id);
    return true;
}

int cb_table_views_of_user(cb_ctx *ctx, const char *user_id, int limit, cb_view_fn fn, void *ud) {
    if (!ctx->store) return CB_E_STATE;
    id_list l = { 0, 0, NULL };
    int rc = CB_STORE_CALL(ctx->store, games_of_user, user_id, limit, collect_member, &l);
    if (rc < 0) { free(l.ids); return rc; }
    cb_section *s = cb_section_new();
    if (!s) { free(l.ids); return CB_E_NOMEM; }
    int n = 0;
    rc = CB_OK;
    for (int i = 0; i < l.n; i++) {
        cb_game_row row;
        memset(&row, 0, sizeof row);
        int g = CB_STORE_CALL(ctx->store, game_get, l.ids[i], &row);
        if (g == CB_E_NOT_FOUND) continue;   // deleted between the two reads
        if (g != CB_OK) { rc = g; break; }
        cb_view_row v;
        g = view_of_row(s, &row, user_id, &v);
        cb_game_row_free(&row);
        if (g == CB_E_NOT_FOUND) { cb_view_row_free(&v); continue; }   // a membership the roster no longer seats
        if (g != CB_OK) { cb_view_row_free(&v); rc = g; break; }
        n++;
        bool more = fn(&v, ud);
        cb_view_row_free(&v);
        if (!more) break;
    }
    cb_section_free(s);
    free(l.ids);
    return rc == CB_OK ? n : rc;
}
