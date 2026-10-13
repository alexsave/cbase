// table_ops.c - (table_ops.h). STUB: the app builder fills this in.
#include "table_ops.h"

#include "util/err.h"

#include <stdlib.h>

int cb_table_run(cb_ctx *ctx, const cb_table_op *op, cb_table_result *out) { return CB_E_NOTIMPL; }
int cb_table_create(cb_ctx *ctx, const cb_user *u, cb_table_result *out) { return CB_E_NOTIMPL; }
int cb_table_view(cb_ctx *ctx, const char *game_id, const char *viewer_id, cb_view_row *out) { return CB_E_NOTIMPL; }
int cb_table_views_of_user(cb_ctx *ctx, const char *user_id, int limit, cb_view_fn fn, void *ud) { return CB_E_NOTIMPL; }

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
