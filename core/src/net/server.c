// The connection state machine - see server.h.
#include "server.h"

#include "loop.h"
#include "router.h"
#include "handler.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { C_HTTP, C_BUSY, C_WS };

// A connection. A WebSocket is a connection in state C_WS, so cb_ws is this struct.
struct job;
struct cb_ws {
    cb_server *srv;
    int fd;
    uint64_t id;
    int state;
    bool dead;           // the socket failed while a handler was busy: free on completion
    bool close_after;    // close once `out` is written
    bool upgrade_after;  // become a WebSocket once `out` is written
    bool closing;        // a WebSocket CLOSE has been queued
    bool buried;         // closed and on the server's grave list
    bool dirty;          // on the server's dirty list: frames queued, flushed at the end of the loop iteration
    cb_buf in, out;
    cb_http_req *req;    // in flight only: &g_req for a handler that runs on the loop, the pool job's copy otherwise
    struct job *job;     // the pool job holding that copy (server free list), NULL when req is &g_req
    cb_http_res res;
    size_t req_len;      // bytes of `in` the current request occupies
    int64_t last_active;
    int64_t started;     // first byte of the current request, 0 when none
    char peer[64];
    // WebSocket
    const cb_ws_handlers *h;
    void *ud;
    char *query;         // the upgrade request's query, kept for realtime
    cb_buf msg;          // a fragmented message being assembled
    int msg_op;          // its opcode, -1 when none
    struct cb_ws *next, *prev;   // every connection, for sweeps and shutdown
};
typedef struct cb_ws conn;

// The loop thread parses every request into this, and runs the loop-side handlers on it; a request that
// goes to the pool gets a heap copy for the time it is in flight (a connection no longer embeds 1.2 KB of it).
static cb_http_req g_req;
// recv lands here, on the loop thread, and only what a request has not finished is copied into the
// connection: an idle connection owns no read or write buffer at all.
static char g_rd[16384];

// An empty buffer is given back, not kept.
static void buf_idle(cb_buf *b) { if (b->len == 0) cb_buf_free(b); }

struct cb_server {
    cb_ctx *ctx;
    cb_server_opts o;
    int lfd;
    uint64_t next_id;
    conn *all;
    conn **dirty;                // connections with frames queued by cb_ws_send this iteration
    int ndirty, capdirty;
    struct job *free_jobs;       // loop thread only: jobs not in flight
    struct job_chunk *job_chunks;
    // Closed connections wait here until the next loop pass, so a callback
    // further up the stack that still holds one never touches freed memory.
    conn *grave;
    uint64_t reap_timer;
    int n;
    int busy;
    int64_t inline_off_until;   // loop ms: no inline dispatch before this (the valve tripped)
    int64_t inline_ran, inline_tripped;
    int64_t refused;        // connections the cap turned away since the last log line
    int64_t refused_log_ms; // when that line was last written
    bool draining;
    int64_t drain_deadline;
    uint64_t sweep_timer;
};

static void conn_close(conn *c);
static void conn_try_parse(conn *c);
static void conn_io(cb_loop *l, int fd, int ev, void *ud);

void cb_server_opts_default(cb_server_opts *o) {
    o->max_body = CB_HTTP_DEFAULT_BODY;
    o->idle_ms = 60000;
    o->head_ms = 15000;
    o->ws_idle_ms = 120000;
    o->max_conns = 16384;
    o->backlog = 512;
    o->inline_budget_us = 2000;
    o->inline_off_ms = 5000;
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return 0;
}

static cb_loop *loop_of(conn *c) { return c->srv->ctx->loop; }

// ---- writing -------------------------------------------------------------------

static void want(conn *c) {
    if (c->dead) return;
    int ev = 0;
    if (c->out.len) ev |= CB_EV_WRITE;
    if (c->state != C_BUSY && !c->close_after && !c->upgrade_after) ev |= CB_EV_READ;
    cb_loop_set(loop_of(c), c->fd, ev);
}

static void become_ws(conn *c);
static bool ws_frames(conn *c);

