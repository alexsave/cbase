// fake_stripe_host.c - the program build/fake_stripe is: the fake Stripe (tools/fake_stripe) served by
// cbase's own HTTP server, with as little of the platform around it as serves at all.
//
// WHAT RUNS. cbase's loop, server, router and request pool (core/src/net, util/pool.h), put together
// here as core/test/unit_net.c puts them together, with no boot: no store, no accounts, no console, no
// realtime, no app, and no setting read from the environment (the cb_config is zeroed: no CORS origin,
// no edge secret). It listens on 127.0.0.1 at --port alone, and LISTEN_PID / LISTEN_FDS are cleared
// first, so no inherited socket can stand in for that port (net/server.c THE INHERITED LISTENER).
// What it links is cbase.mk's FAKE_STRIPE_CORE, the dispatch (handler.c) and the server's own closure,
// and nothing else: a link that needed the store would fail.
//
// WHAT IS HERE. main, the route table (one cb_route per call fake_stripe.h names, and a catch-all for
// Stripe's 404), and the translation: a cb_http_req becomes an fs_req (the headers the fake reads, the
// route's {id} raw, the form body decoded by cbase's cb_http_query_each), and the fs_res it answers
// becomes the cb_http_res. Every answer closes its connection, as the fake's always did. The fake's
// handler code is tools/fake_stripe/fake_stripe.c, apart because it speaks JSON, which core does not,
// and because the repository gate keeps tools/ off core's headers (fake_stripe.h).
#include "../../tools/fake_stripe/fake_stripe.h"

#include "ctx.h"
#include "handler.h"
#include "net/http.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"
#include "util/str.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// The request workers: several, so a slow client of one call does not hold up another (fs_handle
// takes the fake's one lock around the state, fake_stripe.h THREADS).
#define FS_WORKERS 4

// The value of the LAST header of that name, as the fake's own reader kept it, or "".
static void header_last(const cb_http_req *q, const char *name, char *out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < q->n_headers; i++)
        if (strcasecmp(q->headers[i].name, name) == 0) cb_strlcpy(out, q->headers[i].value, cap);
}

// The form body, decoded: each pair copied into the arena (decoded text is never longer than its
// escapes, so the body's length and two NULs a pair hold every one).
typedef struct { fs_req *rq; char *arena; size_t used, cap; } form_walk;

static bool form_pair(const char *key, const char *value, void *ud) {
    form_walk *w = ud;
    if (w->rq->nform >= FS_MAXKV) return false;
    size_t kl = strlen(key), vl = strlen(value);
    if (w->used + kl + vl + 2 > w->cap) return false;   // cannot happen (the arena's size), but never past it
    char *k = w->arena + w->used;
    memcpy(k, key, kl + 1);
    char *v = k + kl + 1;
    memcpy(v, value, vl + 1);
    w->used += kl + vl + 2;
    w->rq->form[w->rq->nform].k = k;
    w->rq->form[w->rq->nform].v = v;
    w->rq->nform++;
    return true;
}

static void serve(const cb_http_req *q, cb_http_res *s, fs_call call) {
    fs_req *rq = calloc(1, sizeof *rq);
    char *text = NULL, *arena = NULL;
    if (!rq) { cb_router_error(s, 503, "out of memory"); return; }
    cb_strlcpy(rq->method, q->method, sizeof rq->method);
    cb_strlcpy(rq->path, q->path, sizeof rq->path);
    // The route's {id}, raw: one too long for the fake's ids is no route of its, as before.
    if (q->n_params) {
        if (q->params[0].value_len >= sizeof rq->id) call = FS_UNKNOWN;
        else {
            memcpy(rq->id, q->params[0].value, q->params[0].value_len);
            rq->id[q->params[0].value_len] = 0;
        }
    }
    header_last(q, "Authorization", rq->auth, sizeof rq->auth);
    header_last(q, "Stripe-Account", rq->stripe_account, sizeof rq->stripe_account);
    header_last(q, "Idempotency-Key", rq->idem_key, sizeof rq->idem_key);
    rq->body = (const char *)q->body;
    rq->body_len = q->body ? q->body_len : 0;
    if (rq->body_len) {
        // cb_http_query_each reads a NUL-ended string: the body is copied (it is followed by whatever
        // the connection read next, not by a NUL).
        text = malloc(rq->body_len + 1);
        size_t cap = rq->body_len + 2 * FS_MAXKV + 2;
        arena = malloc(cap);
        if (!text || !arena) { free(text); free(arena); free(rq); cb_router_error(s, 503, "out of memory"); return; }
        memcpy(text, rq->body, rq->body_len);
        text[rq->body_len] = 0;
        form_walk w = { rq, arena, 0, cap };
        rq->form_bad = cb_http_query_each(text, form_pair, &w) < 0;
    }

    fs_res r;
    memset(&r, 0, sizeof r);
    fs_handle(call, rq, &r);
    cb_res_status(s, r.status);
    cb_res_header(s, "Content-Type", r.ctype[0] ? r.ctype : "application/json");
    if (r.location[0]) cb_res_header(s, "Location", r.location);
    if (r.replayed) cb_res_header(s, "Idempotent-Replayed", "true");
    cb_res_body(s, r.body.p, r.body.n);
    s->close = true;   // one request a connection (Connection: close), as the fake always answered
    fs_res_free(&r);
    free(text);
    free(arena);
    free(rq);
}

