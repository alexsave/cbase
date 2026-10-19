// p3client.c - one human's client as its own process, for profiling a game
// between two processes against `cbase serve`. NOT part of any build: an
// investigation harness. One transport loop, as a host runs it: every request
// goes through the kernel's pump (netclient.h), every stream frame into
// nc_stream_in, every game change into the kernel's client table, moves from
// the client table's legal moves after a think time.
//
//   p3client PORT NAME a|b RDVDIR THINK_MIN_MS THINK_MAX_MS SEED GAMES BOARDS
//
// a creates each game and writes RDVDIR/gid.<k>; b joins it. Both write
// RDVDIR/ready.<name> after signing up and wait for RDVDIR/go, so a profiler
// can attach before play starts. BOARDS 1: the iOS model (every cast is a
// refetch of the envelope); 0: the web model (casts applied by the client table).
#include "net/ws.h"
#include "util/clock.h"
#include "util/str.h"
#include "lib_client.h"

#include "ask.h"
#include "awire.h"
#include "client_table.h"
#include "netclient.h"
#include "netwire.h"
#include "../../shared/c/rng.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <signal.h>

// ---- nano rehearsal additions: edge headers, latency, event log ----
static char g_edge[256];          // X-Cbase-Origin + CloudFront-Viewer-Address, as CloudFront would add
static FILE *g_ev;                // per-move / per-push log: monotonic microseconds
static int64_t g_play_t0_us;
static uint32_t g_play_v;
static double *g_lat; static long g_nlat, g_caplat;
static long g_n_http_err, g_n_why[256];
static int64_t g_deadline_ms = -1;
static int64_t mono_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }
static void lat_add(double ms) { if (g_nlat == g_caplat) { g_caplat = g_caplat ? g_caplat * 2 : 4096; g_lat = realloc(g_lat, (size_t)g_caplat * sizeof *g_lat); } g_lat[g_nlat++] = ms; }
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double pct(double q) { if (!g_nlat) return 0; long i = (long)(q * (double)(g_nlat - 1) + 0.5); return g_lat[i]; }

static int g_seats = 2, g_spectator;   // P3SEATS humans+bots at the table; role s watches
static const char *g_botids[8]; static int g_nbotids;   // P3BOTIDS: bot ids the creator seats, in order
static int g_port, g_role_a, g_creator, g_think_min, g_think_max, g_games, g_boards;
static const char *g_name, *g_rdv;
static uint64_t g_rng;
static int g_last_type;

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void nap(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 }; nanosleep(&ts, NULL); }

// ---- HTTP, one kept-alive connection ----
static int g_http = -1;
static uint8_t g_body[1 << 20];
static int g_len;
static long g_n_http;
static long g_n_refetch, g_n_refused_at_once;

static int send_req(const NcRequest *q) {
    g_len = 0;
    char path[NC_PATH_MAX + 1], headers[NC_AUTH_MAX + 480];
    memcpy(path, q->path, q->path_len);
    path[q->path_len] = 0;
    int h = 0;
    if (q->auth_len) h += snprintf(headers + h, sizeof headers - (size_t)h, "Authorization: %.*s\r\n", (int)q->auth_len, q->auth);
    if (q->method == NC_POST) h += snprintf(headers + h, sizeof headers - (size_t)h, "Content-Type: application/octet-stream\r\n");
    h += snprintf(headers + h, sizeof headers - (size_t)h, "Connection: keep-alive\r\n%s", g_edge);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (g_http < 0) g_http = tc_connect(g_port);
        if (g_http < 0) return 0;
        tc_response r;
        int rc = tc_request(g_http, q->method == NC_GET ? "GET" : "POST", path, headers, q->body, q->body_len, 120000, &r);
        if (rc != 0) { tc_response_free(&r); close(g_http); g_http = -1; continue; }
        g_n_http++;
        if (r.status >= 400 && !(r.status == 409)) { g_n_http_err++; if (g_ev) fprintf(g_ev, "E %d %s %lld\n", r.status, path, (long long)mono_us()); }
        if (q->method == NC_GET && strncmp(path, "/v1/games/", 10) == 0) g_n_refetch++;
        char c[32];
        if (tc_header(&r, "connection", c, sizeof c) && strcasecmp(c, "close") == 0) { close(g_http); g_http = -1; }
        g_len = r.body.len < sizeof g_body ? (int)r.body.len : 0;
        memcpy(g_body, r.body.p, (size_t)g_len);
        int st = r.status;
        tc_response_free(&r);
        return st;
    }
    return 0;
}