// Writes what it can. Returns false when the connection was closed.
static bool flush(conn *c) {
    while (c->out.len) {
        ssize_t w = send(c->fd, c->out.p, c->out.len,
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL
#else
                         0
#endif
        );
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            conn_close(c);
            return false;
        }
        cb_buf_consume(&c->out, (size_t)w);
    }
    buf_idle(&c->out);
    if (c->out.len == 0) {
        if (c->close_after) { conn_close(c); return false; }
        if (c->upgrade_after) {
            become_ws(c);
            // Frames that arrived right behind the handshake.
            if (!c->dead && c->in.len && !ws_frames(c)) return false;
            if (c->dead) return false;
        }
    }
    want(c);
    return true;
}

// ---- HTTP ----------------------------------------------------------------------

static void respond_now(conn *c, int status, const char *text) {
    cb_http_res res;
    cb_res_init(&res);
    cb_res_text(&res, status, text);
    cb_res_serialize(&res, false, &c->out);
    cb_res_free(&res);
    c->close_after = true;
    atomic_fetch_add_explicit(&c->srv->ctx->stats.requests, 1, memory_order_relaxed);
    if (status >= 500) atomic_fetch_add_explicit(&c->srv->ctx->stats.responses_5xx, 1, memory_order_relaxed);
}

// The handler has filled c->res: write it, and get ready for the next request.
// A pooled request: the job and the copy of the request the worker reads
// (~1.2 KB) are one block from a free list. Both ends run on the loop thread
// (dispatch takes one, finish and conn_free give it back), so the list has no
// lock; a block stays out of it from dispatch until the request is finished.
typedef struct job {
    struct job *next;
    cb_server *srv;
    conn *c;
    cb_http_req req;
} job;

#define JOB_CHUNK 32   // blocks allocated at once, and preallocated at start
typedef struct job_chunk {
    struct job_chunk *next;
    job j[JOB_CHUNK];
} job_chunk;

static bool jobs_grow(cb_server *s) {
    job_chunk *k = malloc(sizeof *k);
    if (!k) return false;
    k->next = s->job_chunks;
    s->job_chunks = k;
    for (int i = 0; i < JOB_CHUNK; i++) { k->j[i].next = s->free_jobs; s->free_jobs = &k->j[i]; }
    return true;
}

static job *job_get(cb_server *s) {
    if (!s->free_jobs && !jobs_grow(s)) return NULL;
    job *j = s->free_jobs;
    s->free_jobs = j->next;
    return j;
}

static void job_put(cb_server *s, job *j) { j->next = s->free_jobs; s->free_jobs = j; }

static void finish(conn *c) {
    cb_ctx *ctx = c->srv->ctx;
    bool keep = c->req->keep_alive && !c->res.close && !c->srv->draining;
    if (c->res.upgrade) keep = true;
    cb_res_serialize(&c->res, keep, &c->out);
    atomic_fetch_add_explicit(&ctx->stats.requests, 1, memory_order_relaxed);
    if (c->res.status >= 500) atomic_fetch_add_explicit(&ctx->stats.responses_5xx, 1, memory_order_relaxed);
    if (c->res.upgrade) {
        c->upgrade_after = true;
        c->h = c->res.ws_handlers;
        c->ud = c->res.ws_ud;
        c->query = strdup(c->req->query);
    } else if (!keep) {
        c->close_after = true;
    }
    cb_res_free(&c->res);
    cb_res_init(&c->res);
    if (c->job) { job_put(c->srv, c->job); c->job = NULL; }
    c->req = NULL;
    cb_buf_consume(&c->in, c->req_len);
    c->req_len = 0;
    buf_idle(&c->in);   // bytes of a pipelined next request keep it
    c->started = c->in.len ? cb_loop_now(loop_of(c)) : 0;
    c->state = C_HTTP;
    c->last_active = cb_loop_now(loop_of(c));
    if (flush(c) && c->state == C_HTTP && !c->close_after && !c->upgrade_after && c->in.len) conn_try_parse(c);
}


