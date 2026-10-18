// direct.c - `cbase-bench direct`: is a read faster straight from DynamoDB
// than through c_base (CloudFront -> Lambda -> DynamoDB)?
//
// The answer is in c_base/docs/DIRECT_DYNAMO.md. What this measures:
//
//   SETUP (not timed). A bench account is signed up through /v1, as the web
//   does it (the kernel's own client, netclient.h, builds every request and
//   reads every answer), creates a game, seats a bot and starts it, so the
//   game read below is a dealt game. At the end the account is deleted through
//   /v1/account/delete, which takes the game with it.
//
//   THROUGH c_base, warm (one kept-alive TLS connection, https.c's pool):
//     cbase_board   GET /v1/leaderboard
//     cbase_games   GET /v1/games         (the caller's dashboard)
//     cbase_game    GET /v1/games/<id>    (one game, masked for the caller)
//     cbase_history GET /v1/history       (the caller's finished games)
//     cbase_bots    GET /v1/bots
//
//   DIRECT, warm, signed with the credentials in the environment
//   (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_SESSION_TOKEN, AWS_REGION;
//   `aws configure export-credentials --format env`). The same calls the
//   server makes for each read, built by json_surface.c's item-layer writers and
//   sent by aws_call.c, one after another as store_dynamo.c sends them:
//     direct_board  Query GSI2 RATINGS + Query GSI2 BOTS, every page
//     direct_games  Query GSI1 USERGAMES#<user>, then GetItem GAME#<id> ROW each
//     direct_game   GetItem GAME#<id> ROW
//     direct_history Query GSI2 RATINGS (the caller's own rating, as h_history
//                   finds it) + Query GSI2 PLAYER#<user>, every page
//     direct_bots   Query GSI2 BOTS
//   Every one is a read: no write is ever signed here. Without credentials the
//   direct rows are left out and the run says so.
//
//   THE REFUSAL, always: the direct_game request sent UNSIGNED, which DynamoDB
//   refuses (400, no authentication). It was meant as the network floor, but
//   measured slower than the signed read (16.4 against 13.5 ms): the refusal
//   path costs time of its own, so it is a rough bound, not a floor.
//
//   LOAD: every round reads every RATINGS row four times (/v1/leaderboard and
//   /v1/history, and their direct twins), about 4.5 read units each on gsi2,
//   which is provisioned at 5. At n=200 it outran DynamoDB's burst credit and
//   throttled the live index (docs/DIRECT_DYNAMO.md); keep n small, or space
//   the runs, until those reads stop scanning the whole board.
//
//   THE EDGE: GET --edge (a cached static file on the same CDN), the latency a
//   leaderboard cached at CloudFront would have; the note counts the hits.
//
//   COLD (--burst K): K fresh connections at once to /health, so Lambda must
//   start new environments for some of them; a response whose uptime_s is 0 is
//   a cold one. Both rows are fresh connections (TLS setup included), so their
//   difference is the cold start.
//
// The rows are interleaved (one of each per round), so a drift in the network
// lands on every row alike.
#include "bench.h"
#include "net.h"

#include "aws/aws_call.h"
#include "aws/https.h"
#include "aws/sigv4.h"
#include "aws_json.h"
#include "store/rows_foolish.h"

#include "ask.h"
#include "netclient.h"
#include "roster.h"
#include "view.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIMEOUT_MS 15000
#define WARMUP     5

typedef struct {
    const char *name;
    int64_t    *v;
    int         n;
    int64_t     sent, recv;
} series;

static void add(series *s, int64_t us, size_t sent, size_t recv) {
    s->v[s->n++] = us;
    s->sent += (int64_t)sent;
    s->recv += (int64_t)recv;
}

static void row(bs_run *run, const series *s) {
    if (s->n) bs_run_lat(run, s->name, s->v, s->n, s->sent / s->n, s->recv / s->n);
}

// ---- through c_base: the kernel's client asks, this file carries ---------------------

static int64_t now_ms(void) { return (int64_t)time(NULL) * 1000; }

// Sends what netclient built. The answer is in *r. false on a transport
// failure, and for the session's refresh (netclient asked for it first): its
// answer is read here, and the caller builds its request again.
static bool carry(bn_http *h, const NcRequest *q, bn_resp *r) {
    char headers[NC_AUTH_MAX + 128];
    int k = 0;
    headers[0] = 0;
    if (q->auth_len) k = snprintf(headers, sizeof headers, "Authorization: %.*s\r\n", q->auth_len, q->auth);
    if (q->body_len) snprintf(headers + k, sizeof headers - (size_t)k, "Content-Type: application/octet-stream\r\n");
    char path[NC_PATH_MAX + 1];
    memcpy(path, q->path, q->path_len);
    path[q->path_len] = 0;
    if (bn_http_request(h, q->method == NC_POST ? "POST" : "GET", path, headers, q->body_len ? q->body : NULL,
                        q->body_len, TIMEOUT_MS, r) != 0)
        return false;
    if (!q->is_refresh) return true;
    nc_session_read(r->status, r->body.p, (int)r->body.len);
    return false;
}

