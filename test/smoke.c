// smoke.c - `cbase serve` end to end, as a client sees it.
//
//   build/test/smoke build/cbase
//
// Starts the real binary on a free port with a fresh database directory, waits
// for its "listening on" line, and talks to it over HTTP. ALWAYS it proves the
// skeleton: the process starts (the kernel self-test passed), answers
// GET /health, and drains on SIGTERM with exit status 0.
//
// THE GAME SCENARIO (the brief's), always, unless CBASE_SMOKE_GAME=0, over
// the binary surface (DESIGN.md "Binary surface") as the clients speak it,
// every request built and every answer read by the kernel's client library
// (netclient.h): three humans sign up; A creates, B joins, A adds a bot, both
// start; each holds an MQTT stream socket: A on its own seat's casts, B on its
// views, C, who never sits, on the game's spectator casts. The humans play legal moves (chosen on their own masked
// envelope with the kernel's client table, as the browser does) while the bot
// pool drives the bot. Once mid-game, when only the bots owe a move, the
// server is killed and restarted with nobody bumping: the heartbeat must
// resume the bot after the dead process's lease runs out. At the end: game
// over, every push stream strictly increasing in version (B's last view push
// is B's stored view), the ratings changed (the leaderboard), one replay
// stored (A's history), and after a clean restart on the same database the
// game reloads at the same version with the same envelope.
//
// Every module is real here: auth (real JWTs), the SQLite store, the /v1
// routes, realtime (rt_local + stream_mqtt, through rt_deps' production table),
// the app.
//
// THE BOTS' TIME RUNS SMOKE_SPEEDUP TIMES FASTER (CBASE_BOT_SPEEDUP, config.h):
// the pace, the lease, the scan's staleness window and serve's scan period are
// each production's over SMOKE_SPEEDUP, so every ratio between them holds and
// the game, the crash's lease expiry and the idle human's rest each take
// seconds. unit_app holds production's values and the division itself.
#include "t.h"
#include "lib_store_backend.h"

#include "lib_client.h"
#include "lib_proc.h"
#include "fixture_unfurl.h"

#include "awire.h"
#include "client_table.h"
#include "evwire.h"
#include "legal.h"
#include "table.h"
#include "net/ws.h"
#include "util/clock.h"
#include "app/bots.h"

#include "ask.h"
#include "netclient.h"
#include "netwire.h"
#include "mqtt/mqtt.h"

#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../../shared/c/rng.h"

#define SMOKE_SPEEDUP 20
static const char *g_bin;
static tp_child g_child;
static int g_port;
static char g_dbdir[400];