static void job_done(cb_loop *l, void *ud) {
    job *j = ud;
    cb_server *s = j->srv;
    conn *c = j->c;
    s->busy--;
    c->state = C_HTTP;
    if (c->dead) { conn_close(c); return; }
    finish(c);
}

static void job_run(void *ud) {
    job *j = ud;
    conn *c = j->c;
    cb_handle(j->srv->ctx, c->req, &c->res);
    // The loop frees the connection only after this post runs, so `c` is stable.
    // If the loop has stopped (shutdown), cb_server_free reclaims the connection
    // and with it the job.
    (void)cb_loop_post(j->srv->ctx->loop, job_done, j);
}

// A CB_ROUTE_INLINE request runs on the loop thread only when ALL hold: inline is enabled and the
// valve is not tripped, the request pool has nothing queued or running (so the request cannot
// reorder behind work already handed off, and no worker is about to need the CPU), and this
// is the last event ready in this loop pass (nobody else is waiting for the loop).
static bool inline_ok(cb_server *s, const cb_http_req *req) {
    cb_ctx *ctx = s->ctx;
    if (s->o.inline_budget_us <= 0 || !ctx->req_pool) return false;
    if (cb_loop_now(ctx->loop) < s->inline_off_until) return false;
    if (!cb_pool_idle(ctx->req_pool) || cb_loop_ready_left(ctx->loop) != 0) return false;
    return cb_handle_inline_ok(ctx, req);
}

void cb_server_inline_stats(const cb_server *s, int64_t *ran, int64_t *tripped) {
    if (ran) *ran = s->inline_ran;
    if (tripped) *tripped = s->inline_tripped;
}

static void dispatch(conn *c) {
    cb_server *s = c->srv;
    cb_ctx *ctx = s->ctx;
    cb_res_init(&c->res);
    c->req = &g_req;
    if (cb_handle_on_loop(ctx, c->req)) {
        cb_handle(ctx, c->req, &c->res);
        finish(c);
        return;
    }
    if (inline_ok(s, c->req)) {
        // Fast and the loop is free: no hand-off, no wake, no second post. The budget valve
        // turns this off when a handler turns out slow (a held lock, a fsync, a disk stall).
        int64_t t0 = cb_mono_us();
        cb_handle(ctx, c->req, &c->res);
        int64_t took = cb_mono_us() - t0;
        s->inline_ran++;
        if (took > s->o.inline_budget_us) {
            s->inline_tripped++;
            s->inline_off_until = cb_loop_now(ctx->loop) + s->o.inline_off_ms;
            if (s->inline_tripped == 1)
                CB_WARN("net", "inline request took %lld us (budget %d): inline dispatch is off for %d ms",
                        (long long)took, s->o.inline_budget_us, s->o.inline_off_ms);
        }
        finish(c);
        return;
    }
    job *j = job_get(s);
    if (!j) { cb_router_error(&c->res, 503, "out of memory"); finish(c); return; }
    j->req = g_req;
    c->req = &j->req;
    c->job = j;
    j->srv = s; j->c = c;
    c->state = C_BUSY;
    s->busy++;
    want(c);
    if (!ctx->req_pool || cb_pool_submit(ctx->req_pool, job_run, j) != CB_OK) {
        s->busy--;
        c->state = C_HTTP;
        cb_router_error(&c->res, 503, "server is shutting down");
        finish(c);
    }
}

static void conn_try_parse(conn *c) {
    if (c->state != C_HTTP || c->close_after || c->upgrade_after || c->in.len == 0) return;
    // cb_http_parse wants a NUL-safe buffer; cb_buf always has one past len.
    int r = cb_http_parse((char *)c->in.p, c->in.len, (size_t)c->srv->o.max_body, &g_req);
    if (r == CB_HTTP_MORE) {
        if (!c->started) c->started = cb_loop_now(loop_of(c));
        return;
    }
    if (r < 0) {
        respond_now(c, -r, cb_http_reason(-r));
        flush(c);
        return;
    }
    c->req_len = (size_t)r;
    cb_strlcpy(g_req.peer, c->peer, sizeof g_req.peer);
    g_req.conn_id = c->id;
    dispatch(c);
}

// ---- WebSocket ------------------------------------------------------------------

