// What both backends share: freeing rows, closing, and the list orders.
#include "store.h"
#include "store_priv.h"

#include "bot_roster.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void cb_game_row_free(cb_game_row *g) {
    free(g->state);
    free(g->roster);
    free(g->logs);
    g->state = g->roster = g->logs = NULL;
    g->state_len = g->roster_len = g->logs_len = 0;
}

void cb_snapshot_row_free(cb_snapshot_row *s) {
    free(s->moves);
    free(s->extras);
    s->moves = s->extras = NULL;
    s->moves_len = s->extras_len = 0;
}

void cb_chat_row_free(cb_chat_row *c) {
    free(c->message);
    c->message = NULL;
}

// ---- busy: a throttle given up on (store.h CB_STORE_CALL) ----------------------

static _Thread_local int  t_depth;
static _Thread_local bool t_busy;

void cb_store_begin_(void) {
    if (t_depth++ == 0) t_busy = false;
}

int cb_store_end_(int rc) {
    if (t_depth > 0) t_depth--;
    return rc == CB_E_DB && t_busy ? CB_E_BUSY : rc;
}

void cb_store_mark_busy(void) { t_busy = true; }

int cb_store_games_get_each(cb_store *s, const char *const *ids, int n, cb_game_fn fn, void *ud) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        cb_game_row row;
        memset(&row, 0, sizeof row);
        int rc = s->ops->game_get(s, ids[i], &row);
        if (rc == CB_E_NOT_FOUND) continue;
        if (rc != CB_OK) return rc;
        free(row.logs);
        row.logs = NULL;
        row.logs_len = 0;
        count++;
        bool more = fn(&row, ud);
        cb_game_row_free(&row);
        if (!more) break;
    }
    return count;
}

static void *side_main(void *p) {
    cb_store_side *s = p;
    cb_store_set_deadline(s->deadline);
    s->fn(s->ud);
    return NULL;
}

void cb_store_side_start(cb_store_side *s, void (*fn)(void *ud), void *ud) {
    s->fn = fn;
    s->ud = ud;
    s->deadline = cb_store_deadline();
    s->started = pthread_create(&s->th, NULL, side_main, s) == 0;
    if (!s->started) fn(ud);
}

void cb_store_side_join(cb_store_side *s) {
    if (s->started) pthread_join(s->th, NULL);
    s->started = false;
}

void cb_store_close(cb_store *s) {
    if (s && s->ops->close) s->ops->close(s);
}

// ---- the orders both backends promise (store_priv.h) ---------------------------

static int bot_tier(const char *key) {
    int idx = bot_roster_find(key);
    const BotRosterEntry *e = idx >= 0 ? bot_roster_at(idx) : NULL;
    return e && e->tier ? e->tier : 1000;
}

int cb_store_bot_ladder_cmp(const cb_bot_row *a, const cb_bot_row *b) {
    int ta = bot_tier(a->strategy_key), tb = bot_tier(b->strategy_key);
    if (ta != tb) return ta < tb ? -1 : 1;
    // "%Miami 7" before "%Miami 10": a shorter name of the same family sorts first.
    size_t la = strlen(a->nickname), lb = strlen(b->nickname);
    if (la != lb) return la < lb ? -1 : 1;
    int c = strcmp(a->nickname, b->nickname);
    return c ? c : strcmp(a->id, b->id);
}

static int bot_qcmp(const void *a, const void *b) { return cb_store_bot_ladder_cmp(a, b); }
void cb_store_bots_sort(cb_bot_row *rows, int n) {
    if (n > 1) qsort(rows, (size_t)n, sizeof *rows, bot_qcmp);
}

static int rating_qcmp(const void *pa, const void *pb) {
    const cb_rating_row *a = pa, *b = pb;
    if (a->elo_rating != b->elo_rating) return a->elo_rating > b->elo_rating ? -1 : 1;
    return strcmp(a->user_id, b->user_id);
}
void cb_store_ratings_sort(cb_rating_row *rows, int n) {
    if (n > 1) qsort(rows, (size_t)n, sizeof *rows, rating_qcmp);
}

static bool contains_nocase(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0) return true;
    return n == 0;
}

bool cb_store_user_matches(const cb_user_row *u, const char *needle) {
    if (!needle || !*needle) return true;
    return contains_nocase(u->email, needle) || contains_nocase(u->username, needle);
}
