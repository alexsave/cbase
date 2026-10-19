// unit_core.c - the entries a provider's ingress calls (core.h): an HTTP
// request counted, a CONNECT's verdict with the origin proof checked before
// the token is read, and a job with nothing to do answered as done.
#include "t.h"

#include "cbase/app.h"
#include "config.h"
#include "core.h"
#include "ctx.h"
#include "net/router.h"
#include "realtime/rt_deps.h"
#include "util/err.h"
#include "util/log.h"

#include <string.h>

#define UA     "11111111-1111-4111-8111-111111111111"
#define SECRET "edge-origin-0123456789abcdef0123456789"

static int g_status;
static void h_status(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { cb_res_text(res, g_status, "x"); }

static int http(cb_ctx *ctx, const char *path) {
    cb_http_req q;
    memset(&q, 0, sizeof q);
    snprintf(q.method, sizeof q.method, "GET");
    q.path = path;
    q.query = "";
    q.minor = 1;
    cb_http_res r;
    cb_res_init(&r);
    cb_core_http(ctx, &q, &r);
    int st = r.status;
    cb_res_free(&r);
    return st;
}

static unsigned long long requests(cb_ctx *c) { return atomic_load(&c->stats.requests); }
static unsigned long long errors(cb_ctx *c) { return atomic_load(&c->stats.responses_5xx); }

static void http_counts_requests_and_5xx(void) {
    static cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    strcpy(cfg.allowed_origins, "*");
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.router = cb_router_new();
    T_EQ_INT(cb_router_add(ctx.router, "GET", "/s", 0, h_status), CB_OK);
    // Each status as the handler answered it: 5xx counted, the rest only as requests.
    static const struct { int status; int is_5xx; } S[] = { { 200, 0 }, { 499, 0 }, { 500, 1 }, { 503, 1 }, { 599, 1 },
                                                          { 404, 0 } };
    unsigned long long want_req = 0, want_5xx = 0;
    for (size_t i = 0; i < sizeof S / sizeof S[0]; i++) {
        g_status = S[i].status;
        T_EQ_INT(http(&ctx, "/s"), S[i].status);
        want_req++;
        want_5xx += (unsigned long long)S[i].is_5xx;
        T_EQ_INT(requests(&ctx), want_req);
        T_EQ_INT(errors(&ctx), want_5xx);
    }
    // The router's own answers are requests too (a 404 and a 405 are not 5xx).
    T_EQ_INT(http(&ctx, "/none"), 404);
    T_EQ_INT(requests(&ctx), want_req + 1);
    T_EQ_INT(errors(&ctx), want_5xx);
    cb_router_free(ctx.router);
}

// The token check, counted: "tok-" UA verifies as UA, anything else is CB_E_AUTH.
static int g_verifies;
static int fake_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    g_verifies++;
    if (strcmp(jwt, "tok-" UA) != 0) return CB_E_AUTH;
    memset(out, 0, sizeof *out);
    strcpy(out->id, UA);
    out->expires_at = 2000000000;
    return CB_OK;
}

static void connect_checks_the_origin_before_the_token(void) {
    cb_rt_deps saved = cb_rt_deps_g;
    cb_rt_deps_g.verify_token = fake_verify;
    static cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.origin_secret, sizeof cfg.origin_secret, "%s", SECRET);
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    const char *tok = "tok-" UA, *id = UA "-x";
    const int64_t now = 1700000000000;
    cb_mqtt_grant g;

    // Not through the edge: refused, and the token never read.
    g_verifies = 0;
    T_EQ_INT(cb_core_connect(&ctx, tok, strlen(tok), id, strlen(id), NULL, now, &g), CB_E_AUTH);
    T_EQ_INT(cb_core_connect(&ctx, tok, strlen(tok), id, strlen(id), "", now, &g), CB_E_AUTH);
    T_EQ_INT(cb_core_connect(&ctx, tok, strlen(tok), id, strlen(id), SECRET "x", now, &g), CB_E_AUTH);
    T_EQ_INT(cb_core_connect(&ctx, tok, strlen(tok), id, strlen(id), "edge-origin", now, &g), CB_E_AUTH);
    T_EQ_INT(g_verifies, 0);

    // Through the edge: the token decides, and the grant is the user's.
    T_EQ_INT(cb_core_connect(&ctx, tok, strlen(tok), id, strlen(id), SECRET, now, &g), CB_OK);
    T_EQ_INT(g_verifies, 1);
    T_EQ_STR(g.user_id, UA);
    T_CHECK(g.expires_s > now / 1000);
    T_EQ_INT(cb_core_connect(&ctx, "tok-x", 5, id, strlen(id), SECRET, now, &g), CB_E_AUTH);
    T_EQ_INT(g_verifies, 2);

    // No secret configured: the origin is open, and the token decides.
    cfg.origin_secret[0] = 0;
    T_EQ_INT(cb_core_connect(&ctx, tok, strlen(tok), id, strlen(id), NULL, now, &g), CB_OK);
    T_EQ_INT(g_verifies, 3);
    cb_rt_deps_g = saved;
}

// ---- cb_core_job ---------------------------------------------------------------------

static int g_job_rc;
static char g_job_key[80];
static int probe_job(cb_ctx *ctx, const char *key) {
    snprintf(g_job_key, sizeof g_job_key, "%s", key);
    return g_job_rc;
}
static int g_sched_runs;
static int probe_schedule(cb_ctx *ctx, int64_t now_ms) {
    g_sched_runs++;
    return 5;
}
static const cb_job JOBS[] = { { "probe", probe_job } };
static const cb_schedule SCHEDULES[] = { { "tick", 1000, probe_schedule, false } };
static const cb_app APP = { .name = "core_test", .jobs = JOBS, .n_jobs = 1, .schedules = SCHEDULES, .n_schedules = 1 };

static void job_with_nothing_to_do_is_done(void) {
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.app = &APP;
    g_job_rc = CB_OK;
    T_EQ_INT(cb_core_job(&ctx, "probe", "k1"), CB_OK);
    T_EQ_STR(g_job_key, "k1");
    // Nothing to do: done, not a failure a retry could help.
    g_job_rc = CB_E_NOT_FOUND;
    T_EQ_INT(cb_core_job(&ctx, "probe", "k2"), CB_OK);
    T_EQ_STR(g_job_key, "k2");
    // A job the app does not declare: the same.
    T_EQ_INT(cb_core_job(&ctx, "nobody", "k3"), CB_OK);
    // Any other failure is the job's answer.
    g_job_rc = CB_E_IO;
    T_EQ_INT(cb_core_job(&ctx, "probe", "k4"), CB_E_IO);
    g_job_rc = CB_E_DB;
    T_EQ_INT(cb_core_job(&ctx, "probe", "k5"), CB_E_DB);

    // And a schedule by name, through its entry.
    g_sched_runs = 0;
    T_EQ_INT(cb_core_schedule(&ctx, "tick", 1), 5);
    T_EQ_INT(cb_core_schedule(&ctx, "", 1), 5);
    T_EQ_INT(cb_core_schedule(&ctx, "tock", 1), CB_E_NOT_FOUND);
    T_EQ_INT(g_sched_runs, 2);
}

int main(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    T_RUN(http_counts_requests_and_5xx);
    T_RUN(connect_checks_the_origin_before_the_token);
    T_RUN(job_with_nothing_to_do_is_done);
    return T_DONE();
}
