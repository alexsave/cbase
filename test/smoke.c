// smoke.c - `cbase serve` end to end, as a client sees it.
//
//   build/test/smoke build/cbase
//
// Starts the real binary on a free port with a fresh database directory, waits
// for its "listening on" line, and talks to it over HTTP. ALWAYS it proves the
// skeleton: the process starts (the kernel self-test passed), answers
// GET /health, and drains on SIGTERM with exit status 0.
//
// THE GAME SCENARIO (the brief's), always, unless CBASE_SMOKE_GAME=0: two
// humans sign up (GoTrue's /auth/v1/signup); A opens a Realtime WebSocket
// (Phoenix vsn 2.0.0, as realtime-js) on its private gu- topic, B one in vsn
// 1.0.0 (as supabase-swift) on its pv- player_views changes and the game-
// spectator stream; A creates, B joins, A adds a bot, both start, and the
// humans play legal moves (chosen on their own masked envelope with the
// kernel's client table, as the browser does) while the bot pool drives the
// bot. Once mid-game, when only the bots owe a move, the server is killed
// and restarted with nobody bumping: the 10 s heartbeat must resume the bot
// after the dead process's lease runs out. At the end: game over, every push
// stream strictly increasing in version (B's last pv- change is B's stored
// row), the ratings changed (user_elo_ratings), one snapshot stored
// (game_snapshots), and after a clean restart on the same database the game
// reloads (player_views) at the same version with the same envelope.
//
// Every module is real here: auth (real JWTs), the SQLite store, postgrest,
// realtime (rt_local + phoenix, through rt_deps' production table), the app.
#include "t.h"
#include "lib_store_backend.h"

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
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

// One realtime socket and what it heard. vsn 2.0.0 frames are arrays
// [join_ref, ref, topic, event, payload] (realtime-js), vsn 1.0.0 frames are
// objects {join_ref, ref, topic, event, payload} (supabase-swift).
typedef struct {
    int      fd;
    bool     v1;
    int      n_cast;              // broadcast animation_events: the cast's version
    uint32_t cast_v[4096];
    int      n_pv;                // postgres_changes on player_views: record.version
    int64_t  pv_v[4096];
    int      bad_pv;              // a pv change whose record.view is not "\x" hex
    int64_t  last_hb;             // when this socket last sent a heartbeat
} listener;

static void hear(listener *l, const char *j, size_t n) {
    const char *E = l->v1 ? "event" : "3", *P = l->v1 ? "payload" : "4";
    char event[64], path[64];
    if (!cb_json_get_str(j, n, E, event, sizeof event)) return;
    if (strcmp(event, "broadcast") == 0) {
        static char b64[16384];
        snprintf(path, sizeof path, "%s.payload.b", P);
        if (!cb_json_get_str(j, n, path, b64, sizeof b64)) return;
        static uint8_t cast[12288];
        int k = cb_b64_decode(b64, strlen(b64), cast, sizeof cast);
        uint32_t v = 0;
        if (k > 0 && evwire_cast_header_get(cast, k, &v) > 0 && l->n_cast < 4096) l->cast_v[l->n_cast++] = v;
    } else if (strcmp(event, "postgres_changes") == 0) {
        int64_t v = -1;
        static char view[32768];
        snprintf(path, sizeof path, "%s.data.record.version", P);
        bool ok = cb_json_get_i64(j, n, path, &v);
        snprintf(path, sizeof path, "%s.data.record.view", P);
        ok = ok && cb_json_get_str(j, n, path, view, sizeof view) && strncmp(view, "\\x", 2) == 0;
        if (!ok) l->bad_pv++;
        else if (l->n_pv < 4096) l->pv_v[l->n_pv++] = v;
    }
}

