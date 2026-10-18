// roundtrip.c - `cbase-bench roundtrip`: a real game, measured as a client sees it.
//
// Each worker (--concurrency of them, each its own pair of players and its own
// connections) signs up two users, opens one MQTT stream socket per player
// (/mqtt, which `cbase serve` and IoT Core behind the edge both serve)
// subscribed to that player's views (u/<uid>/v), and plays two-human
// games: create, join, both ready, then moves until the game ends, and a new
// game after that. Per move it measures:
//
//   view       GET /v1/games/<id> for the mover (the client's read of its
//              envelope; the move is chosen on it with the kernel's client
//              table and legal moves, as the browser does)
//   action     POST /v1/action, request out to response in
//   push_view  the OPPONENT's view push for the new version: its arrival time
//              on the opponent's socket minus the time the action's response
//              arrived. A reader thread per socket stamps each frame as it
//              lands, so a push that beats the response is a negative number,
//              not a lost one.
//
// Every body is the kernel's (netwire.h, table.h, ask.h), through its pure
// codecs: the client library (netclient.h) keeps one session in statics, and
// the workers are threads. Setup (sign-up, create, join, ready) is not timed.
// Bytes per request are the HTTP heads and bodies (net.h).
#include "bench.h"
#include "net.h"

#include "ask.h"
#include "awire.h"
#include "client_table.h"
#include "legal.h"
#include "table.h"
#include "net/ws.h"
#include "netclient.h"   // NC_PING_MS: the browser client's keep-alive, one owner
#include "netwire.h"
#include "mqtt/mqtt.h"
#include "util/uuid.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../../shared/c/rng.h"

#define TIMEOUT_MS 15000
#define PUSH_WAIT_MS 5000
#define RING 1024

// ---- one stream socket and its reader thread ---------------------------------------

typedef struct {
    bn_mq           mq;
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
    cb_buf b;
    cb_buf_init(&b);
    NwFrame f;
    MqttPacket p;
    int64_t last_ping = bn_now_us();
    while (!atomic_load(&l->stop)) {
        // The clients' keep-alive: a PINGREQ every 30 s inside the 60 s CONNECT asked for.
        if (bn_now_us() - last_ping > (int64_t)NC_PING_MS * 1000) {
            bn_mq_ping(&l->mq);
            last_ping = bn_now_us();
        }
        int t = bn_mq_next(&l->mq, 200, &p, &b);
        if (t < 0) break;
        if (t != MQTT_PUBLISH) continue;
        int64_t ts = bn_now_us();
        if (nw_frame_decode(p.payload, p.payload_len, &f) != NW_OK || f.kind != NW_WS_VIEW || !f.blob.len) continue;
        int64_t t_us = ts;
        int64_t v = f.version;
        pthread_mutex_lock(&l->mu);
        l->t_us[l->count % RING] = t_us;
        l->ver[l->count % RING] = v;
        l->count++;
        pthread_cond_broadcast(&l->cv);
        pthread_mutex_unlock(&l->mu);
    }
    cb_buf_free(&b);
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
    char token[NW_ACCESS_MAX + 1];
    char uid[NW_ID_MAX + 1];
    char auth[NW_ACCESS_MAX + 64];   // the header every call carries
} player;

typedef struct {
    const bench_opts *o;
    bn_url    u;
    bn_http   http;
    player    p[2];
    listener  l[2];
    char      gid[32];
    bool      have_game;
    uint64_t  rng;
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
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNUP;
    q.name_len = (uint8_t)snprintf(q.name, sizeof q.name, "BENCH%02X%02X%02X%02X%d", rnd[0], rnd[1], rnd[2], rnd[3], k);
    q.password_len = (uint8_t)snprintf(q.password, sizeof q.password, "bench-pass-123");
    uint8_t body[256];
    int n = nw_request_encode(&q, body, sizeof body);
    bn_resp r;
    bn_resp_init(&r);
    int s = n > 0 ? call(w, "POST", "/v1/auth/signup", "Content-Type: application/octet-stream\r\n", body, (size_t)n, &r) : -1;
    NwSession ses;
    bool ok = s == 200 && nw_session_decode(r.body.p, (int)r.body.len, &ses) == NW_OK;
    if (ok) {
        snprintf(p->token, sizeof p->token, "%.*s", (int)ses.access_len, ses.access);
        snprintf(p->uid, sizeof p->uid, "%.*s", (int)ses.user_id_len, ses.user_id);
        snprintf(p->auth, sizeof p->auth, "Authorization: Bearer %s\r\n", p->token);
    } else {
        fprintf(stderr, "bench: signup answered %d\n", s);
    }
    bn_resp_free(&r);
    return ok ? 0 : -1;
}