static void start_server(void) {
    char line[512];
    static char envdb[512];
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/cbase.sqlite", g_dbdir);
    // Fresh accounts, rated at once (CBASE_RATED_MIN_AGE, table_ops.c): the scenario checks the leaderboard moves.
    static char speedup[48];
    snprintf(speedup, sizeof speedup, "CBASE_BOT_SPEEDUP=%d", SMOKE_SPEEDUP);
    char *env[] = { "CBASE_LISTEN=127.0.0.1:0", envdb, "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret",
                    "CBASE_RATED_MIN_AGE=0", speedup, NULL };
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

// GET and HEAD /m/<payload>: the link preview a crawler fetches, with no key.
static void unfurl(void) {
    char path[4096];
    snprintf(path, sizeof path, "/m/%s", UNFURL_GOLDENS[0].payload);
    tc_response g, h;
    T_EQ_INT(tc_http(g_port, "GET", path, NULL, NULL, 0, &g), 0);
    T_EQ_INT(g.status, 200);
    char v[128];
    T_EQ_STR(tc_header(&g, "content-type", v, sizeof v), "text/html; charset=utf-8");
    T_EQ_STR(tc_header(&g, "cache-control", v, sizeof v), "public, max-age=300");
    for (int k = 0; k < UNFURL_N_TAGS; k++) T_CHECK(strstr(cb_buf_cstr(&g.body), UNFURL_GOLDENS[0].tags[k]) != NULL);
    T_EQ_INT(tc_http(g_port, "HEAD", path, NULL, NULL, 0, &h), 0);
    T_EQ_INT(h.status, 200);
    char cl[32];
    snprintf(cl, sizeof cl, "%zu", g.body.len);
    T_EQ_STR(tc_header(&h, "content-length", v, sizeof v), cl);
    T_EQ_INT(h.body.len, 0);
    tc_response_free(&g);
    tc_response_free(&h);
    // HEAD on any GET route: /health answered 405 to it before.
    T_EQ_INT(tc_http(g_port, "HEAD", "/health", NULL, NULL, 0, &h), 0);
    T_EQ_INT(h.status, 200);
    T_EQ_INT(h.body.len, 0);
    tc_response_free(&h);
}

// The drain names who sent the signal (main_serve.c): here, this test.
static void drains_on_sigterm(void) {
    char want[64];
    snprintf(want, sizeof want, "SIGTERM from pid %d ", (int)getpid());
    T_EQ_INT(kill(g_child.pid, SIGTERM), 0);
    T_EQ_INT(tp_wait_line(&g_child, want, 15000, NULL, 0), 0);
    T_EQ_INT(tp_stop(&g_child, 0, 15000), 0);
}

// ---- the game scenario ---------------------------------------------------------------

// A signed-in user: the client library's session, saved between turns (it
// holds one session at a time, as one app does).
typedef struct {
    char    name[32];
    char    id[64];
    uint8_t blob[NC_SESSION_BLOB_MAX];
    int     blob_len;
} smoke_user;

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// The last answer, as the host received it.
static int g_status;
static uint8_t g_body[1 << 20];
static int g_len;

// One request over HTTP, as a host's transport sends it.
#define SMOKE_ANSWER_MS 120000

static int send_req(const NcRequest *q) {
    g_status = -1;
    g_len = 0;
    if (!q) return -1;
    char path[NC_PATH_MAX + 1], headers[NC_AUTH_MAX + 128];
    memcpy(path, q->path, q->path_len);
    path[q->path_len] = 0;
    int h = 0;
    if (q->auth_len) h += snprintf(headers + h, sizeof headers - (size_t)h, "Authorization: %.*s\r\n", (int)q->auth_len, q->auth);
    if (q->method == NC_POST) h += snprintf(headers + h, sizeof headers - (size_t)h, "Content-Type: application/octet-stream\r\n");
    headers[h] = 0;
    // A HANG GUARD, NOT A SPEED CLAIM: the answer may take as long as a
    // starved machine makes it. lib_client's 10 s once failed a move the
    // server answered correctly, late, on a Mac at load ~700 (a lost answer to
    // a move is the client's INTERNAL, a non-200 here); reproduced by freezing
    // the server with SIGSTOP for 11 s mid-request.
    tc_response r;
    int fd = tc_connect(g_port);
    if (fd < 0) return -1;
    int rc = tc_request(fd, q->method == NC_GET ? "GET" : "POST", path, headers, q->body, q->body_len, SMOKE_ANSWER_MS, &r);
    close(fd);
    if (rc != 0) return -1;
    g_status = r.status;
    g_len = r.body.len < sizeof g_body ? (int)r.body.len : 0;
    memcpy(g_body, r.body.p, (size_t)g_len);
    tc_response_free(&r);
    return g_status;
}

// u's intent, through the library's pump as a host runs it: the session
// loaded, every request it names sent (a refresh first when it asks for one)
// until the intent's ticket is done, the session saved again. The status of
// the answer that finished it (g_body holds it), -1 when none did.
static int call(smoke_user *u, int kind, const char *gid, const void *bytes, int n, int64_t intent) {
    T_EQ_INT(nc_session_load(u->blob, u->blob_len), NW_OK);
    NcIntent in;
    memset(&in, 0, sizeof in);
    in.kind = (uint8_t)kind;
    if (gid) { in.gid_len = (uint8_t)strlen(gid); memcpy(in.gid, gid, in.gid_len); }
    if (n > 0) { in.bytes_len = (uint16_t)n; memcpy(in.bytes, bytes, (size_t)n); }
    in.intent = intent;
    int st = -1;
    uint32_t t = nc_do(&in, cb_now_ms());
    for (const NcRequest *q; t && (q = nc_next_request(cb_now_ms()));) {
        uint32_t id = q->id;
        int got = send_req(q);
        const NcDone *d = nc_on_response(id, got < 0 ? 0 : got, g_body, g_len, cb_now_ms());
        if (d && d->ticket == t) { st = got; break; }
    }
    u->blob_len = nc_session_save(u->blob, sizeof u->blob);
    return st;
}

static int signup(smoke_user *u, const char *name) {
    snprintf(u->name, sizeof u->name, "%s", name);
    nc_reset();
    send_req(nc_req_signup(name, (int)strlen(name), "smoke-pass-123", 14));
    int why = nc_session_read(g_status, g_body, g_len);
    if (why) { fprintf(stderr, "signup %s: %d why %d\n", name, g_status, why); return -1; }
    const NcSessionInfo *i = nc_session_info();
    snprintf(u->id, sizeof u->id, "%.*s", (int)i->user_id_len, i->user_id);
    u->blob_len = nc_session_save(u->blob, sizeof u->blob);
    return u->blob_len > 0 ? 0 : -1;
}

// An ask; the asker's envelope into env. The status.
static int ask(smoke_user *u, int kind, const char *gid, cb_buf *env) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = (uint8_t)kind;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    int st = n > 0 ? call(u, NC_DO_ASK, NULL, body, n, -1) : -1;
    cb_buf_reset(env);
    cb_buf_append(env, g_body, (size_t)g_len);
    return st;
}

