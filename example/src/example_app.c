// example_app.c - the example product (example_app.h): two routes, a job, a schedule and the app
// struct.
#include "example_app.h"


#include "cbase/app.h"
#include "net/http.h"
#include "util/err.h"

#include <stdatomic.h>
#include <string.h>

static void h_hello(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_res_text(res, 200, EXAMPLE_HELLO);
}

static void h_echo(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (req->body_len > EXAMPLE_ECHO_MAX) {
        cb_res_text(res, 413, "too long\n");
        return;
    }
    cb_res_send(res, 200, "application/octet-stream", req->body, req->body_len);
}

// Designated rows name what they set, and a field cb_route gains later (docs/PLATFORM.md 2.9)
// defaults to 0 without an edit here.
static const cb_route ROUTES[] = {
    { .method = "GET", .pattern = "/v1/hello", .flags = CB_ROUTE_LOOP, .fn = h_hello },
    { .method = "POST", .pattern = "/v1/echo", .flags = CB_ROUTE_POOL, .fn = h_echo },
};
static const cb_route_list ROUTE_LIST = { ROUTES, (int)(sizeof ROUTES / sizeof ROUTES[0]) };
static const cb_route_list *const ROUTE_LISTS[] = { &ROUTE_LIST };

// A job, started by name with a key (cbase/app.h cb_job): it greets "world" and has nothing to do
// for any other key, which is CB_E_NOT_FOUND (not a failure: a retry could not help).
static int job_greet(cb_ctx *ctx, const char *key) { return strcmp(key, "world") == 0 ? CB_OK : CB_E_NOT_FOUND; }
static const cb_job JOBS[] = { { "greet", job_greet } };

// A schedule (cbase/app.h cb_schedule): counts its runs.
static atomic_long g_ticks;
static int schedule_tick(cb_ctx *ctx, int64_t now_ms) { return (int)(atomic_fetch_add(&g_ticks, 1) & 0x7fffffff); }
static const cb_schedule SCHEDULES[] = { { "tick", EXAMPLE_TICK_MS, schedule_tick, false } };

static const cb_app EXAMPLE_APP = {
    .name = "example",
    .routes = ROUTE_LISTS,
    .n_route_lists = 1,
    .health_route = true,   // the platform's routes it uses (cbase/app.h): its smokes, the deploy check
    .ready_route = true,
    .admin_routes = true,
    .auth_routes = true,
    .jobs = JOBS,
    .n_jobs = 1,
    .schedules = SCHEDULES,
    .n_schedules = 1,
};

const cb_app *const cb_main_app = &EXAMPLE_APP;
