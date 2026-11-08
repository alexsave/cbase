// unit_http.c - the HTTP parser, the response writer and the router's matching.
#include "t.h"

#include "cbase/app.h"
#include "handler.h"
#include "net/http.h"
#include "net/router.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>

static int parse(const char *text, size_t max_body, cb_http_req *r, char **keep) {
    // A private copy: the parser writes into its buffer.
    size_t n = strlen(text);
    char *b = malloc(n + 1);
    memcpy(b, text, n + 1);
    *keep = b;
    return cb_http_parse(b, n, max_body, r);
}

static void a_whole_request(void) {
    const char *t = "POST /functions/v1/action?x=1&y=a%20b HTTP/1.1\r\n"
                    "Host: api\r\nAuthorization: Bearer  tok.en \r\napikey: k\r\n"
                    "Content-Type: application/octet-stream\r\nContent-Length: 5\r\n\r\nhelloNEXT";
    cb_http_req r;
    char *b;
    T_EQ_INT(parse(t, 1024, &r, &b), (long long)strlen(t) - 4);
    T_EQ_STR(r.method, "POST");
    T_EQ_INT(r.meth, CB_M_POST);   // named once, at parse
    T_EQ_STR(r.path, "/functions/v1/action");
    T_EQ_STR(r.query, "x=1&y=a%20b");
    T_EQ_INT(r.minor, 1);
    T_CHECK(r.keep_alive);
    T_EQ_INT(r.n_headers, 5);
    T_EQ_STR(cb_http_header_get(&r, "content-type"), "application/octet-stream");
    T_EQ_STR(cb_http_header_get(&r, "APIKEY"), "k");
    T_EQ_STR(cb_http_bearer(&r), "tok.en");
    T_EQ_INT(r.body_len, 5);
    T_EQ_MEM(r.body, r.body_len, "hello", 5);
    char v[16];
    T_CHECK(cb_http_query_get(&r, "y", v, sizeof v));
    T_EQ_STR(v, "a b");
    T_CHECK(!cb_http_query_get(&r, "z", v, sizeof v));
    free(b);
}

static void incomplete_requests_ask_for_more(void) {
    const char *t = "GET /health HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nab";
    size_t n = strlen(t);
    char b[256];
    memcpy(b, t, n + 1);
    cb_http_req r;
    for (size_t k = 0; k < n; k++) T_EQ_INT(cb_http_parse(b, k, 1024, &r), CB_HTTP_MORE);
    T_EQ_INT(cb_http_parse(b, n, 1024, &r), CB_HTTP_MORE);   // two body bytes short
    T_EQ_STR(b, t);                                          // and the buffer untouched
}

