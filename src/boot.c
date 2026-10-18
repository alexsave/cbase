#include "boot.h"

#include "app/bots.h"
#include "app/limits.h"
#include "auth/auth.h"
#include "handler.h"
#include "net/router.h"
#include "realtime/rt_iot.h"
#include "realtime/rt_local.h"
#include "store/store.h"
#include "table.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cb_kernel_selftest(char *err, size_t cap) {
    static const char uid[] = "00000000-0000-4000-8000-000000000000";
    static const char gid[] = "selfte";
    enum { ARENA = 65536 };
    Game *g = calloc(1, sizeof *g);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    uint8_t *arena = malloc(ARENA), *env = malloc(ARENA);
    Table t;
    TableCommit c;
    EnvHeader h;
    uint8_t small[16];
    int rc = CB_E_STATE;
    const char *why = "allocation failed";
    if (!g || !snaps || !arena || !env) goto out;
    table_init(&t, g, snaps);
    why = "table_create refused a lobby";
    if (table_create(&t, uid, (int)strlen(uid), "SELFTEST", 8) != TABLE_OK) goto out;
    why = "table_commit_products refused the lobby";
    if (table_commit_products(&t, gid, 6, 1, cb_now_ms(), &c, arena, ARENA) < 0) goto out;
    why = "the committed lobby is not WAITING";
    if (c.status != GAME_STATUS_WAITING || c.num_players != 1) goto out;
    // Reload the durable products into a fresh table, as a request would.
    Table t2;
    table_init(&t2, g, snaps);
    why = "table_load refused the state the commit wrote";
    if (table_load(&t2, arena + c.state.off, c.state.len, arena + c.roster.off, c.roster.len) != TABLE_OK) goto out;
    why = "table_envelope refused seat 0";
    int n = table_envelope(&t2, gid, 6, 0, 1, env, ARENA);
    if (n <= 0) goto out;
    why = "the envelope's header does not read back (seat 0, version 1)";
    if (!env_header_read(env, n, &h) || h.seat != 0 || h.version != 1) goto out;
    why = "the action response is not TABLE_RESP_BYTES (7) long";
    if (table_action_response(TABLE_APPLIED, 0, 1, small, sizeof small) != TABLE_RESP_BYTES || TABLE_RESP_BYTES != 7) goto out;
    why = "the refusal is not ASK_REFUSAL_BYTES (10) long";
    AskRefusal r = { ASK_WHY_NOT_FOUND, 0, 0 };
    if (ask_refusal_encode(&r, small, sizeof small) != ASK_REFUSAL_BYTES || ASK_REFUSAL_BYTES != 10) goto out;
    rc = CB_OK;
    why = "";
out:
    if (rc != CB_OK) snprintf(err, cap, "kernel self-test: %s", why);
    free(g);
    free(snaps);
    free(arena);
    free(env);
    return rc;
}

// CB_E_NOTIMPL from a module under construction is a warning, not a failure.
static int soft(const char *what, int rc, char *err, size_t cap) {
    if (rc == CB_E_NOTIMPL) { CB_WARN("boot", "%s: not implemented yet, running without it", what); return CB_OK; }
    if (rc != CB_OK) snprintf(err, cap, "%s: %s", what, cb_strerror(rc));
    return rc;
}

int cb_boot(cb_ctx *ctx, char *err, size_t cap) {
    const cb_config *cfg = ctx->cfg;
    ctx->version = CB_VERSION;
    ctx->started_ms = cb_now_ms();
    // A test knob production never runs with (config.h CBASE_BOT_SPEEDUP).
    if (ctx->mode == CB_MODE_LAMBDA && cfg->bot_speedup != 1) {
        snprintf(err, cap, "CBASE_BOT_SPEEDUP=%d is for cbase serve's tests: Lambda runs the bots at their own pace",
                 cfg->bot_speedup);
        return CB_E_INVALID;
    }
    int rc = cb_kernel_selftest(err, cap);
    if (rc != CB_OK) return rc;

    const char *store = cfg->store[0] ? cfg->store : ctx->mode == CB_MODE_LAMBDA ? "dynamo" : "sqlite";
    rc = strcmp(store, "dynamo") == 0 ? cb_store_open_dynamo(cfg->table, &ctx->store)
                                      : cb_store_open_sqlite(cfg->db_path, &ctx->store);
    if ((rc = soft(strcmp(store, "dynamo") == 0 ? "store dynamo" : "store sqlite", rc, err, cap)) != CB_OK) return rc;
    if ((rc = soft("auth", cb_auth_new(ctx, &ctx->auth), err, cap)) != CB_OK) return rc;
    // Under Lambda the stream is IoT Core's, when this stack has it
    // (CBASE_IOT_ENDPOINT); with none, nothing is pushed.
    if (ctx->mode == CB_MODE_LAMBDA) rc = cfg->iot_endpoint[0] ? cb_rt_iot_new(ctx, &ctx->rt) : CB_OK;
    else rc = cb_rt_local_new(ctx, &ctx->rt);
    if ((rc = soft("realtime", rc, err, cap)) != CB_OK) return rc;
    ctx->kick_bots = ctx->mode == CB_MODE_LAMBDA ? cb_bots_kick_lambda : cb_bots_kick_local;
    cb_limit_set_admin_fail_burst(cfg->admin_fail_limit);

    ctx->router = cb_router_new();
    if (!ctx->router) { snprintf(err, cap, "out of memory"); return CB_E_NOMEM; }
    rc = cb_routes_register(ctx);
    if (rc != CB_OK) { snprintf(err, cap, "route registration: %s", cb_strerror(rc)); return rc; }
    return CB_OK;
}

void cb_unboot(cb_ctx *ctx) {
    cb_router_free(ctx->router);
    ctx->router = NULL;
    cb_rt_free(ctx->rt);
    ctx->rt = NULL;
    cb_auth_free(ctx->auth);
    ctx->auth = NULL;
    cb_store_close(ctx->store);
    ctx->store = NULL;
}
