// rt_apigw.c - realtime under Lambda: API Gateway's WebSocket API (rt_apigw.h).
//
// API Gateway holds the sockets; each event is one invocation, so the
// subscription state lives in the store's CONN row, read and written around
// cb_nw_on_frame. Replies and pushes are POSTs to the management API, each a
// text frame of base64 (stream.h).
//
// A connection's frames are NOT serialized: API Gateway invokes the function
// for each one as it arrives, so a page's three subscriptions run at once. The row is
// therefore stored by compare-and-set on its version (store.h conn_put), and
// a frame that loses re-reads and runs again; its replies are held until its
// run is the one stored. (cbase serve's rt_local runs a connection's frames
// one at a time and needs none of this.)
//
// The CONN row's expires_at is the DynamoDB TTL: a connection not heard from
// for CONN_TTL_MS is dropped by the table itself (a missed $disconnect). It is
// pushed forward on a frame only when the row changes anyway or a third of the
// TTL has gone, so the 25-second heartbeats do not each cost a write.
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

#define CONN_TTL_MS (2LL * 3600 * 1000)

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

// A frame's replies are held until its subscription change is stored: a
// frame that loses the race is run again on the fresh row, and only the run
// that stored is answered (a subscription acknowledged whose topic was then
// lost is one that never hears a push).
typedef struct {
    cb_buf frames;    // each frame as a 4-byte length, then its bytes
} held;

static void hold_reply(void *ud, const char *frame, size_t len) {
    held *h = ud;
    uint32_t n = (uint32_t)len;
    cb_buf_append(&h->frames, &n, sizeof n);
    cb_buf_append(&h->frames, frame, len);
}

// One post to a connection. The message goes as base64 text: API Gateway
// refuses binary frames from clients and documents no opcode for what
// @connections posts, and text decodes the same whichever one it picks
// (DESIGN.md "Binary surface").
static int post(const char *endpoint, const char *conn_id, const char *frame, size_t len) {
    if (len > (size_t)INT32_MAX / 2) return 413;
    int cap = (int)((len + 2) / 3 * 4);
    char *text = malloc((size_t)cap + 1);
    if (!text) return 500;
    int n = nw_text_encode((const uint8_t *)frame, (int)len, text, cap);
    int st = n < 0 ? 500 : cb_rt_deps_g.apigw_post(endpoint, conn_id, text, (size_t)n);
    free(text);
    return st;
}

// Posts the held frames in order. False when the connection is gone.
static bool post_held(const char *endpoint, const char *conn_id, const held *h) {
    if (!endpoint[0]) return true;
    for (size_t at = 0; at + sizeof(uint32_t) <= h->frames.len;) {
        uint32_t n;
        memcpy(&n, h->frames.p + at, sizeof n);
        at += sizeof n;
        int st = post(endpoint, conn_id, (const char *)h->frames.p + at, n);
        at += n;
        if (st == CB_APIGW_GONE) return false;
        if (st != 200) CB_WARN("realtime", "reply to %s: %d", conn_id, st);
    }
    return true;
}

static bool same_subscriptions(const cb_conn_row *a, const cb_conn_row *b) {
    if (strcmp(a->user_id, b->user_id) != 0 || a->n_topics != b->n_topics) return false;
    for (int i = 0; i < a->n_topics; i++)
        if (strcmp(a->topics[i], b->topics[i]) != 0 || strcmp(a->join_refs[i], b->join_refs[i]) != 0) return false;
    return true;
}

// Tries at a frame before it gives up on storing it. A page joins three or
// four channels at once and each lost race costs one read and one write, so
// this is far above what a client produces.
#define CONN_CAS_TRIES 12

