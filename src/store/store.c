// What both backends share: freeing rows, and closing.
#include "store.h"

#include <stdlib.h>

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