#define CALL(name, call) \
    static void name(cb_ctx *ctx, const cb_http_req *q, cb_http_res *s) { (void)ctx; serve(q, s, call); }
CALL(h_test_now, FS_TEST_NOW)
CALL(h_test_onboard, FS_TEST_ONBOARD)
CALL(h_test_pay, FS_TEST_PAY)
CALL(h_page_onboard, FS_PAGE_ONBOARD)
CALL(h_page_pay, FS_PAGE_PAY)
CALL(h_page_do_pay, FS_PAGE_DO_PAY)
CALL(h_page_do_cancel, FS_PAGE_DO_CANCEL)
CALL(h_account_create, FS_ACCOUNT_CREATE)
CALL(h_account_get, FS_ACCOUNT_GET)
CALL(h_account_link, FS_ACCOUNT_LINK)
CALL(h_session_create, FS_SESSION_CREATE)
CALL(h_session_get, FS_SESSION_GET)
CALL(h_session_expire, FS_SESSION_EXPIRE)
CALL(h_refund, FS_REFUND)
CALL(h_unknown, FS_UNKNOWN)

// Every call README.md lists; anything else, any method, is the catch-all's (the router passes over
// a route whose method does not fit to the next that does: router.h WHICH ROUTE WINS).
static const cb_route ROUTES[] = {
    { .method = "POST", .pattern = "/test/now", .fn = h_test_now },
    { .method = "POST", .pattern = "/test/accounts/:id/complete_onboarding", .fn = h_test_onboard },
    { .method = "POST", .pattern = "/test/sessions/:id/pay", .fn = h_test_pay },
    { .method = "GET", .pattern = "/onboard/:id", .fn = h_page_onboard },
    { .method = "GET", .pattern = "/c/pay/:id", .fn = h_page_pay },
    { .method = "POST", .pattern = "/c/pay/:id/pay", .fn = h_page_do_pay },
    { .method = "POST", .pattern = "/c/pay/:id/cancel", .fn = h_page_do_cancel },
    { .method = "POST", .pattern = "/v1/accounts", .fn = h_account_create },
    { .method = "GET", .pattern = "/v1/accounts/:id", .fn = h_account_get },
    { .method = "POST", .pattern = "/v1/account_links", .fn = h_account_link },
    { .method = "POST", .pattern = "/v1/checkout/sessions", .fn = h_session_create },
    { .method = "GET", .pattern = "/v1/checkout/sessions/:id", .fn = h_session_get },
    { .method = "POST", .pattern = "/v1/checkout/sessions/:id/expire", .fn = h_session_expire },
    { .method = "POST", .pattern = "/v1/refunds", .fn = h_refund },
    { .method = "*", .pattern = "/*", .fn = h_unknown },
};

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    int rc = fs_options(argc, argv);
    if (rc >= 0) return rc;
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    cb_log_init(CB_LOG_WARN, 0);

    static cb_config cfg;
    static cb_ctx ctx;
    ctx.cfg = &cfg;
    ctx.version = CB_VERSION;
    ctx.started_ms = cb_now_ms();
    ctx.loop = cb_loop_new();
    ctx.req_pool = cb_pool_new(FS_WORKERS, "fake_stripe");
    ctx.router = cb_router_new();
    if (!ctx.loop || !ctx.req_pool || !ctx.router) { fprintf(stderr, "fake_stripe: out of memory\n"); return 1; }
    rc = cb_router_add_all(ctx.router, ROUTES, (int)(sizeof ROUTES / sizeof ROUTES[0]));
    if (rc != CB_OK) { fprintf(stderr, "fake_stripe: routes: %s\n", cb_strerror(rc)); return 1; }
    cb_server_opts o;
    cb_server_opts_default(&o);
    o.max_body = FS_REQ_MAX;
    ctx.server = cb_server_new(&ctx, &o);
    int port = 0;
    if (!ctx.server || cb_server_listen(ctx.server, "127.0.0.1", fs_port(), &port) != CB_OK) {
        fprintf(stderr, "fake_stripe: bind 127.0.0.1:%d failed\n", fs_port());
        return 1;
    }
    ctx.bound_port = port;
    fs_listening(port);
    cb_loop_run(ctx.loop);
    return 1;   // the loop ends only on an error that is not a clean exit
}
