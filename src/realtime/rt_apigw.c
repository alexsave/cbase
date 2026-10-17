// rt_apigw.c - realtime under Lambda: API Gateway's WebSocket API (rt_apigw.h).
//
// API Gateway holds the sockets; each event is one invocation, so the
// subscription state lives in the store's CONN row, read once per frame and
// written back (with its topic index) only when the frame changed it.
//
// A connection's frames are NOT serialized: API Gateway invokes the function
// for each one as it arrives, so frames sent together run at once. The row is
// therefore stored by compare-and-set on its version (store.h conn_put), and
// a frame that loses re-reads and runs again; its replies are held until its
// run is the one stored. (cbase serve's rt_local runs a connection's frames
// one at a time and needs none of this.) A client that subscribes to several
// things at once sends them in one NW_WS_BATCH, so a page's subscriptions are
// one invocation and one write, and nothing races.
//
// REPLIES. With CBASE_WS_ROUTE_REPLY=yes the $default route has a route
// response, and the last message of a frame's replies rides back in the
// invocation's answer: a ping's pong and a batch's replies (one NW_WS_BATCH)
// cost no POST at all. Any messages before it (an old client's ACK ahead of
// its catch-up) are POSTed first, in order, which API Gateway delivers ahead
// of the answer. Without it every reply is a POST. A ping reads no row.
//
// The CONN row's expires_at is the DynamoDB TTL, set once when the row is
// first stored: API Gateway closes every connection within two hours, so a
// row past CONN_TTL_MS is one whose $disconnect was missed, and the table
// drops it itself. No frame pushes it forward.
#include "rt_apigw.h"

#include "aws/apigw_mgmt.h"
#include "rt_deps.h"
#include "stream.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// API Gateway's cap on a WebSocket connection is two hours (not adjustable);
// ten minutes past it covers a first write a little after $connect.
#define CONN_TTL_MS ((2LL * 3600 + 600) * 1000)

typedef struct {
    cb_ctx *ctx;
    atomic_bool warned_no_endpoint;
} apigw;

static const cb_rt_transport APIGW_T;

// The management endpoint: CBASE_WS_ENDPOINT, else the event's own
// https://<domain>/<stage> (both name the same stage when API Gateway calls us).
static void endpoint_of(cb_ctx *ctx, const cb_ws_event *ev, char *out, size_t cap) {
    if (ctx->cfg && ctx->cfg->ws_endpoint[0]) cb_strlcpy(out, ctx->cfg->ws_endpoint, cap);
    else if (ev && ev->domain[0]) snprintf(out, cap, "https://%s/%s", ev->domain, ev->stage);
    else if (cap) out[0] = 0;
}

// ---- replies ----------------------------------------------------------------------------

// A frame's reply messages, held until its subscription change is stored: a
// frame that loses the race is run again on the fresh row, and only the run
// that stored is answered (a subscription acknowledged whose topic was then
// lost is one that never hears a push).
typedef struct {
    cb_buf frames;    // each message as a 4-byte length, then its bytes
    int    n;
} held;

static void hold_reply(void *ud, const char *frame, size_t len) {
    held *h = ud;
    uint32_t n = (uint32_t)len;
    cb_buf_append(&h->frames, &n, sizeof n);
    cb_buf_append(&h->frames, frame, len);
    h->n++;
}

// A message as text: API Gateway refuses binary frames from clients and
// documents no opcode for what @connections posts, and text decodes the same
// whichever one it picks (DESIGN.md "Binary surface"). CB_OK or CB_E_NOMEM.
static int as_text(const char *frame, size_t len, cb_buf *out) {
    if (len > (size_t)INT32_MAX / 2) return CB_E_NOMEM;
    size_t cap = (len + 2) / 3 * 4;
    cb_buf_reset(out);
    if (!cb_buf_reserve(out, cap + 1)) return CB_E_NOMEM;
    int n = nw_text_encode((const uint8_t *)frame, (int)len, (char *)out->p, (int)cap);
    if (n < 0) return CB_E_NOMEM;
    out->len = (size_t)n;
    return CB_OK;
}

// One post to a connection.
static int post(const char *endpoint, const char *conn_id, const char *frame, size_t len) {
    cb_buf text;
    cb_buf_init(&text);
    int st = as_text(frame, len, &text) != CB_OK ? 500 : cb_rt_deps_g.apigw_post(endpoint, conn_id, (const char *)text.p, text.len);
    cb_buf_free(&text);
    return st;
}

