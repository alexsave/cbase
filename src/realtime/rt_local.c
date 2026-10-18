// rt_local.c - realtime for `cbase serve` (rt_local.h): one hub, two protocols.
//
// THREADS. Every socket and the connection list belong to the loop thread.
// A client message is handled on the REQUEST POOL, because it may read the
// store (the nw1 stream's membership and catch-up; MQTT's CONNECT reads the
// account) and the loop must never block on SQLite: the loop copies the
// connection's protocol state into a job, the pool runs the protocol on the
// copy and collects the replies, and the loop takes the new state back and
// sends them. One job per connection at a time, so a connection's messages are
// handled in the order they arrived; messages that come meanwhile wait in its
// inbox. A publish from any thread copies its message and posts it to the
// loop, which sends it to each connection that hears it as a binary frame.
//
// nw1 (GET /v1/stream?proto=nw1, stream.h): a client sends text frames of
// base64 (what API Gateway requires), decoded here before they are queued, or
// binary frames as they are; a push is the message itself.
// MQTT (GET /mqtt, subprotocol "mqtt", stream_mqtt.h): binary frames only, as
// MQTT over WebSocket requires; a push is a PUBLISH at QoS 0 whose payload is
// the message, on the target's MQTT topic (realtime.h cb_rt_topic). Whatever
// stream_mqtt.c refuses ends the connection, as IoT Core ends it.
#include "rt_local.h"

#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "net/ws.h"
#include "stream.h"
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

enum { PROTO_NW = 1, PROTO_MQTT };

typedef struct lconn {
    struct lconn *prev, *next;
    cb_ws *ws;
    uint64_t ws_id;
    uint8_t proto;          // PROTO_*
    cb_conn_row phx;        // nw1: the state as of the last finished job
    cb_mqtt_conn *mq;       // MQTT: the same, owned
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
    uint8_t proto;
    cb_conn_row phx;
    cb_mqtt_conn *mq;       // MQTT: the job's copy, owned
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
    cb_ctx *ctx = j->L->ctx;
    if (j->proto == PROTO_MQTT)
        j->rc = cb_mqtt_on_bytes(ctx, j->mq, (const uint8_t *)j->frame->text, j->frame->len, cb_now_ms(), collect, j);
    else
        j->rc = cb_nw_on_frame(ctx, &j->phx, (const uint8_t *)j->frame->text, j->frame->len, collect, j);
}