// One /v1 request built by `build`, retried once when the session was refreshed.
typedef const NcRequest *(*build_fn)(const void *arg);

static bool ask(bn_http *h, build_fn build, const void *arg, bn_resp *r) {
    for (int i = 0; i < 2; i++) {
        const NcRequest *q = build(arg);
        if (!q) return false;
        bool was_refresh = q->is_refresh;
        if (carry(h, q, r)) return true;
        if (!was_refresh) return false;
    }
    return false;
}

static const NcRequest *b_board(const void *a) { (void)a; return nc_req_board(); }
static const NcRequest *b_games(const void *a) { (void)a; return nc_req_games(now_ms()); }
static const NcRequest *b_game(const void *a) { return nc_req_game(a, (int)strlen(a), now_ms()); }
static const NcRequest *b_history(const void *a) { (void)a; return nc_req_history(now_ms()); }
static const NcRequest *b_bots(const void *a) { (void)a; return nc_req_bots(now_ms()); }
static const NcRequest *b_create(const void *a) { (void)a; return nc_req_create(now_ms()); }
static const NcRequest *b_delete(const void *a) { (void)a; return nc_req_delete_account(now_ms()); }
static const NcRequest *b_meta(const void *a) {
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(a, body, sizeof body);
    return n < 0 ? NULL : nc_req_meta(body, n, now_ms());
}

// The game id an envelope's roster trailer names, into gid (CB_GAME_ID_MAX + 1).
static bool gid_of(const uint8_t *env, int len, char *gid) {
    EnvHeader eh;
    if (!env_header_read(env, len, &eh) || eh.trailer_at >= len) return false;
    static Roster ro;
    char id[ROSTER_GAME_ID_MAX + 1];   // what the trailer reader may write
    int gl = 0, status, passing, used;
    uint32_t ai;
    if (roster_trailer_read(&ro, id, &gl, &status, &ai, &passing, env + eh.trailer_at, len - eh.trailer_at, &used) != ROSTER_OK ||
        gl <= 0 || gl > CB_GAME_ID_MAX)
        return false;
    memcpy(gid, id, (size_t)gl);
    gid[gl] = 0;
    return true;
}

static bool meta(bn_http *h, int kind, const char *gid, bn_resp *r) {
    static TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = (uint8_t)kind;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    return ask(h, b_meta, &a, r) && nc_status_read(r->status, r->body.p, (int)r->body.len) == 0;
}

// Signs up, creates a game, seats a bot and starts it. false says why on stderr.
static bool setup(bn_http *h, char *gid, char *user) {
    bn_resp r;
    bn_resp_init(&r);
    unsigned x = (unsigned)arc4random();
    char name[32], pw[32];
    snprintf(name, sizeof name, "benchd%08x", x);
    snprintf(pw, sizeof pw, "bd-%08x%08x", (unsigned)arc4random(), x);
    nc_reset();
    const NcRequest *q = nc_req_signup(name, (int)strlen(name), pw, (int)strlen(pw));
    bool ok = q && carry(h, q, &r) && nc_session_read(r.status, r.body.p, (int)r.body.len) == NW_OK;
    if (!ok) fprintf(stderr, "bench: signup answered %d\n", r.status);
    if (ok) {
        const NcSessionInfo *s = nc_session_info();
        memcpy(user, s->user_id, s->user_id_len);
        user[s->user_id_len] = 0;
        ok = ask(h, b_create, NULL, &r) && nc_status_read(r.status, r.body.p, (int)r.body.len) == 0 &&
             gid_of(r.body.p, (int)r.body.len, gid);
        if (!ok) fprintf(stderr, "bench: create answered %d\n", r.status);
    }
    if (ok && !meta(h, ASK_ADD_BOT, gid, &r)) { fprintf(stderr, "bench: add bot answered %d\n", r.status); ok = false; }
    if (ok && !meta(h, ASK_START, gid, &r)) { fprintf(stderr, "bench: start answered %d\n", r.status); ok = false; }
    if (ok) fprintf(stderr, "bench: account %s, game %s dealt\n", name, gid);
    bn_resp_free(&r);
    return ok;
}

// ---- direct: the server's own DynamoDB calls, signed here ----------------------------