// ---- the stream, buffered framing ----
static int g_ws = -1;
static uint8_t g_wsin[1 << 18];
static size_t g_wsn;
static long g_n_frames;

static void ws_send(int op, const void *p, size_t n) { if (g_ws >= 0) tc_ws_send(g_ws, op, p, n); }
static void stream_flush(void) {
    for (const NcOut *o; (o = nc_stream_next(cb_now_ms()));) ws_send(CB_WS_OP_BIN, o->bytes, o->len);
}
static void stream_open(void) {
    const NcRequest *q = nc_req_stream();
    char path[NC_PATH_MAX + 1];
    memcpy(path, q->path, q->path_len);
    path[q->path_len] = 0;
    char wh[400]; snprintf(wh, sizeof wh, "Sec-WebSocket-Protocol: mqtt\r\n%s", g_edge);
    g_ws = tc_ws_open(g_port, path, wh);
    for (int tries = 0; g_ws < 0 && getenv("P3RETRY") && tries < 600; tries++) {   // a server restart: keep trying for 60 s
        nap(100); g_ws = tc_ws_open(g_port, path, wh);
        if (g_ws >= 0 && g_ev) fprintf(g_ev, "W reconnect %lld after %d tries\n", (long long)mono_us(), tries + 1);
    }
    g_wsn = 0;
    if (g_ws < 0) { fprintf(stderr, "%s: stream open failed\n", g_name); exit(1); }
    nc_stream_opened(cb_now_ms());
    stream_flush();
}

// ---- the game ----
static ClientSlot g_slot;
static ClientTable g_c;
static LegalMoves g_moves;
static char g_gid[NW_GID_MAX + 1];
static int g_have;          // the table holds this game
static int g_game_over, g_ready_sent, g_joined, g_play_pending;
static int64_t g_think_due = -1;
static uint32_t g_played_v;
static long g_n_moves, g_n_refused, g_n_casts, g_n_views;

static int legal_wire(uint8_t *wire, int cap) {
    if (!g_have) return 0;
    int seat = g_c.view.my_seat;
    if (seat < 0 || g_c.view.status != GAME_STATUS_PLAYING) return 0;
    calculate_legal_moves(g_c.g, seat, &g_moves);
    int start = g_moves.n ? (int)(rng_u32(&g_rng) % (uint32_t)g_moves.n) : 0;
    for (int k = 0; k < g_moves.n; k++) {
        const LegalMove *m = &g_moves.moves[(start + k) % g_moves.n];
        if (m->type == MOVE_WAIT) continue;
        AwireAction a;
        memset(&a, 0, sizeof a);
        a.kind = m->type;
        a.n = m->n_cards;
        for (int i = 0; i < m->n_cards; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
        int n = awire_encode(&a, wire, cap);
        if (n > 0 && client_validate(&g_c, &g_c.view, wire, n) == 0) { g_last_type = m->type; return n; }
    }
    return 0;
}

static int submit(int kind, const void *bytes, int n, int64_t intent) {
    NcIntent in;
    memset(&in, 0, sizeof in);
    in.kind = (uint8_t)kind;
    if (g_gid[0] && kind != NC_DO_CREATE) { in.gid_len = (uint8_t)strlen(g_gid); memcpy(in.gid, g_gid, in.gid_len); }
    if (n > 0) { in.bytes_len = (uint16_t)n; memcpy(in.bytes, bytes, (size_t)n); }
    in.intent = intent;
    if (!nc_do(&in, cb_now_ms())) { fprintf(stderr, "%s: intent %d refused at once why %d intent %lld n %d\n", g_name, kind, nc_error()->why, (long long)intent, n);  return 0; }
    return 1;
}
static void submit_ask(int kind) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = (uint8_t)kind;
    a.gid_len = (uint8_t)strlen(g_gid);
    memcpy(a.gid, g_gid, a.gid_len);
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    submit(NC_DO_ASK, body, n, -1);
}