// u's view of the game: its version, the envelope into env. false when there is none.
static bool game_view(smoke_user *u, const char *gid, int64_t *version, cb_buf *env) {
    if (call(u, NC_DO_OPEN_GAME, gid, NULL, 0, -1) != 200) return false;
    const NcGameRow *row = nc_view_read(g_status, g_body, g_len);
    if (!row) return false;
    *version = row->version;
    if (env) {
        cb_buf_reset(env);
        cb_buf_append(env, g_body + row->env_off, row->env_len);
    }
    return true;
}

typedef struct {
    ClientSlot  slot;
    ClientTable c;
    LegalMoves  moves;
} smoke_client;

// A legal move for the viewer on their envelope, as the browser would offer
// one: the kernel's legal moves on the masked board, each judged again by
// client_validate. Its wire length, or 0 when the viewer has none.
static int choose_move(smoke_client *sc, const cb_buf *env, uint8_t *wire, int cap, uint64_t *rng) {
    client_init(&sc->c, &sc->slot);
    if (client_adopt_envelope(&sc->c, env->p, (int)env->len) < 0) return 0;
    int seat = sc->c.view.my_seat;
    if (seat < 0 || sc->c.view.status != GAME_STATUS_PLAYING) return 0;
    calculate_legal_moves(sc->c.g, seat, &sc->moves);
    int start = sc->moves.n ? (int)(rng_u32(rng) % (uint32_t)sc->moves.n) : 0;
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

// One stream socket and what it heard: the versions of the casts and of the
// views pushed, in order. It speaks MQTT (shared/c/mqtt) on /mqtt, as the
// clients do against IoT Core, and each PUBLISH's payload is a netwire.h frame.
typedef struct {
    int      fd;
    char     token[NW_ACCESS_MAX + 1];
    char     uid[64];
    char     topic[128];          // the one filter it subscribes
    int      n_cast;
    uint32_t cast_v[4096];
    int      n_view;
    uint32_t view_v[4096];
    int      refused;             // SUBACKs that refused, and closes
    int64_t  last_ping;           // when this socket last pinged
    // n_cast at each reopen (the crash's): a socket that was closed missed what
    // was cast meanwhile, so the versions may jump there and only there.
    int      n_reopen;
    int      reopen_at[16];
} listener;

// Every cast follows the one before it (version + 1), but across a reopen.
// A commit the stream never casts is a gap the kernel client takes for a lost
// push, and it fetches the game whole (netclient.c NW_WS_CAST).
static int cast_gaps(const listener *l) {
    int gaps = 0;
    for (int i = 1; i < l->n_cast; i++) {
        bool reopened = false;
        for (int k = 0; k < l->n_reopen; k++) reopened |= l->reopen_at[k] == i;
        if (!reopened && l->cast_v[i] != l->cast_v[i - 1] + 1) {
            if (gaps < 4) fprintf(stderr, "smoke: a cast at v%u follows v%u\n", l->cast_v[i], l->cast_v[i - 1]);
            gaps++;
        }
    }
    return gaps;
}

static void mq_send(listener *l, const uint8_t *b, int n) {
    if (n > 0) tc_ws_send(l->fd, CB_WS_OP_BIN, b, (size_t)n);
}

// Every packet of one WebSocket message; the SUBACK's first code into *suback.
static void hear(listener *l, const uint8_t *p, size_t n, int *suback) {
    MqttPacket m;
    for (int at = 0, k; at < (int)n && (k = mqtt_parse(p + at, (int)n - at, 1 << 20, MQTT_FROM_SERVER, &m)) > 0; at += k) {
        if (m.type == MQTT_SUBACK) {
            if (m.n_items && m.items[0] == MQTT_SUBACK_FAILURE) l->refused++;
            if (suback && m.n_items) *suback = m.items[0];
        }
        if (m.type != MQTT_PUBLISH) continue;
        static NwFrame f;
        if (nw_frame_decode(m.payload, m.payload_len, &f) != NW_OK) continue;
        if (f.kind == NW_WS_CAST && l->n_cast < 4096) l->cast_v[l->n_cast++] = f.version;
        else if (f.kind == NW_WS_VIEW && f.blob.len && l->n_view < 4096) l->view_v[l->n_view++] = f.version;
    }
}

// Reads every message that arrives within timeout_ms, then each next one that
// arrives within gap_ms of the last. The game loop takes only what is already
// there (0, 0): what is still in flight is read on the next turn. Like the
// clients, pings every 30 s inside its 60 s keep-alive.
static void drain_ws(listener *l, int timeout_ms, int gap_ms) {
    if (l->fd >= 0 && mono_ms() - l->last_ping > NC_PING_MS) {
        uint8_t b[2];
        mq_send(l, b, mqtt_write_bare(b, sizeof b, MQTT_PINGREQ));
        l->last_ping = mono_ms();
    }
    cb_buf f;
    cb_buf_init(&f);
    int op;
    while (l->fd >= 0 && (op = tc_ws_recv(l->fd, timeout_ms, &f)) >= 0) {
        if (op == CB_WS_OP_CLOSE) { l->refused++; break; }
        hear(l, f.p, f.len, NULL);
        cb_buf_reset(&f);
        timeout_ms = gap_ms;
    }
    cb_buf_free(&f);
}

// Opens the socket, CONNECTs with the token and subscribes; true once the
// subscription is granted. Pushes that arrive first are heard.
static bool ws_open(listener *l) {
    if (l->n_cast > 0 && l->n_reopen < 16) l->reopen_at[l->n_reopen++] = l->n_cast;
    l->fd = tc_ws_open(g_port, "/mqtt", "Sec-WebSocket-Protocol: mqtt\r\n");
    l->last_ping = mono_ms();
    if (l->fd < 0) return false;
    static uint8_t b[NW_ACCESS_MAX + 256];
    char id[80];
    int idn = snprintf(id, sizeof id, "%s-smoke", l->uid);
    int n = mqtt_write_connect(b, sizeof b, id, idn, 60, "token", 5, (const uint8_t *)l->token, (int)strlen(l->token));
    MqttTopic t = { l->topic, (int)strlen(l->topic), 0 };
    int m = n > 0 ? mqtt_write_subscribe(b + n, (int)sizeof b - n, 1, &t, 1) : -1;
    if (m < 0) return false;
    mq_send(l, b, n + m);
    cb_buf in;
    cb_buf_init(&in);
    int code = -1;
    for (int i = 0; i < 50 && code < 0; i++) {
        int op = tc_ws_recv(l->fd, 5000, &in);
        if (op < 0 || op == CB_WS_OP_CLOSE) break;
        hear(l, in.p, in.len, &code);
        cb_buf_reset(&in);
    }
    cb_buf_free(&in);
    return code == 0;
}

// The listener for u: its bearer token (what a built request carries) and its one topic.
static void ws_for(listener *l, smoke_user *u, const char *topic) {
    T_EQ_INT(nc_session_load(u->blob, u->blob_len), NW_OK);
    const NcRequest *q = nc_req_games(cb_now_ms());
    T_CHECK(q && !q->is_refresh && q->auth_len > 7);
    if (q) snprintf(l->token, sizeof l->token, "%.*s", (int)q->auth_len - 7, q->auth + 7);
    snprintf(l->uid, sizeof l->uid, "%s", u->id);
    snprintf(l->topic, sizeof l->topic, "%s", topic);
    l->fd = -1;
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

// The scan's staleness clock (SQLite: the row's updated_at, which every lease
// acquire moves); -1 under DynamoDB, where it lives on the SCAN marker.
static int64_t db_scan_clock(const char *gid) {
    if (tsb_dynamo()) return -1;
    char path[512];
    snprintf(path, sizeof path, "%s/cbase.sqlite", g_dbdir);
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int64_t at = -2;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK
        && sqlite3_prepare_v2(db, "SELECT updated_at FROM games WHERE id = ?1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_bind_text(st, 1, gid, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        at = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return at;
}

// THE IDLE HUMAN (production, 2026-10-17: 1,037 of 1,149 bot cycles in a week
// drove nothing). One human against one bot; the human owes the move and walks
// away. No bot can act, so the stored row must not say the bots are owed one,
// and serve's heartbeat (a scan every CB_BOT_SCAN_EVERY_MS, a game stale after
// CB_BOT_STALE_MS, both over SMOKE_SPEEDUP) must never start a drive for it: no
// lease is taken (SQLite's lease acquire moves the row's updated_at) and
// nothing is committed, across two scans past the staleness window.
static void an_idle_human_turn_is_never_kicked(void) {
    smoke_user D;
    if (signup(&D, "SMOKED") != 0) { T_CHECK(0); return; }
    smoke_client *sc = calloc(1, sizeof *sc);
    cb_buf env;
    cb_buf_init(&env);
    T_EQ_INT(call(&D, NC_DO_CREATE, NULL, NULL, 0, -1), 200);
    client_init(&sc->c, &sc->slot);
    T_CHECK(client_adopt_envelope(&sc->c, g_body, g_len) >= 0);
    char gid[32];
    snprintf(gid, sizeof gid, "%.*s", (int)sc->c.gid_len, sc->c.gid);
    T_EQ_INT(ask(&D, ASK_ADD_BOT, gid, &env), 200);
    T_EQ_INT(ask(&D, ASK_START, gid, &env), 200);
    // Until D owes a move: the bot pool plays the bot's (the deal's pacing first).
    uint8_t wire[64];
    uint64_t rng = 11;
    bool owes = false;
    const int64_t t0 = mono_ms();
    while (!owes && mono_ms() - t0 < 30000) {
        int64_t seen;
        if (!game_view(&D, gid, &seen, &env)) break;   // a read, as a client polls; a bump is limited
        owes = choose_move(sc, &env, wire, sizeof wire, &rng) > 0;
        if (!owes) {
            struct timespec ts = { 0, 100 * 1000000 };
            nanosleep(&ts, NULL);
        }
    }
    T_CHECK(owes);
    // While D deliberates a bot may still act (an attacker throws in or says
    // good), paced: the bots are done when the stored row says no
    // bot owes a move (needs_bots). Not "the version held for longer than a
    // pace": a starved bot pool holds the version too (it failed once that
    // way with the server frozen for 12 s, a bot's move still to come).
    int64_t nb = -1, v0 = -1, v1 = -1;
    while (db_game(gid, &nb, &v0) && nb != 0 && mono_ms() - t0 < 120000) {
        struct timespec ts = { 0, 200 * 1000000 };
        nanosleep(&ts, NULL);
    }
    T_CHECK(db_game(gid, &nb, &v0));
    T_EQ_INT(nb, 0);
    const int64_t clock0 = db_scan_clock(gid);
    // Two scans past the staleness window, and a fifth of one more (production's 22 s).
    const int64_t rest = (CB_BOT_STALE_MS + 2 * CB_BOT_SCAN_EVERY_MS + 2000) / SMOKE_SPEEDUP;
    cb_sleep_ms(rest);
    T_CHECK(db_game(gid, &nb, &v1));
    T_EQ_INT(v1, v0);
    T_EQ_INT(db_scan_clock(gid), clock0);
    fprintf(stderr, "smoke: an idle human's turn rested %lld ms at version %lld, scan clock %lld -> %lld\n", (long long)rest,
            (long long)v0, (long long)clock0, (long long)db_scan_clock(gid));
    cb_buf_free(&env);
    free(sc);
}

static void game_scenario(void) {
    smoke_user A, B, C;
    int sa = signup(&A, "SMOKEA"), sb = signup(&B, "SMOKEB"), sc_ = signup(&C, "SMOKEC");
    T_EQ_INT(sa, 0);
    T_EQ_INT(sb, 0);
    T_EQ_INT(sc_, 0);
    if (sa || sb || sc_) return;   // nothing below can pass without the sessions
    smoke_client *sc = calloc(1, sizeof *sc);
    cb_buf env;
    cb_buf_init(&env);

    // create: A's envelope names the game.
    int created = call(&A, NC_DO_CREATE, NULL, NULL, 0, -1);
    T_EQ_INT(created, 200);
    if (created != 200) { cb_buf_free(&env); free(sc); return; }
    client_init(&sc->c, &sc->slot);
    T_CHECK(client_adopt_envelope(&sc->c, g_body, g_len) >= 0);
    char gid[32];
    snprintf(gid, sizeof gid, "%.*s", (int)sc->c.gid_len, sc->c.gid);
    T_EQ_INT(strlen(gid), 12);   // app_internal.h CB_GAME_ID_HEX (finding L3)

    // A hears its own seat's casts, B its views (the games list), C the
    // spectators' casts. Together they cover fanout.c's three pushes.
    listener *LA = calloc(1, sizeof *LA), *LB = calloc(1, sizeof *LB), *LC = calloc(1, sizeof *LC);
    char topic[128];
    snprintf(topic, sizeof topic, "u/%s/c/%s", A.id, gid);
    ws_for(LA, &A, topic);   // A's own seat's casts
    snprintf(topic, sizeof topic, "u/%s/v", B.id);
    ws_for(LB, &B, topic);   // B's views
    snprintf(topic, sizeof topic, "s/%s", gid);
    ws_for(LC, &C, topic);   // the spectators' casts
#define SUBSCRIBE() T_CHECK(ws_open(LA) && ws_open(LB) && ws_open(LC))
    SUBSCRIBE();

    T_EQ_INT(ask(&B, ASK_JOIN, gid, &env), 200);
    T_EQ_INT(ask(&A, ASK_ADD_BOT, gid, &env), 200);
    T_EQ_INT(ask(&A, ASK_START, gid, &env), 200);
    T_EQ_INT(ask(&B, ASK_START, gid, &env), 200);
    client_init(&sc->c, &sc->slot);
    T_CHECK(client_adopt_envelope(&sc->c, env.p, (int)env.len) >= 0);
    T_EQ_INT(sc->c.view.status, GAME_STATUS_PLAYING);

    // Play: each human reads the game for their envelope, as a client does,
    // and moves when they have a move; the bot pool plays the bot (a move
    // that leaves the bot to act wakes it). Not by bumping: a bump is a
    // seated player's nudge, limited (limits.h), not a way to poll.
    //
    // Once, when neither human has a move and the stored row says the bots owe
    // one, the server is KILLED (no drain) and started again on the same file,
    // and nobody moves: only serve's heartbeat (the scan) can restart the
    // drive, and only once the dead process's lease has run out. The
    // stored version moving on proves the heartbeat, the lease expiry and the
    // reload from a crash.
    smoke_user *humans[2] = { &A, &B };
    uint64_t rng = 7;
    int status = GAME_STATUS_PLAYING, moves = 0, crash_tries = 0;
    bool crashed = false, rearranged = false;
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
            int64_t seen;
            if (!game_view(humans[h], gid, &seen, &env)) {
                fprintf(stderr, "smoke: %s's game read answered %d (why %d)\n", humans[h]->name, g_status, nc_status_read(g_status, g_body, g_len));
                continue;
            }
            answered = true;
            client_init(&sc->c, &sc->slot);
            if (client_adopt_envelope(&sc->c, env.p, (int)env.len) < 0) continue;
            status = sc->c.view.status;
            uint32_t version = sc->c.view.version;
            if ((int64_t)version != last_version) { last_version = version; last_progress = mono_ms(); }
            // Once, A reverses its hand: a commit that shows nobody anything and
            // must still be cast (the gap check below).
            const int me = sc->c.view.my_seat;
            if (h == 0 && !rearranged && moves >= 4 && me >= 0 && sc->c.g->players[me].hand_count >= 2) {
                TableAsk ra;
                memset(&ra, 0, sizeof ra);
                ra.kind = ASK_REARRANGE_HAND;
                ra.gid_len = (uint8_t)strlen(gid);
                memcpy(ra.gid, gid, ra.gid_len);
                ra.n_idx = (uint8_t)sc->c.g->players[me].hand_count;
                for (int i = 0; i < ra.n_idx; i++) ra.idx[i] = (uint8_t)(ra.n_idx - 1 - i);
                uint8_t body[ASK_BYTES_MAX];
                int bn = ask_encode(&ra, body, sizeof body);
                T_EQ_INT(bn > 0 ? call(&A, NC_DO_ASK, NULL, body, bn, -1) : -1, 200);
                rearranged = true;
                continue;   // the board moved on: read it again
            }
            uint8_t wire[64];
            int n = choose_move(sc, &env, wire, sizeof wire, &rng);
            if (n <= 0) continue;
            int64_t t_play = mono_ms();
            int played = call(humans[h], NC_DO_PLAY, gid, wire, n, version);
            if (played != 200)
                fprintf(stderr, "smoke: %s's move at v%u answered %d (last HTTP status %d, why %d) after %lld ms\n", humans[h]->name,
                        version, played, g_status, nc_status_read(g_status, g_body, g_len), (long long)(mono_ms() - t_play));
            T_EQ_INT(played, 200);
            T_CHECK(nc_action_read(g_status, g_body, g_len) != NULL);
            moved = true;
            moves++;
        }
        drain_ws(LA, 0, 0);
        drain_ws(LB, 0, 0);
        drain_ws(LC, 0, 0);
        if (!answered) break;   // the server stopped answering: the asserts below say so
        int64_t nb = 0, ver = 0;
        if (!moved && !crashed && moves >= 4 && crash_tries < 5 && status == GAME_STATUS_PLAYING
            && db_game(gid, &nb, &ver) && nb) {
            crash_tries++;
            tp_stop(&g_child, SIGKILL, 5000);
            close(LA->fd);
            close(LB->fd);
            close(LC->fd);
            bool owed = db_game(gid, &nb, &ver) && nb;
            start_server();
            SUBSCRIBE();
            if (!owed) continue;   // the bot moved before the kill landed: try again later
            crashed = true;
            int64_t t0 = mono_ms(), now_v = ver;
            while (mono_ms() - t0 < 75000 && now_v <= ver) {
                struct timespec ts = { 0, 500 * 1000000 };
                nanosleep(&ts, NULL);
                int64_t v;
                if (game_view(&A, gid, &v, NULL)) now_v = v;
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
    if (status != GAME_STATUS_GAME_OVER) {
        // A stall is a stuck thread until shown otherwise: print every thread's
        // stack of the live server, where gdb is there to do it (CI's Linux).
        char cmd[160];
        snprintf(cmd, sizeof cmd, "command -v gdb >/dev/null && gdb -q -batch -p %d -ex 'thread apply all bt' 1>&2", (int)g_child.pid);
        if (system(cmd) != 0) fprintf(stderr, "smoke: no stacks (gdb is not here, or could not attach)\n");
    }
    T_EQ_INT(status, GAME_STATUS_GAME_OVER);
    T_CHECK(moves > 0);
    T_CHECK(crashed);
    T_CHECK(rearranged);

    // Pushes, each stream strictly increasing in version: A's own casts, the
    // spectators' casts, B's views.
    drain_ws(LA, 1000, 50);
    drain_ws(LB, 1000, 50);
    drain_ws(LC, 1000, 50);
    fprintf(stderr, "smoke: heard %d seat casts, %d spectator casts, %d views\n", LA->n_cast, LC->n_cast, LB->n_view);
    T_CHECK(LA->n_cast > 0);
    T_CHECK(LC->n_cast > 0);
    T_CHECK(LB->n_view > 0);
    T_EQ_INT(LA->refused + LB->refused + LC->refused, 0);
    for (int i = 1; i < LA->n_cast; i++) T_CHECK(LA->cast_v[i] > LA->cast_v[i - 1]);
    for (int i = 1; i < LC->n_cast; i++) T_CHECK(LC->cast_v[i] > LC->cast_v[i - 1]);
    // And none is missing: every commit is cast, to the seat and to the
    // spectators, the ones that show nothing (a good over an uncovered table, a
    // hand rearranged) included (table_ops.c).
    T_EQ_INT(cast_gaps(LA), 0);
    T_EQ_INT(cast_gaps(LC), 0);
    for (int i = 1; i < LB->n_view; i++) T_CHECK(LB->view_v[i] > LB->view_v[i - 1]);

    // B's last view push is B's stored view.
    int64_t bv = -1;
    T_CHECK(game_view(&B, gid, &bv, NULL));
    if (LB->n_view > 0 && LB->view_v[LB->n_view - 1] != bv) {
        fprintf(stderr, "smoke: B's view is at version %lld; the pushes heard end", (long long)bv);
        for (int i = LB->n_view > 6 ? LB->n_view - 6 : 0; i < LB->n_view; i++) fprintf(stderr, " %u", LB->view_v[i]);
        fprintf(stderr, "\n");
    }
    T_CHECK(LB->n_view > 0 && LB->view_v[LB->n_view - 1] == bv);
    close(LA->fd);
    close(LB->fd);
    close(LC->fd);
    free(LA);
    free(LB);
    free(LC);

    // The ratings (A is on the board with one game) and the replay (A's history holds one).
    T_EQ_INT(call(&A, NC_DO_LEADERBOARD, NULL, NULL, 0, -1), 200);
    int rows = nc_list_read(g_status, g_body, g_len, NW_BOARD);
    bool on_board = false;
    for (int i = 0; i < rows; i++) {
        const NcBoardRow *r = nc_board_row(g_body, g_len, i);
        if (r && !r->is_bot && r->id_len == strlen(A.id) && memcmp(r->id, A.id, r->id_len) == 0) on_board = r->games == 1;
    }
    T_CHECK(on_board);
    T_EQ_INT(call(&A, NC_DO_HISTORY, NULL, NULL, 0, -1), 200);
    T_EQ_INT(nc_list_read(g_status, g_body, g_len, NW_HISTORY), 1);
    const NcHistoryHead *head = nc_history_head(g_body, g_len);
    T_CHECK(head && head->has_rating);
    T_EQ_INT(call(&A, NC_DO_REPLAY, gid, NULL, 0, -1), 200);
    const NcReplay *rp = nc_replay_read(g_status, g_body, g_len);
    T_CHECK(rp && rp->moves_len > 0);

    // The view, then a restart on the same database: the same view.
    int64_t v1 = -1, v2 = -2;
    cb_buf env2;
    cb_buf_init(&env2);
    T_CHECK(game_view(&A, gid, &v1, &env));
    T_EQ_INT(tp_stop(&g_child, SIGTERM, 15000), 0);
    start_server();
    T_CHECK(game_view(&A, gid, &v2, &env2));
    T_EQ_INT(v2, v1);
    T_EQ_MEM(env2.p, env2.len, env.p, env.len);
    T_CHECK(env.len > 0);

    cb_buf_free(&env);
    cb_buf_free(&env2);
    free(sc);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: smoke <path to cbase>\n"); return 2; }
    g_bin = argv[1];
    T_RUN(starts_and_listens);
    if (g_port > 0) T_RUN(health);
    if (g_port > 0) T_RUN(unfurl);
    const char *game = getenv("CBASE_SMOKE_GAME");
    if (g_port > 0 && !(game && strcmp(game, "0") == 0)) T_RUN(an_idle_human_turn_is_never_kicked);
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
