// rt_local.c - realtime for `cbase serve` (rt_local.h): the MQTT hub.
//
// THREADS. Every socket and the connection list belong to the loop thread.
// A client message is handled on the REQUEST POOL, because a CONNECT reads the
// account from the store and the loop must never block on SQLite: the loop copies the
// connection's protocol state into a job, the pool runs the protocol on the
// copy and collects the replies, and the loop takes the new state back and
// sends them. One job per connection at a time, so a connection's messages are
// handled in the order they arrived; messages that come meanwhile wait in its
// inbox. A publish from any thread copies its message and posts it to the
// loop, which sends it to each connection that hears it as a binary frame.
//
// GET /mqtt, subprotocol "mqtt" (stream_mqtt.h): binary frames only, as MQTT
// over WebSocket requires; a push is a PUBLISH at QoS 0 whose payload is the
// message, on the target's topic (realtime.h cb_rt_topic). Whatever
// stream_mqtt.c refuses ends the connection, as IoT Core ends it.
#include "rt_local.h"

#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "net/ws.h"
#include "stream_mqtt.h"
#include "util/clock.h"
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
    cb_mqtt_conn *mq;       // the state as of the last finished job, owned
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
    cb_mqtt_conn *mq;       // the job's copy, owned
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
    free(j->mq);
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

static void job_run_frame(job *j) {
    j->rc = cb_mqtt_on_bytes(j->L->ctx, j->mq, (const uint8_t *)j->frame->text, j->frame->len, cb_now_ms(), collect, j);
}

static void job_done(cb_loop *l, void *ud) {
    job *j = ud;
    local *L = j->L;
    uint64_t id = j->ws_id;
    lconn *c = find(L, id);
    if (c) {
        c->busy = false;
        // The state first, then the replies: a SUBSCRIBE is live before its SUBACK leaves.
        free(c->mq);
        c->mq = j->mq;
        j->mq = NULL;
        for (msg *m = j->out_head; m && send_to(L, id, m->text, m->len); m = m->next) {}
        c = find(L, id);
        if (c && j->rc != CB_OK) {
            // stream_mqtt.h: whatever it refuses ends the connection, with no reason on the wire.
            if (j->rc != CB_E_STATE) CB_DEBUG("realtime", "mqtt %llu closed: %s", (unsigned long long)id, cb_strerror(j->rc));
            cb_ws_close(c->ws, j->rc == CB_E_STATE ? 1000 : 1008);
        } else if (c) {
            pump(L, c);
        }
    }
    job_free(j);
}

static void job_run(void *ud) {
    job *j = ud;
    job_run_frame(j);
    if (cb_loop_post(j->L->ctx->loop, job_done, j) != CB_OK) job_free(j);   // the loop has stopped
}

// Starts the next frame of `c` if none is in flight.
static void pump(local *L, lconn *c) {
    if (c->busy || !c->in_head) return;
    job *j = calloc(1, sizeof *j);
    if (!j) return;   // try again on the next frame
    j->L = L;
    j->ws_id = c->ws_id;
    j->mq = malloc(sizeof *j->mq);
    if (!j->mq) { free(j); return; }
    memcpy(j->mq, c->mq, sizeof *j->mq);
    j->frame = c->in_head;
    c->in_head = c->in_head->next;
    if (!c->in_head) c->in_tail = NULL;
    c->in_bytes -= j->frame->len;
    j->frame->next = NULL;
    cb_pool *pool = L->ctx->req_pool;
    c->busy = true;
    if (pool && cb_pool_submit(pool, job_run, j) == CB_OK) return;
    // No pool (or it is stopping): handle it here. Only the tests run without one.
    job_run_frame(j);
    job_done(L->ctx->loop, j);
}

// ---- the socket ----------------------------------------------------------------------

static lconn *conn_open(cb_ws *ws, local *L) {
    lconn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->ws = ws;
    c->ws_id = cb_ws_id(ws);
    c->mq = malloc(sizeof *c->mq);
    if (!c->mq) { free(c); return NULL; }
    cb_mqtt_conn_init(c->mq);
    // The token may ride in the upgrade URL (token=) instead of CONNECT's password.
    cb_mqtt_query_token(cb_ws_query(ws), c->mq->query_token, sizeof c->mq->query_token);
    c->next = L->head;
    if (L->head) L->head->prev = c;
    L->head = c;
    return c;
}

static void on_open(cb_ws *ws, void *ud) {
    cb_rt *rt = ud;
    if (!conn_open(ws, rt->impl)) cb_ws_close(ws, 1011);
}