// The table changed: decide what this human does next.
static void decide(void) {
    if (!g_have) return;
    int st = g_c.view.status;
    if (getenv("P3DEBUG")) fprintf(stderr, "%s decide st %d np %d seat %d v %u played_v %u\n", g_name, st, g_c.view.num_players, g_c.view.my_seat, g_c.view.version, g_played_v);
    if (st == GAME_STATUS_GAME_OVER) { g_game_over = 1; return; }
    if (st != GAME_STATUS_PLAYING) {   // the lobby
        static int bot_asked = -1;
        if (g_spectator) return;
        if (getenv("P3BOT") && g_creator && g_c.view.num_players < g_seats && bot_asked != (int)g_c.view.version) {
            bot_asked = (int)g_c.view.version;
            TableAsk a;
            memset(&a, 0, sizeof a);
            a.kind = ASK_ADD_BOT;
            a.gid_len = (uint8_t)strlen(g_gid);
            memcpy(a.gid, g_gid, a.gid_len);
            int bi = g_c.view.num_players - 1;   // the creator is the one human
            if (bi >= 0 && bi < g_nbotids) { a.bot_len = (uint8_t)strlen(g_botids[bi]); memcpy(a.bot, g_botids[bi], a.bot_len); }
            uint8_t body[ASK_BYTES_MAX];
            int n = ask_encode(&a, body, sizeof body);
            submit(NC_DO_ASK, body, n, -1);
        }
        if (!g_ready_sent && g_c.view.num_players >= g_seats && (g_creator || g_seats > 2)) { g_ready_sent = 1; submit_ask(ASK_START); }
        return;
    }
    if (g_play_pending || g_think_due >= 0) return;
    if (g_c.view.version <= g_played_v) return;   // already moved on this board: wait for the next one
    uint8_t w[64];
    if (legal_wire(w, sizeof w) <= 0) return;
    int span = g_think_max - g_think_min;
    g_think_due = mono_ms() + g_think_min + (span > 0 ? (int)(rng_u32(&g_rng) % (uint32_t)span) : 0);
}

static void adopt_env(const uint8_t *p, int n) {
    client_init(&g_c, &g_slot);
    if (client_adopt_envelope(&g_c, p, n) < 0) return;
    g_have = 1;
    g_n_views++;
}
static void apply_cast(const uint8_t *p, int n) {
    if (!g_have) return;
    static uint8_t ident[4096];
    int il = client_identity(&g_c, ident, sizeof ident);
    if (il < 0) il = 0;
    int cor = client_cast_open(&g_c, p, n, ident, il);
    if (getenv("P3DEBUG")) fprintf(stderr, "%s cast n %d open %d\n", g_name, n, cor);
    if (cor < 0) return;
    while (client_push_next(&g_c) == 1) {}
    client_push_final(&g_c);
    g_n_casts++;
}

static void read_events(void) {
    for (int i = 0;; i++) {
        const NcEvent *e = nc_event(i);
        if (!e) break;
        if (e->gid_len != strlen(g_gid) || memcmp(e->gid, g_gid, e->gid_len) != 0) continue;
        if (getenv("P3DEBUG") && e->kind != NC_EV_NONE) fprintf(stderr, "%s ev kind %d what %d v %u table v %u\n", g_name, e->kind, e->what, e->version, g_have ? g_c.view.version : 0);
        if (g_ev && (e->kind == NC_EV_VIEW || e->kind == NC_EV_CAST)) fprintf(g_ev, "R %s %u %lld %d\n", g_gid, e->version, (long long)mono_us(), e->kind);
        if (e->kind == NC_EV_VIEW && e->payload_len) adopt_env(nc_event_payload(i), (int)e->payload_len);
        else if (e->kind == NC_EV_CAST && e->payload_len) apply_cast(nc_event_payload(i), (int)e->payload_len);
    }
}