static void become_ws(conn *c) {
    c->upgrade_after = false;
    c->state = C_WS;
    c->msg_op = -1;
    atomic_fetch_sub_explicit(&c->srv->ctx->stats.http_conns, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->srv->ctx->stats.ws_conns, 1, memory_order_relaxed);
    if (c->h && c->h->on_open) c->h->on_open(c, c->ud);
    free(c->query);   // read once, by on_open (cb_ws_query)
    c->query = NULL;
}

static int queue_frame(conn *c, int opcode, const void *data, size_t len) {
    uint8_t hdr[14];
    size_t hn = cb_ws_frame_header(hdr, true, opcode, len, NULL);
    cb_buf_append(&c->out, hdr, hn);
    cb_buf_append(&c->out, data, len);
    return c->out.oom ? CB_E_NOMEM : CB_OK;
}

static void mark_dirty(conn *c);

int cb_ws_send(cb_ws *c, int opcode, const void *data, size_t len) {
    if (c->state != C_WS || c->closing || c->dead) return CB_E_STATE;
    if (c->out.len + len > CB_WS_MAX_QUEUED) {
        CB_WARN("ws", "conn %llu owes %zu bytes: closing it", (unsigned long long)c->id, c->out.len);
        cb_ws_close(c, 1008);
        return CB_E_FULL;
    }
    int rc = queue_frame(c, opcode, data, len);
    if (rc == CB_OK) mark_dirty(c);
    return rc;
}

int cb_ws_send_text(cb_ws *c, const char *text, size_t len) { return cb_ws_send(c, CB_WS_OP_TEXT, text, len); }

void cb_ws_close(cb_ws *c, uint16_t code) {
    if (c->state != C_WS || c->closing || c->dead) return;
    uint8_t p[2] = { (uint8_t)(code >> 8), (uint8_t)code };
    queue_frame(c, CB_WS_OP_CLOSE, p, 2);
    c->closing = true;
    c->close_after = true;
    flush(c);
}

uint64_t cb_ws_id(const cb_ws *c) { return c->id; }
const char *cb_ws_query(const cb_ws *c) { return c->query ? c->query : ""; }

// Reads every whole frame in `in`. Returns false when the connection was closed.
static bool ws_frames(conn *c) {
    for (;;) {
        cb_ws_frame f;
        int n = cb_ws_frame_parse(c->in.p, c->in.len, CB_WS_MAX_MESSAGE, &f);
        if (n == 0) return c->out.len ? flush(c) : true;   // a PONG queued above goes out now
        if (n < 0 || !f.masked) {   // RFC 6455 5.1: a client frame must be masked
            cb_ws_close(c, 1002);
            return false;
        }
        uint8_t *payload = c->in.p + f.header_len;
        cb_ws_unmask(payload, (size_t)f.len, f.mask);
        if (f.opcode == CB_WS_OP_PING) {
            if (!c->closing) queue_frame(c, CB_WS_OP_PONG, payload, (size_t)f.len);
        } else if (f.opcode == CB_WS_OP_CLOSE) {
            if (!c->closing) {
                uint8_t p[2] = { 0x03, 0xE8 };   // 1000
                queue_frame(c, CB_WS_OP_CLOSE, p, 2);
                c->closing = true;
            }
            c->close_after = true;
            cb_buf_consume(&c->in, (size_t)n);
            return flush(c);
        } else if (f.opcode == CB_WS_OP_PONG) {
            // nothing to do
        } else {
            if (f.opcode != CB_WS_OP_CONT) {
                if (c->msg_op != -1) { cb_ws_close(c, 1002); return false; }
                c->msg_op = f.opcode;
            } else if (c->msg_op == -1) {
                cb_ws_close(c, 1002);
                return false;
            }
            if (c->msg.len + f.len > CB_WS_MAX_MESSAGE) { cb_ws_close(c, 1009); return false; }
            if (f.fin && c->msg.len == 0) {
                // The common case: one frame, no copy.
                if (!c->closing && c->h && c->h->on_message) c->h->on_message(c, c->msg_op, payload, (size_t)f.len, c->ud);
                c->msg_op = -1;
            } else {
                cb_buf_append(&c->msg, payload, (size_t)f.len);
                if (f.fin) {
                    if (!c->closing && c->h && c->h->on_message) c->h->on_message(c, c->msg_op, c->msg.p, c->msg.len, c->ud);
                    cb_buf_free(&c->msg);
                    c->msg_op = -1;
                }
            }
        }
        // The handler may have closed this connection; it is still allocated
        // until the next loop pass, but must not be read further.
        if (c->dead) return false;
        cb_buf_consume(&c->in, (size_t)n);
        if (c->in.len == 0) {
            cb_buf_free(&c->in);
            return c->out.len ? flush(c) : true;
        }
    }
}