static int on_default(cb_ctx *ctx, const cb_ws_event *ev) {
    char endpoint[600];
    endpoint_of(ctx, ev, endpoint, sizeof endpoint);
    held h;
    cb_buf_init(&h.frames);
    int ret = CB_OK;
    for (int attempt = 0;; attempt++) {
        if (attempt == CONN_CAS_TRIES) {
            // Unanswered, so the client's own timeout rejoins.
            CB_ERROR("realtime", "conn %s: lost the row %d times running; frame dropped", ev->connection_id, attempt);
            break;
        }
        cb_conn_row row, before;
        int rc = CB_STORE_CALL(ctx->store, conn_get, ev->connection_id, &row);
        if (rc == CB_E_NOT_FOUND) {
            // The row is gone (its TTL ran out, or $connect's write was lost):
            // start afresh.
            memset(&row, 0, sizeof row);
            cb_strlcpy(row.conn_id, ev->connection_id, sizeof row.conn_id);
            row.vsn = CB_VSN_NW;
        } else if (rc != CB_OK) {
            CB_ERROR("realtime", "conn %s: load failed: %s", ev->connection_id, cb_strerror(rc));
            ret = rc;
            break;
        }
        before = row;
        cb_buf_reset(&h.frames);
        uint8_t *msg = malloc(ev->body_len + 1);
        int n = msg ? nw_text_decode((const char *)ev->body, (int)ev->body_len, msg, (int)ev->body_len + 1) : -1;
        rc = !msg ? CB_E_NOMEM : n < 0 ? CB_E_INVALID : cb_nw_on_frame(ctx, &row, msg, (size_t)n, hold_reply, &h);
        free(msg);
        if (rc == CB_E_INVALID) {
            // Not a frame of the protocol: the socket is dropped.
            if (endpoint[0]) cb_rt_deps_g.apigw_delete(endpoint, ev->connection_id);
            CB_STORE_CALL(ctx->store, conn_delete, ev->connection_id);
            break;
        }
        int64_t now = cb_now_ms();
        bool changed = !same_subscriptions(&row, &before);
        if (changed || row.expires_at - now < CONN_TTL_MS * 2 / 3) {
            row.expires_at = now + CONN_TTL_MS;
            row.version = before.version;
            int w = CB_STORE_CALL(ctx->store, conn_put, &row);
            if (w == CB_E_CONFLICT) continue;   // another frame of this connection stored first
            if (w != CB_OK) CB_ERROR("realtime", "conn %s: save failed: %s", ev->connection_id, cb_strerror(w));
        }
        if (!post_held(endpoint, ev->connection_id, &h)) CB_STORE_CALL(ctx->store, conn_delete, ev->connection_id);
        break;
    }
    cb_buf_free(&h.frames);
    return ret;
}

int cb_rt_apigw_event(cb_ctx *ctx, const cb_ws_event *ev, int *status_out) {
    *status_out = 500;
    if (!ctx->store) { *status_out = 503; return CB_E_STATE; }
    if (strcmp(ev->route_key, "$connect") == 0) {
        // The stream asks with proto=nw1; its first frame names the user.
        if (cb_nw_proto_of_query(ev->query) != CB_VSN_NW) { *status_out = 400; return CB_E_INVALID; }
        cb_conn_row row;
        memset(&row, 0, sizeof row);
        cb_strlcpy(row.conn_id, ev->connection_id, sizeof row.conn_id);
        row.vsn = CB_VSN_NW;
        row.expires_at = cb_now_ms() + CONN_TTL_MS;
        int rc = CB_STORE_CALL(ctx->store, conn_put, &row);
        if (rc != CB_OK) { CB_ERROR("realtime", "connect %s: %s", ev->connection_id, cb_strerror(rc)); return rc; }
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
        int rc = on_default(ctx, ev);
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

static void apigw_deliver(cb_rt *rt, const char *topic, const uint8_t *frame, size_t len) {
    apigw *A = rt->impl;
    cb_ctx *ctx = rt->ctx;
    char endpoint[600];
    endpoint_of(ctx, NULL, endpoint, sizeof endpoint);
    if (!endpoint[0]) {
        if (!atomic_exchange(&A->warned_no_endpoint, true)) CB_WARN("realtime", "CBASE_WS_ENDPOINT is unset: pushes are dropped");
        return;
    }
    if (!ctx->store) return;
    // Collected first and posted after: no store call is held open across the network.
    targets ts = { cb_now_ms(), 0, 0, NULL };
    int rc = CB_STORE_CALL(ctx->store, conns_by_topic, topic, collect_conn, &ts);
    if (rc < 0) CB_ERROR("realtime", "push %s: connections not read: %s", topic, cb_strerror(rc));
    for (size_t i = 0; i < ts.n; i++) {
        int st = post(endpoint, ts.t[i].id, (const char *)frame, len);
        if (st == CB_APIGW_GONE) CB_STORE_CALL(ctx->store, conn_delete, ts.t[i].id);
        else if (st != 200) CB_WARN("realtime", "push %s to %s: %d", topic, ts.t[i].id, st);
    }
    free(ts.t);
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
