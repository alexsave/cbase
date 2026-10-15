// roundtrip.c - `cbase-bench roundtrip`: a real game, measured as a client sees it.
//
// Each worker (--concurrency of them, each its own pair of players and its own
// connections) signs up two users, opens one Realtime socket per player
// (Phoenix vsn 2.0.0, as realtime-js) joined to that player's pv- topic, and
// plays two-human games: create, join, both ready, then moves until the game
// ends, and a new game after that. Per move it measures:
//
//   player_views  GET /rest/v1/player_views for the mover (the client's read
//                 of its envelope; the move is chosen on it with the kernel's
//                 client table and legal moves, as the browser does)
//   action        POST /functions/v1/action, request out to response in
//   push_pv       the OPPONENT's pv- postgres_changes for the new version:
//                 its arrival time on the opponent's socket minus the time
//                 the action's response arrived. A reader thread per socket
//                 stamps each frame as it lands, so a push that beats the
//                 response is a negative number, not a lost one.
//
// Setup (sign-up, create, join, ready) is not timed. Bytes per request are
// the HTTP heads and bodies (net.h).
#include "bench.h"
#include "net.h"

#include "ask.h"
#include "awire.h"
#include "client_table.h"
#include "legal.h"
#include "table.h"
#include "json_surface.h"
#include "util/hex.h"
#include "util/uuid.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIMEOUT_MS 15000
#define PUSH_WAIT_MS 5000
#define RING 1024

// ---- one realtime socket and its reader thread ---------------------------------------

typedef struct {
    bn_ws          *ws;
    pthread_t       th;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int64_t         t_us[RING];
    int64_t         ver[RING];
    long            count;
    atomic_bool     stop;
    bool            running;
} listener;

static void *reader(void *arg) {
    listener *l = arg;
    cb_buf f;
    cb_buf_init(&f);
    int64_t last_hb = bn_now_us();
    while (!atomic_load(&l->stop)) {
        // realtime-js's heartbeat, so the server's idle sweep keeps the socket.
        if (bn_now_us() - last_hb > 25000000) {
            size_t hn;
            const char *hb = cb_js_client_heartbeat(&hn);
            bn_ws_send_text(l->ws, hb, hn);
            last_hb = bn_now_us();
        }
        int op = bn_ws_recv(l->ws, 200, &f);
        if (op == -2) break;
        if (op < 0) continue;
        int64_t t = bn_now_us();
        int64_t v;
        if (!cb_js_client_pv_push_version(cb_buf_cstr(&f), f.len, &v)) continue;
        pthread_mutex_lock(&l->mu);
        l->t_us[l->count % RING] = t;
        l->ver[l->count % RING] = v;
        l->count++;
        pthread_cond_broadcast(&l->cv);
        pthread_mutex_unlock(&l->mu);
    }
    cb_buf_free(&f);
    return NULL;
}

// The arrival time of the push for `version`, waiting up to timeout_ms. false: none.
static bool wait_version(listener *l, int64_t version, int timeout_ms, int64_t *t_us) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += timeout_ms / 1000;
    dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    bool found = false;
    pthread_mutex_lock(&l->mu);
    for (;;) {
        long lo = l->count > RING ? l->count - RING : 0;
        for (long i = l->count - 1; i >= lo && !found; i--)
            if (l->ver[i % RING] == version) { *t_us = l->t_us[i % RING]; found = true; }
        if (found || pthread_cond_timedwait(&l->cv, &l->mu, &dl) != 0) break;
    }
    pthread_mutex_unlock(&l->mu);
    return found;
}

// ---- a player ------------------------------------------------------------------------

typedef struct {
    char id[64];
    char token[2048];
    char auth[2600];   // the headers every call carries
} player;

typedef struct {
    const bench_opts *o;
    bn_url    u;
    bn_http   http;
    player    p[2];
    listener  l[2];
    char      gid[32];
    bool      have_game;
    uint32_t  rng;
    ClientSlot  slot;
    ClientTable ct;
    LegalMoves  moves;
    // samples
    int64_t  *act, *pv, *push;
    int       n_act, n_pv, n_push, cap, pv_cap;
    int64_t   act_sent, act_recv, pv_sent, pv_recv;
    int       rejected, lost_push, games, errors;
} worker;

static atomic_int g_claimed;

static int call(worker *w, const char *method, const char *path, const char *headers, const void *body, size_t n,
                bn_resp *r) {
    if (bn_http_request(&w->http, method, path, headers, body, n, TIMEOUT_MS, r) != 0) return -1;
    return r->status;
}