// ---- I/O and lifetime -------------------------------------------------------------

// cb_ws_send queues and marks; the loop's end-of-iteration hook writes each
// marked connection once. A commit's view and cast pushes to one player, and
// every push of a drain that reaches the same socket, leave in one send().
static void mark_dirty(conn *c) {
    cb_server *s = c->srv;
    if (c->dirty) return;
    if (s->ndirty == s->capdirty) {
        int cap = s->capdirty ? s->capdirty * 2 : 64;
        conn **d = realloc(s->dirty, (size_t)cap * sizeof *d);
        if (!d) { flush(c); return; }
        s->dirty = d;
        s->capdirty = cap;
    }
    s->dirty[s->ndirty++] = c;
    c->dirty = true;
}

static void flush_dirty(cb_loop *l, void *ud) {
    (void)l;
    cb_server *s = ud;
    // flush can close connections (the one it writes, never another), so the
    // list is walked by index and entries a free nulled are skipped.
    for (int i = 0; i < s->ndirty; i++) {
        conn *c = s->dirty[i];
        if (!c) continue;
        c->dirty = false;
        if (!c->dead && !c->buried) flush(c);
    }
    s->ndirty = 0;
}

static void conn_free(conn *c) {
    if (c->dirty) {
        cb_server *s = c->srv;
        for (int i = 0; i < s->ndirty; i++) if (s->dirty[i] == c) s->dirty[i] = NULL;
    }
    cb_buf_free(&c->in);
    cb_buf_free(&c->out);
    cb_buf_free(&c->msg);
    cb_res_free(&c->res);
    if (c->job) job_put(c->srv, c->job);
    free(c->query);
    free(c);
}

static void unlink_conn(conn *c) {
    cb_server *s = c->srv;
    if (c->prev) c->prev->next = c->next; else s->all = c->next;
    if (c->next) c->next->prev = c->prev;
    s->n--;
}

static void reap(cb_loop *l, void *ud) {
    cb_server *s = ud;
    s->reap_timer = 0;
    while (s->grave) {
        conn *c = s->grave;
        s->grave = c->next;
        conn_free(c);
    }
}

static void conn_close(conn *c) {
    cb_server *s = c->srv;
    if (c->buried) return;
    if (c->fd >= 0) {
        cb_loop_remove(s->ctx->loop, c->fd);
        close(c->fd);
        c->fd = -1;
    }
    if (c->state == C_BUSY) {
        // A handler is running against c->req/c->res: free it when it is done.
        c->dead = true;
        return;
    }
    c->dead = true;
    c->buried = true;
    if (c->state == C_WS) {
        atomic_fetch_sub_explicit(&s->ctx->stats.ws_conns, 1, memory_order_relaxed);
        if (c->h && c->h->on_close) c->h->on_close(c, c->ud);
    } else {
        atomic_fetch_sub_explicit(&s->ctx->stats.http_conns, 1, memory_order_relaxed);
    }
    unlink_conn(c);
    c->prev = NULL;
    c->next = s->grave;
    s->grave = c;
    if (!s->reap_timer) s->reap_timer = cb_loop_timer(s->ctx->loop, 0, reap, s);
}