static void on_done(const NcDone *d, const uint8_t *body) {
    if (d->why && d->kind != NC_DO_PLAY && d->kind != NC_DO_ASK) { fprintf(stderr, "%s: intent %d failed why %d status %d\n", g_name, d->kind, d->why, d->status); exit(1); }
    switch (d->kind) {
    case NC_DO_CREATE:
        snprintf(g_gid, sizeof g_gid, "%.*s", (int)d->gid_len, d->gid);
        break;
    case NC_DO_ASK:
        if (getenv("P3DEBUG")) fprintf(stderr, "%s: ask done why %d code %d status %d v %u\n", g_name, d->why, d->code, d->status, d->version);
        if (d->why) fprintf(stderr, "%s: ask refused why %d code %d\n", g_name, d->why, d->code);
        if (!g_creator) g_joined = 1;
        break;
    case NC_DO_PLAY: {
        g_play_pending = 0;
        int64_t t1 = mono_us();
        lat_add((double)(t1 - g_play_t0_us) / 1000.0);
        if (d->why || d->action_status != TABLE_STATUS_APPLIED) { g_n_refused++; g_n_why[d->why ? d->why : 255]++; if (g_ev) fprintf(g_ev, "X %s %u why %d st %d code %d status %d\n", g_gid, g_play_v, d->why, d->action_status, d->code, d->status); }
        if (g_ev) fprintf(g_ev, "M %s %u %lld %lld %u\n", g_gid, g_play_v, (long long)g_play_t0_us, (long long)t1, d->version);
        }
        break;
    }
    if (body && d->a_len && !d->stale && d->kind != NC_DO_PLAY) adopt_env(body + d->a_off, (int)d->a_len);
}

// Sends every request the pump has, feeding answers back.
static void pump(void) {
    for (const NcRequest *q; (q = nc_next_request(cb_now_ms()));) {
        uint32_t id = q->id;
        int st = send_req(q);
        if (getenv("P3DEBUG")) fprintf(stderr, "%s req %.*s %d\n", g_name, (int)q->path_len, q->path, q->method);
        const NcDone *d = nc_on_response(id, st, g_body, g_len, cb_now_ms());
        read_events();
        if (d) on_done(d, g_body);
    }
    for (const NcDone *d; (d = nc_next_done());) on_done(d, NULL);
    stream_flush();
}

static void stream_read(void) {
    ssize_t r = recv(g_ws, g_wsin + g_wsn, sizeof g_wsin - g_wsn, 0);
    if (r <= 0) {
        if (r < 0 && errno == EINTR) return;
        close(g_ws); g_ws = -1;
        nc_stream_closed(cb_now_ms());
        stream_open();
        return;
    }
    g_wsn += (size_t)r;
    size_t off = 0;
    for (;;) {
        cb_ws_frame f;
        int n = cb_ws_frame_parse(g_wsin + off, g_wsn - off, sizeof g_wsin, &f);
        if (n <= 0) break;
        const uint8_t *pl = g_wsin + off + f.header_len;
        g_n_frames++;
        if (f.opcode == CB_WS_OP_PING) ws_send(CB_WS_OP_PONG, pl, (size_t)f.len);
        else if (f.opcode == CB_WS_OP_TEXT || f.opcode == CB_WS_OP_BIN) {
            nc_stream_in(pl, (int)f.len, f.opcode == CB_WS_OP_TEXT, cb_now_ms());
            read_events();
        }
        off += (size_t)n;
    }
    memmove(g_wsin, g_wsin + off, g_wsn - off);
    g_wsn -= off;
}

static void wait_file(const char *path, char *out, size_t cap) {
    for (;;) {
        FILE *f = fopen(path, "r");
        if (f) {
            size_t n = out ? fread(out, 1, cap - 1, f) : 0;
            fclose(f);
            if (out) { out[n] = 0; char *nl = strchr(out, '\n'); if (nl) *nl = 0; }
            if (!out || n > 0) return;
        }
        if (out && g_deadline_ms >= 0 && mono_ms() >= g_deadline_ms) { out[0] = 0; return; }
        nap(20);
    }
}
static void write_file(const char *path, const char *text) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { perror(tmp); exit(1); }
    fputs(text, f);
    fclose(f);
    rename(tmp, path);
}

