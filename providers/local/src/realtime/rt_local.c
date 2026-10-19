// rt_local.c - realtime for `cbase serve` (rt_local.h): the socket hub.
//
// GET /ws upgrades to a plain binary WebSocket. One WebSocket message is one
// frame (shared/c/stream_frame.h): the client says HELLO (its access token),
// SUB and UNSUB (a room to watch), REQ (an HTTP route's request) and PING; the
// server answers READY, REPLY and PONG and pushes the product's own frames.
//
// WHO HEARS A PUSH. No topics. A target (realtime.h cb_rt_to) is either a USER
// (its family's pattern names {user}: views, a own casts, chat), delivered
// to every open connection of that user, or a room's SPECTATORS (the pattern
// names only {doc}), delivered to the connections that sent SUB for that room.
// The hub keeps two maps and nothing per subscription: user -> connections
// (the connections chain through their user's hash bucket) and room ->
// spectators.
//
// THREADS. Every socket and both maps belong to the loop thread. A HELLO and a
// REQ run on the REQUEST POOL, because they read the store and run the same
// handler an HTTP request does, and the loop must never block on SQLite: one
// job per connection at a time, so a connection's frames are handled in the
// order they arrived; frames that come meanwhile wait in its inbox. A publish
// from any thread copies its frame and posts it to the loop.
//
// A REQ IS AN HTTP REQUEST. It names a route (method and path) and carries the
// body; the route's handler runs exactly as for HTTP, with the connection's
// user in place of the bearer (http.h auth_user), and its status and body come
// back in the REPLY. Routes that run on the loop and /v1/auth/* (a session is
// made and ended over HTTP) are refused.
//
// A SOCKET NEVER OUTLIVES ITS LOGIN. HELLO verifies the token with its session
// (auth.h cb_auth_verify_token), and the connection ends when the token does.
// cb_rt_end_sessions (sign-out, revocation, a password change, a deleted
// account) closes the sockets it names; and because that is posted to the
// loop, every REQ also compares the hub's end counter with the one its
// connection was last checked at and, when it moved, reads the session again
// before the handler runs.
#include "rt_local.h"

#include "auth/auth.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "net/ws.h"
#include "realtime/rt_deps.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include "../../../../../shared/c/stream_frame.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A frame waiting to be handled, or a reply waiting to be sent.
typedef struct msg {
    struct msg *next;
    size_t len;
    char text[];
} msg;

#define WATCH_MAX 32   // rooms one connection may watch; one more is refused (429)

typedef struct lconn {
    struct lconn *prev, *next;     // every connection
    struct lconn *hnext;           // the by-id table's chain
    struct lconn *unext;           // the by-user table's chain (open connections only)
    cb_ws *ws;
    uint64_t ws_id;
    bool hello_seen;               // the first frame came, and was a HELLO
    bool open;                     // HELLO taken: user is set
    bool busy;                     // a job is out on the pool
    cb_user *user;                 // once open, owned
    int64_t expires_s;             // once open: the connection ends then (the token's exp)
    uint64_t checked_gen;          // the end counter the session was last known live at
    msg *in_head, *in_tail;        // frames not handled yet
    size_t in_bytes;
    uint8_t nwatch;
    char **watch;                  // the rooms watched, malloc'd [0..nwatch)
} lconn;

// One room's spectators.
typedef struct gent {
    struct gent *next;
    uint64_t h;
    char *gid;
    uint64_t *ids;                 // ws ids
    uint32_t n, cap;
} gent;

typedef struct {
    cb_ctx *ctx;
    lconn *head;                   // loop thread only
    lconn **byid;                  // ws id -> lconn, chained on hnext
    lconn **byuser;                // user id -> its open connections, chained on unext (same size)
    size_t nbyid, nconn;
    gent **rooms;                  // room -> spectators, chained
    size_t ngames, nent;
    _Atomic uint64_t end_gen;      // bumped (any thread) whenever sessions were ended
} local;