// Answers the held messages: every one but the last POSTed in order, and the
// last as `answer` (the route response) when route replies are on, else
// POSTed too. False when the connection is gone.
static bool answer_held(cb_ctx *ctx, const char *endpoint, const char *conn_id, const held *h, cb_buf *answer) {
    bool route = ctx->cfg && ctx->cfg->ws_route_reply && answer;
    int i = 0;
    for (size_t at = 0; at + sizeof(uint32_t) <= h->frames.len; i++) {
        uint32_t n;
        memcpy(&n, h->frames.p + at, sizeof n);
        at += sizeof n;
        const char *frame = (const char *)h->frames.p + at;
        at += n;
        if (route && i == h->n - 1) {
            if (as_text(frame, n, answer) != CB_OK) CB_ERROR("realtime", "reply to %s: not encoded", conn_id);
            break;
        }
        if (!endpoint[0]) continue;
        int st = post(endpoint, conn_id, frame, n);
        if (st == CB_APIGW_GONE) return false;
        if (st != 200) CB_WARN("realtime", "reply to %s: %d", conn_id, st);
    }
    return true;
}

static bool same_subscriptions(const cb_conn_row *a, const cb_conn_row *b) {
    if (strcmp(a->user_id, b->user_id) != 0 || a->n_topics != b->n_topics) return false;
    for (int i = 0; i < a->n_topics; i++)
        if (strcmp(a->topics[i], b->topics[i]) != 0) return false;
    return true;
}

// Tries at a frame before it gives up on storing it. A batched client never
// races itself; an old client's three or four frames at once lose a few times.
#define CONN_CAS_TRIES 12

// The row as read, or a fresh one (none stored, or its TTL ran out).
static int load(cb_ctx *ctx, const char *conn_id, cb_conn_row *row) {
    int rc = CB_STORE_CALL(ctx->store, conn_get, conn_id, row);
    if (rc != CB_E_NOT_FOUND) return rc;
    memset(row, 0, sizeof *row);
    cb_strlcpy(row->conn_id, conn_id, sizeof row->conn_id);
    row->expires_at = cb_now_ms() + CONN_TTL_MS;
    return CB_OK;
}

static int on_default(cb_ctx *ctx, const cb_ws_event *ev, cb_buf *answer) {
    char endpoint[600];
    endpoint_of(ctx, ev, endpoint, sizeof endpoint);
    held h;
    cb_buf_init(&h.frames);
    h.n = 0;
    int ret = CB_OK;
    uint8_t *msg = malloc(ev->body_len + 1);
    int n = msg ? nw_text_decode((const char *)ev->body, (int)ev->body_len, msg, (int)ev->body_len + 1) : -1;
    if (!msg) ret = CB_E_NOMEM;
    else if (n >= 0 && cb_nw_is_ping(msg, (size_t)n)) {
        // The pong needs no row.
        cb_conn_row none;
        memset(&none, 0, sizeof none);
        cb_nw_on_frame(ctx, &none, msg, (size_t)n, hold_reply, &h);
        if (!answer_held(ctx, endpoint, ev->connection_id, &h, answer))
            CB_STORE_CALL(ctx->store, conn_delete, ev->connection_id);
    } else {
        for (int attempt = 0;; attempt++) {
            if (attempt == CONN_CAS_TRIES) {
                // Unanswered, so the client's own timeout rejoins.
                CB_ERROR("realtime", "conn %s: lost the row %d times running; frame dropped", ev->connection_id, attempt);
                break;
            }
            cb_conn_row row, was;
            int rc = load(ctx, ev->connection_id, &row);
            if (rc != CB_OK) {
                CB_ERROR("realtime", "conn %s: load failed: %s", ev->connection_id, cb_strerror(rc));
                ret = rc;
                break;
            }
            was = row;
            cb_buf_reset(&h.frames);
            h.n = 0;
            rc = n < 0 ? CB_E_INVALID : cb_nw_on_frame(ctx, &row, msg, (size_t)n, hold_reply, &h);
            if (rc == CB_E_INVALID) {
                // Not a frame of the protocol: the socket is dropped.
                if (endpoint[0]) cb_rt_deps_g.apigw_delete(endpoint, ev->connection_id);
                if (was.version) CB_STORE_CALL(ctx->store, conn_delete, ev->connection_id);
                break;
            }
            if (!same_subscriptions(&row, &was)) {
                int w;
                if (!row.user_id[0] && !row.n_topics) {
                    // Nobody and nothing: the row is not worth keeping.
                    w = was.version ? CB_STORE_CALL(ctx->store, conn_delete, ev->connection_id) : CB_OK;
                } else {
                    w = CB_STORE_CALL(ctx->store, conn_put, &was, &row);
                    if (w == CB_E_CONFLICT) continue;   // another frame of this connection stored first
                }
                if (w != CB_OK) CB_ERROR("realtime", "conn %s: save failed: %s", ev->connection_id, cb_strerror(w));
            }
            if (!answer_held(ctx, endpoint, ev->connection_id, &h, answer))
                CB_STORE_CALL(ctx->store, conn_delete, ev->connection_id);
            break;
        }
    }
    free(msg);
    cb_buf_free(&h.frames);
    return ret;
}

