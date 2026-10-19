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
#include "realtime/stream_mqtt.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include <pthread.h>
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
    struct lconn *hnext;    // the by-id table's chain
    struct lconn *wprev, *wnext;   // the wildcard list (only while in_wild)
    bool in_wild;
    int nix;                // exact filters this connection is indexed under
    char *ix[CB_MQTT_SUBS];
    cb_ws *ws;
    uint64_t ws_id;
    cb_mqtt_conn *mq;       // the state as of the last finished job, owned
    bool busy;              // a job is out on the pool
    msg *in_head, *in_tail; // frames not handled yet
    size_t in_bytes;
} lconn;

// One topic's subscribers: the exact filters (no + or #) of the open
// connections, found by hash, so a publish touches its subscribers and not
// every connection. Filters with a wildcard are rare (the policy allows them
// under s/ and u/<uid>/) and live in a short list scanned per publish.
typedef struct tent {
    struct tent *next;
    uint64_t h;
    char *topic;
    uint64_t *ids;          // ws ids
    uint32_t n, cap;
} tent;

typedef struct {
    cb_ctx *ctx;
    lconn *head;            // loop thread only
    lconn **byid;           // ws id -> lconn, chained on hnext
    size_t nbyid, nconn;
    tent **topics;          // topic -> subscribers, chained
    size_t ntopics, nent;
    lconn *wild;            // connections holding a wildcard filter
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

// ---- the indexes (loop thread only) ------------------------------------------------

static size_t id_slot(uint64_t id, size_t nb) { return (size_t)((id * 0x9E3779B97F4A7C15ull) >> 20) & (nb - 1); }

static lconn *find(local *L, uint64_t ws_id) {
    if (!L->nbyid) return NULL;
    for (lconn *c = L->byid[id_slot(ws_id, L->nbyid)]; c; c = c->hnext)
        if (c->ws_id == ws_id) return c;
    return NULL;
}

static bool byid_add(local *L, lconn *c) {
    if (L->nconn + 1 > L->nbyid) {
        size_t nb = L->nbyid ? L->nbyid * 2 : 256;
        lconn **t = calloc(nb, sizeof *t);
        if (!t) return false;
        for (lconn *x = L->head; x; x = x->next) {   // c is not on the list yet
            size_t i = id_slot(x->ws_id, nb);
            x->hnext = t[i];
            t[i] = x;
        }
        free(L->byid);
        L->byid = t;
        L->nbyid = nb;
    }
    size_t i = id_slot(c->ws_id, L->nbyid);
    c->hnext = L->byid[i];
    L->byid[i] = c;
    L->nconn++;
    return true;
}

static void byid_del(local *L, lconn *c) {
    lconn **pp = &L->byid[id_slot(c->ws_id, L->nbyid)];
    while (*pp && *pp != c) pp = &(*pp)->hnext;
    if (*pp) { *pp = c->hnext; L->nconn--; }
}

static uint64_t topic_hash(const char *t) {
    uint64_t h = 1469598103934665603ull;
    for (; *t; t++) h = (h ^ (uint8_t)*t) * 1099511628211ull;
    return h;
}

static tent *topic_find(local *L, const char *t, uint64_t h) {
    if (!L->ntopics) return NULL;
    for (tent *e = L->topics[h & (L->ntopics - 1)]; e; e = e->next)
        if (e->h == h && strcmp(e->topic, t) == 0) return e;
    return NULL;
}

static bool topic_add(local *L, const char *t, uint64_t id) {
    uint64_t h = topic_hash(t);
    tent *e = topic_find(L, t, h);
    if (!e) {
        if (L->nent + 1 > L->ntopics) {
            size_t nb = L->ntopics ? L->ntopics * 2 : 256;
            tent **tb = calloc(nb, sizeof *tb);
            if (!tb) return false;
            for (size_t i = 0; i < L->ntopics; i++)
                for (tent *x = L->topics[i], *nx; x; x = nx) {
                    nx = x->next;
                    x->next = tb[x->h & (nb - 1)];
                    tb[x->h & (nb - 1)] = x;
                }
            free(L->topics);
            L->topics = tb;
            L->ntopics = nb;
        }
        e = calloc(1, sizeof *e);
        if (!e) return false;
        e->topic = strdup(t);
        if (!e->topic) { free(e); return false; }
        e->h = h;
        e->next = L->topics[h & (L->ntopics - 1)];
        L->topics[h & (L->ntopics - 1)] = e;
        L->nent++;
    }
    if (e->n == e->cap) {
        uint32_t nc = e->cap ? e->cap * 2 : 4;
        uint64_t *ni = realloc(e->ids, nc * sizeof *ni);
        if (!ni) return false;
        e->ids = ni;
        e->cap = nc;
    }
    e->ids[e->n++] = id;
    return true;
}

static void topic_del(local *L, const char *t, uint64_t id) {
    uint64_t h = topic_hash(t);
    tent *e = topic_find(L, t, h);
    if (!e) return;
    for (uint32_t i = 0; i < e->n; i++)
        if (e->ids[i] == id) { e->ids[i] = e->ids[--e->n]; break; }
    if (e->n) return;
    tent **pp = &L->topics[h & (L->ntopics - 1)];
    while (*pp != e) pp = &(*pp)->next;
    *pp = e->next;
    L->nent--;
    free(e->ids);
    free(e->topic);
    free(e);
}

static bool is_wild(const char *f) { return strchr(f, '+') || strchr(f, '#'); }

static void wild_set(local *L, lconn *c, bool on) {
    if (on == c->in_wild) return;
    if (on) {
        c->wprev = NULL;
        c->wnext = L->wild;
        if (L->wild) L->wild->wprev = c;
        L->wild = c;
    } else {
        if (c->wprev) c->wprev->wnext = c->wnext;
        else L->wild = c->wnext;
        if (c->wnext) c->wnext->wprev = c->wprev;
        c->wprev = c->wnext = NULL;
    }
    c->in_wild = on;
}

// Brings the indexes to what the connection's state says it hears: the
// filters of an OPEN connection, nothing for any other state. Called wherever
// c->mq is replaced or changed, so the indexes are never ahead of or behind
// what cb_mqtt_hears would answer.
static void reindex(local *L, lconn *c) {
    const cb_mqtt_conn *m = c->mq;
    bool open = m->state == CB_MQTT_OPEN;
    bool wild = false;
    for (int i = 0; i < c->nix;) {   // drop what is no longer subscribed
        bool keep = false;
        for (int k = 0; open && k < m->n_subs && !keep; k++) keep = strcmp(m->subs[k], c->ix[i]) == 0;
        if (keep) { i++; continue; }
        topic_del(L, c->ix[i], c->ws_id);
        free(c->ix[i]);
        c->ix[i] = c->ix[--c->nix];
    }
    for (int k = 0; open && k < m->n_subs; k++) {
        if (is_wild(m->subs[k])) { wild = true; continue; }
        bool have = false;
        for (int i = 0; i < c->nix && !have; i++) have = strcmp(m->subs[k], c->ix[i]) == 0;
        if (have || c->nix >= CB_MQTT_SUBS) continue;
        char *copy = strdup(m->subs[k]);
        if (!copy) continue;
        if (!topic_add(L, copy, c->ws_id)) { free(copy); continue; }
        c->ix[c->nix++] = copy;
    }
    wild_set(L, c, wild);
}

static void unindex(local *L, lconn *c) {
    for (int i = 0; i < c->nix; i++) { topic_del(L, c->ix[i], c->ws_id); free(c->ix[i]); }
    c->nix = 0;
    wild_set(L, c, false);
    byid_del(L, c);
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
    if (j->mq) cb_mqtt_conn_free(j->mq);
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
        cb_mqtt_conn_free(c->mq);
        free(c->mq);
        c->mq = j->mq;
        j->mq = NULL;
        reindex(L, c);
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
    j->mq = calloc(1, sizeof *j->mq);
    if (!j->mq) { free(j); return; }
    if (cb_mqtt_conn_clone(j->mq, c->mq) != CB_OK) { free(j->mq); free(j); return; }
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
    c->mq = calloc(1, sizeof *c->mq);
    if (!c->mq) { free(c); return NULL; }
    if (!byid_add(L, c)) { free(c->mq); free(c); return NULL; }
    // The token may ride in the upgrade URL (token=) instead of CONNECT's password.
    char qt[CB_RT_TOKEN_MAX + 1];
    cb_mqtt_query_token(cb_ws_query(ws), qt, sizeof qt);
    if (!cb_mqtt_conn_set_token(c->mq, qt)) { byid_del(L, c); free(c->mq); free(c); return NULL; }
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
    // A keepalive is answered right here: PINGREQ is two bytes and changes no state, so it needs no
    // copy of the connection, no pool hand-off and no allocation. Only when nothing is queued or in
    // flight for this connection (the PINGRESP must not overtake a reply still being computed), the
    // connection is open, and its token has not run out (the pooled path closes it then).
    if (len == 2 && data[0] == 0xC0 && data[1] == 0x00 && !c->busy && !c->in_head && c->mq->state == CB_MQTT_OPEN &&
        cb_now_ms() / 1000 < c->mq->expires_s) {
        static const char pingresp[2] = { (char)0xD0, 0x00 };
        cb_ws_send(ws, CB_WS_OP_BIN, pingresp, sizeof pingresp);
        return;
    }
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
    unindex(L, c);
    msgs_free(c->in_head);
    cb_mqtt_conn_free(c->mq);
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

typedef struct pub {
    cb_rt *rt;
    char topic[MQTT_TOPIC_MAX + 1];
    size_t packet_len, packet_cap;
    char *packet;                     // the PUBLISH carrying the message
    struct pub *next;                 // on the free list
} pub;

// A delivery is allocated on a request thread (or the loop, for an inline request) and freed on the loop.
// musl's allocator unmaps a group when its last slot is freed, so a steady one-in, one-out stream of
// these cost an mmap and a munmap each. Up to PUB_KEEP of them, packet buffer included, wait on a
// free list instead; the lock is held for a pointer swap.
#define PUB_KEEP 64
static pthread_mutex_t g_pub_mu = PTHREAD_MUTEX_INITIALIZER;
static pub *g_pub_free;
static int g_pub_n;

static pub *pub_get(void) {
    pthread_mutex_lock(&g_pub_mu);
    pub *p = g_pub_free;
    if (p) { g_pub_free = p->next; g_pub_n--; }
    pthread_mutex_unlock(&g_pub_mu);
    if (p) { p->next = NULL; return p; }
    return calloc(1, sizeof *p);
}

static void pub_free(pub *p) {
    pthread_mutex_lock(&g_pub_mu);
    if (g_pub_n < PUB_KEEP) {
        p->next = g_pub_free;
        g_pub_free = p;
        g_pub_n++;
        pthread_mutex_unlock(&g_pub_mu);
        return;
    }
    pthread_mutex_unlock(&g_pub_mu);
    free(p->packet);
    free(p);
}


static void pub_on_loop(cb_loop *l, void *ud) {
    pub *p = ud;
    local *L = p->rt->impl;
    // Collect the ids first: a send can close a socket and free its lconn.
    size_t n = 0, cap = 0;
    uint64_t *ids = NULL;
    int tl = (int)strlen(p->topic);
    tent *e = topic_find(L, p->topic, topic_hash(p->topic));
    if (e && e->n) {
        ids = malloc(e->n * sizeof *ids);
        if (ids) { memcpy(ids, e->ids, e->n * sizeof *ids); n = cap = e->n; }
    }
    // A wildcard holder is added once, even when it also holds the exact filter.
    for (lconn *c = L->wild; c; c = c->wnext) {
        if (!cb_mqtt_hears(c->mq, p->topic, tl)) continue;
        bool dup = false;
        for (size_t i = 0; i < n && !dup; i++) dup = ids[i] == c->ws_id;
        if (dup) continue;
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
    pub *p = pub_get();
    if (!p) return 1;
    p->rt = rt;
    int tl = cb_rt_topic(to, p->topic, sizeof p->topic);
    if (tl <= 0) { pub_free(p); return 1; }
    int pl = len <= (size_t)INT32_MAX ? mqtt_publish_len(tl, (int)len) : -1;
    if (pl > 0 && p->packet_cap < (size_t)pl) {
        free(p->packet);
        p->packet = malloc((size_t)pl);
        p->packet_cap = p->packet ? (size_t)pl : 0;
    }
    int w = pl > 0 && p->packet ? mqtt_write_publish((uint8_t *)p->packet, pl, p->topic, tl, frame, (int)len) : -1;
    if (w < 0) { pub_free(p); return 1; }
    p->packet_len = (size_t)w;
    if (cb_loop_post(loop, pub_on_loop, p) != CB_OK) { pub_free(p); return 1; }
    return 0;
}

static void local_free(cb_rt *rt) {
    local *L = rt->impl;
    while (L->head) lconn_free(L, L->head);
    free(L->byid);
    free(L->topics);
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
        reindex(L, c);   // not open: it hears nothing
        cb_mqtt_conn_free(c->mq);
        c->mq->state = CB_MQTT_CLOSED;
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

// No deliver_lanes: a push is a copy posted to the loop, so a batch has nothing to overlap.
static const cb_rt_transport LOCAL_T = { "local", local_deliver, local_free, local_forget, NULL };

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
