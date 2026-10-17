// rt_local.c - realtime for `cbase serve` (rt_local.h).
//
// THREADS. Every socket and the connection list belong to the loop thread.
// A client frame is handled on the REQUEST POOL, because a subscription may
// read the store (membership, the catch-up view) and the loop must never block
// on SQLite: the loop copies the connection's subscription state into a job,
// the pool runs cb_nw_on_frame on the copy and collects the replies, and the
// loop takes the new state back and sends them. One job per connection at a
// time, so a connection's frames are handled in the order they arrived; frames
// that come meanwhile wait in its inbox. A publish from any thread copies its
// message and posts it to the loop, which sends it to each subscriber as a
// binary frame. A client sends text frames of base64 (what API Gateway
// requires), decoded here before they are queued, or binary frames as they are.
#include "rt_local.h"

#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "net/ws.h"
#include "stream.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A frame waiting to be handled, or a reply waiting to be sent.
typedef struct msg {
    struct msg *next;
    size_t len;
    char text[];
} msg;

typedef struct lconn {
    struct lconn *prev, *next;
    cb_ws *ws;
    uint64_t ws_id;
    cb_conn_row phx;        // the state as of the last finished job
    bool busy;              // a job is out on the pool
    msg *in_head, *in_tail; // frames not handled yet
    size_t in_bytes;
} lconn;

typedef struct {
    cb_ctx *ctx;
    lconn *head;            // loop thread only
} local;

#define INBOX_MAX (1u << 20)   // a client this far ahead of us is not a stream client

static const cb_rt_transport LOCAL_T;

// A message of n bytes copied from p, or n uninitialised bytes when p is NULL
// (the caller decodes into them).
static msg *msg_new(const char *p, size_t n) {
    msg *m = malloc(sizeof *m + n + 1);
    if (!m) return NULL;
    m->next = NULL;
    m->len = n;
    if (p && n) memcpy(m->text, p, n);
    m->text[n] = 0;
    return m;
}

static void msgs_free(msg *m) {
    while (m) { msg *n = m->next; free(m); m = n; }
}

static lconn *find(local *L, uint64_t ws_id) {
    for (lconn *c = L->head; c; c = c->next)
        if (c->ws_id == ws_id) return c;
    return NULL;
}

// ---- one frame on the pool -------------------------------------------------------

typedef struct {
    local *L;
    uint64_t ws_id;
    cb_conn_row phx;
    msg *frame;
    msg *out_head, *out_tail;
    int rc;
} job;

static void collect(void *ud, const char *frame, size_t len) {
    job *j = ud;
    msg *m = msg_new(frame, len);
    if (!m) return;   // a lost reply; the client times out and rejoins
    if (j->out_tail) j->out_tail->next = m;
    else j->out_head = m;
    j->out_tail = m;
}

static void job_free(job *j) {
    msgs_free(j->frame);
    msgs_free(j->out_head);
    free(j);
}

static void pump(local *L, lconn *c);

// Sends one message to the connection with that id, as a binary frame. false
// when it is gone (closed before, or by this send: a failed send closes the
// socket, and on_close frees the lconn, so the caller must look it up again).
static bool send_to(local *L, uint64_t ws_id, const char *bytes, size_t len) {
    lconn *c = find(L, ws_id);
    if (!c) return false;
    cb_ws_send(c->ws, CB_WS_OP_BIN, bytes, len);
    return find(L, ws_id) != NULL;
}

static int on_frame(cb_ctx *ctx, cb_conn_row *c, const msg *m, cb_rt_send_fn send, void *ud) {
    return cb_nw_on_frame(ctx, c, (const uint8_t *)m->text, m->len, send, ud);
}

static void job_done(cb_loop *l, void *ud) {
    job *j = ud;
    local *L = j->L;
    uint64_t id = j->ws_id;
    lconn *c = find(L, id);
    if (c) {
        c->busy = false;
        c->phx = j->phx;
        for (msg *m = j->out_head; m && send_to(L, id, m->text, m->len); m = m->next) {}
        c = find(L, id);
        // A frame that is not the protocol: the socket is dropped.
        if (c && j->rc == CB_E_INVALID) cb_ws_close(c->ws, 1003);
        else if (c) pump(L, c);
    }
    job_free(j);
}

static void job_run(void *ud) {
    job *j = ud;
    j->rc = on_frame(j->L->ctx, &j->phx, j->frame, collect, j);
    if (cb_loop_post(j->L->ctx->loop, job_done, j) != CB_OK) job_free(j);   // the loop has stopped
}

// Starts the next frame of `c` if none is in flight.
static void pump(local *L, lconn *c) {
    if (c->busy || !c->in_head) return;
    job *j = calloc(1, sizeof *j);
    if (!j) return;   // try again on the next frame
    j->L = L;
    j->ws_id = c->ws_id;
    j->phx = c->phx;
    j->frame = c->in_head;
    c->in_head = c->in_head->next;
    if (!c->in_head) c->in_tail = NULL;
    c->in_bytes -= j->frame->len;
    j->frame->next = NULL;
    cb_pool *pool = L->ctx->req_pool;
    c->busy = true;
    if (pool && cb_pool_submit(pool, job_run, j) == CB_OK) return;
    // No pool (or it is stopping): handle it here. Only the tests run without one.
    j->rc = on_frame(L->ctx, &j->phx, j->frame, collect, j);
    job_done(L->ctx->loop, j);
}

// ---- the socket ----------------------------------------------------------------------