static void on_message(cb_ws *ws, int opcode, const uint8_t *data, size_t len, void *ud) {
    cb_rt *rt = ud;
    local *L = rt->impl;
    lconn *c = find(L, cb_ws_id(ws));
    if (!c) return;
    if (c->in_bytes + len > INBOX_MAX) { cb_ws_close(ws, 1008); return; }
    // MQTT over WebSocket is binary only (MQTT 3.1.1, 6.0).
    if (opcode == CB_WS_OP_TEXT) { cb_ws_close(ws, 1003); return; }
    msg *m = msg_new((const char *)data, len);
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
    free(c->mq);
    free(c);
}

static void on_close(cb_ws *ws, void *ud) {
    cb_rt *rt = ud;
    local *L = rt->impl;
    lconn *c = find(L, cb_ws_id(ws));
    if (c) lconn_free(L, c);   // a job still out finds nothing and frees itself
}

static const cb_ws_handlers WS_MQTT = { on_open, on_message, on_close };

// ---- publishing ------------------------------------------------------------------------

typedef struct {
    cb_rt *rt;
    char topic[MQTT_TOPIC_MAX + 1];
    size_t packet_len;
    char *packet;                     // the PUBLISH carrying the message
} pub;

static void pub_free(pub *p) {
    free(p->packet);
    free(p);
}

static bool hears(const lconn *c, const pub *p) { return cb_mqtt_hears(c->mq, p->topic, (int)strlen(p->topic)); }

static void pub_on_loop(cb_loop *l, void *ud) {
    pub *p = ud;
    local *L = p->rt->impl;
    // Collect the ids first: a send can close a socket and free its lconn.
    size_t n = 0, cap = 0;
    uint64_t *ids = NULL;
    for (lconn *c = L->head; c; c = c->next) {
        if (!hears(c, p)) continue;
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
        send_to(L, ids[i], p->packet, p->packet_len);
    }
    free(ids);
    pub_free(p);
}

// Posted to the loop, which writes each subscriber's socket: a failure to hand
// it over counts as one; what the loop's writes do later is the socket's.
static int local_deliver(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len) {
    cb_loop *loop = rt->ctx->loop;
    if (!loop) return 0;
    pub *p = calloc(1, sizeof *p);
    if (!p) return 1;
    p->rt = rt;
    int tl = cb_rt_topic(to, p->topic, sizeof p->topic);
    if (tl <= 0) { pub_free(p); return 1; }
    int pl = len <= (size_t)INT32_MAX ? mqtt_publish_len(tl, (int)len) : -1;
    p->packet = pl > 0 ? malloc((size_t)pl) : NULL;
    int w = p->packet ? mqtt_write_publish((uint8_t *)p->packet, pl, p->topic, tl, frame, (int)len) : -1;
    if (w < 0) { pub_free(p); return 1; }
    p->packet_len = (size_t)w;
    if (cb_loop_post(loop, pub_on_loop, p) != CB_OK) { pub_free(p); return 1; }
    return 0;
}

static void local_free(cb_rt *rt) {
    local *L = rt->impl;
    while (L->head) lconn_free(L, L->head);
    free(L);
    free(rt);
}

// Every socket of the user is closed (1008): on the loop, which owns them.
typedef struct {
    cb_rt  *rt;
    cb_uuid user;
} forget_job;

static void forget_on_loop(cb_loop *l, void *ud) {
    forget_job *f = ud;
    local *L = f->rt->impl;
    for (lconn *c = L->head, *next; c; c = next) {
        next = c->next;
        if (strcmp(c->mq->user_id, f->user) != 0) continue;
        // A packet still out on the pool comes back to a closed connection.
        c->mq->state = CB_MQTT_CLOSED;
        c->mq->n_subs = 0;
        cb_ws_close(c->ws, 1008);
    }
    free(f);
}

static void local_forget(cb_rt *rt, const char *user_id) {
    cb_loop *loop = rt->ctx->loop;
    forget_job *f = loop ? calloc(1, sizeof *f) : NULL;
    if (!f) return;
    f->rt = rt;
    cb_strlcpy(f->user, user_id, sizeof f->user);
    if (cb_loop_post(loop, forget_on_loop, f) != CB_OK) free(f);
}

static const cb_rt_transport LOCAL_T = { "local", local_deliver, local_free, local_forget };

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

// ---- the upgrade route -----------------------------------------------------------------

// GET /mqtt, subprotocol "mqtt" (stream_mqtt.h): the path and protocol AWS IoT
// Core serves MQTT over WebSocket on, so a client is the same against either.
// The CONNECT names the user.
static void h_mqtt(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ctx->rt || ctx->rt->t != &LOCAL_T) { cb_router_error(res, 503, "realtime is not running"); return; }
    cb_ws_upgrade_protocol(req, res, &WS_MQTT, ctx->rt, "mqtt");
}

int cb_rt_local_routes(cb_ctx *ctx) { return cb_router_add(ctx->router, "GET", "/mqtt", CB_ROUTE_LOOP, h_mqtt); }
