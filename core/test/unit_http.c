// unit_http.c - the HTTP parser, the response writer and the router's matching.
#include "t.h"

#include "net/http.h"
#include "net/router.h"
#include "util/err.h"

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
    T_CHECK(cb_res_has_header(&res, "content-type"));
    T_CHECK(!cb_res_has_header(&res, "X-Bad"));
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

static void cors(void) {
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    strcpy(cfg.allowed_origins, "https://foolish.cards, http://localhost:3000");
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    cb_http_req r;
    char *b;
    parse("GET /v1/stream?proto=nw1 HTTP/1.1\r\nOrigin: http://localhost:3000\r\n\r\n", 0, &r, &b);
    cb_http_res res;
    cb_res_init(&res);
    cb_router_cors(&ctx, &r, &res);
    cb_buf out;
    cb_buf_init(&out);
    cb_res_serialize(&res, true, &out);
    T_CHECK(strstr(cb_buf_cstr(&out), "Access-Control-Allow-Origin: http://localhost:3000\r\n") != NULL);
    cb_buf_free(&out);
    cb_res_free(&res);
    free(b);
    parse("GET /v1/bots HTTP/1.1\r\nOrigin: https://evil.example\r\n\r\n", 0, &r, &b);
    cb_res_init(&res);
    cb_router_cors(&ctx, &r, &res);
    T_CHECK(!cb_res_has_header(&res, "Access-Control-Allow-Origin"));
    cb_res_free(&res);
    free(b);
}

int main(void) {
    T_RUN(a_whole_request);
    T_RUN(incomplete_requests_ask_for_more);
    T_RUN(refusals);
    T_RUN(keep_alive_rules);
    T_RUN(query_iteration);
    T_RUN(responses);
    T_RUN(routing);
    T_RUN(cors);
    return T_DONE();
}
