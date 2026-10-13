// smoke.c - `cbase serve` end to end, as a client sees it.
//
//   build/test/smoke build/cbase
//
// Starts the real binary on a free port with a fresh database directory, waits
// for its "listening on" line, and talks to it over HTTP. ALWAYS it proves the
// skeleton: the process starts (the kernel self-test passed), answers
// GET /health, and drains on SIGTERM with exit status 0.
//
// THE GAME SCENARIO (the brief's), run when CBASE_SMOKE_GAME=1: two humans
// sign up (GoTrue's /auth/v1/signup), A opens a Realtime WebSocket (Phoenix
// vsn 2.0.0) and joins its private gu- topic, A creates, B joins, A adds a bot,
// both start, and the humans play legal moves (chosen on their own masked
// envelope with the kernel's client table, as the browser does) while the bot
// pool drives the bot, until the game is over; then the ratings changed
// (PostgREST user_elo_ratings), a snapshot is stored (game_snapshots), the
// pushes A received carried strictly increasing versions, and after a restart
// on the same database the game reloads (player_views) at the same version
// with the same envelope.
//
// It needs every module: auth (signup, bearer verify), the sqlite store,
// postgrest (the three reads), realtime (rt_local + phoenix), and the app. It
// is behind the variable because those land in parallel; the integrator turns
// it on in the Makefile's smoke target (`CBASE_SMOKE_GAME=1 build/test/smoke
// build/cbase`) once they are merged, and from then on it should always run.
#include "t.h"

#include "lib_client.h"
#include "lib_proc.h"

#include "awire.h"
#include "client_table.h"
#include "evwire.h"
#include "legal.h"
#include "table.h"
#include "util/b64.h"
#include "util/hex.h"
#include "util/json.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *g_bin;
static tp_child g_child;
static int g_port;
static char g_dbdir[400];

#define ANON "smoke-anon"

static void start_server(void) {
    char line[512];
    static char envdb[512];
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/cbase.sqlite", g_dbdir);
    char *env[] = { "CBASE_LISTEN=127.0.0.1:0", envdb, "CBASE_ANON_KEY=" ANON,
                    "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", NULL };
    char *argv[] = { (char *)g_bin, "serve", NULL };
    g_port = 0;
    T_EQ_INT(tp_start(&g_child, argv, env), 0);
    T_EQ_INT(tp_wait_line(&g_child, "listening on", 20000, line, sizeof line), 0);
    const char *colon = strrchr(line, ':');
    g_port = colon ? atoi(colon + 1) : 0;
    T_CHECK(g_port > 0);
}

static void starts_and_listens(void) {
    if (tp_tempdir(g_dbdir, sizeof g_dbdir) != 0) { T_CHECK(0); return; }
    start_server();
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

// ---- the game scenario ---------------------------------------------------------------

typedef struct {
    char name[32];
    char id[64];
    char token[2048];
    char bearer[2200];   // "Authorization: Bearer ...\r\napikey: ...\r\n"
} smoke_user;

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int signup(smoke_user *u, const char *name) {
    snprintf(u->name, sizeof u->name, "%s", name);
    char body[512];
    snprintf(body, sizeof body, "{\"email\":\"%s@smoke.test\",\"password\":\"smoke-pass-123\",\"data\":{\"username\":\"%s\"}}",
             name, name);
    tc_response r;
    if (tc_http(g_port, "POST", "/auth/v1/signup", "apikey: " ANON "\r\nContent-Type: application/json\r\n",
                body, strlen(body), &r) != 0) return -1;
    const char *j = cb_buf_cstr(&r.body);
    int ok = r.status == 200 && cb_json_get_str(j, r.body.len, "access_token", u->token, sizeof u->token)
             && cb_json_get_str(j, r.body.len, "user.id", u->id, sizeof u->id);
    if (!ok) fprintf(stderr, "signup %s: %d %s\n", name, r.status, j);
    tc_response_free(&r);
    snprintf(u->bearer, sizeof u->bearer, "Authorization: Bearer %s\r\napikey: " ANON "\r\n", u->token);
    return ok ? 0 : -1;
}

// POST /functions/v1/<fn> with octets. The status, the body into out.
static int fn_call(const smoke_user *u, const char *fn, const void *body, size_t n, cb_buf *out) {
    char path[64], headers[2400];
    snprintf(path, sizeof path, "/functions/v1/%s", fn);
    snprintf(headers, sizeof headers, "%sContent-Type: application/octet-stream\r\n", u->bearer);
    tc_response r;
    if (tc_http(g_port, "POST", path, headers, body, n, &r) != 0) return -1;
    cb_buf_reset(out);
    cb_buf_append(out, r.body.p, r.body.len);
    int status = r.status;
    tc_response_free(&r);
    return status;
}

static int ask(const smoke_user *u, int kind, const char *gid, cb_buf *env) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = (uint8_t)kind;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    return n > 0 ? fn_call(u, "meta", body, (size_t)n, env) : -1;
}

