// rt_apigw.c - realtime under Lambda: API Gateway's WebSocket API (rt_apigw.h).
//
// API Gateway holds the sockets; each event is one invocation, so the
// subscription state lives in the store's CONN row, read and written around
// cb_phx_on_frame. Replies and pushes are POSTs to the management API.
//
// The CONN row's expires_at is the DynamoDB TTL: a connection not heard from
// for CONN_TTL_MS is dropped by the table itself (a missed $disconnect). It is
// pushed forward on a frame only when the row changes anyway or a third of the
// TTL has gone, so the 25-second heartbeats do not each cost a write.
#include "rt_apigw.h"

#include "aws/apigw_mgmt.h"
#include "phoenix.h"
#include "rt_deps.h"
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

typedef struct { const char *key; char *out; size_t cap; bool found; } qget;

static bool qget_fn(const char *key, const char *value, void *ud) {
    qget *q = ud;
    if (strcmp(key, q->key) != 0) return true;
    q->found = cb_strlcpy(q->out, value, q->cap) < q->cap;
    return false;
}

static bool apikey_ok(cb_ctx *ctx, const char *query) {
    if (!ctx->cfg || !ctx->cfg->anon_key[0]) return true;   // the check is off (router.h)
    char key[sizeof ctx->cfg->anon_key];
    qget q = { "apikey", key, sizeof key, false };
    if (cb_http_query_each(query ? query : "", qget_fn, &q) < 0 || !q.found) return false;
    return strcmp(key, ctx->cfg->anon_key) == 0;
}

// ---- replies -------------------------------------------------------------------------

typedef struct {
    const char *endpoint;
    const char *conn_id;
    bool gone;
} replier;

static void post_reply(void *ud, const char *frame, size_t len) {
    replier *r = ud;
    if (r->gone || !r->endpoint[0]) return;
    int st = cb_rt_deps_g.apigw_post(r->endpoint, r->conn_id, frame, len);
    if (st == CB_APIGW_GONE) r->gone = true;
    else if (st != 200) CB_WARN("realtime", "reply to %s: %d", r->conn_id, st);
}

static bool same_subscriptions(const cb_conn_row *a, const cb_conn_row *b) {
    if (strcmp(a->user_id, b->user_id) != 0 || a->n_topics != b->n_topics) return false;
    for (int i = 0; i < a->n_topics; i++)
        if (strcmp(a->topics[i], b->topics[i]) != 0 || strcmp(a->join_refs[i], b->join_refs[i]) != 0) return false;
    return true;
}

static int on_default(cb_ctx *ctx, const cb_ws_event *ev) {
    cb_conn_row row, before;
    int rc = CB_STORE_CALL(ctx->store, conn_get, ev->connection_id, &row);
    if (rc == CB_E_NOT_FOUND) {
        // The row is gone (its TTL ran out, or $connect's write was lost):
        // start afresh, taking the serializer from the frame's own shape.
        memset(&row, 0, sizeof row);
        cb_strlcpy(row.conn_id, ev->connection_id, sizeof row.conn_id);
        row.vsn = ev->body_len && ev->body[0] == '[' ? 2 : 1;
    } else if (rc != CB_OK) {
        CB_ERROR("realtime", "conn %s: load failed: %s", ev->connection_id, cb_strerror(rc));
        return rc;
    }
    before = row;
    char endpoint[600];
    endpoint_of(ctx, ev, endpoint, sizeof endpoint);
    replier r = { endpoint, ev->connection_id, false };
    rc = cb_phx_on_frame(ctx, &row, (const char *)ev->body, ev->body_len, post_reply, &r);
    if (r.gone) { CB_STORE_CALL(ctx->store, conn_delete, ev->connection_id); return CB_OK; }
    if (rc == CB_E_INVALID) {
        // Not a Phoenix frame: Phoenix drops such a socket, and so do we.
        if (endpoint[0]) cb_rt_deps_g.apigw_delete(endpoint, ev->connection_id);
        CB_STORE_CALL(ctx->store, conn_delete, ev->connection_id);
        return CB_OK;
    }
    int64_t now = cb_now_ms();
    bool changed = !same_subscriptions(&row, &before);
    if (changed || row.expires_at - now < CONN_TTL_MS * 2 / 3) {
        row.expires_at = now + CONN_TTL_MS;
        int w = CB_STORE_CALL(ctx->store, conn_put, &row);
        if (w != CB_OK) CB_ERROR("realtime", "conn %s: save failed: %s", ev->connection_id, cb_strerror(w));
    }
    return CB_OK;
}

int cb_rt_apigw_event(cb_ctx *ctx, const cb_ws_event *ev, int *status_out) {
    *status_out = 500;
    if (!ctx->store) { *status_out = 503; return CB_E_STATE; }
    if (strcmp(ev->route_key, "$connect") == 0) {
        if (!apikey_ok(ctx, ev->query)) { *status_out = 401; return CB_E_AUTH; }
        int vsn = cb_phx_vsn_of_query(ev->query);
        if (vsn < 0) { *status_out = 400; return CB_E_INVALID; }
        cb_conn_row row;
        memset(&row, 0, sizeof row);
        cb_strlcpy(row.conn_id, ev->connection_id, sizeof row.conn_id);
        row.vsn = vsn;
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
    struct target { char id[64]; int vsn; } *t;
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
    ts->t[ts->n].vsn = r->vsn;
    ts->n++;
    return true;
}

static void apigw_deliver(cb_rt *rt, const char *topic, const char *const frames[2], const size_t lens[2]) {
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
        int v = ts.t[i].vsn == 2 ? 1 : 0;
        int st = cb_rt_deps_g.apigw_post(endpoint, ts.t[i].id, frames[v], lens[v]);
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