static int signup(worker *w, player *p, int k) {
    uint8_t rnd[4];
    cb_random_bytes(rnd, sizeof rnd);
    char name[32], email[64];
    snprintf(name, sizeof name, "bench%02x%02x%02x%02x%d", rnd[0], rnd[1], rnd[2], rnd[3], k);
    snprintf(email, sizeof email, "%s@bench.invalid", name);
    cb_buf body;
    cb_buf_init(&body);
    cb_js_client_signup_body(&body, email, "bench-pass-123", name);
    char headers[512];
    snprintf(headers, sizeof headers, "apikey: %s\r\nContent-Type: application/json\r\n", w->o->anon);
    bn_resp r;
    bn_resp_init(&r);
    int s = call(w, "POST", "/auth/v1/signup", headers, cb_buf_cstr(&body), body.len, &r);
    cb_buf_free(&body);
    const char *j = cb_buf_cstr(&r.body);
    bool ok = s == 200 && cb_js_client_session(j, r.body.len, p->token, sizeof p->token, p->id, sizeof p->id);
    if (!ok) fprintf(stderr, "bench: signup answered %d: %.300s\n", s, j);
    bn_resp_free(&r);
    snprintf(p->auth, sizeof p->auth, "Authorization: Bearer %s\r\napikey: %s\r\n", p->token, w->o->anon);
    return ok ? 0 : -1;
}

static bool listen_pv(worker *w, int k) {
    listener *l = &w->l[k];
    char path[512];
    snprintf(path, sizeof path, "/realtime/v1/websocket?apikey=%s&vsn=2.0.0", w->o->anon);
    l->ws = bn_ws_open(&w->u, path, TIMEOUT_MS);
    if (!l->ws) return false;
    cb_buf f;
    cb_buf_init(&f);
    cb_js_client_pv_join(&f, w->p[k].id, w->p[k].token);
    if (bn_ws_send_text(l->ws, cb_buf_cstr(&f), f.len) != 0) { cb_buf_free(&f); return false; }
    bool ok = false;
    for (int i = 0; i < 20; i++) {
        if (bn_ws_recv(l->ws, TIMEOUT_MS, &f) < 0) break;
        int st = cb_js_client_reply_status(cb_buf_cstr(&f));
        if (st >= 0) { ok = st == 1; break; }
    }
    cb_buf_free(&f);
    if (!ok) { fprintf(stderr, "bench: the pv- join was refused\n"); return false; }
    pthread_mutex_init(&l->mu, NULL);
    pthread_cond_init(&l->cv, NULL);
    atomic_store(&l->stop, false);
    l->running = pthread_create(&l->th, NULL, reader, l) == 0;
    return l->running;
}

static int fn(worker *w, int k, const char *name, const void *body, size_t n, bn_resp *r) {
    char path[64], headers[2800];
    snprintf(path, sizeof path, "/functions/v1/%s", name);
    snprintf(headers, sizeof headers, "%sContent-Type: application/octet-stream\r\n", w->p[k].auth);
    return call(w, "POST", path, headers, body, n, r);
}

static int ask(worker *w, int k, int kind, bn_resp *r) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = (uint8_t)kind;
    a.gid_len = (uint8_t)strlen(w->gid);
    memcpy(a.gid, w->gid, a.gid_len);
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    return n > 0 ? fn(w, k, "meta", body, (size_t)n, r) : -1;
}

static bool adopt(worker *w, const uint8_t *env, size_t n) {
    client_init(&w->ct, &w->slot);
    return client_adopt_envelope(&w->ct, env, (int)n) >= 0;
}

// A fresh two-human game: A creates, B joins, both ready. false on any refusal.
static bool new_game(worker *w) {
    bn_resp r;
    bn_resp_init(&r);
    bool ok = fn(w, 0, "create", NULL, 0, &r) == 200 && adopt(w, r.body.p, r.body.len);
    if (ok) snprintf(w->gid, sizeof w->gid, "%.*s", (int)w->ct.gid_len, w->ct.gid);
    ok = ok && ask(w, 1, ASK_JOIN, &r) == 200 && ask(w, 0, ASK_START, &r) == 200 && ask(w, 1, ASK_START, &r) == 200 &&
         adopt(w, r.body.p, r.body.len) && w->ct.view.status == GAME_STATUS_PLAYING;
    if (!ok) fprintf(stderr, "bench: a new game failed (%d): %.200s\n", r.status, cb_buf_cstr(&r.body));
    bn_resp_free(&r);
    w->have_game = ok;
    if (ok) w->games++;
    return ok;
}