static void refusals(void) {
    cb_http_req r;
    char *b;
    T_EQ_INT(parse("GET / HTTP/1.1\r\nContent-Length: 2000\r\n\r\n", 1024, &r, &b), -413); free(b);
    T_EQ_INT(parse("GET / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n", 1024, &r, &b), -413); free(b);
    T_EQ_INT(parse("GET / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n", 1024, &r, &b), -400); free(b);
    T_EQ_INT(parse("GET / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", 1024, &r, &b), -400); free(b);
    T_EQ_INT(parse("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 1024, &r, &b), -411); free(b);
    T_EQ_INT(parse("GET / HTTP/2.0\r\n\r\n", 1024, &r, &b), -505); free(b);
    T_EQ_INT(parse("GET noslash HTTP/1.1\r\n\r\n", 1024, &r, &b), -400); free(b);
    T_EQ_INT(parse("GET / HTTP/1.1\r\nNo colon here\r\n\r\n", 1024, &r, &b), -400); free(b);
    T_EQ_INT(parse("G(T / HTTP/1.1\r\n\r\n", 1024, &r, &b), -400); free(b);
    T_EQ_INT(parse("GET / HTTP/1.1\r\nA: b\nc\r\n\r\n", 1024, &r, &b), -400); free(b);
    // Too many headers.
    char big[8192] = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < CB_HTTP_MAX_HEADERS + 1; i++) strcat(big, "X-A: b\r\n");
    strcat(big, "\r\n");
    T_EQ_INT(parse(big, 1024, &r, &b), -431); free(b);
    // A head that never ends within the cap.
    size_t hn = CB_HTTP_MAX_HEAD + 10;
    char *head = malloc(hn + 1);
    memcpy(head, "GET / HTTP/1.1\r\nX: ", 19);
    memset(head + 19, 'a', hn - 19);
    head[hn] = 0;
    T_EQ_INT(cb_http_parse(head, hn, 1024, &r), -431);
    free(head);
}

static void keep_alive_rules(void) {
    cb_http_req r;
    char *b;
    T_CHECK(parse("GET / HTTP/1.1\r\nConnection: close\r\n\r\n", 0, &r, &b) > 0 && !r.keep_alive); free(b);
    T_CHECK(parse("GET / HTTP/1.0\r\n\r\n", 0, &r, &b) > 0 && !r.keep_alive); free(b);
    T_CHECK(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", 0, &r, &b) > 0 && r.keep_alive); free(b);
    T_CHECK(parse("GET /x HTTP/1.1\r\n\r\n", 0, &r, &b) > 0 && r.keep_alive && strcmp(r.query, "") == 0); free(b);
}

static bool count_pairs(const char *k, const char *v, void *ud) {
    int *n = ud;
    if (*n == 0) T_EQ_STR(k, "select");
    if (*n == 0) T_EQ_STR(v, "view,status");
    if (*n == 1) T_EQ_STR(k, "player_id");
    if (*n == 1) T_EQ_STR(v, "eq.u 1");
    (*n)++;
    return true;
}

static void query_iteration(void) {
    int n = 0;
    T_EQ_INT(cb_http_query_each("select=view%2Cstatus&player_id=eq.u+1&&flag", count_pairs, &n), 3);
    T_EQ_INT(n, 3);
    T_EQ_INT(cb_http_query_each("a=%zz", count_pairs, &n), CB_E_INVALID);
}

static void responses(void) {
    cb_http_res res;
    cb_res_init(&res);
    cb_res_send(&res, 415, "application/octet-stream", "\x01\x02", 2);
    cb_res_header(&res, "X-Bad", "a\r\nInjected: yes");   // refused
    cb_res_header(&res, "X-Bad: x\r\nY", "v");             // a name that splits the head: refused
    cb_res_header(&res, "X-Colon:", "v");
    T_CHECK(cb_res_has_header(&res, "content-type"));
    T_CHECK(!cb_res_has_header(&res, "X-Bad"));
    T_CHECK(!cb_res_has_header(&res, "X-Colon"));
    cb_buf out;
    cb_buf_init(&out);
    cb_res_serialize(&res, true, &out);
    const char *want = "HTTP/1.1 415 Unsupported Media Type\r\nContent-Type: application/octet-stream\r\n"
                       "Content-Length: 2\r\nConnection: keep-alive\r\n\r\n\x01\x02";
    T_EQ_MEM(out.p, out.len, want, strlen(want));
    cb_buf_reset(&out);
    cb_res_free(&res);
    cb_res_init(&res);
    res.status = 204;
    cb_res_body(&res, "ignored", 7);
    cb_res_serialize(&res, false, &out);
    T_EQ_STR(cb_buf_cstr(&out), "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
    cb_res_free(&res);
    cb_buf_free(&out);
}

static void h_a(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {}
static void h_b(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {}
static void h_c(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {}

static void routing(void) {
    cb_router *r = cb_router_new();
    T_EQ_INT(cb_router_add(r, "GET", "/rest/v1/*", 0, h_a), CB_OK);
    T_EQ_INT(cb_router_add(r, "POST", "/rest/v1/chat_messages", 0, h_b), CB_OK);
    T_EQ_INT(cb_router_add(r, "*", "/health", CB_ROUTE_LOOP, h_c), CB_OK);
    T_EQ_INT(cb_router_add(r, "GET", "/rest/v1/*", 0, h_b), CB_E_CONFLICT);
    T_EQ_INT(cb_router_add(r, "GET", "nope", 0, h_b), CB_E_INVALID);
    bool known = false;
    const cb_route *m = cb_router_match(r, cb_http_method_id("GET"), "/rest/v1/player_views", &known);
    T_CHECK(m && m->fn == h_a);
    m = cb_router_match(r, cb_http_method_id("POST"), "/rest/v1/chat_messages", &known);
    T_CHECK(m && m->fn == h_b);
    m = cb_router_match(r, cb_http_method_id("DELETE"), "/rest/v1/chat_messages", &known);
    T_CHECK(!m && known);   // 405, not 404
    m = cb_router_match(r, cb_http_method_id("HEAD"), "/health", &known);
    T_CHECK(m && m->fn == h_c);
    m = cb_router_match(r, cb_http_method_id("GET"), "/rest/v2", &known);
    T_CHECK(!m && !known);
    m = cb_router_match(r, cb_http_method_id("GET"), "/healthz", &known);
    T_CHECK(!m && !known);   // an exact pattern is not a prefix
    cb_router_free(r);
}

// The table is sorted at add: whatever order routes arrive in, the best match wins.
static void routing_priority(void) {
    const int GET = cb_http_method_id("GET"), POST = cb_http_method_id("POST"), HEAD = cb_http_method_id("HEAD");
    cb_router *r = cb_router_new();
    // prefix first, then the longer prefix, then the exact: the worst order for a first-fit scan
    T_EQ_INT(cb_router_add(r, "GET", "/v1/*", 0, h_a), CB_OK);
    T_EQ_INT(cb_router_add(r, "GET", "/v1/games/*", 0, h_b), CB_OK);
    T_EQ_INT(cb_router_add(r, "GET", "/v1/games", 0, h_c), CB_OK);
    T_EQ_INT(cb_router_add(r, "POST", "/v1/games/new", 0, h_a), CB_OK);
    bool known = false;
    const cb_route *m = cb_router_match(r, GET, "/v1/games", &known);
    T_CHECK(m && m->fn == h_c);   // exact beats the prefixes
    m = cb_router_match(r, GET, "/v1/games/7", &known);
    T_CHECK(m && m->fn == h_b);   // the longer prefix beats the shorter
    m = cb_router_match(r, GET, "/v1/gamesx", &known);
    T_CHECK(m && m->fn == h_a);   // "/v1/games" is exact, "/v1/games/" is not its prefix
    m = cb_router_match(r, GET, "/v1/", &known);
    T_CHECK(m && m->fn == h_a);
    m = cb_router_match(r, GET, "/v1", &known);
    T_CHECK(!m && !known);        // 404: shorter than every stem, and "/v1/*" needs the slash
    m = cb_router_match(r, POST, "/v1/games/new", &known);
    T_CHECK(m && m->fn == h_a);
    m = cb_router_match(r, GET, "/v1/games/new", &known);
    T_CHECK(m && m->fn == h_b);   // the exact POST route does not fit a GET: falls to the prefix
    m = cb_router_match(r, POST, "/v1/games/7", &known);
    T_CHECK(!m && known);         // 405
    m = cb_router_match(r, HEAD, "/v1/games", &known);
    T_CHECK(m && m->fn == h_c);   // HEAD answers through GET
    m = cb_router_match(r, HEAD, "/nowhere", &known);
    T_CHECK(!m && !known);
    cb_router_free(r);
}

static void h_d(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {}
static void h_e(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {}

// A request for `path` (a test's own: bound to the route that matched, as cb_handle binds it).
static const cb_route *bound(const cb_router *r, int meth, const char *path, cb_http_req *q) {
    memset(q, 0, sizeof *q);
    q->path = path;
    bool known = false;
    const cb_route *m = cb_router_match(r, meth, path, &known);
    cb_router_bind(m, q);
    return m;
}

// Path parameters: what they match, what the handler reads, and which route wins.
static void route_params(void) {
    const int GET = cb_http_method_id("GET"), POST = cb_http_method_id("POST");
    cb_router *r = cb_router_new();
    T_EQ_INT(cb_router_add(r, "GET", "/decks/:id/notes/:nid", 0, h_a), CB_OK);
    T_EQ_INT(cb_router_add(r, "GET", "/decks/:id", 0, h_b), CB_OK);
    T_EQ_INT(cb_router_add(r, "GET", "/decks/new", 0, h_c), CB_OK);         // exact, added after
    T_EQ_INT(cb_router_add(r, "GET", "/decks/*", 0, h_d), CB_OK);           // prefix
    T_EQ_INT(cb_router_add(r, "POST", "/decks/:id", 0, h_e), CB_OK);        // same shape, another method
    cb_http_req q;
    char v[64];
    const cb_route *m = bound(r, GET, "/decks/42/notes/n7", &q);
    T_CHECK(m && m->fn == h_a);
    T_EQ_INT(q.n_params, 2);
    T_CHECK(cb_route_param(&q, "id", v, sizeof v));
    T_EQ_STR(v, "42");
    T_CHECK(cb_route_param(&q, "nid", v, sizeof v));
    T_EQ_STR(v, "n7");
    T_CHECK(!cb_route_param(&q, "deck", v, sizeof v));   // no such parameter
    T_CHECK(!cb_route_param(&q, "i", v, sizeof v));      // a name is whole, not a prefix
    // Exact beats parameter beats prefix, though added in the other order.
    m = bound(r, GET, "/decks/new", &q);
    T_CHECK(m && m->fn == h_c);
    T_EQ_INT(q.n_params, 0);
    m = bound(r, GET, "/decks/7", &q);
    T_CHECK(m && m->fn == h_b);
    T_CHECK(cb_route_param(&q, "id", v, sizeof v));
    T_EQ_STR(v, "7");
    m = bound(r, POST, "/decks/7", &q);
    T_CHECK(m && m->fn == h_e);
    m = bound(r, GET, "/decks/7/notes", &q);            // no 4-segment parameter route with 3: the prefix
    T_CHECK(m && m->fn == h_d);
    T_EQ_INT(q.n_params, 0);
    m = bound(r, GET, "/decks/", &q);                   // a parameter is never empty: the prefix
    T_CHECK(m && m->fn == h_d);
    m = bound(r, GET, "/decks//notes/x", &q);
    T_CHECK(m && m->fn == h_d);
    // Encoded characters: matched raw, so "%2F" stays inside its segment; the value is decoded,
    // '+' kept, and a bad escape, a NUL or a value that does not fit is refused.
    m = bound(r, GET, "/decks/a%2Fb/notes/caf%C3%A9+x%20y", &q);
    T_CHECK(m && m->fn == h_a);
    T_CHECK(cb_route_param(&q, "id", v, sizeof v));
    T_EQ_STR(v, "a/b");
    T_CHECK(cb_route_param(&q, "nid", v, sizeof v));
    T_EQ_STR(v, "caf\xc3\xa9+x y");
    m = bound(r, GET, "/decks/%zz", &q);
    T_CHECK(m && m->fn == h_b);
    T_CHECK(!cb_route_param(&q, "id", v, sizeof v));
    m = bound(r, GET, "/decks/a%00b", &q);
    T_CHECK(!cb_route_param(&q, "id", v, sizeof v));
    m = bound(r, GET, "/decks/123456", &q);
    T_CHECK(!cb_route_param(&q, "id", v, 6));            // 6 bytes and the NUL do not fit in 6
    T_CHECK(cb_route_param(&q, "id", v, 7));
    // 405 and 404 as for any route.
    bool known = false;
    T_CHECK(!cb_router_match(r, cb_http_method_id("DELETE"), "/decks/7", &known) && known);
    T_CHECK(!cb_router_match(r, GET, "/deck/7", &known) && !known);
    cb_router_free(r);
}

// cb_handle binds the route's parameters before its handler runs.
static char g_seen[32];
static void h_seen(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    if (!cb_route_param(q, "nid", g_seen, sizeof g_seen)) strcpy(g_seen, "(none)");
}

static void route_params_reach_the_handler(void) {
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.router = cb_router_new();
    T_EQ_INT(cb_router_add(ctx.router, "GET", "/decks/:id/notes/:nid", 0, h_seen), CB_OK);
    cb_http_req q;
    char *b;
    T_CHECK(parse("GET /decks/1/notes/n%2B2?x=1 HTTP/1.1\r\n\r\n", 0, &q, &b) > 0);
    q.n_params = 3;   // whatever was there before is replaced
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&ctx, &q, &res);
    T_EQ_INT(res.status, 200);
    T_EQ_STR(g_seen, "n+2");
    T_EQ_INT(q.n_params, 2);
    cb_res_free(&res);
    free(b);
    // A route without parameters leaves none, whatever the request held.
    T_EQ_INT(cb_router_add(ctx.router, "GET", "/plain", 0, h_seen), CB_OK);
    T_CHECK(parse("GET /plain HTTP/1.1\r\n\r\n", 0, &q, &b) > 0);
    q.params[0] = (cb_http_param){ "nid", "x", 3, 1 };
    q.n_params = 1;
    cb_res_init(&res);
    cb_handle(&ctx, &q, &res);
    T_EQ_STR(g_seen, "(none)");
    T_EQ_INT(q.n_params, 0);
    cb_res_free(&res);
    free(b);
    cb_router_free(ctx.router);
}

// The head scan (http.h cb_http_scan): what the server reads to pick a body cap before the body.
static void head_scan(void) {
    cb_http_head h;
    const char *t = "PUT /decks/9/media?x=1 HTTP/1.1\r\nContent-Length: 70000\r\n\r\nab";
    T_EQ_INT(cb_http_scan(t, strlen(t), &h), 1);
    T_EQ_INT(h.meth, CB_M_PUT);
    T_EQ_MEM(h.path, h.path_len, "/decks/9/media", 14);
    T_EQ_INT((long long)h.body_len, 70000);           // no body limit of its own
    T_EQ_INT(cb_http_scan(t, 20, &h), CB_HTTP_MORE);   // the head not all there yet
    t = "GET /x HTTP/1.1\r\n\r\n";
    T_EQ_INT(cb_http_scan(t, strlen(t), &h), 1);
    T_EQ_MEM(h.path, h.path_len, "/x", 2);
    T_EQ_INT((long long)h.body_len, 0);
    t = "GET x HTTP/1.1\r\n\r\n";                  // no path a route could have: none named
    T_EQ_INT(cb_http_scan(t, strlen(t), &h), 1);
    T_EQ_INT((long long)h.path_len, 0);
    t = "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    T_EQ_INT(cb_http_scan(t, strlen(t), &h), -411);    // chunked stays refused
}

// A route's max_body: the router keeps the largest, refuses one past CB_ROUTE_MAX_BODY, matches a
// path that is not NUL-ended, and cb_handle refuses a whole body past the cap before the handler.
static int g_ran;
static void h_count(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { g_ran++; }
static void route_body_caps(void) {
    cb_router *r = cb_router_new();
    T_EQ_INT((long long)cb_router_max_body(r), 0);
    T_EQ_INT((long long)cb_router_max_body(NULL), 0);
    const cb_route rs[] = {
        { "POST", "/up", 0, h_count, 0, 5000 },
        { "POST", "/small", 0, h_count, 0, 10 },
        { "POST", "/plain", 0, h_count, 0, 0 },
        { "POST", "/huge", 0, h_count, 0, CB_ROUTE_MAX_BODY + 1 },
    };
    T_EQ_INT(cb_router_add_all(r, rs, 3), CB_OK);
    T_EQ_INT((long long)cb_router_max_body(r), 5000);
    T_EQ_INT(cb_router_add_all(r, rs + 3, 1), CB_E_INVALID);
    T_EQ_INT((long long)cb_router_max_body(r), 5000);
    bool known = false;
    const cb_route *m = cb_router_match_n(r, CB_M_POST, "/upX", 3, &known);
    T_CHECK(m && m->max_body == 5000);
    T_CHECK(!cb_router_match_n(r, CB_M_POST, "/up", 2, &known) && !known);
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.router = r;
    static const uint8_t body[11] = "0123456789";
    const struct { const char *path; size_t n; int status, ran; } cases[] = {
        { "/small", 10, 200, 1 }, { "/small", 11, 413, 0 }, { "/plain", 11, 200, 1 }, { "/up", 11, 200, 1 },
    };
    for (int i = 0; i < 4; i++) {
        cb_http_req q;
        memset(&q, 0, sizeof q);
        strcpy(q.method, "POST");
        q.path = cases[i].path;
        q.query = "";
        q.body = body;
        q.body_len = cases[i].n;
        cb_http_res res;
        cb_res_init(&res);
        g_ran = 0;
        cb_handle(&ctx, &q, &res);
        T_EQ_INT(res.status, cases[i].status);
        T_EQ_INT(g_ran, cases[i].ran);
        cb_res_free(&res);
    }
    cb_router_free(r);
}

// The first differing segment decides between two parameter patterns, whatever the order added;
// the same shape under one method is a conflict; malformed parameters are refused.
static void route_param_conflicts(void) {
    const int GET = cb_http_method_id("GET");
    for (int order = 0; order < 2; order++) {
        cb_router *r = cb_router_new();
        if (order == 0) {
            T_EQ_INT(cb_router_add(r, "GET", "/decks/:id/notes", 0, h_a), CB_OK);
            T_EQ_INT(cb_router_add(r, "GET", "/decks/new/:x", 0, h_b), CB_OK);
            T_EQ_INT(cb_router_add(r, "GET", "/:a/:b/notes", 0, h_c), CB_OK);
        } else {
            T_EQ_INT(cb_router_add(r, "GET", "/:a/:b/notes", 0, h_c), CB_OK);
            T_EQ_INT(cb_router_add(r, "GET", "/decks/new/:x", 0, h_b), CB_OK);
            T_EQ_INT(cb_router_add(r, "GET", "/decks/:id/notes", 0, h_a), CB_OK);
        }
        cb_http_req q;
        char v[16];
        const cb_route *m = bound(r, GET, "/decks/new/notes", &q);
        T_CHECK(m && m->fn == h_b);            // "new" is literal where the other has :id
        T_CHECK(cb_route_param(&q, "x", v, sizeof v));
        T_EQ_STR(v, "notes");
        m = bound(r, GET, "/decks/9/notes", &q);
        T_CHECK(m && m->fn == h_a);            // "decks" literal beats :a
        m = bound(r, GET, "/cards/9/notes", &q);
        T_CHECK(m && m->fn == h_c);
        T_CHECK(cb_route_param(&q, "a", v, sizeof v));
        T_EQ_STR(v, "cards");
        // The same shape, whatever its names, is the same route.
        T_EQ_INT(cb_router_add(r, "GET", "/decks/:deck/notes", 0, h_d), CB_E_CONFLICT);
        T_EQ_INT(cb_router_add(r, "GET", "/decks/:id/notes", 0, h_d), CB_E_CONFLICT);
        T_EQ_INT(cb_router_add(r, "*", "/decks/:deck/notes", 0, h_d), CB_OK);   // another method: not a conflict
        T_EQ_INT(cb_router_add(r, "GET", "/decks/:id/notez", 0, h_d), CB_OK);   // another shape
        cb_router_free(r);
    }
    // A pattern of parameters alone still beats every prefix.
    cb_router *r = cb_router_new();
    T_EQ_INT(cb_router_add(r, "GET", "/x/*", 0, h_a), CB_OK);
    T_EQ_INT(cb_router_add(r, "GET", "/:a/:b", 0, h_b), CB_OK);
    cb_http_req pq;
    const cb_route *pm = bound(r, GET, "/x/y", &pq);
    T_CHECK(pm && pm->fn == h_b);
    pm = bound(r, GET, "/x/y/z", &pq);
    T_CHECK(pm && pm->fn == h_a);
    cb_router_free(r);
    r = cb_router_new();
    const char *bad[] = { "/a/:", "/a/:b-c", "/a/:id/:id", "/a/:id/*", "/a/:x/:b/:c/:d/:e/:f/:g/:h/:i" };
    for (int i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) T_EQ_INT(cb_router_add(r, "GET", bad[i], 0, h_a), CB_E_INVALID);
    // A ':' inside a segment is a literal, as it always was.
    T_EQ_INT(cb_router_add(r, "GET", "/a/b:c", 0, h_a), CB_OK);
    cb_http_req q;
    T_CHECK(bound(r, GET, "/a/b:c", &q) != NULL);
    T_CHECK(bound(r, GET, "/a/bxc", &q) == NULL);
    cb_router_free(r);
}

// The headers a response carries, as one string ("Name: value\r\n..."), for exact compares.
static const char *heads(const cb_http_res *res) { return cb_buf_cstr(&res->headers); }

// One request through the router's CORS: a preflight (OPTIONS) or an answer's headers.
static void cors_run(const cb_ctx *ctx, const char *method, const char *origin, cb_http_res *res) {
    char text[256];
    if (origin) snprintf(text, sizeof text, "%s /v1/decks HTTP/1.1\r\nOrigin: %s\r\n\r\n", method, origin);
    else snprintf(text, sizeof text, "%s /v1/decks HTTP/1.1\r\n\r\n", method);
    cb_http_req r;
    char *b;
    T_CHECK(parse(text, 0, &r, &b) > 0);
    cb_res_init(res);
    if (strcmp(method, "OPTIONS") == 0) cb_router_preflight(ctx, &r, res);
    cb_router_cors(ctx, &r, res);
    free(b);
}

static const char *const APP_ORIGINS[] = { "https://app.example", "http://localhost:5173" };

// The default is deny: no app list, no deployment list, so no origin gets a CORS header and every
// preflight is refused.
static void cors_default_denies(void) {
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    const char *who[] = { "https://evil.example", "null", NULL };
    for (int i = 0; i < 3; i++) {
        cb_http_res res;
        cors_run(&ctx, "OPTIONS", who[i], &res);
        T_EQ_INT(res.status, 403);
        T_CHECK(strstr(heads(&res), "Access-Control-") == NULL);
        cb_res_free(&res);
        cors_run(&ctx, "GET", who[i], &res);
        T_CHECK(strstr(heads(&res), "Access-Control-") == NULL);
        cb_res_free(&res);
    }
}

// The app's list and the deployment's: a listed origin is echoed, with exactly the right headers;
// an unlisted one gets no Access-Control-* header and its preflight fails.
static void cors_lists(void) {
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    strcpy(cfg.allowed_origins, "https://staging.example, http://localhost:3000");
    cb_app app;
    memset(&app, 0, sizeof app);
    app.cors_origins = APP_ORIGINS;
    app.n_cors_origins = 2;
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.app = &app;
    char err[200] = "";
    T_EQ_INT(cb_router_cors_check(&app, &cfg, err, sizeof err), CB_OK);
    cb_http_res res;
    const char *ok[] = { "https://app.example", "http://localhost:5173", "https://staging.example", "http://localhost:3000" };
    for (int i = 0; i < 4; i++) {
        char want[512];
        cors_run(&ctx, "OPTIONS", ok[i], &res);
        T_EQ_INT(res.status, 200);
        snprintf(want, sizeof want,
                 "Vary: Origin\r\nAccess-Control-Allow-Origin: %s\r\n"
                 "Access-Control-Allow-Headers: authorization, content-type\r\n"
                 "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n"
                 "Access-Control-Max-Age: 7200\r\nContent-Type: text/plain; charset=utf-8\r\n", ok[i]);
        T_EQ_STR(heads(&res), want);
        cb_res_free(&res);
        cors_run(&ctx, "GET", ok[i], &res);
        snprintf(want, sizeof want, "Vary: Origin\r\nAccess-Control-Allow-Origin: %s\r\n", ok[i]);
        T_EQ_STR(heads(&res), want);
        cb_res_free(&res);
    }
    // Near misses are not the origin: a prefix, a suffix, another scheme, a port, a trailing slash.
    const char *no[] = { "https://app.exampl", "https://app.example.evil", "http://app.example", "https://app.example:444",
                         "https://app.example/", "null", "", "*", NULL };
    for (int i = 0; i < (int)(sizeof no / sizeof no[0]); i++) {
        cors_run(&ctx, "OPTIONS", no[i], &res);
        T_EQ_INT(res.status, 403);
        T_CHECK(strstr(heads(&res), "Access-Control-") == NULL);
        cb_res_free(&res);
        cors_run(&ctx, "POST", no[i], &res);
        T_EQ_STR(heads(&res), "Vary: Origin\r\n");   // the answer depends on the Origin, and says so
        cb_res_free(&res);
    }
    // A handler's own Access-Control-Allow-Origin (a public read's "*") is left alone.
    cb_http_req r;
    char *b;
    parse("GET /v1/bots HTTP/1.1\r\nOrigin: https://evil.example\r\n\r\n", 0, &r, &b);
    cb_res_init(&res);
    cb_res_header(&res, "Access-Control-Allow-Origin", "*");
    cb_router_cors(&ctx, &r, &res);
    T_EQ_STR(heads(&res), "Access-Control-Allow-Origin: *\r\n");
    cb_res_free(&res);
    free(b);
}

// Credentials go with an echoed origin only; "*" never goes with them: boot refuses the pair,
// from either list.
static void cors_credentials(void) {
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cb_app app;
    memset(&app, 0, sizeof app);
    app.cors_origins = APP_ORIGINS;
    app.n_cors_origins = 2;
    app.cors_credentials = true;
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.app = &app;
    char err[200] = "";
    T_EQ_INT(cb_router_cors_check(&app, &cfg, err, sizeof err), CB_OK);
    cb_http_res res;
    cors_run(&ctx, "GET", "https://app.example", &res);
    T_EQ_STR(heads(&res), "Vary: Origin\r\nAccess-Control-Allow-Origin: https://app.example\r\n"
                          "Access-Control-Allow-Credentials: true\r\n");
    cb_res_free(&res);
    cors_run(&ctx, "OPTIONS", "https://app.example", &res);
    T_EQ_INT(res.status, 200);
    T_CHECK(strstr(heads(&res), "Access-Control-Allow-Credentials: true\r\n") != NULL);
    cb_res_free(&res);
    cors_run(&ctx, "GET", "https://evil.example", &res);
    T_CHECK(strstr(heads(&res), "Access-Control-") == NULL);
    cb_res_free(&res);
    // "*" beside credentials, in the deployment's list or the app's: refused.
    strcpy(cfg.allowed_origins, "https://x.example, *");
    T_CHECK(cb_config_origin_any(&cfg));
    T_CHECK(!cb_config_origin_allowed(&cfg, "*"));   // "*" is the wildcard, never an origin echoed
    T_CHECK(cb_config_origin_allowed(&cfg, "https://x.example"));
    T_EQ_INT(cb_router_cors_check(&app, &cfg, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "CBASE_ALLOWED_ORIGINS") != NULL);
    cfg.allowed_origins[0] = 0;
    static const char *const any[] = { "https://app.example", "*" };
    app.cors_origins = any;
    T_EQ_INT(cb_router_cors_check(&app, &cfg, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "cors_origins") != NULL);
    // Even past boot's check, "*" goes out without credentials.
    cors_run(&ctx, "GET", "https://evil.example", &res);
    T_EQ_STR(heads(&res), "Access-Control-Allow-Origin: *\r\n");
    cb_res_free(&res);
    // Without credentials "*" is any origin, answered "*" and never with credentials.
    app.cors_credentials = false;
    T_EQ_INT(cb_router_cors_check(&app, &cfg, err, sizeof err), CB_OK);
    const char *who[] = { "https://evil.example", NULL };
    for (int i = 0; i < 2; i++) {
        cors_run(&ctx, "GET", who[i], &res);
        T_EQ_STR(heads(&res), "Access-Control-Allow-Origin: *\r\n");
        cb_res_free(&res);
        cors_run(&ctx, "OPTIONS", who[i], &res);
        T_EQ_INT(res.status, 200);
        T_CHECK(strstr(heads(&res), "Access-Control-Allow-Origin: *\r\n") != NULL);
        T_CHECK(strstr(heads(&res), "Credentials") == NULL);
        cb_res_free(&res);
    }
}

// Boot refuses an app origin that no browser would send.
static void cors_check_shapes(void) {
    cb_app app;
    memset(&app, 0, sizeof app);
    char err[200] = "";
    const char *bad[] = { "app.example", "https://", "https://app.example/", "https://app.example/x", "ftp://a.example",
                          "https://a.example, https://b.example", "https://a b", "null", "" };
    for (int i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
        const char *one[] = { bad[i] };
        app.cors_origins = one;
        app.n_cors_origins = 1;
        T_EQ_INT(cb_router_cors_check(&app, NULL, err, sizeof err), CB_E_INVALID);
    }
    const char *good[] = { "https://app.example", "http://localhost:5173", "http://127.0.0.1:8080", "http://[::1]:3000", "*" };
    for (int i = 0; i < (int)(sizeof good / sizeof good[0]); i++) {
        const char *one[] = { good[i] };
        app.cors_origins = one;
        app.n_cors_origins = 1;
        T_EQ_INT(cb_router_cors_check(&app, NULL, err, sizeof err), CB_OK);
    }
    T_EQ_INT(cb_router_cors_check(NULL, NULL, err, sizeof err), CB_OK);
}

int main(void) {
    T_RUN(a_whole_request);
    T_RUN(incomplete_requests_ask_for_more);
    T_RUN(refusals);
    T_RUN(keep_alive_rules);
    T_RUN(query_iteration);
    T_RUN(responses);
    T_RUN(routing);
    T_RUN(routing_priority);
    T_RUN(route_params);
    T_RUN(route_param_conflicts);
    T_RUN(route_params_reach_the_handler);
    T_RUN(head_scan);
    T_RUN(route_body_caps);
    T_RUN(cors_default_denies);
    T_RUN(cors_lists);
    T_RUN(cors_credentials);
    T_RUN(cors_check_shapes);
    return T_DONE();
}