typedef struct {
    ClientSlot  slot;
    ClientTable c;
    LegalMoves  moves;
} smoke_client;

// A legal move for the viewer on their envelope, as the browser would offer
// one: the kernel's legal moves on the masked board, each judged again by
// client_validate. Its wire length, or 0 when the viewer has none.
static int choose_move(smoke_client *sc, const cb_buf *env, uint8_t *wire, int cap, uint32_t *rng) {
    client_init(&sc->c, &sc->slot);
    if (client_adopt_envelope(&sc->c, env->p, (int)env->len) < 0) return 0;
    int seat = sc->c.view.my_seat;
    if (seat < 0 || sc->c.view.status != GAME_STATUS_PLAYING) return 0;
    calculate_legal_moves(sc->c.g, seat, &sc->moves);
    int start = sc->moves.n ? (int)((*rng = *rng * 1103515245u + 12345u) >> 8) % sc->moves.n : 0;
    for (int k = 0; k < sc->moves.n; k++) {
        const LegalMove *m = &sc->moves.moves[(start + k) % sc->moves.n];
        if (m->type == MOVE_WAIT) continue;
        AwireAction a;
        memset(&a, 0, sizeof a);
        a.kind = m->type;
        a.n = m->n_cards;
        for (int i = 0; i < m->n_cards; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
        int n = awire_encode(&a, wire, cap);
        if (n > 0 && client_validate(&sc->c, &sc->c.view, wire, n) == 0) return n;
    }
    return 0;
}

// The Phoenix vsn 2.0.0 frames A's socket received: every animation_events
// cast's version, in arrival order.
typedef struct {
    int      n;
    uint32_t versions[4096];
} cast_log;

static void drain_ws(int fd, cast_log *log, int timeout_ms) {
    cb_buf f;
    cb_buf_init(&f);
    for (;;) {
        int op = tc_ws_recv(fd, timeout_ms, &f);
        if (op < 0) break;
        const char *j = cb_buf_cstr(&f);
        char event[64], b64[16384];
        // [join_ref, ref, topic, "broadcast", {event, payload:{b}, type}]
        if (cb_json_get_str(j, f.len, "3", event, sizeof event) && strcmp(event, "broadcast") == 0
            && cb_json_get_str(j, f.len, "4.payload.b", b64, sizeof b64)) {
            uint8_t cast[12288];
            int n = cb_b64_decode(b64, strlen(b64), cast, sizeof cast);
            uint32_t v = 0;
            if (n > 0 && evwire_cast_header_get(cast, n, &v) > 0 && log->n < 4096) log->versions[log->n++] = v;
        }
        cb_buf_reset(&f);
        timeout_ms = 50;
    }
    cb_buf_free(&f);
}

// GET a PostgREST path with u's bearer; the body into out. The status.
static int rest_get(const smoke_user *u, const char *path, cb_buf *out) {
    tc_response r;
    if (tc_http(g_port, "GET", path, u->bearer, NULL, 0, &r) != 0) return -1;
    cb_buf_reset(out);
    cb_buf_append(out, r.body.p, r.body.len);
    int s = r.status;
    tc_response_free(&r);
    return s;
}

static void game_scenario(void) {
    smoke_user A, B;
    int sa = signup(&A, "SMOKEA"), sb = signup(&B, "SMOKEB");
    T_EQ_INT(sa, 0);
    T_EQ_INT(sb, 0);
    if (sa || sb) return;   // nothing below can pass without two sessions
    smoke_client *sc = calloc(1, sizeof *sc);
    cb_buf env, tmp;
    cb_buf_init(&env);
    cb_buf_init(&tmp);

    // create: A's envelope names the game.
    int created = fn_call(&A, "create", NULL, 0, &env);
    T_EQ_INT(created, 200);
    if (created != 200) { cb_buf_free(&env); cb_buf_free(&tmp); free(sc); return; }
    client_init(&sc->c, &sc->slot);
    T_CHECK(client_adopt_envelope(&sc->c, env.p, (int)env.len) >= 0);
    char gid[32];
    snprintf(gid, sizeof gid, "%.*s", (int)sc->c.gid_len, sc->c.gid);
    T_EQ_INT(strlen(gid), 6);

    // A's realtime socket on its private topic.
    char wspath[256], join[4096];
    snprintf(wspath, sizeof wspath, "/realtime/v1/websocket?apikey=%s&vsn=2.0.0", ANON);
    int ws = tc_ws_open(g_port, wspath, NULL);
    T_CHECK(ws >= 0);
    snprintf(join, sizeof join,
             "[\"1\",\"1\",\"realtime:gu-%s-%s\",\"phx_join\",{\"config\":{\"broadcast\":{\"ack\":false,\"self\":false},"
             "\"presence\":{\"key\":\"\"},\"postgres_changes\":[],\"private\":true},\"access_token\":\"%s\"}]",
             gid, A.id, A.token);
    T_EQ_INT(tc_ws_send(ws, 1, join, strlen(join)), 0);
    cb_buf f;
    cb_buf_init(&f);
    T_CHECK(tc_ws_recv(ws, 5000, &f) == 1 && strstr(cb_buf_cstr(&f), "\"phx_reply\"") && strstr(cb_buf_cstr(&f), "\"ok\""));
    cb_buf_free(&f);

    T_EQ_INT(ask(&B, ASK_JOIN, gid, &env), 200);
    T_EQ_INT(ask(&A, ASK_ADD_BOT, gid, &env), 200);
    T_EQ_INT(ask(&A, ASK_START, gid, &env), 200);
    T_EQ_INT(ask(&B, ASK_START, gid, &env), 200);
    client_init(&sc->c, &sc->slot);
    T_CHECK(client_adopt_envelope(&sc->c, env.p, (int)env.len) >= 0);
    T_EQ_INT(sc->c.view.status, GAME_STATUS_PLAYING);

    // Play: each human bumps (which also wakes the bots) for their envelope and
    // moves when they have a move; the bot pool plays the bot.
    const smoke_user *humans[2] = { &A, &B };
    uint32_t rng = 7;
    int status = GAME_STATUS_PLAYING, moves = 0;
    const int64_t deadline = mono_ms() + 180000;
    while (status == GAME_STATUS_PLAYING && mono_ms() < deadline) {
        bool moved = false, answered = false;
        for (int h = 0; h < 2 && status == GAME_STATUS_PLAYING; h++) {
            if (ask(humans[h], ASK_BUMP, gid, &env) != 200) continue;
            answered = true;
            client_init(&sc->c, &sc->slot);
            if (client_adopt_envelope(&sc->c, env.p, (int)env.len) < 0) continue;
            status = sc->c.view.status;
            uint32_t version = sc->c.view.version;
            uint8_t wire[64], body[128];
            int n = choose_move(sc, &env, wire, sizeof wire, &rng);
            if (n <= 0) continue;
            memcpy(body, gid, 6);
            memcpy(body + 6, wire, (size_t)n);
            int bn = table_request_encode(body, sizeof body, 6, n, true, version);
            T_EQ_INT(fn_call(humans[h], "action", body, (size_t)bn, &tmp), 200);
            T_EQ_INT(tmp.len, TABLE_RESP_BYTES);
            moved = true;
            moves++;
        }
        if (!answered) break;   // the server stopped answering: the asserts below say so
        if (!moved) {
            // The bots owe a move: give the bot pool a moment.
            struct timespec ts = { 0, 50 * 1000000 };
            nanosleep(&ts, NULL);
        }
    }
    T_EQ_INT(status, GAME_STATUS_GAME_OVER);
    T_CHECK(moves > 0);

    // Pushes: strictly increasing versions on A's private topic.
    cast_log *casts = calloc(1, sizeof *casts);
    drain_ws(ws, casts, 1000);
    T_CHECK(casts->n > 0);
    for (int i = 1; i < casts->n; i++) T_CHECK(casts->versions[i] > casts->versions[i - 1]);
    free(casts);

    // Ratings and the snapshot.
    char path[512];
    snprintf(path, sizeof path, "/rest/v1/user_elo_ratings?select=user_id,elo_rating,games_played&user_id=eq.%s", A.id);
    T_EQ_INT(rest_get(&A, path, &tmp), 200);
    int64_t played = 0;
    T_CHECK(cb_json_get_i64(cb_buf_cstr(&tmp), tmp.len, "0.games_played", &played) && played == 1);
    snprintf(path, sizeof path, "/rest/v1/game_snapshots?select=id,moves,extras&game_id=eq.%s", gid);
    T_EQ_INT(rest_get(&A, path, &tmp), 200);
    T_EQ_INT(cb_json_array_len(cb_buf_cstr(&tmp), tmp.len, ""), 1);

    // The view row, then a restart on the same database: the same row.
    snprintf(path, sizeof path, "/rest/v1/player_views?select=view,version&game_id=eq.%s&player_id=eq.%s", gid, A.id);
    T_EQ_INT(rest_get(&A, path, &tmp), 200);
    int64_t v1 = -1, v2 = -2;
    char view1[16384] = "", view2[16384] = "";
    T_CHECK(cb_json_get_i64(cb_buf_cstr(&tmp), tmp.len, "0.version", &v1));
    T_CHECK(cb_json_get_str(cb_buf_cstr(&tmp), tmp.len, "0.view", view1, sizeof view1));
    T_EQ_INT(tp_stop(&g_child, SIGTERM, 15000), 0);
    start_server();
    T_EQ_INT(rest_get(&A, path, &tmp), 200);
    T_CHECK(cb_json_get_i64(cb_buf_cstr(&tmp), tmp.len, "0.version", &v2));
    T_CHECK(cb_json_get_str(cb_buf_cstr(&tmp), tmp.len, "0.view", view2, sizeof view2));
    T_EQ_INT(v2, v1);
    T_EQ_STR(view2, view1);
    T_CHECK(strncmp(view1, "\\x", 2) == 0);

    cb_buf_free(&env);
    cb_buf_free(&tmp);
    free(sc);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: smoke <path to cbase>\n"); return 2; }
    g_bin = argv[1];
    T_RUN(starts_and_listens);
    if (g_port > 0) T_RUN(health);
    const char *game = getenv("CBASE_SMOKE_GAME");
    if (g_port > 0 && game && strcmp(game, "1") == 0) T_RUN(game_scenario);
    else printf("NOTE smoke: the game scenario is off (CBASE_SMOKE_GAME=1 runs it)\n");
    T_RUN(drains_on_sigterm);
    if (g_dbdir[0]) {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dbdir);
        if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", g_dbdir);
    }
    return T_DONE();
}