static void on_open(cb_ws *ws, void *ud) {
    cb_rt *rt = ud;
    local *L = rt->impl;
    lconn *c = calloc(1, sizeof *c);
    if (!c) { cb_ws_close(ws, 1011); return; }
    c->ws = ws;
    c->ws_id = cb_ws_id(ws);
    snprintf(c->phx.conn_id, sizeof c->phx.conn_id, "local-%llu", (unsigned long long)c->ws_id);
    c->next = L->head;
    if (L->head) L->head->prev = c;
    L->head = c;
}

static void on_message(cb_ws *ws, int opcode, const uint8_t *data, size_t len, void *ud) {
    cb_rt *rt = ud;
    local *L = rt->impl;
    lconn *c = find(L, cb_ws_id(ws));
    if (!c) return;
    if (c->in_bytes + len > INBOX_MAX) { cb_ws_close(ws, 1008); return; }
    msg *m;
    if (opcode == CB_WS_OP_TEXT) {
        // The text form: base64 of the message.
        m = msg_new(NULL, len);
        int n = m ? nw_text_decode((const char *)data, (int)len, (uint8_t *)m->text, (int)len) : -1;
        if (m && n < 0) { free(m); cb_ws_close(ws, 1003); return; }
        if (m) m->len = (size_t)n;
    } else {
        m = msg_new((const char *)data, len);
    }
    if (!m) return;
    if (c->in_tail) c->in_tail->next = m;
    else c->in_head = m;
    c->in_tail = m;
    c->in_bytes += len;
    pump(L, c);
}

static void lconn_free(local *L, lconn *c) {
    if (c->prev) c->prev->next = c->next;
    else L->head = c->next;
    if (c->next) c->next->prev = c->prev;
    msgs_free(c->in_head);
    free(c);
}

static void on_close(cb_ws *ws, void *ud) {
    cb_rt *rt = ud;
    local *L = rt->impl;
    lconn *c = find(L, cb_ws_id(ws));
    if (c) lconn_free(L, c);   // a job still out finds nothing and frees itself
}

static const cb_ws_handlers WS_H = { on_open, on_message, on_close };

// ---- publishing ------------------------------------------------------------------------

typedef struct {
    cb_rt *rt;
    char topic[CB_TOPIC_MAX];
    size_t len;
    char *frame;
} pub;

static void pub_free(pub *p) {
    free(p->frame);
    free(p);
}

static bool joined(const cb_conn_row *c, const char *topic) {
    for (int i = 0; i < c->n_topics; i++)
        if (strcmp(c->topics[i], topic) == 0) return true;
    return false;
}

static void pub_on_loop(cb_loop *l, void *ud) {
    pub *p = ud;
    local *L = p->rt->impl;
    // Collect the ids first: a send can close a socket and free its lconn.
    size_t n = 0, cap = 0;
    uint64_t *ids = NULL;
    for (lconn *c = L->head; c; c = c->next) {
        if (!joined(&c->phx, p->topic)) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 16;
            uint64_t *ni = realloc(ids, nc * sizeof *ids);
            if (!ni) break;
            ids = ni;
            cap = nc;
        }
        ids[n++] = c->ws_id;
    }
    for (size_t i = 0; i < n; i++) {
        lconn *c = find(L, ids[i]);
        if (!c) continue;
        send_to(L, ids[i], p->frame, p->len);
    }
    free(ids);
    pub_free(p);
}

// Posted to the loop, which writes each subscriber's socket: a failure to hand
// it over counts as one; what the loop's writes do later is the socket's.
static int local_deliver(cb_rt *rt, const char *topic, const uint8_t *frame, size_t len) {
    cb_loop *loop = rt->ctx->loop;
    if (!loop) return 0;
    pub *p = calloc(1, sizeof *p);
    if (!p) return 1;
    p->rt = rt;
    cb_strlcpy(p->topic, topic, sizeof p->topic);
    p->frame = malloc(len ? len : 1);
    if (!p->frame) { pub_free(p); return 1; }
    memcpy(p->frame, frame, len);
    p->len = len;
    if (cb_loop_post(loop, pub_on_loop, p) != CB_OK) { pub_free(p); return 1; }
    return 0;
}

static void local_free(cb_rt *rt) {
    local *L = rt->impl;
    while (L->head) lconn_free(L, L->head);
    free(L);
    free(rt);
}

static const cb_rt_transport LOCAL_T = { "local", local_deliver, local_free };

int cb_rt_local_new(cb_ctx *ctx, cb_rt **out) {
    *out = NULL;
    cb_rt *rt = calloc(1, sizeof *rt);
    local *L = calloc(1, sizeof *L);
    if (!rt || !L) { free(rt); free(L); return CB_E_NOMEM; }
    L->ctx = ctx;
    rt->t = &LOCAL_T;
    rt->ctx = ctx;
    rt->impl = L;
    *out = rt;
    return CB_OK;
}

// ---- the upgrade route ---------------------------------------------------------------

// GET /v1/stream?proto=nw1 (stream.h). The first frame names the user.
static void h_upgrade(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ctx->rt || ctx->rt->t != &LOCAL_T) { cb_router_error(res, 503, "realtime is not running"); return; }
    if (cb_nw_proto_of_query(req->query) != CB_VSN_NW) { cb_router_error(res, 400, "unsupported proto"); return; }
    cb_ws_upgrade(req, res, &WS_H, ctx->rt);
}

int cb_rt_local_routes(cb_ctx *ctx) {
    return cb_router_add(ctx->router, "GET", "/v1/stream", CB_ROUTE_LOOP, h_upgrade);
}