// Reads every frame that arrives within timeout_ms (then 50 ms between frames).
// Like realtime-js, sends a Phoenix heartbeat every 25 s: a socket the client
// keeps silent is closed by the server's 120 s idle sweep, as a real one is.
static void drain_ws(listener *l, int timeout_ms) {
    if (l->fd >= 0 && mono_ms() - l->last_hb > 25000) {
        const char *hb = l->v1 ? "{\"join_ref\":null,\"ref\":\"hb\",\"topic\":\"phoenix\",\"event\":\"heartbeat\",\"payload\":{}}"
                               : "[null,\"hb\",\"phoenix\",\"heartbeat\",{}]";
        tc_ws_send(l->fd, 1, hb, strlen(hb));
        l->last_hb = mono_ms();
    }
    cb_buf f;
    cb_buf_init(&f);
    while (l->fd >= 0 && tc_ws_recv(l->fd, timeout_ms, &f) >= 0) {
        hear(l, cb_buf_cstr(&f), f.len);
        cb_buf_reset(&f);
        timeout_ms = 50;
    }
    cb_buf_free(&f);
}

// Joins `topic` with `config` (a JSON object) as the holder of `token`; true on
// an ok reply. Pushes that arrive first are heard.
static bool ws_join(listener *l, const char *topic, const char *config, const char *token, int ref) {
    static char frame[8192];
    if (l->v1)
        snprintf(frame, sizeof frame,
                 "{\"join_ref\":\"%d\",\"ref\":\"%d\",\"topic\":\"realtime:%s\",\"event\":\"phx_join\","
                 "\"payload\":{\"config\":%s,\"access_token\":\"%s\"}}", ref, ref, topic, config, token);
    else
        snprintf(frame, sizeof frame, "[\"%d\",\"%d\",\"realtime:%s\",\"phx_join\",{\"config\":%s,\"access_token\":\"%s\"}]",
                 ref, ref, topic, config, token);
    if (tc_ws_send(l->fd, 1, frame, strlen(frame)) != 0) return false;
    cb_buf f;
    cb_buf_init(&f);
    bool ok = false;
    for (int i = 0; i < 50; i++) {
        if (tc_ws_recv(l->fd, 5000, &f) < 0) break;
        const char *j = cb_buf_cstr(&f);
        if (strstr(j, "\"phx_reply\"")) { ok = strstr(j, "\"status\":\"ok\"") != NULL; break; }
        hear(l, j, f.len);
        cb_buf_reset(&f);
    }
    cb_buf_free(&f);
    return ok;
}

static bool ws_open(listener *l) {
    char path[256];
    snprintf(path, sizeof path, "/realtime/v1/websocket?apikey=%s&vsn=%s", ANON, l->v1 ? "1.0.0" : "2.0.0");
    l->fd = tc_ws_open(g_port, path, NULL);
    l->last_hb = mono_ms();
    return l->fd >= 0;
}