int cb_rt_apigw_event(cb_ctx *ctx, const cb_ws_event *ev, int *status_out, cb_buf *answer) {
    *status_out = 500;
    if (answer) cb_buf_reset(answer);
    if (!ctx->store) { *status_out = 503; return CB_E_STATE; }
    if (strcmp(ev->route_key, "$connect") == 0) {
        // Only through the edge (config.h CBASE_ORIGIN_SECRET), and only the
        // stream's protocol. Nothing is written: a connection costs the table
        // nothing until a frame names a user (on_default), so opening
        // sockets is not a way to spend the table's writes.
        if (ctx->cfg && !cb_config_origin_proven(ctx->cfg, ev->has_origin ? ev->origin : NULL)) { *status_out = 403; return CB_E_AUTH; }
        if (cb_nw_proto_of_query(ev->query) != CB_VSN_NW) { *status_out = 400; return CB_E_INVALID; }
        *status_out = 200;
        return CB_OK;
    }
    if (strcmp(ev->route_key, "$disconnect") == 0) {
        int rc = CB_STORE_CALL(ctx->store, conn_delete, ev->connection_id);
        if (rc != CB_OK && rc != CB_E_NOT_FOUND) { CB_ERROR("realtime", "disconnect %s: %s", ev->connection_id, cb_strerror(rc)); return rc; }
        *status_out = 200;
        return CB_OK;
    }
    if (strcmp(ev->route_key, "$default") == 0) {
        int rc = on_default(ctx, ev, answer);
        if (rc != CB_OK) return rc;
        *status_out = 200;
        return CB_OK;
    }
    *status_out = 400;
    return CB_E_INVALID;
}

// ---- publishing ------------------------------------------------------------------------

typedef struct {
    int64_t now;
    size_t n, cap;
    struct target { char id[64]; } *t;
} targets;

static bool collect_conn(const cb_conn_row *r, void *ud) {
    targets *ts = ud;
    if (r->expires_at && r->expires_at < ts->now) return true;   // TTL deletion lags; the row is dead
    if (ts->n == ts->cap) {
        size_t nc = ts->cap ? ts->cap * 2 : 16;
        struct target *nt = realloc(ts->t, nc * sizeof *nt);
        if (!nt) return false;
        ts->t = nt;
        ts->cap = nc;
    }
    cb_strlcpy(ts->t[ts->n].id, r->conn_id, sizeof ts->t[0].id);
    ts->n++;
    return true;
}

static int apigw_deliver(cb_rt *rt, const char *topic, const uint8_t *frame, size_t len) {
    apigw *A = rt->impl;
    cb_ctx *ctx = rt->ctx;
    char endpoint[600];
    endpoint_of(ctx, NULL, endpoint, sizeof endpoint);
    if (!endpoint[0]) {
        if (!atomic_exchange(&A->warned_no_endpoint, true)) CB_WARN("realtime", "CBASE_WS_ENDPOINT is unset: pushes are dropped");
        return 0;   // configured not to push: nothing failed
    }
    if (!ctx->store) return 0;
    int failed = 0;
    // Collected first and posted after: no store call is held open across the network.
    targets ts = { cb_now_ms(), 0, 0, NULL };
    int rc = CB_STORE_CALL(ctx->store, conns_by_topic, topic, collect_conn, &ts);
    if (rc < 0) {
        CB_ERROR("realtime", "push %s: connections not read: %s", topic, cb_strerror(rc));
        failed++;
    }
    for (size_t i = 0; i < ts.n; i++) {
        int st = post(endpoint, ts.t[i].id, (const char *)frame, len);
        if (st == CB_APIGW_GONE) CB_STORE_CALL(ctx->store, conn_delete, ts.t[i].id);
        else if (st != 200) {
            CB_WARN("realtime", "push %s to %s: %d", topic, ts.t[i].id, st);
            failed++;
        }
    }
    free(ts.t);
    return failed;
}

static void apigw_free(cb_rt *rt) {
    free(rt->impl);
    free(rt);
}

static const cb_rt_transport APIGW_T = { "apigw", apigw_deliver, apigw_free };

int cb_rt_apigw_new(cb_ctx *ctx, cb_rt **out) {
    *out = NULL;
    cb_rt *rt = calloc(1, sizeof *rt);
    apigw *A = calloc(1, sizeof *A);
    if (!rt || !A) { free(rt); free(A); return CB_E_NOMEM; }
    A->ctx = ctx;
    rt->t = &APIGW_T;
    rt->ctx = ctx;
    rt->impl = A;
    *out = rt;
    return CB_OK;
}