static void conn_io(cb_loop *l, int fd, int ev, void *ud) {
    conn *c = ud;
    if (ev & CB_EV_WRITE) { if (!flush(c)) return; }
    if (ev & (CB_EV_READ | CB_EV_ERR)) {
        if (c->state == C_BUSY) {
            // Paused for reads; only an error can be reported here.
            if (ev & CB_EV_ERR) { cb_loop_remove(l, fd); close(fd); c->fd = -1; c->dead = true; }
            return;
        }
        for (;;) {
            ssize_t r = recv(fd, g_rd, sizeof g_rd, 0);
            if (r > 0) {
                cb_buf_append(&c->in, g_rd, (size_t)r);
                if (c->in.oom) { conn_close(c); return; }
                c->in.p[c->in.len] = 0;
                c->last_active = cb_loop_now(l);
                size_t limit = c->state == C_WS ? CB_WS_MAX_MESSAGE + 64 : CB_HTTP_MAX_HEAD + (size_t)c->srv->o.max_body;
                if (c->in.len > limit) {
                    if (c->state == C_WS) { cb_ws_close(c, 1009); return; }
                    respond_now(c, 413, "Content Too Large");
                    flush(c);
                    return;
                }
                if ((size_t)r < sizeof g_rd) break;   // drained the socket
                continue;
            }
            if (r == 0) { conn_close(c); return; }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            conn_close(c);
            return;
        }
        if (c->state == C_WS) { ws_frames(c); return; }
        conn_try_parse(c);
    }
}

static void on_accept(cb_loop *l, int lfd, int ev, void *ud) {
    cb_server *s = ud;
    for (;;) {
        struct sockaddr_storage sa;
        socklen_t sl = sizeof sa;
        int fd = accept(lfd, (struct sockaddr *)&sa, &sl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) CB_WARN("net", "accept: %s", strerror(errno));
            return;
        }
        if (s->n >= s->o.max_conns && !s->draining) {
            // The cap, said out loud: at most one line a minute, with how many were turned away since the last.
            s->refused++;
            int64_t now = cb_now_ms();
            if (now - s->refused_log_ms >= 60000) {
                CB_WARN("net", "connection cap %d reached: %lld refused since the last report (CBASE_MAX_CONNS)",
                        s->o.max_conns, (long long)s->refused);
                s->refused = 0;
                s->refused_log_ms = now;
            }
            close(fd);
            continue;
        }
        if (s->draining || set_nonblock(fd)) { close(fd); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        conn *c = calloc(1, sizeof *c);
        if (!c) { close(fd); continue; }
        c->srv = s;
        c->fd = fd;
        c->id = ++s->next_id;
        c->msg_op = -1;
        c->last_active = cb_loop_now(l);
        cb_buf_init(&c->in);
        cb_buf_init(&c->out);
        cb_buf_init(&c->msg);
        cb_res_init(&c->res);
        char host[INET6_ADDRSTRLEN] = "?";
        if (sa.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr, host, sizeof host);
        else if (sa.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&sa)->sin6_addr, host, sizeof host);
        cb_strlcpy(c->peer, host, sizeof c->peer);
        if (cb_loop_add(l, fd, CB_EV_READ, conn_io, c) != CB_OK) { close(fd); conn_free(c); continue; }
        c->next = s->all;
        if (s->all) s->all->prev = c;
        s->all = c;
        s->n++;
        atomic_fetch_add_explicit(&s->ctx->stats.http_conns, 1, memory_order_relaxed);
    }
}

static void sweep(cb_loop *l, void *ud) {
    cb_server *s = ud;
    int64_t now = cb_loop_now(l);
    for (conn *c = s->all, *next; c; c = next) {
        next = c->next;
        if (c->state == C_BUSY || c->dead) continue;
        if (c->state == C_WS) {
            if (now - c->last_active > s->o.ws_idle_ms) cb_ws_close(c, 1001);
        } else if (c->started && now - c->started > s->o.head_ms) {
            respond_now(c, 408, "Request Timeout");
            flush(c);
        } else if (!c->started && !c->out.len && now - c->last_active > s->o.idle_ms) {
            conn_close(c);
        }
    }
    if (s->draining) {
        bool idle = s->busy == 0;
        if (idle || now >= s->drain_deadline) {
            if (!idle) CB_WARN("net", "drain timed out with %d request(s) in flight", s->busy);
            cb_loop_stop(l);
            return;
        }
        s->sweep_timer = cb_loop_timer(l, 50, sweep, s);
        return;
    }
    s->sweep_timer = cb_loop_timer(l, 1000, sweep, s);
}

