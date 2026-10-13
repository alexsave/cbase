// smoke.c - `cbase serve` end to end, as a client sees it.
//
//   build/test/smoke build/cbase
//
// Starts the real binary on a free port with a fresh database directory, waits
// for its "listening on" line, and talks to it over HTTP. TODAY it proves the
// skeleton: the process starts (the kernel self-test passed), answers
// GET /health, and drains on SIGTERM with exit status 0. The game builders
// extend it to the brief's full scenario: two humans sign up, create, join,
// add a bot, start, play to game over over HTTP + WebSocket, ratings change, a
// snapshot is stored, pushes arrive with monotone versions, then the process
// restarts and the game reloads from the store.
#include "t.h"

#include "lib_client.h"
#include "lib_proc.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static const char *g_bin;
static tp_child g_child;
static int g_port;

static void starts_and_listens(void) {
    char db[400], line[512];
    if (tp_tempdir(db, sizeof db) != 0) { T_CHECK(0); return; }
    static char envdb[512];
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/cbase.sqlite", db);
    char *env[] = { "CBASE_LISTEN=127.0.0.1:0", envdb, "CBASE_ANON_KEY=smoke-anon",
                    "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", NULL };
    char *argv[] = { (char *)g_bin, "serve", NULL };
    T_EQ_INT(tp_start(&g_child, argv, env), 0);
    T_EQ_INT(tp_wait_line(&g_child, "listening on", 20000, line, sizeof line), 0);
    const char *colon = strrchr(line, ':');
    g_port = colon ? atoi(colon + 1) : 0;
    T_CHECK(g_port > 0);
}

static void health(void) {
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/health", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    T_CHECK(strncmp(cb_buf_cstr(&r.body), "ok cbase ", 9) == 0);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "mode=serve") != NULL);
    tc_response_free(&r);
    // A route nobody registered yet is a clean 404, not a crash.
    T_EQ_INT(tc_http(g_port, "GET", "/nope", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 404);
    tc_response_free(&r);
}

static void drains_on_sigterm(void) {
    T_EQ_INT(tp_stop(&g_child, SIGTERM, 15000), 0);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: smoke <path to cbase>\n"); return 2; }
    g_bin = argv[1];
    T_RUN(starts_and_listens);
    if (g_port > 0) T_RUN(health);
    T_RUN(drains_on_sigterm);
    return T_DONE();
}
