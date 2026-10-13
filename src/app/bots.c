// bots.c - (bots.h). STUB: the app builder fills this in.
#include "bots.h"

#include "util/err.h"
#include "util/log.h"

int cb_bots_drive(cb_ctx *ctx, const char *game_id) { return CB_E_NOTIMPL; }
int cb_bots_scan(cb_ctx *ctx, int64_t now_ms) { return CB_E_NOTIMPL; }
void cb_bots_kick_local(cb_ctx *ctx, const char *game_id) { CB_WARN("bots", "kick %s: not implemented", game_id); }
void cb_bots_kick_lambda(cb_ctx *ctx, const char *game_id) { CB_WARN("bots", "kick %s: not implemented", game_id); }