static void job_done(cb_loop *l, void *ud) {
    job *j = ud;
    local *L = j->L;
    uint64_t id = j->ws_id;
    lconn *c = find(L, id);
    if (c) {
        c->busy = false;
        if (c->proto == PROTO_MQTT) {
            // The state first, then the replies: a SUBSCRIBE is live before its SUBACK leaves.
            free(c->mq);
            c->mq = j->mq;
            j->mq = NULL;
        } else {
            c->phx = j->phx;
        }
        for (msg *m = j->out_head; m && send_to(L, id, m->text, m->len); m = m->next) {}
        c = find(L, id);
        if (c && c->proto == PROTO_MQTT && j->rc != CB_OK) {
            // stream_mqtt.h: whatever it refuses ends the connection, with no reason on the wire.
            if (j->rc != CB_E_STATE) CB_DEBUG("realtime", "mqtt %llu closed: %s", (unsigned long long)id, cb_strerror(j->rc));
            cb_ws_close(c->ws, j->rc == CB_E_STATE ? 1000 : 1008);
        } else if (c && j->rc == CB_E_INVALID) {
            cb_ws_close(c->ws, 1003);   // nw1: a frame that is not the protocol
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
    j->proto = c->proto;
    if (c->proto == PROTO_MQTT) {
        j->mq = malloc(sizeof *j->mq);
        if (!j->mq) { free(j); return; }
        memcpy(j->mq, c->mq, sizeof *j->mq);
    } else {
        j->phx = c->phx;
    }
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

static lconn *conn_open(cb_ws *ws, local *L, uint8_t proto) {
    lconn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->ws = ws;
    c->ws_id = cb_ws_id(ws);
    c->proto = proto;
    if (proto == PROTO_MQTT) {
        c->mq = malloc(sizeof *c->mq);
        if (!c->mq) { free(c); return NULL; }
        cb_mqtt_conn_init(c->mq);
        // The token may ride in the upgrade URL (token=) instead of CONNECT's password.
        cb_mqtt_query_token(cb_ws_query(ws), c->mq->query_token, sizeof c->mq->query_token);
    }
    snprintf(c->phx.conn_id, sizeof c->phx.conn_id, "local-%llu", (unsigned long long)c->ws_id);
    c->next = L->head;
    if (L->head) L->head->prev = c;
    L->head = c;
    return c;
}

static void on_open_nw(cb_ws *ws, void *ud) {
    cb_rt *rt = ud;
    if (!conn_open(ws, rt->impl, PROTO_NW)) cb_ws_close(ws, 1011);
}

static void on_open_mqtt(cb_ws *ws, void *ud) {
    cb_rt *rt = ud;
    if (!conn_open(ws, rt->impl, PROTO_MQTT)) cb_ws_close(ws, 1011);
}

static void on_message(cb_ws *ws, int opcode, const uint8_t *data, size_t len, void *ud) {
    cb_rt *rt = ud;
    local *L = rt->impl;
    lconn *c = find(L, cb_ws_id(ws));
    if (!c) return;
    if (c->in_bytes + len > INBOX_MAX) { cb_ws_close(ws, 1008); return; }
    msg *m;
    if (opcode == CB_WS_OP_TEXT) {
        // MQTT over WebSocket is binary only (MQTT 3.1.1, 6.0).
        if (c->proto == PROTO_MQTT) { cb_ws_close(ws, 1003); return; }
        // nw1's text form: base64 of the message.
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
    free(c->mq);
    free(c);
}

static void on_close(cb_ws *ws, void *ud) {
    cb_rt *rt = ud;
    local *L = rt->impl;
    lconn *c = find(L, cb_ws_id(ws));
    if (c) lconn_free(L, c);   // a job still out finds nothing and frees itself
}

static const cb_ws_handlers WS_NW = { on_open_nw, on_message, on_close };
static const cb_ws_handlers WS_MQTT = { on_open_mqtt, on_message, on_close };

// ---- publishing ------------------------------------------------------------------------

typedef struct {
    cb_rt *rt;
    char nw_topic[CB_TOPIC_MAX];      // "" when the target has none
    char topic[MQTT_TOPIC_MAX + 1];   // "" when the target has none
    size_t len, packet_len;
    char *frame;                      // the message (nw1's push)
    char *packet;                     // the PUBLISH carrying it (MQTT's), NULL when no topic
} pub;

static void pub_free(pub *p) {
    free(p->frame);
    free(p->packet);
    free(p);
}

static bool joined(const cb_conn_row *c, const char *topic) {
    for (int i = 0; i < c->n_topics; i++)
        if (strcmp(c->topics[i], topic) == 0) return true;
    return false;
}

static bool hears(const lconn *c, const pub *p) {
    if (c->proto == PROTO_MQTT) return p->packet && cb_mqtt_hears(c->mq, p->topic, (int)strlen(p->topic));
    return p->nw_topic[0] && joined(&c->phx, p->nw_topic);
}

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
        if (c->proto == PROTO_MQTT) send_to(L, ids[i], p->packet, p->packet_len);
        else send_to(L, ids[i], p->frame, p->len);
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
    bool nw = cb_rt_nw_topic(to, p->nw_topic, sizeof p->nw_topic) > 0;
    int tl = cb_rt_topic(to, p->topic, sizeof p->topic);
    if (!nw && tl <= 0) { pub_free(p); return 0; }   // no connection here can hear it
    p->frame = malloc(len ? len : 1);
    if (!p->frame) { pub_free(p); return 1; }
    memcpy(p->frame, frame, len);
    p->len = len;
    if (tl > 0) {
        int pl = len <= (size_t)INT32_MAX ? mqtt_publish_len(tl, (int)len) : -1;
        p->packet = pl > 0 ? malloc((size_t)pl) : NULL;
        int w = p->packet ? mqtt_write_publish((uint8_t *)p->packet, pl, p->topic, tl, frame, (int)len) : -1;
        if (w < 0) { pub_free(p); return 1; }
        p->packet_len = (size_t)w;
    }
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
        bool mine = c->proto == PROTO_MQTT ? strcmp(c->mq->user_id, f->user) == 0 : strcmp(c->phx.user_id, f->user) == 0;
        if (!mine) continue;
        // A frame still out on the pool comes back to nobody.
        c->phx.user_id[0] = 0;
        c->phx.n_topics = 0;
        if (c->mq) { c->mq->state = CB_MQTT_CLOSED; c->mq->n_subs = 0; }
        cb_ws_close(c->ws, 1008);
    }
    free(f);
}

static void local_forget(cb_rt *rt, const char *user_id, const cb_rt_to *targets, int n) {
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

// ---- the upgrade routes ----------------------------------------------------------------

// GET /v1/stream?proto=nw1 (stream.h). The first frame names the user.
static void h_upgrade(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ctx->rt || ctx->rt->t != &LOCAL_T) { cb_router_error(res, 503, "realtime is not running"); return; }
    if (cb_nw_proto_of_query(req->query) != CB_VSN_NW) { cb_router_error(res, 400, "unsupported proto"); return; }
    cb_ws_upgrade(req, res, &WS_NW, ctx->rt);
}

// GET /mqtt, subprotocol "mqtt" (stream_mqtt.h): the path and protocol AWS IoT
// Core serves MQTT over WebSocket on, so a client is the same against either.
// The CONNECT names the user.
static void h_mqtt(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ctx->rt || ctx->rt->t != &LOCAL_T) { cb_router_error(res, 503, "realtime is not running"); return; }
    cb_ws_upgrade_protocol(req, res, &WS_MQTT, ctx->rt, "mqtt");
}

int cb_rt_local_routes(cb_ctx *ctx) {
    int rc = cb_router_add(ctx->router, "GET", "/v1/stream", CB_ROUTE_LOOP, h_upgrade);
    return rc != CB_OK ? rc : cb_router_add(ctx->router, "GET", "/mqtt", CB_ROUTE_LOOP, h_mqtt);
}