typedef struct {
    char   host[128];
    char   table[256];
    size_t sent, recv;
} direct;

// One signed request; its body in *resp. false unless DynamoDB answered 200.
static bool d_send(direct *d, const dw_req *q, cb_buf *resp) {
    char ct[96], tg[96];
    snprintf(ct, sizeof ct, "Content-Type: %s", dw_content_type());
    snprintf(tg, sizeof tg, "X-Amz-Target: %s", q->target);
    const char *const hs[2] = { ct, tg };
    cb_https_response r;
    int rc = cb_aws_call(dw_service(), "POST", d->host, 443, true, "/", hs, 2, q->body.p, q->body.len, TIMEOUT_MS, &r);
    if (rc != CB_OK) return false;
    d->sent += q->body.len;
    d->recv += r.body.len;
    cb_buf_reset(resp);
    cb_buf_append(resp, r.body.p, r.body.len);
    bool ok = r.status == 200;
    if (!ok) {
        char why[320];
        dw_error_text(cb_buf_cstr(resp), resp->len, why, sizeof why);
        fprintf(stderr, "bench: %s answered %d %s\n", q->target, r.status, why);
    }
    cb_https_response_free(&r);
    return ok;
}

typedef bool (*item_fn)(cb_span it, void *ud);

// The queries the server makes for each read (store/rows_foolish.c).
#define Q_RATINGS ((cb_iquery){ .index = 2, .pk = "RATINGS", .forward = true })
#define Q_BOTS    ((cb_iquery){ .index = 2, .pk = "BOTS", .forward = true })
#define Q_PLAYER  ((cb_iquery){ .index = 2 })
#define Q_USERGAMES ((cb_iquery){ .index = 1 })

// q with its partition key "<prefix><user>" (held until the next call).
static cb_iquery q_user(cb_iquery q, const char *prefix, const char *user) {
    static char pk[128];
    snprintf(pk, sizeof pk, "%s%s", prefix, user);
    q.pk = pk;
    return q;
}

// Every page of one Query, as store_dynamo.c's query_all walks it.
static bool d_query(direct *d, cb_iquery k, item_fn fn, void *ud) {
    dw_req q;
    dw_req_init(&q);
    cb_buf resp, start;
    cb_buf_init(&resp);
    cb_buf_init(&start);
    bool ok = true;
    for (;;) {
        dw_iquery(&q, d->table, &k, cb_span_of((const char *)start.p, start.len));
        if (!(ok = d_send(d, &q, &resp))) break;
        cb_span items = dw_items(cb_buf_cstr(&resp), resp.len), it;
        size_t pos = 0;
        while (fn && dw_array_next(items, &pos, &it)) fn(it, ud);
        cb_span last = dw_last_key(cb_buf_cstr(&resp), resp.len);
        if (!last.n) break;
        cb_buf_reset(&start);
        cb_buf_append(&start, last.p, last.n);
    }
    dw_req_free(&q);
    cb_buf_free(&resp);
    cb_buf_free(&start);
    return ok;
}

static bool d_get_game(direct *d, const char *gid) {
    dw_req q;
    dw_req_init(&q);
    char pk[64];
    snprintf(pk, sizeof pk, "GAME#%s", gid);
    dw_iget(&q, d->table, (cb_ikey){ pk, "ROW" }, true);
    cb_buf resp;
    cb_buf_init(&resp);
    cb_span it;
    bool ok = d_send(d, &q, &resp) && dw_item(cb_buf_cstr(&resp), resp.len, &it);
    cb_buf_free(&resp);
    dw_req_free(&q);
    return ok;
}

typedef struct {
    char ids[64][CB_GAME_ID_MAX + 1];
    int  n;
} gids;

static bool collect(cb_span it, void *ud) {
    gids *g = ud;
    static cb_item m;
    cb_item_reset(&m);
    if (g->n < 64 && dw_item_read(it, &m) && cb_item_s(&m, "game_id", g->ids[g->n], sizeof g->ids[0])) g->n++;
    return true;
}

static bool d_board(direct *d) {
    return d_query(d, Q_RATINGS, NULL, NULL) && d_query(d, Q_BOTS, NULL, NULL);
}

static bool d_history(direct *d, const char *user) {
    return d_query(d, Q_RATINGS, NULL, NULL) && d_query(d, q_user(Q_PLAYER, "PLAYER#", user), NULL, NULL);
}

static bool d_games(direct *d, const char *user) {
    static gids g;
    g.n = 0;
    if (!d_query(d, q_user(Q_USERGAMES, "USERGAMES#", user), collect, &g)) return false;
    for (int i = 0; i < g.n; i++)
        if (!d_get_game(d, g.ids[i])) return false;
    return g.n > 0;
}