// The game's row as the server stored it: from the database file itself, or
// (CBASE_STORE=dynamo, `make check-dynamo`) from the server's DynamoDB table.
static bool db_game(const char *gid, int64_t *needs_bots, int64_t *version) {
    if (tsb_dynamo()) {
        static cb_store *dy;
        const char *t = getenv("CBASE_TABLE");
        if (!dy && cb_store_open_dynamo(t && *t ? t : "cbase", &dy) != CB_OK) return false;
        cb_game_row g;
        if (CB_STORE_CALL(dy, game_get, gid, &g) != CB_OK) return false;
        *needs_bots = g.needs_bots;
        *version = g.version;
        cb_game_row_free(&g);
        return true;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/cbase.sqlite", g_dbdir);
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    bool ok = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK
              && sqlite3_prepare_v2(db, "SELECT needs_bots, version FROM games WHERE id = ?1", -1, &st, NULL) == SQLITE_OK
              && sqlite3_bind_text(st, 1, gid, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW;
    if (ok) {
        *needs_bots = sqlite3_column_int64(st, 0);
        *version = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return ok;
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
    char path_pv[512];
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

    // A (realtime-js, vsn 2.0.0) hears its private gu- topic; B (supabase-swift,
    // vsn 1.0.0) hears its pv- player_views changes and the game- spectator
    // stream. Together they cover fanout.c's three pushes.
    listener *LA = calloc(1, sizeof *LA), *LB = calloc(1, sizeof *LB);
    LB->v1 = true;
    char topic[160], pvconf[512];
    snprintf(pvconf, sizeof pvconf,
             "{\"broadcast\":{\"ack\":false,\"self\":false},\"presence\":{\"key\":\"\"},\"postgres_changes\":"
             "[{\"event\":\"*\",\"schema\":\"public\",\"table\":\"player_views\",\"filter\":\"player_id=eq.%s\"}],"
             "\"private\":false}", B.id);
    static const char GU_CONF[] = "{\"broadcast\":{\"ack\":false,\"self\":false},\"presence\":{\"key\":\"\"},"
                                  "\"postgres_changes\":[],\"private\":true}";
#define SUBSCRIBE()                                                                     \
    do {                                                                                \
        T_CHECK(ws_open(LA) && ws_open(LB));                                            \
        snprintf(topic, sizeof topic, "gu-%s-%s", gid, A.id);                            \
        T_CHECK(ws_join(LA, topic, GU_CONF, A.token, 1));                               \
        snprintf(topic, sizeof topic, "pv-%s", B.id);                                    \
        T_CHECK(ws_join(LB, topic, pvconf, B.token, 1));                                \
        snprintf(topic, sizeof topic, "game-%s", gid);                                   \
        T_CHECK(ws_join(LB, topic, "{\"private\":false}", B.token, 2));                 \
    } while (0)
    SUBSCRIBE();

    T_EQ_INT(ask(&B, ASK_JOIN, gid, &env), 200);
    T_EQ_INT(ask(&A, ASK_ADD_BOT, gid, &env), 200);
    T_EQ_INT(ask(&A, ASK_START, gid, &env), 200);
    T_EQ_INT(ask(&B, ASK_START, gid, &env), 200);
    client_init(&sc->c, &sc->slot);
    T_CHECK(client_adopt_envelope(&sc->c, env.p, (int)env.len) >= 0);
    T_EQ_INT(sc->c.view.status, GAME_STATUS_PLAYING);

    // Play: each human bumps (which also wakes the bots) for their envelope and
    // moves when they have a move; the bot pool plays the bot.
    //
    // Once, when neither human has a move and the stored row says the bots owe
    // one, the server is KILLED (no drain) and started again on the same file,
    // and nobody bumps: only serve's heartbeat (the 10 s scan) can restart the
    // drive, and only once the dead process's 25 s lease has run out. The
    // stored version moving on proves the heartbeat, the lease expiry and the
    // reload from a crash.
    const smoke_user *humans[2] = { &A, &B };
    uint32_t rng = 7;
    int status = GAME_STATUS_PLAYING, moves = 0, crash_tries = 0;
    bool crashed = false;
    // The game ends when it ends: a long one (many pickups) plus the crash
    // takes minutes. What must not happen is a stall, so the loop gives up
    // only when the version has not moved for STALL_MS (well past the crash's
    // lease wait), under a generous overall cap.
    enum { STALL_MS = 90000, CAP_MS = 900000 };
    const int64_t t_start = mono_ms();
    int64_t last_version = -1, last_progress = t_start;
    while (status == GAME_STATUS_PLAYING && mono_ms() - last_progress < STALL_MS && mono_ms() - t_start < CAP_MS) {
        bool moved = false, answered = false;
        for (int h = 0; h < 2 && status == GAME_STATUS_PLAYING; h++) {
            if (ask(humans[h], ASK_BUMP, gid, &env) != 200) continue;
            answered = true;
            client_init(&sc->c, &sc->slot);
            if (client_adopt_envelope(&sc->c, env.p, (int)env.len) < 0) continue;
            status = sc->c.view.status;
            uint32_t version = sc->c.view.version;
            if ((int64_t)version != last_version) { last_version = version; last_progress = mono_ms(); }
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
        drain_ws(LA, 0);
        drain_ws(LB, 0);
        if (!answered) break;   // the server stopped answering: the asserts below say so
        int64_t nb = 0, ver = 0;
        if (!moved && !crashed && moves >= 4 && crash_tries < 5 && status == GAME_STATUS_PLAYING
            && db_game(gid, &nb, &ver) && nb) {
            crash_tries++;
            tp_stop(&g_child, SIGKILL, 5000);
            close(LA->fd);
            close(LB->fd);
            bool owed = db_game(gid, &nb, &ver) && nb;
            start_server();
            SUBSCRIBE();
            if (!owed) continue;   // the bot moved before the kill landed: try again later
            crashed = true;
            snprintf(path_pv, sizeof path_pv, "/rest/v1/player_views?select=version&game_id=eq.%s&player_id=eq.%s", gid,
                     A.id);
            int64_t t0 = mono_ms(), now_v = ver;
            while (mono_ms() - t0 < 75000 && now_v <= ver) {
                struct timespec ts = { 0, 500 * 1000000 };
                nanosleep(&ts, NULL);
                if (rest_get(&A, path_pv, &tmp) == 200) cb_json_get_i64(cb_buf_cstr(&tmp), tmp.len, "0.version", &now_v);
            }
            fprintf(stderr, "smoke: after a crash at version %lld the bots moved on to %lld in %lld ms, unbumped\n",
                    (long long)ver, (long long)now_v, (long long)(mono_ms() - t0));
            T_CHECK(now_v > ver);
            last_progress = mono_ms();
            continue;
        }
        if (!moved) {
            // The bots owe a move: give the bot pool a moment.
            struct timespec ts = { 0, 50 * 1000000 };
            nanosleep(&ts, NULL);
        }
    }
    fprintf(stderr, "smoke: %s after %d human moves, %lld s, version %lld\n",
            status == GAME_STATUS_GAME_OVER ? "game over" : "NO PROGRESS", moves, (long long)((mono_ms() - t_start) / 1000),
            (long long)last_version);
    T_EQ_INT(status, GAME_STATUS_GAME_OVER);
    T_CHECK(moves > 0);
    T_CHECK(crashed);

    // Pushes, each stream strictly increasing in version: A's private casts,
    // the spectators' casts, B's player_views changes (each with "\x" hex).
    drain_ws(LA, 1000);
    drain_ws(LB, 1000);
    fprintf(stderr, "smoke: heard %d gu- casts, %d game- casts, %d pv- changes\n", LA->n_cast, LB->n_cast, LB->n_pv);
    T_CHECK(LA->n_cast > 0);
    T_CHECK(LB->n_cast > 0);
    T_CHECK(LB->n_pv > 0);
    T_EQ_INT(LB->bad_pv, 0);
    for (int i = 1; i < LA->n_cast; i++) T_CHECK(LA->cast_v[i] > LA->cast_v[i - 1]);
    for (int i = 1; i < LB->n_cast; i++) T_CHECK(LB->cast_v[i] > LB->cast_v[i - 1]);
    for (int i = 1; i < LB->n_pv; i++) T_CHECK(LB->pv_v[i] > LB->pv_v[i - 1]);

    // Ratings and the snapshot.
    char path[512];
    // B's last pv- change is B's stored row.
    snprintf(path, sizeof path, "/rest/v1/player_views?select=version&game_id=eq.%s&player_id=eq.%s", gid, B.id);
    T_EQ_INT(rest_get(&B, path, &tmp), 200);
    int64_t bv = -1;
    T_CHECK(cb_json_get_i64(cb_buf_cstr(&tmp), tmp.len, "0.version", &bv));
    if (LB->n_pv > 0 && LB->pv_v[LB->n_pv - 1] != bv) {
        fprintf(stderr, "smoke: B's row is at version %lld; the pv- changes heard end", (long long)bv);
        for (int i = LB->n_pv > 6 ? LB->n_pv - 6 : 0; i < LB->n_pv; i++) fprintf(stderr, " %lld", (long long)LB->pv_v[i]);
        fprintf(stderr, "\n");
    }
    T_CHECK(LB->n_pv > 0 && LB->pv_v[LB->n_pv - 1] == bv);
    close(LA->fd);
    close(LB->fd);
    free(LA);
    free(LB);
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
    if (g_port > 0 && !(game && strcmp(game, "0") == 0)) T_RUN(game_scenario);
    else printf("NOTE smoke: the game scenario is off (CBASE_SMOKE_GAME=0)\n");
    T_RUN(drains_on_sigterm);
    if (g_dbdir[0]) {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dbdir);
        if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", g_dbdir);
    }
    return T_DONE();
}