#define INBOX_MAX (1u << 20)   // a client this far ahead of us is not a stream client

static const cb_rt_transport LOCAL_T;

static msg *msg_new(const void *p, size_t n) {
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

static uint64_t str_hash(const char *t) {
    uint64_t h = 1469598103934665603ull;
    for (; *t; t++) h = (h ^ (uint8_t)*t) * 1099511628211ull;
    return h;
}

// ---- the maps (loop thread only) ---------------------------------------------------

static size_t id_slot(uint64_t id, size_t nb) { return (size_t)((id * 0x9E3779B97F4A7C15ull) >> 20) & (nb - 1); }
static size_t user_slot(const char *user, size_t nb) { return (size_t)(str_hash(user) & (nb - 1)); }

static lconn *find(local *L, uint64_t ws_id) {
    if (!L->nbyid) return NULL;
    for (lconn *c = L->byid[id_slot(ws_id, L->nbyid)]; c; c = c->hnext)
        if (c->ws_id == ws_id) return c;
    return NULL;
}

static bool maps_add(local *L, lconn *c) {
    if (L->nconn + 1 > L->nbyid) {
        size_t nb = L->nbyid ? L->nbyid * 2 : 256;
        lconn **t = calloc(nb, sizeof *t), **u = calloc(nb, sizeof *u);
        if (!t || !u) { free(t); free(u); return false; }
        for (lconn *x = L->head; x; x = x->next) {   // c is not on the list yet
            size_t i = id_slot(x->ws_id, nb);
            x->hnext = t[i];
            t[i] = x;
            if (x->open) {
                size_t k = user_slot(x->user->id, nb);
                x->unext = u[k];
                u[k] = x;
            }
        }
        free(L->byid);
        free(L->byuser);
        L->byid = t;
        L->byuser = u;
        L->nbyid = nb;
    }
    size_t i = id_slot(c->ws_id, L->nbyid);
    c->hnext = L->byid[i];
    L->byid[i] = c;
    L->nconn++;
    return true;
}

static void user_add(local *L, lconn *c) {
    size_t k = user_slot(c->user->id, L->nbyid);
    c->unext = L->byuser[k];
    L->byuser[k] = c;
}

static void user_del(local *L, lconn *c) {
    if (!c->open) return;
    lconn **pp = &L->byuser[user_slot(c->user->id, L->nbyid)];
    while (*pp && *pp != c) pp = &(*pp)->unext;
    if (*pp) *pp = c->unext;
    c->unext = NULL;
}

static gent *room_find(local *L, const char *gid, uint64_t h) {
    if (!L->ngames) return NULL;
    for (gent *e = L->rooms[h & (L->ngames - 1)]; e; e = e->next)
        if (e->h == h && strcmp(e->gid, gid) == 0) return e;
    return NULL;
}

static bool room_add(local *L, const char *gid, uint64_t id) {
    uint64_t h = str_hash(gid);
    gent *e = room_find(L, gid, h);
    if (!e) {
        if (L->nent + 1 > L->ngames) {
            size_t nb = L->ngames ? L->ngames * 2 : 64;
            gent **tb = calloc(nb, sizeof *tb);
            if (!tb) return false;
            for (size_t i = 0; i < L->ngames; i++)
                for (gent *x = L->rooms[i], *nx; x; x = nx) {
                    nx = x->next;
                    x->next = tb[x->h & (nb - 1)];
                    tb[x->h & (nb - 1)] = x;
                }
            free(L->rooms);
            L->rooms = tb;
            L->ngames = nb;
        }
        e = calloc(1, sizeof *e);
        if (!e) return false;
        e->gid = strdup(gid);
        if (!e->gid) { free(e); return false; }
        e->h = h;
        e->next = L->rooms[h & (L->ngames - 1)];
        L->rooms[h & (L->ngames - 1)] = e;
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

static void room_del(local *L, const char *gid, uint64_t id) {
    uint64_t h = str_hash(gid);
    gent *e = room_find(L, gid, h);
    if (!e) return;
    for (uint32_t i = 0; i < e->n; i++)
        if (e->ids[i] == id) { e->ids[i] = e->ids[--e->n]; break; }
    if (e->n) return;
    gent **pp = &L->rooms[h & (L->ngames - 1)];
    while (*pp != e) pp = &(*pp)->next;
    *pp = e->next;
    L->nent--;
    free(e->ids);
    free(e->gid);
    free(e);
}

static int watch_index(const lconn *c, const char *gid) {
    for (int i = 0; i < c->nwatch; i++)
        if (strcmp(c->watch[i], gid) == 0) return i;
    return -1;
}

// ---- sending (loop thread) ---------------------------------------------------------

// Sends one frame to the connection with that id as a binary message. false
// when it is gone (closed before, or by this send: a failed send closes the
// socket, and on_close frees the lconn, so the caller must look it up again).
static bool send_to(local *L, uint64_t ws_id, const void *bytes, size_t len) {
    lconn *c = find(L, ws_id);
    if (!c) return false;
    cb_ws_send(c->ws, CB_WS_OP_BIN, bytes, len);
    return find(L, ws_id) != NULL;
}

static void reply_to(local *L, uint64_t ws_id, uint32_t id, uint16_t status, const void *body, size_t n) {
    uint8_t stack[256];
    uint8_t *o = n + 8 <= sizeof stack ? stack : malloc(n + 8);
    if (!o) return;
    int h = sf_put_reply_head(o, n + 8, id, status);
    if (n) memcpy(o + h, body, n);
    send_to(L, ws_id, o, n + (size_t)h);
    if (o != stack) free(o);
}

// ---- one frame on the pool -------------------------------------------------------

typedef struct {
    local *L;
    uint64_t ws_id;
    msg *frame;
    cb_user user;            // the connection's, for a REQ
    bool has_user;
    uint64_t gen;            // the end counter the connection was checked at
    int rc;                  // CB_OK, or why the connection ends
    // a HELLO's grant
    cb_user *granted;
    int64_t expires_s;
    // a REQ's reply
    uint32_t req_id;
    uint16_t status;
    cb_buf body;
} job;

static void job_free(job *j) {
    msgs_free(j->frame);
    free(j->granted);
    cb_buf_free(&j->body);
    free(j);
}

static void pump(local *L, lconn *c);

// Runs the route the frame names, as an HTTP request of the connection's user.
static void run_req(job *j, const sf_frame *f) {
    cb_ctx *ctx = j->L->ctx;
    j->req_id = f->id;
    char path[SF_PATH_MAX + 1];
    memcpy(path, f->path, f->path_len);
    path[f->path_len] = 0;
    cb_http_req req;
    memset(&req, 0, sizeof req);
    req.meth = f->meth == SF_METH_GET ? CB_M_GET : CB_M_POST;
    cb_strlcpy(req.method, f->meth == SF_METH_GET ? "GET" : "POST", sizeof req.method);
    req.path = path;
    req.query = "";
    req.minor = 1;
    req.keep_alive = true;
    cb_strlcpy(req.peer, "ws", sizeof req.peer);
    req.conn_id = j->ws_id;
    req.body = f->body_len ? f->body : NULL;
    req.body_len = f->body_len;
    req.auth_user = &j->user;
    if (f->meth == SF_METH_POST) {
        req.headers[0] = (cb_http_header){ "Content-Type", "application/octet-stream" };
        req.n_headers = 1;
    }
    bool known = false;
    const cb_route *r = ctx->router ? cb_router_match(ctx->router, req.meth, path, &known) : NULL;
    cb_http_res res;
    cb_res_init(&res);
    if (strncmp(path, "/v1/auth/", 9) == 0) j->status = 403;   // a session is made and ended over HTTP
    else if (!r || (r->flags & CB_ROUTE_LOOP)) j->status = known ? 405 : 404;
    else {
        r->fn(ctx, &req, &res);
        j->status = (uint16_t)(res.status ? res.status : 200);
        if (res.body.len) {
            cb_buf_append(&j->body, res.body.p, res.body.len);
            if (j->body.oom) { j->status = 500; cb_buf_free(&j->body); }
        }
    }
    cb_res_free(&res);
}

static void job_run_frame(job *j) {
    sf_frame f;
    if (sf_parse((const uint8_t *)j->frame->text, j->frame->len, &f) != 0) { j->rc = CB_E_INVALID; return; }
    local *L = j->L;
    cb_ctx *ctx = L->ctx;
    if (f.kind == SF_HELLO) {
        char tok[SF_TOKEN_MAX + 1];
        memcpy(tok, f.tok, f.tok_len);
        tok[f.tok_len] = 0;
        cb_user *u = calloc(1, sizeof *u);
        if (!u) { j->rc = CB_E_NOMEM; return; }
        // The token, its session and its account (rt_deps.h): a refusal ends the connection, with no reason on the wire.
        j->rc = cb_rt_deps_g.verify_token(ctx, tok, u);
        if (j->rc != CB_OK) { free(u); return; }
        j->granted = u;
        j->expires_s = u->expires_at;
        return;
    }
    if (f.kind != SF_REQ || !j->has_user) { j->rc = CB_E_INVALID; return; }
    // Sessions were ended since this connection was last checked (cb_rt_end_sessions): is it still live?
    uint64_t now_gen = atomic_load(&L->end_gen);
    if (now_gen != j->gen) {
        j->rc = cb_rt_deps_g.session_live(ctx, &j->user);
        if (j->rc != CB_OK) return;
        j->gen = now_gen;
    }
    run_req(j, &f);
}

static void job_done(cb_loop *l, void *ud) {
    job *j = ud;
    local *L = j->L;
    uint64_t id = j->ws_id;
    lconn *c = find(L, id);
    if (c) {
        c->busy = false;
        if (j->rc == CB_OK && j->granted) {
            // A session ended while the HELLO was out may have been this one's: it connects again.
            if (atomic_load(&L->end_gen) != j->gen) j->rc = CB_E_AUTH;
            else {
                c->user = j->granted;
                j->granted = NULL;
                c->open = true;
                c->expires_s = j->expires_s;
                c->checked_gen = j->gen;
                user_add(L, c);
                static const uint8_t ready[2] = { SF_V, SF_READY };
                send_to(L, id, ready, sizeof ready);
            }
        } else if (j->rc == CB_OK) {
            c->checked_gen = j->gen;
            reply_to(L, id, j->req_id, j->status, j->body.p, j->body.len);
        }
        c = find(L, id);
        if (c && j->rc != CB_OK) {
            CB_DEBUG("realtime", "socket %llu closed: %s", (unsigned long long)id, cb_strerror(j->rc));
            cb_ws_close(c->ws, 1008);
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

// SUB and UNSUB change the hub's maps, so they run here on the loop. Only a
// room's casts need a registration: the user's own views, own casts and chat
// reach every connection of theirs, and their reply is all a client waits for.
static void do_sub(local *L, lconn *c, const sf_frame *f) {
    uint16_t status = 200;
    char gid[SF_GID_MAX + 1];
    memcpy(gid, f->gid, f->gid_len);
    gid[f->gid_len] = 0;
    if (f->what == 2 && f->gid_len == 0) status = 400;
    else if (f->what == 2) {
        int at = watch_index(c, gid);
        if (f->kind == SF_UNSUB) {
            if (at >= 0) {
                room_del(L, gid, c->ws_id);
                free(c->watch[at]);
                c->watch[at] = c->watch[--c->nwatch];
            }
        } else if (at < 0) {
            if (c->nwatch >= WATCH_MAX) status = 429;
            else {
                char **nw = realloc(c->watch, ((size_t)c->nwatch + 1) * sizeof *nw);
                char *copy = nw ? strdup(gid) : NULL;
                if (!nw || !copy) { if (nw) c->watch = nw; free(copy); status = 503; }
                else {
                    c->watch = nw;
                    if (!room_add(L, gid, c->ws_id)) { free(copy); status = 503; }
                    else c->watch[c->nwatch++] = copy;
                }
            }
        }
    }
    reply_to(L, c->ws_id, f->id, status, NULL, 0);
}

// Handles the connection's queued frames in order: SUB, UNSUB and PING here,
// the first HELLO or REQ as a job (and then stops until it is done).
static void pump(local *L, lconn *c) {
    uint64_t id = c->ws_id;
    while (c && !c->busy && c->in_head) {
        msg *m = c->in_head;
        c->in_head = m->next;
        if (!c->in_head) c->in_tail = NULL;
        c->in_bytes -= m->len;
        m->next = NULL;
        sf_frame f;
        if (sf_parse((const uint8_t *)m->text, m->len, &f) != 0 || (!c->open && f.kind != SF_HELLO) ||
            (c->open && f.kind == SF_HELLO)) {
            free(m);
            cb_ws_close(c->ws, 1008);
            return;
        }
        if (c->open && cb_now_ms() / 1000 >= c->expires_s) {   // the token ran out: reconnect with a fresh one
            free(m);
            cb_ws_close(c->ws, 1008);
            return;
        }
        if (f.kind == SF_SUB || f.kind == SF_UNSUB) {
            do_sub(L, c, &f);
            free(m);
        } else if (f.kind == SF_PING) {
            static const uint8_t pong[2] = { SF_V, SF_PONG };
            send_to(L, id, pong, sizeof pong);
            free(m);
        } else {
            job *j = calloc(1, sizeof *j);
            if (!j) { free(m); cb_ws_close(c->ws, 1011); return; }
            j->L = L;
            j->ws_id = id;
            j->frame = m;
            j->gen = c->open ? c->checked_gen : atomic_load(&L->end_gen);
            if (c->open) { j->user = *c->user; j->has_user = true; }
            cb_pool *pool = L->ctx->req_pool;
            c->busy = true;
            if (pool && cb_pool_submit(pool, job_run, j) == CB_OK) return;
            // No pool (or it is stopping): handle it here. Only the tests run without one.
            job_run_frame(j);
            job_done(L->ctx->loop, j);
            return;
        }
        c = find(L, id);   // a send may have closed it
    }
}

// ---- the socket ----------------------------------------------------------------------

static void on_open(cb_ws *ws, void *ud) {
    cb_rt *rt = ud;
    local *L = rt->impl;
    lconn *c = calloc(1, sizeof *c);
    if (!c) { cb_ws_close(ws, 1011); return; }
    c->ws = ws;
    c->ws_id = cb_ws_id(ws);
    if (!maps_add(L, c)) { free(c); cb_ws_close(ws, 1011); return; }
    c->next = L->head;
    if (L->head) L->head->prev = c;
    L->head = c;
}

static void on_message(cb_ws *ws, int opcode, const uint8_t *data, size_t len, void *ud) {
    cb_rt *rt = ud;
    local *L = rt->impl;
    lconn *c = find(L, cb_ws_id(ws));
    if (!c) return;
    if (opcode == CB_WS_OP_TEXT) { cb_ws_close(ws, 1003); return; }   // frames are binary
    if (c->in_bytes + len > INBOX_MAX) { cb_ws_close(ws, 1008); return; }
    // The first frame is the HELLO and the only one: anything else first, or a second HELLO, ends it.
    if (!c->hello_seen) {
        if (len < 2 || data[0] != SF_V || data[1] != SF_HELLO) { cb_ws_close(ws, 1008); return; }
        c->hello_seen = true;
    }
    msg *m = msg_new(data, len);
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
    user_del(L, c);
    for (int i = 0; i < c->nwatch; i++) { room_del(L, c->watch[i], c->ws_id); free(c->watch[i]); }
    free(c->watch);
    lconn **pp = &L->byid[id_slot(c->ws_id, L->nbyid)];
    while (*pp && *pp != c) pp = &(*pp)->hnext;
    if (*pp) { *pp = c->hnext; L->nconn--; }
    msgs_free(c->in_head);
    free(c->user);
    free(c);
}

static void on_close(cb_ws *ws, void *ud) {
    cb_rt *rt = ud;
    local *L = rt->impl;
    lconn *c = find(L, cb_ws_id(ws));
    if (c) lconn_free(L, c);   // a job still out finds nothing and frees itself
}

static const cb_ws_handlers WS_HUB = { on_open, on_message, on_close };

// ---- publishing ------------------------------------------------------------------------

#define PUB_USER 1
#define PUB_ROOM 2

typedef struct pub {
    cb_rt *rt;
    int scope;                        // PUB_USER, PUB_ROOM
    char key[48];                     // the user id, or the room id
    size_t len, cap;
    uint8_t *frame;                   // the push, as it goes out
    struct pub *next;                 // on the free list
} pub;

// A delivery is allocated on a request thread (or the loop, for an inline request) and freed on the loop.
// musl's allocator unmaps a group when its last slot is freed, so a steady one-in, one-out stream of
// these cost an mmap and a munmap each. Up to PUB_KEEP of them, frame buffer included, wait on a
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
    free(p->frame);
    free(p);
}

// Sends the frame to each id (a send can close a socket and free its lconn, so
// the ids are collected first and looked up again).
static void send_all(local *L, const uint64_t *ids, size_t n, const uint8_t *frame, size_t len) {
    for (size_t i = 0; i < n; i++) send_to(L, ids[i], frame, len);
}

static void pub_on_loop(cb_loop *l, void *ud) {
    pub *p = ud;
    local *L = p->rt->impl;
    // A request parked on this user (or room) by the long poll looks again (net/server.h cb_server_wake).
    if (L->ctx->server) cb_server_wake(L->ctx->server, p->key);
    uint64_t stack[32];
    uint64_t *ids = stack;
    size_t n = 0, cap = sizeof stack / sizeof *stack;
    if (p->scope == PUB_USER && L->nbyid) {
        for (lconn *c = L->byuser[user_slot(p->key, L->nbyid)]; c; c = c->unext) {
            if (strcmp(c->user->id, p->key) != 0) continue;
            if (n == cap) {
                uint64_t *ni = malloc(cap * 2 * sizeof *ni);
                if (!ni) break;
                memcpy(ni, ids, n * sizeof *ni);
                if (ids != stack) free(ids);
                ids = ni;
                cap *= 2;
            }
            ids[n++] = c->ws_id;
        }
    } else if (p->scope == PUB_ROOM) {
        gent *e = room_find(L, p->key, str_hash(p->key));
        if (e) {
            if (e->n > cap) {
                ids = malloc(e->n * sizeof *ids);
                if (!ids) { ids = stack; }
                else cap = e->n;
            }
            for (uint32_t i = 0; i < e->n && n < cap; i++) ids[n++] = e->ids[i];
        }
    }
    send_all(L, ids, n, p->frame, p->len);
    if (ids != stack) free(ids);
    pub_free(p);
}

// Posted to the loop, which writes each hearer's socket: a failure to hand it
// over counts as one; what the loop's writes do later is the socket's.
static int local_deliver(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len) {
    cb_loop *loop = rt->ctx->loop;
    if (!loop || !to->family) return 0;
    // A family that names {user} is the user's own, to all their connections; one that names
    // only {doc} is the room's spectators'.
    bool user = strstr(to->family->pattern, "{user}") != NULL;
    const char *key = user ? to->user_id : to->doc_id;
    if (!key || !*key || strlen(key) >= sizeof ((pub *)0)->key) return 1;
    pub *p = pub_get();
    if (!p) return 1;
    p->rt = rt;
    p->scope = user ? PUB_USER : PUB_ROOM;
    cb_strlcpy(p->key, key, sizeof p->key);
    if (p->cap < len) {
        free(p->frame);
        p->frame = malloc(len);
        p->cap = p->frame ? len : 0;
    }
    if (!p->frame) { pub_free(p); return 1; }
    memcpy(p->frame, frame, len);
    p->len = len;
    if (cb_loop_post(loop, pub_on_loop, p) != CB_OK) { pub_free(p); return 1; }
    return 0;
}

static void local_free(cb_rt *rt) {
    local *L = rt->impl;
    while (L->head) lconn_free(L, L->head);
    free(L->byid);
    free(L->byuser);
    free(L->rooms);
    free(L);
    free(rt);
}

// The user's sockets of an ended session are closed (1008), on the loop, which owns them.
typedef struct {
    cb_rt  *rt;
    cb_uuid user;
    cb_uuid session, keep;
    bool has_session, has_keep;
} end_job;

static void end_on_loop(cb_loop *l, void *ud) {
    end_job *e = ud;
    local *L = e->rt->impl;
    uint64_t stack[16];
    uint64_t *ids = stack;
    size_t n = 0, cap = sizeof stack / sizeof *stack;
    if (L->nbyid) {
        for (lconn *c = L->byuser[user_slot(e->user, L->nbyid)]; c; c = c->unext) {
            if (strcmp(c->user->id, e->user) != 0) continue;
            if (e->has_session && strcmp(c->user->session_id, e->session) != 0) continue;
            if (e->has_keep && strcmp(c->user->session_id, e->keep) == 0) continue;
            if (n == cap) {
                uint64_t *ni = malloc(cap * 2 * sizeof *ni);
                if (!ni) break;
                memcpy(ni, ids, n * sizeof *ni);
                if (ids != stack) free(ids);
                ids = ni;
                cap *= 2;
            }
            ids[n++] = c->ws_id;
        }
    }
    for (size_t i = 0; i < n; i++) {
        lconn *c = find(L, ids[i]);
        if (c) cb_ws_close(c->ws, 1008);   // its queued frames go with it (on_close)
    }
    if (ids != stack) free(ids);
    free(e);
}

static void local_forget(cb_rt *rt, const char *user_id, const char *session_id, const char *keep_session) {
    local *L = rt->impl;
    // At once, on this thread: a REQ that starts from here on reads its session again (job_run_frame).
    atomic_fetch_add(&L->end_gen, 1);
    cb_loop *loop = rt->ctx->loop;
    end_job *e = loop ? calloc(1, sizeof *e) : NULL;
    if (!e) return;
    e->rt = rt;
    cb_strlcpy(e->user, user_id, sizeof e->user);
    if (session_id) { cb_strlcpy(e->session, session_id, sizeof e->session); e->has_session = true; }
    if (keep_session) { cb_strlcpy(e->keep, keep_session, sizeof e->keep); e->has_keep = true; }
    if (cb_loop_post(loop, end_on_loop, e) != CB_OK) free(e);
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

// GET /ws: a plain binary WebSocket, no subprotocol. The HELLO names the user.
static void h_ws(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ctx->rt || ctx->rt->t != &LOCAL_T) { cb_router_error(res, 503, "realtime is not running"); return; }
    cb_ws_upgrade(req, res, &WS_HUB, ctx->rt);
}

int cb_rt_local_routes(cb_ctx *ctx) { return cb_router_add(ctx->router, "GET", "/ws", CB_ROUTE_LOOP, h_ws); }