// The unsigned GetItem: refused at DynamoDB's door. true for that refusal.
static bool d_floor(direct *d, const char *gid, size_t *sent, size_t *recv) {
    dw_req q;
    dw_req_init(&q);
    char pk[64];
    snprintf(pk, sizeof pk, "GAME#%s", gid);
    dw_iget(&q, d->table, (cb_ikey){ pk, "ROW" }, true);
    char headers[256];
    snprintf(headers, sizeof headers, "Content-Type: %s\r\nX-Amz-Target: %s\r\n", dw_content_type(), q.target);
    cb_https_response r;
    int rc = cb_https_request(d->host, 443, "POST", "/", headers, q.body.p, q.body.len, TIMEOUT_MS, &r);
    bool ok = rc == CB_OK && r.status >= 400 && r.status < 500;
    if (rc == CB_OK) {
        *sent = q.body.len;
        *recv = r.body.len;
        cb_https_response_free(&r);
    }
    dw_req_free(&q);
    return ok;
}

// ---- the cold burst ------------------------------------------------------------------

typedef struct {
    const bn_url *u;
    int           k;
    int64_t       us;
    long          uptime;
    bool          ok;
} shot;

// The start line: every shot waits on it, so the K requests leave together
// (macOS has no pthread_barrier).
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static int             g_waiting, g_open;

static void start_line(int k) {
    pthread_mutex_lock(&g_mu);
    if (++g_waiting == k) { g_open = 1; pthread_cond_broadcast(&g_cv); }
    while (!g_open) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
}

static void *fire(void *arg) {
    shot *s = arg;
    bn_http h;
    bn_http_init(&h, s->u);
    bn_resp r;
    bn_resp_init(&r);
    start_line(s->k);
    int64_t t0 = bn_now_us();
    s->ok = bn_http_request(&h, "GET", "/health", NULL, NULL, 0, 60000, &r) == 0 && r.status == 200;
    s->us = bn_now_us() - t0;
    const char *up = s->ok ? strstr(cb_buf_cstr(&r.body), "uptime_s=") : NULL;
    s->uptime = up ? strtol(up + 9, NULL, 10) : -1;
    bn_resp_free(&r);
    bn_http_close(&h);
    return NULL;
}

// ---- the run ---------------------------------------------------------------------------