// A legal move for the adopted envelope's viewer, as the browser offers one.
// Its wire length, or 0 when the viewer has none.
static int choose_move(worker *w, uint8_t *wire, int cap) {
    int seat = w->ct.view.my_seat;
    if (seat < 0 || w->ct.view.status != GAME_STATUS_PLAYING) return 0;
    calculate_legal_moves(w->ct.g, seat, &w->moves);
    int start = w->moves.n ? (int)((w->rng = w->rng * 1103515245u + 12345u) >> 8) % w->moves.n : 0;
    for (int k = 0; k < w->moves.n; k++) {
        const LegalMove *m = &w->moves.moves[(start + k) % w->moves.n];
        if (m->type == MOVE_WAIT) continue;
        AwireAction a;
        memset(&a, 0, sizeof a);
        a.kind = m->type;
        a.n = m->n_cards;
        for (int i = 0; i < m->n_cards; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
        int n = awire_encode(&a, wire, cap);
        if (n > 0 && client_validate(&w->ct, &w->ct.view, wire, n) == 0) return n;
    }
    return 0;
}

// The mover's envelope through player_views, timed. false on a failure.
static bool read_view(worker *w, int k, cb_buf *env) {
    char path[512];
    snprintf(path, sizeof path, "/rest/v1/player_views?select=view,version&game_id=eq.%s&player_id=eq.%s", w->gid,
             w->p[k].id);
    bn_resp r;
    bn_resp_init(&r);
    int64_t t0 = bn_now_us();
    int s = call(w, "GET", path, w->p[k].auth, NULL, 0, &r);
    int64_t dt = bn_now_us() - t0;
    bool ok = s == 200;
    if (ok && w->n_pv < w->pv_cap) {
        w->pv[w->n_pv++] = dt;
        w->pv_sent += (int64_t)r.sent;
        w->pv_recv += (int64_t)r.recv;
    }
    static _Thread_local char hex[1 << 17];
    ok = ok && cb_js_client_first_view(cb_buf_cstr(&r.body), r.body.len, hex, sizeof hex) &&
         strncmp(hex, "\\x", 2) == 0;
    if (ok) {
        size_t hl = strlen(hex + 2);
        cb_buf_reset(env);
        ok = cb_buf_reserve(env, hl / 2 + 1);
        int n = ok ? cb_hex_decode(hex + 2, hl, env->p, hl / 2 + 1) : -1;
        ok = n >= 0;
        if (ok) env->len = (size_t)n;
    }
    if (!ok) fprintf(stderr, "bench: player_views answered %d: %.200s\n", s, cb_buf_cstr(&r.body));
    bn_resp_free(&r);
    return ok;
}

// One move by player k, timed, with the opponent's push. 1 moved, 0 nothing to
// do, -1 a failure.
static int move(worker *w, int k, cb_buf *env) {
    if (!read_view(w, k, env)) return -1;
    if (!adopt(w, env->p, env->len)) return -1;
    if (w->ct.view.status == GAME_STATUS_GAME_OVER) { w->have_game = false; return 0; }
    uint8_t wire[64], body[128];
    int n = choose_move(w, wire, sizeof wire);
    if (n <= 0) return 0;
    if (atomic_fetch_add(&g_claimed, 1) >= w->o->n) return 0;
    size_t gl = strlen(w->gid);
    memcpy(body, w->gid, gl);
    memcpy(body + gl, wire, (size_t)n);
    int bn = table_request_encode(body, sizeof body, (int)gl, n, true, w->ct.view.version);
    bn_resp r;
    bn_resp_init(&r);
    int64_t t0 = bn_now_us();
    int s = fn(w, k, "action", body, (size_t)bn, &r);
    int64_t t1 = bn_now_us();
    TableResponse tr;
    bool ok = s == 200 && table_response_decode(r.body.p, (int)r.body.len, &tr) == 0;
    if (ok) {
        w->act[w->n_act++] = t1 - t0;
        w->act_sent += (int64_t)r.sent;
        w->act_recv += (int64_t)r.recv;
        if (tr.status != TABLE_STATUS_APPLIED) w->rejected++;
        else {
            int64_t t_push;
            if (wait_version(&w->l[1 - k], tr.version, PUSH_WAIT_MS, &t_push)) w->push[w->n_push++] = t_push - t1;
            else w->lost_push++;
        }
    } else {
        fprintf(stderr, "bench: action answered %d\n", s);
    }
    bn_resp_free(&r);
    return ok ? 1 : -1;
}

static void *work(void *arg) {
    worker *w = arg;
    bn_http_init(&w->http, &w->u);
    if (signup(w, &w->p[0], 0) || signup(w, &w->p[1], 1) || !listen_pv(w, 0) || !listen_pv(w, 1)) {
        w->errors++;
        return NULL;
    }
    cb_buf env;
    cb_buf_init(&env);
    int idle = 0;
    while (atomic_load(&g_claimed) < w->o->n && w->errors < 10) {
        if (!w->have_game && !new_game(w)) { w->errors++; continue; }
        bool moved = false;
        for (int k = 0; k < 2 && w->have_game; k++) {
            int m = move(w, k, &env);
            if (m < 0) w->errors++;
            if (m > 0) moved = true;
        }
        // Neither player has a move: a stuck table is abandoned for a new one.
        idle = moved ? 0 : idle + 1;
        if (idle > 20) { w->have_game = false; idle = 0; }
    }
    cb_buf_free(&env);
    return NULL;
}

static void stop_listener(listener *l) {
    if (l->running) {
        atomic_store(&l->stop, true);
        pthread_join(l->th, NULL);
        pthread_mutex_destroy(&l->mu);
        pthread_cond_destroy(&l->cv);
    }
    bn_ws_close(l->ws);
}

int bench_roundtrip(const bench_opts *o) {
    bn_url u;
    if (!bn_url_parse(o->url, &u)) { fprintf(stderr, "bench: --url must be http(s)://host[:port]\n"); return 2; }
    atomic_store(&g_claimed, 0);
    int nw = o->concurrency;
    worker **ws = calloc((size_t)nw, sizeof *ws);
    pthread_t *th = calloc((size_t)nw, sizeof *th);
    for (int i = 0; i < nw; i++) {
        worker *w = ws[i] = calloc(1, sizeof *w);
        w->o = o;
        w->u = u;
        w->rng = 7u + (uint32_t)i * 7919u;
        // Every move claims one slot, so a worker's moves fit o->n; player_views
        // reads outnumber moves (a player with nothing to do still reads).
        w->cap = o->n + 1;
        w->pv_cap = 4 * o->n + 4;
        w->act = calloc((size_t)w->cap, sizeof(int64_t));
        w->push = calloc((size_t)w->cap, sizeof(int64_t));
        w->pv = calloc((size_t)w->pv_cap, sizeof(int64_t));
    }
    int64_t t0 = bn_now_us();
    for (int i = 0; i < nw; i++) pthread_create(&th[i], NULL, work, ws[i]);
    for (int i = 0; i < nw; i++) pthread_join(th[i], NULL);
    int64_t wall = bn_now_us() - t0;

    // Merge.
    size_t total = (size_t)nw * (4 * (size_t)o->n + 4);
    int64_t *act = calloc(total, sizeof(int64_t)), *pv = calloc(total, sizeof(int64_t)),
            *push = calloc(total, sizeof(int64_t));
    int na = 0, npv = 0, npush = 0, rejected = 0, lost = 0, games = 0, errors = 0;
    int64_t as = 0, ar = 0, ps = 0, pr = 0;
    for (int i = 0; i < nw; i++) {
        worker *w = ws[i];
        memcpy(act + na, w->act, (size_t)w->n_act * sizeof(int64_t));
        memcpy(pv + npv, w->pv, (size_t)w->n_pv * sizeof(int64_t));
        memcpy(push + npush, w->push, (size_t)w->n_push * sizeof(int64_t));
        na += w->n_act;
        npv += w->n_pv;
        npush += w->n_push;
        as += w->act_sent;
        ar += w->act_recv;
        ps += w->pv_sent;
        pr += w->pv_recv;
        rejected += w->rejected;
        lost += w->lost_push;
        games += w->games;
        errors += w->errors;
        stop_listener(&w->l[0]);
        stop_listener(&w->l[1]);
        bn_http_close(&w->http);
        free(w->act);
        free(w->pv);
        free(w->push);
        free(w);
    }
    free(ws);
    free(th);

    char date[32], host[64], note[256];
    bench_date(date, sizeof date);
    bench_host(host, sizeof host);
    snprintf(note, sizeof note, "concurrency=%d games=%d rejected=%d lost_push=%d errors=%d wall_s=%lld%s%s", nw, games,
             rejected, lost, errors, (long long)(wall / 1000000), o->note && *o->note ? " " : "", o->note ? o->note : "");
    bench_note_load(note, sizeof note);
    bs_run run;
    bs_run_init(&run, "roundtrip", date, host, o->url, note);
    bs_run_lat(&run, "action", act, na, na ? as / na : 0, na ? ar / na : 0);
    bs_run_lat(&run, "player_views", pv, npv, npv ? ps / npv : 0, npv ? pr / npv : 0);
    bs_run_lat(&run, "push_pv", push, npush, 0, 0);
    free(act);
    free(pv);
    free(push);
    int rc = bench_save(&run, o);
    if (na < o->n) {
        fprintf(stderr, "bench: only %d of %d moves were measured\n", na, o->n);
        return 1;
    }
    return rc == 0 && errors == 0 ? 0 : 1;
}