cb_server *cb_server_new(cb_ctx *ctx, const cb_server_opts *opts) {
    cb_server *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->ctx = ctx;
    if (opts) s->o = *opts; else cb_server_opts_default(&s->o);
    s->lfd = -1;
    return s;
}

int cb_server_listen(cb_server *s, const char *host, int port, int *bound_port) {
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    int gr = getaddrinfo(host, ps, &hints, &ai);
    if (gr != 0) { CB_ERROR("net", "listen %s:%d: %s", host, port, gai_strerror(gr)); return CB_E_IO; }
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    int one = 1;
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 ||
        bind(fd, ai->ai_addr, ai->ai_addrlen) < 0 || listen(fd, s->o.backlog > 0 ? s->o.backlog : 512) < 0 || set_nonblock(fd)) {
        CB_ERROR("net", "listen %s:%d: %s", host, port, strerror(errno));
        if (fd >= 0) close(fd);
        freeaddrinfo(ai);
        return CB_E_IO;
    }
    freeaddrinfo(ai);
    struct sockaddr_storage sa;
    socklen_t sl = sizeof sa;
    getsockname(fd, (struct sockaddr *)&sa, &sl);
    int bound = ntohs(sa.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&sa)->sin6_port : ((struct sockaddr_in *)&sa)->sin_port);
    if (bound_port) *bound_port = bound;
    if (cb_loop_add(s->ctx->loop, fd, CB_EV_READ, on_accept, s) != CB_OK) { close(fd); return CB_E_IO; }
    cb_loop_after(s->ctx->loop, flush_dirty, s);
    s->lfd = fd;
    s->sweep_timer = cb_loop_timer(s->ctx->loop, 1000, sweep, s);
    return CB_OK;
}

void cb_server_drain(cb_server *s, int64_t timeout_ms) {
    if (s->draining) return;
    s->draining = true;
    s->drain_deadline = cb_loop_now(s->ctx->loop) + timeout_ms;
    if (s->lfd >= 0) { cb_loop_remove(s->ctx->loop, s->lfd); close(s->lfd); s->lfd = -1; }
    for (conn *c = s->all, *next; c; c = next) {
        next = c->next;
        if (c->state == C_WS) cb_ws_close(c, 1001);
        else if (c->state == C_HTTP && !c->out.len && !c->req_len) conn_close(c);
    }
    if (s->sweep_timer) cb_loop_cancel(s->ctx->loop, s->sweep_timer);
    s->sweep_timer = cb_loop_timer(s->ctx->loop, 0, sweep, s);
}

void cb_server_free(cb_server *s) {
    if (!s) return;
    if (s->inline_ran) CB_INFO("net", "inline dispatch: %lld requests on the loop thread, valve tripped %lld times", (long long)s->inline_ran, (long long)s->inline_tripped);
    while (s->all) {
        conn *c = s->all;
        if (c->state == C_BUSY) c->state = C_HTTP;   // the pool is gone: nothing runs against it
        conn_close(c);
    }
    if (s->reap_timer) cb_loop_cancel(s->ctx->loop, s->reap_timer);
    if (s->sweep_timer) cb_loop_cancel(s->ctx->loop, s->sweep_timer);
    reap(s->ctx->loop, s);
    if (s->lfd >= 0) close(s->lfd);
    free(s->dirty);
    while (s->job_chunks) { job_chunk *k = s->job_chunks; s->job_chunks = k->next; free(k); }
    free(s);
}

void cb_server_ws_each(cb_server *s, void (*fn)(cb_ws *ws, void *ud), void *ud) {
    for (conn *c = s->all, *next; c; c = next) {
        next = c->next;
        if (c->state == C_WS && !c->dead && !c->closing) fn(c, ud);
    }
}