int bench_direct(const bench_opts *o) {
    bn_url u;
    if (!bn_url_parse(o->url, &u) || !u.tls) { fprintf(stderr, "bench: --url must be https://host\n"); return 2; }
    bn_url eu = { 0 };
    char edge_path[256] = "/";
    if (o->edge) {
        char host[256], base[256];
        int port;
        bool tls;
        if (cb_aws_url_parse(o->edge, host, sizeof host, &port, &tls, base, sizeof base) != CB_OK || !tls) {
            fprintf(stderr, "bench: --edge must be https://host/path\n");
            return 2;
        }
        eu.tls = true;
        eu.port = port;
        snprintf(eu.host, sizeof eu.host, "%s", host);
        if (base[0]) snprintf(edge_path, sizeof edge_path, "%s", base);
    }
    direct d = { 0 };
    cb_aws_creds creds;
    bool signed_ok = cb_aws_creds_from_env(&creds) == CB_OK;
    dw_host(signed_ok ? creds.region : (o->region ? o->region : "us-east-1"), d.host, sizeof d.host);
    snprintf(d.table, sizeof d.table, "%s", o->table ? o->table : "cbase");
    memset(&creds, 0, sizeof creds);
    if (!signed_ok) fprintf(stderr, "bench: no AWS credentials in the environment: the signed direct rows are left out\n");

    bn_http h, e;
    bn_http_init(&h, &u);
    if (o->edge) bn_http_init(&e, &eu);
    char gid[CB_GAME_ID_MAX + 1] = "", user[64] = "";
    if (!setup(&h, gid, user)) { bn_http_close(&h); return 1; }

    enum { C_BOARD, C_GAMES, C_GAME, C_HISTORY, C_BOTS, D_BOARD, D_GAMES, D_GAME, D_HISTORY, D_BOTS, D_FLOOR, EDGE, NS };
    static const char *const names[NS] = { "cbase_board", "cbase_games", "cbase_game", "cbase_history", "cbase_bots",
                                           "direct_board", "direct_games", "direct_game", "direct_history", "direct_bots",
                                           "direct_floor_unsigned", "edge_hit" };
    static const build_fn via[C_BOTS + 1] = { b_board, b_games, b_game, b_history, b_bots };
    series s[NS];
    for (int i = 0; i < NS; i++) s[i] = (series){ names[i], calloc((size_t)o->n, sizeof(int64_t)), 0, 0, 0 };
    int errors = 0, edge_hits = 0;
    bn_resp r;
    bn_resp_init(&r);
    for (int round = -WARMUP; round < o->n && errors < 10; round++) {
        bool keep = round >= 0;
        for (int op = 0; op < NS; op++) {
            if (op >= D_BOARD && op <= D_BOTS && !signed_ok) continue;
            if (op == EDGE && !o->edge) continue;
            int64_t t0 = bn_now_us();
            bool ok = false;
            size_t sent = 0, recv = 0;
            if (op <= C_BOTS) {
                ok = ask(&h, via[op], op == C_GAME ? gid : NULL, &r);
                ok = ok && r.status == 200;
                sent = r.sent;
                recv = r.recv;
            } else if (op <= D_BOTS) {
                d.sent = d.recv = 0;
                switch (op) {
                case D_BOARD: ok = d_board(&d); break;
                case D_GAMES: ok = d_games(&d, user); break;
                case D_GAME: ok = d_get_game(&d, gid); break;
                case D_HISTORY: ok = d_history(&d, user); break;
                default: ok = d_query(&d, Q_BOTS, NULL, NULL); break;
                }
                sent = d.sent;
                recv = d.recv;
            } else if (op == D_FLOOR) {
                ok = d_floor(&d, gid, &sent, &recv);
            } else {
                ok = bn_http_request(&e, "GET", edge_path, NULL, NULL, 0, TIMEOUT_MS, &r) == 0 && r.status == 200;
                if (ok && keep && strstr(cb_buf_cstr(&r.head), "Hit from cloudfront")) edge_hits++;
                sent = r.sent;
                recv = r.recv;
            }
            int64_t us = bn_now_us() - t0;
            if (!ok) { fprintf(stderr, "bench: %s failed (status %d)\n", names[op], r.status); errors++; continue; }
            if (keep) add(&s[op], us, sent, recv);
        }
        if (keep && (round + 1) % 50 == 0) fprintf(stderr, "bench: %d/%d rounds\n", round + 1, o->n);
    }

    // The cold burst, on fresh connections.
    series cold = { "burst_cold", calloc(64, sizeof(int64_t)), 0, 0, 0 };
    series fresh = { "burst_warm_fresh_conn", calloc(64, sizeof(int64_t)), 0, 0, 0 };
    int burst = o->burst < 0 ? 0 : o->burst > 64 ? 64 : o->burst;
    if (burst > 0) {
        cb_https_set_ca_file(NULL);   // empties the pool: every shot opens its own connection
        shot shots[64];
        pthread_t th[64];
        for (int i = 0; i < burst; i++) {
            shots[i] = (shot){ &u, burst, 0, -1, false };
            pthread_create(&th[i], NULL, fire, &shots[i]);
        }
        for (int i = 0; i < burst; i++) pthread_join(th[i], NULL);
        for (int i = 0; i < burst; i++) {
            if (!shots[i].ok) { errors++; continue; }
            fprintf(stderr, "bench: burst %d: %.2f ms, uptime_s=%ld\n", i, shots[i].us / 1000.0, shots[i].uptime);
            add(shots[i].uptime == 0 ? &cold : &fresh, shots[i].us, 0, 0);
        }
    }

    // The account goes, and its game with it.
    if (!ask(&h, b_delete, NULL, &r) || nc_status_read(r.status, r.body.p, (int)r.body.len) != 0) {
        fprintf(stderr, "bench: account delete answered %d; delete it in the admin console\n", r.status);
        errors++;
    }
    bn_resp_free(&r);
    bn_http_close(&h);
    if (o->edge) bn_http_close(&e);

    char date[32], host[64], note[256];
    bench_date(date, sizeof date);
    bench_host(host, sizeof host);
    snprintf(note, sizeof note, "signed=%s edge_hits=%d/%d burst=%d%s%s", signed_ok ? "yes" : "no", edge_hits, s[EDGE].n,
             burst, o->note && *o->note ? " " : "", o->note ? o->note : "");
    bench_note_load(note, sizeof note);
    bs_run run;
    bs_run_init(&run, "direct", date, host, o->url, note);
    for (int i = 0; i < NS; i++) row(&run, &s[i]);
    row(&run, &cold);
    row(&run, &fresh);
    for (int i = 0; i < NS; i++) free(s[i].v);
    free(cold.v);
    free(fresh.v);
    int rc = bench_save(&run, o);
    return rc == 0 && errors == 0 ? 0 : 1;
}
