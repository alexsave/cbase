// storeless_app.c - the smallest storeless product (cbase/app.h storeless), in the shape a real one
// ships: one route of its own and none of the platform's (no /health, /ready, console or account
// routes), the log at "warn", and a Host allow-list. It exists so the platform's smokes
// (core/test/smoke_boot.c) run against an app that keeps no store and changes every serve default
// a product may change, as well as against the example, which keeps a store and declares them.
#include "cbase/app.h"
#include "net/http.h"

static void h_hello(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { cb_res_text(res, 200, "hello\n"); }

static const cb_route ROUTES[] = {
    { .method = "GET", .pattern = "/v1/hello", .flags = CB_ROUTE_LOOP, .fn = h_hello },
};
static const cb_route_list ROUTE_LIST = { ROUTES, (int)(sizeof ROUTES / sizeof ROUTES[0]) };
static const cb_route_list *const ROUTE_LISTS[] = { &ROUTE_LIST };

// A server on the user's own machine, bound to port 0: its loopback names at the port it bound.
static const char *const HOSTS[] = { "127.0.0.1", "localhost", "[::1]" };

static const cb_app STORELESS_APP = {
    .name = "storeless",
    .routes = ROUTE_LISTS,
    .n_route_lists = 1,
    .storeless = true,
    .log_level = "warn",   // the startup's INFO lines are not written unless CBASE_LOG_LEVEL asks
    .host_allow = HOSTS,
    .n_host_allow = (int)(sizeof HOSTS / sizeof HOSTS[0]),
};

const cb_app *const cb_main_app = &STORELESS_APP;
