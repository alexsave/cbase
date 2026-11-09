// minimal.c - the server: one route, GET /api/hello, answering "hello from cbase", and the page
// (GET /: build/page/index.html, cdom's default page with the module bundled into it, read per
// request so a rebuilt page needs no restart).
#include "cbase/app.h"   // cb_app, cb_route, cb_main_app
#include "net/files.h"   // cb_send_file
#include "net/http.h"    // cb_http_req, cb_http_res, cb_res_text

static void h_hello(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_res_text(res, 200, "hello from cbase\n");
}

static void h_page(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_send_file(ctx, req, res, "build/page/index.html", NULL, "no-store");
}

static const cb_route ROUTES[] = {
    { .method = "GET", .pattern = "/api/hello", .flags = CB_ROUTE_LOOP, .fn = h_hello },
    { .method = "GET", .pattern = "/", .fn = h_page },
};
static const cb_route_list ROUTE_LIST = { ROUTES, (int)(sizeof ROUTES / sizeof ROUTES[0]) };
static const cb_route_list *const ROUTE_LISTS[] = { &ROUTE_LIST };

static const cb_app APP = {
    .name = "minimal",
    .routes = ROUTE_LISTS, .n_route_lists = 1,
    .storeless = true,   // no store and no accounts: it runs outside dev mode with no setting at all
};
const cb_app *const cb_main_app = &APP;
