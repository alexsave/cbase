// storeless_app.c - the smallest storeless product (cbase/app.h storeless): one route of its own
// and the platform's /health, no store. It exists so the platform's smokes (core/test/smoke_boot.c)
// run against an app that keeps no store as well as against the example, which keeps one.
#include "cbase/app.h"
#include "net/http.h"

static void h_hello(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { cb_res_text(res, 200, "hello\n"); }

static const cb_route ROUTES[] = {
    { .method = "GET", .pattern = "/v1/hello", .flags = CB_ROUTE_LOOP, .fn = h_hello },
};
static const cb_route_list ROUTE_LIST = { ROUTES, (int)(sizeof ROUTES / sizeof ROUTES[0]) };
static const cb_route_list *const ROUTE_LISTS[] = { &ROUTE_LIST };

static const cb_app STORELESS_APP = {
    .name = "storeless",
    .routes = ROUTE_LISTS,
    .n_route_lists = 1,
    .storeless = true,
    .health_route = true,   // the startup-line smoke asks /health
};

const cb_app *const cb_main_app = &STORELESS_APP;