// Subscribes to the player's views as the browser's client does (netclient.c):
// CONNECT with the token, then the SUBSCRIBE of u/<uid>/v.
static bool listen_games(worker *w, int k) {
    listener *l = &w->l[k];
    cb_buf_init(&l->mq.in);
    l->mq.ws = bn_mq_open(&w->u, NULL, TIMEOUT_MS);
    if (!l->mq.ws) return false;
    if (bn_mq_connect(&l->mq, w->p[k].uid, w->p[k].token, TIMEOUT_MS) != 1) {
        fprintf(stderr, "bench: the stream's CONNECT was refused\n");
        return false;
    }
    char topic[NW_ID_MAX + 8];
    snprintf(topic, sizeof topic, "u/%s/v", w->p[k].uid);
    if (bn_mq_subscribe(&l->mq, 1, topic, TIMEOUT_MS) != 1) {
        fprintf(stderr, "bench: the views subscription was refused\n");
        return false;
    }
    pthread_mutex_init(&l->mu, NULL);
    pthread_cond_init(&l->cv, NULL);
    atomic_store(&l->stop, false);
    l->running = pthread_create(&l->th, NULL, reader, l) == 0;
    return l->running;
}

static int fn(worker *w, int k, const char *name, const void *body, size_t n, bn_resp *r) {
    char path[64], headers[2800];
    snprintf(path, sizeof path, "/v1/%s", name);
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
    int start = w->moves.n ? (int)(rng_u32(&w->rng) % (uint32_t)w->moves.n) : 0;
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

// The mover's envelope through GET /v1/games/<id>, timed. false on a failure.
static bool read_view(worker *w, int k, cb_buf *env) {
    char path[64];
    snprintf(path, sizeof path, "/v1/games/%s", w->gid);
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
    uint32_t version;
    NwBlob e;
    ok = ok && nw_view_decode(r.body.p, (int)r.body.len, &version, &e) == NW_OK && e.len;
    if (ok) {
        cb_buf_reset(env);
        cb_buf_append(env, e.p, e.len);
    }
    if (!ok) fprintf(stderr, "bench: the view answered %d\n", s);
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
    // --gap: a player thinks between the board arriving and the move, and the
    // hops in front of the Lambda close idle connections meanwhile.
    if (w->o->gap_ms > 0) usleep((useconds_t)w->o->gap_ms * 1000u);
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
    if (signup(w, &w->p[0], 0) || signup(w, &w->p[1], 1) || !listen_games(w, 0) || !listen_games(w, 1)) {
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
    bn_mq_close(&l->mq);
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
        // Every move claims one slot, so a worker's moves fit o->n; view
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
    snprintf(note, sizeof note, "concurrency=%d gap_ms=%d games=%d rejected=%d lost_push=%d errors=%d wall_s=%lld%s%s", nw, o->gap_ms, games,
             rejected, lost, errors, (long long)(wall / 1000000), o->note && *o->note ? " " : "", o->note ? o->note : "");
    bench_note_load(note, sizeof note);
    bs_run run;
    bs_run_init(&run, "roundtrip", date, host, o->url, note);
    bs_run_lat(&run, "action", act, na, na ? as / na : 0, na ? ar / na : 0);
    bs_run_lat(&run, "view", pv, npv, npv ? ps / npv : 0, npv ? pr / npv : 0);
    bs_run_lat(&run, "push_view", push, npush, 0, 0);
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
