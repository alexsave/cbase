// foolish_app.c - foolish as the platform's app (cbase/app.h): its routes
// and the kernel self-test every main runs before it serves.
#include "foolish_app.h"

#include "bots.h"
#include "functions_http.h"
#include "unfurl_http.h"
#include "v1_http.h"

#include "table.h"
#include "util/clock.h"
#include "util/err.h"

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

static const cb_route_list *const FOOLISH_ROUTES[] = { &cb_functions_route_list, &cb_v1_route_list,
                                                       &cb_unfurl_route_list };

// The bot drive, started by a kick (bots.h cb_bots_kick), and the scan that
// catches a lost one: EventBridge's literal "scan" under Lambda.
static const cb_job FOOLISH_JOBS[] = { { CB_BOTS_JOB, cb_bots_drive } };
static const cb_schedule FOOLISH_SCHEDULES[] = { { CB_BOTS_SCHEDULE, CB_BOT_SCAN_EVERY_MS, cb_bots_scan } };

const cb_app cb_foolish_app = {
    .name = "foolish",
    .routes = FOOLISH_ROUTES,
    .n_route_lists = (int)(sizeof FOOLISH_ROUTES / sizeof FOOLISH_ROUTES[0]),
    .selftest = cb_kernel_selftest,
    .jobs = FOOLISH_JOBS,
    .n_jobs = 1,
    .schedules = FOOLISH_SCHEDULES,
    .n_schedules = 1,
};