int main(int argc, char **argv) {
    if (argc != 10) { fprintf(stderr, "usage: p3client PORT NAME a|b RDVDIR THINK_MIN THINK_MAX SEED GAMES BOARDS\n"); return 2; }
    g_port = atoi(argv[1]); g_name = argv[2]; g_role_a = argv[3][0] == 'a'; g_rdv = argv[4];
    g_think_min = atoi(argv[5]); g_think_max = atoi(argv[6]); g_rng = strtoull(argv[7], NULL, 10) | 1;
    g_games = atoi(argv[8]); g_boards = atoi(argv[9]);
    g_spectator = argv[3][0] == 's';
    if (getenv("P3SEATS")) g_seats = atoi(getenv("P3SEATS"));
    { static char ids[800]; const char *e = getenv("P3BOTIDS");
      if (e) { snprintf(ids, sizeof ids, "%s", e); for (char *p = strtok(ids, ","); p && g_nbotids < 8; p = strtok(NULL, ",")) g_botids[g_nbotids++] = p; } }
    signal(SIGPIPE, SIG_IGN);
    { const char *os = getenv("P3ORIGIN"); const char *va = getenv("P3VIEWER");
      int e = 0; if (os && *os) e += snprintf(g_edge + e, sizeof g_edge - (size_t)e, "X-Cbase-Origin: %s\r\n", os);
      if (va && *va) snprintf(g_edge + e, sizeof g_edge - (size_t)e, "CloudFront-Viewer-Address: %s\r\n", va); }
    { char ep[700]; snprintf(ep, sizeof ep, "%s/ev.%s", argv[4], argv[2]); g_ev = fopen(ep, "w"); }
    nc_reset();
    nc_set_boards(g_boards);

    // Sign up through the pump.
    NcIntent in;
    memset(&in, 0, sizeof in);
    in.kind = NC_DO_SIGN_UP;
    in.name_len = (uint8_t)strlen(g_name); memcpy(in.name, g_name, in.name_len);
    in.password_len = 14; memcpy(in.password, "p3prof-pass-12", 14);
    nc_do(&in, cb_now_ms());
    pump();
    if (!nc_session_info()->signed_in) { fprintf(stderr, "%s: sign-up failed why %d status %d code %d\n", g_name, nc_error()->why, nc_error()->status, nc_error()->code); return 1; }
    stream_open();

    char path[600];
    snprintf(path, sizeof path, "%s/ready.%s", g_rdv, g_name);
    write_file(path, "1\n");
    snprintf(path, sizeof path, "%s/go", g_rdv);
    wait_file(path, NULL, 0);

    int64_t t0 = mono_ms();
    if (getenv("P3DURATION_MS") && atoll(getenv("P3DURATION_MS")) > 0) g_deadline_ms = t0 + atoll(getenv("P3DURATION_MS"));
    long cpu_moves_total = 0;
    for (int k = 0; k < g_games; k++) {
        g_have = g_game_over = g_ready_sent = g_joined = g_play_pending = 0;
        g_think_due = -1;
        g_played_v = 0;
        char prev[NW_GID_MAX + 1];
        snprintf(prev, sizeof prev, "%s", g_gid);
        if (prev[0]) nc_stream_unsub(NW_SUB_GAME, prev, (int)strlen(prev));
        snprintf(path, sizeof path, "%s/gid.%d", g_rdv, k);
        g_creator = g_spectator ? 0 : getenv("P3BOT") ? 1 : g_seats > 2 ? g_role_a : (g_role_a ^ (k & 1));   // the two take turns creating (a create limit per account)
        if (g_creator) {
            g_gid[0] = 0;
            submit(NC_DO_CREATE, NULL, 0, -1);
            pump();
            if (!g_gid[0]) { fprintf(stderr, "%s: create gave no game\n", g_name); return 1; }
            nc_stream_sub(NW_SUB_GAME, g_gid, (int)strlen(g_gid));
            stream_flush();
            char line[64];
            snprintf(line, sizeof line, "%s\n", g_gid);
            write_file(path, line);
        } else {
            wait_file(path, g_gid, sizeof g_gid);
            if (!g_gid[0]) { fprintf(stderr, "%s: deadline waiting for game %d\n", g_name, k); break; }
            nc_gate_reset(g_gid, (int)strlen(g_gid));
            nc_stream_sub(NW_SUB_GAME, g_gid, (int)strlen(g_gid));
            stream_flush();
            if (g_spectator) submit(NC_DO_OPEN_GAME, NULL, 0, -1);
            else submit_ask(ASK_JOIN);
            pump();
        }
        decide();
        pump();
        int64_t tg = mono_ms(), last_change = tg;
        long seen_v = -1;
        while (!g_game_over) {
            if (g_deadline_ms >= 0 && mono_ms() >= g_deadline_ms) break;
            int64_t now = mono_ms();
            int due = nc_due_ms(cb_now_ms());
            int to = due < 0 ? 1000 : due;
            if (g_think_due >= 0) { int t = (int)(g_think_due - now); if (t < 0) t = 0; if (t < to) to = t; }
            if (to > 1000) to = 1000;
            struct pollfd p = { g_ws, POLLIN, 0 };
            int r = poll(&p, 1, to);
            if (r > 0) stream_read();
            if (g_think_due >= 0 && mono_ms() >= g_think_due) {
                g_think_due = -1;
                uint8_t w[64];
                int n = legal_wire(w, sizeof w);
                if (n > 0) {
                    g_play_pending = 1;
                    g_played_v = g_c.view.version;
                    if (getenv("P3DEBUG")) fprintf(stderr, "%s PLAY type %d at v %u\n", g_name, g_last_type, g_c.view.version);
                    g_play_t0_us = mono_us(); g_play_v = g_c.view.version;
                    if (submit(NC_DO_PLAY, w, n, g_c.view.version)) g_n_moves++;
                    else if (getenv("P3FIX")) { g_play_pending = 0; g_played_v = 0; g_n_refused_at_once++; }
                }
            }
            pump();
            decide();
            pump();
            if (g_have && (long)g_c.view.version != seen_v) { seen_v = g_c.view.version; last_change = mono_ms(); if (getenv("P3BOT") && g_ev) { fprintf(g_ev, "V %u %lld turn_me=%d\n", g_c.view.version, (long long)mono_us(), g_think_due >= 0); fflush(g_ev); } }
            if (mono_ms() - last_change > 60000) { fprintf(stderr, "%s: STALL at version %ld\n", g_name, seen_v); if (g_ev) fprintf(g_ev, "S %s %ld\n", g_gid, seen_v); break; }
        }
        if (!g_game_over) { fprintf(stderr, "%s: deadline in game %d at v%u\n", g_name, k, g_c.view.version); break; }
        fprintf(stderr, "%s: game %d over at v%u in %lld ms (fool seat %d, my seat %d)\n", g_name, k, g_c.view.version,
                (long long)(mono_ms() - tg), g_c.view.fool, g_c.view.my_seat);
        cpu_moves_total += g_c.view.version;
        if (!g_role_a && k + 1 < g_games) {}
    }
    int64_t wall = mono_ms() - t0;
    nap(1500); pump();
    fprintf(stderr, "%s: refused_at_once=%ld; pump after 1.5 s idle:\n", g_name, g_n_refused_at_once);
    
    printf("%s: games=%d wall_ms=%lld my_moves=%ld refused=%ld refetch=%ld http=%ld frames=%ld casts=%ld views=%ld moves_per_s=%.2f\n", g_name, g_games,
           (long long)wall, g_n_moves, g_n_refused, g_n_refetch, g_n_http, g_n_frames, g_n_casts, g_n_views, wall > 0 ? g_n_moves * 1000.0 / (double)wall : 0);
    qsort(g_lat, (size_t)g_nlat, sizeof *g_lat, dcmp);
    long whys = 0; for (int i = 0; i < 256; i++) whys += g_n_why[i];
    printf("%s: lat_n=%ld p50_ms=%.2f p95_ms=%.2f p99_ms=%.2f max_ms=%.2f http_err=%ld refused_why=%ld\n", g_name, g_nlat, pct(0.5), pct(0.95), pct(0.99), g_nlat ? g_lat[g_nlat-1] : 0, g_n_http_err, whys);
    if (g_ev) fflush(g_ev);
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("%s: cpu_user_s=%.3f cpu_sys_s=%.3f (whole process, sign-up included)\n", g_name,
           ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6, ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);
    fflush(stdout);
    snprintf(path, sizeof path, "%s/done.%s", g_rdv, g_name);
    write_file(path, "1\n");
    snprintf(path, sizeof path, "%s/quit", g_rdv);
    wait_file(path, NULL, 0);
    (void)cpu_moves_total;
    return 0;
}
