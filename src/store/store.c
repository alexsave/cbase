// What both backends share: freeing rows, closing, and the list orders.
#include "store.h"
#include "store_priv.h"

#include "bot_roster.h"

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
