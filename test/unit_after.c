// unit_after.c - the after-response window (app/after.h): with it closed a
// commit's pushes and a kick happen at once; with it open they wait, as
// copies of their own bytes, and cb_after_run does them in call order, closes
// the window, counts the pushes that failed and flushes the edge cache.
#include "t.h"

#include "app/after.h"
#include "app/edge.h"
#include "app/fanout.h"
#include "realtime/realtime.h"
#include "util/log.h"

#include <stdlib.h>

static cb_ctx g_ctx;
static cb_config g_cfg;

// What happened, in order: 'P' a publish (its game and first byte), 'K' a kick.
static struct { int n; char kind[16]; char game[16][16]; uint8_t byte[16]; } g_log;

static void observe(const cb_fanout *f) {
    if (g_log.n >= 16) return;
    g_log.kind[g_log.n] = 'P';
    snprintf(g_log.game[g_log.n], 16, "%s", f->game_id);
    g_log.byte[g_log.n] = f->n_seats && f->seats[0].envelope ? f->seats[0].envelope[0] : 0;
    g_log.n++;
}

static void kick(cb_ctx *ctx, const char *game_id) {
    if (g_log.n >= 16) return;
    g_log.kind[g_log.n] = 'K';
    snprintf(g_log.game[g_log.n], 16, "%s", game_id);
    g_log.n++;
}

// A transport whose every delivery fails: the drain counts them.
static int g_delivered;
static int failing_deliver(cb_rt *rt, const char *topic, const uint8_t *frame, size_t len) {
    g_delivered++;
    return 1;
}
static const cb_rt_transport FAIL_T = { .name = "fail", .deliver = failing_deliver };
static cb_rt g_rt = { &FAIL_T, &g_ctx, NULL };

static void setup(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&g_cfg, 0, sizeof g_cfg);
    g_ctx.cfg = &g_cfg;
    g_ctx.kick_bots = kick;
    memset(&g_log, 0, sizeof g_log);
    cb_fanout_observer = observe;
}

static cb_fanout fanout(const char *game, uint8_t *env, uint8_t *cast) {
    cb_fanout f;
    memset(&f, 0, sizeof f);
    f.game_id = game;
    f.version = 3;
    f.status = "playing";
    f.broadcast = true;
    f.n_seats = 1;
    snprintf(f.seats[0].user_id, sizeof f.seats[0].user_id, "aaaaaaaa-0000-4000-8000-000000000001");
    f.seats[0].envelope = env;
    f.seats[0].envelope_len = 2;
    f.seats[0].cast = cast;
    f.seats[0].cast_len = 2;
    f.spectator_cast = cast;
    f.spectator_cast_len = 2;
    return f;
}

static void closed_window_acts_at_once(void) {
    setup();
    T_CHECK(!cb_after_open());
    uint8_t env[2] = { 7, 7 }, cast[2] = { 8, 8 };
    cb_fanout f = fanout("aaaaaa", env, cast);
    T_CHECK(!cb_after_fanout(&f));   // not queued
    cb_fanout_commit(&g_ctx, &f);
    cb_after_kick(&g_ctx, "aaaaaa");
    T_EQ_INT(g_log.n, 2);
    T_EQ_INT(g_log.kind[0], 'P');
    T_EQ_INT(g_log.kind[1], 'K');
    // A context with nobody to kick: nothing, not a crash.
    g_ctx.kick_bots = NULL;
    cb_after_kick(&g_ctx, "aaaaaa");
    T_EQ_INT(g_log.n, 2);
    cb_after_run(&g_ctx);   // nothing queued, window closed: safe
    T_EQ_INT(g_log.n, 2);
}

static void open_window_waits_and_keeps_order(void) {
    setup();
    cb_after_begin();
    T_CHECK(cb_after_open());
    uint8_t env1[2] = { 1, 1 }, cast1[2] = { 2, 2 }, env2[2] = { 3, 3 }, cast2[2] = { 4, 4 };
    cb_fanout f1 = fanout("aaaaaa", env1, cast1), f2 = fanout("bbbbbb", env2, cast2);
    cb_fanout_commit(&g_ctx, &f1);
    cb_after_kick(&g_ctx, "aaaaaa");
    cb_fanout_commit(&g_ctx, &f2);
    T_EQ_INT(g_log.n, 0);   // nothing before the answer
    // The queue holds copies: the caller's buffers may change or go.
    env1[0] = 99;
    env2[0] = 99;
    cb_edge_stale(CB_EDGE_BOARD);
    T_CHECK(cb_edge_pending() != 0);
    cb_after_run(&g_ctx);
    T_CHECK(!cb_after_open());
    T_EQ_INT(g_log.n, 3);
    T_EQ_INT(g_log.kind[0], 'P');
    T_EQ_STR(g_log.game[0], "aaaaaa");
    T_EQ_INT(g_log.byte[0], 1);
    T_EQ_INT(g_log.kind[1], 'K');
    T_EQ_INT(g_log.kind[2], 'P');
    T_EQ_STR(g_log.game[2], "bbbbbb");
    T_EQ_INT(g_log.byte[2], 3);
    T_EQ_INT(cb_edge_pending(), 0);   // flushed (no distribution: the marks are let go)
    // What the drained work itself commits acts at once: the window is closed.
    cb_fanout_commit(&g_ctx, &f1);
    T_EQ_INT(g_log.n, 4);
}

static void failed_pushes_are_counted_not_fatal(void) {
    setup();
    g_ctx.rt = &g_rt;
    g_delivered = 0;
    cb_after_begin();
    uint8_t env[2] = { 1, 1 }, cast[2] = { 2, 2 };
    cb_fanout f = fanout("aaaaaa", env, cast);
    cb_fanout_commit(&g_ctx, &f);
    T_EQ_INT(g_delivered, 0);
    cb_after_run(&g_ctx);
    T_EQ_INT(g_delivered, 3);   // the view, the seat's cast, the spectators' cast
    T_EQ_INT(cb_rt_failures_take(), 0);   // the drain took its own count
    // A fan-out with no bytes at all still queues and publishes.
    cb_after_begin();
    cb_fanout empty;
    memset(&empty, 0, sizeof empty);
    empty.game_id = "cccccc";
    empty.status = "waiting";
    T_CHECK(cb_after_fanout(&empty));
    cb_after_run(&g_ctx);
    T_EQ_INT(g_log.n, 2);
    T_EQ_STR(g_log.game[1], "cccccc");
}

int main(void) {
    T_RUN(closed_window_acts_at_once);
    T_RUN(open_window_waits_and_keeps_order);
    T_RUN(failed_pushes_are_counted_not_fatal);
    cb_fanout_observer = NULL;
    return T_DONE();
}
