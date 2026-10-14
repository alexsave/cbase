// unit_unfurl.c - GET /m/<payload> (app/unfurl_http.h) against the TypeScript
// route's goldens (fixture_unfurl.h), and the router's HEAD (net/router.h).
#include "t.h"

#include "app/unfurl_http.h"
#include "config.h"
#include "fixture_unfurl.h"
#include "handler.h"
#include "net/http.h"
#include "net/router.h"
#include "util/err.h"

#include <stdlib.h>

// The page's <title> and <meta> tags in order, leaving out the three the TS
// page did not have from this route (charset, viewport, the refresh), each
// copied as written. Returns how many were found (at most cap).
static int page_tags(const char *html, char out[][1024], int cap) {
    int n = 0;
    for (const char *p = html; (p = strchr(p, '<')) != NULL; p++) {
        const char *end;
        if (strncmp(p, "<title>", 7) == 0) {
            end = strstr(p, "</title>");
            if (!end) break;
            end += 8;
        } else if (strncmp(p, "<meta ", 6) == 0) {
            end = strstr(p, "/>");
            if (!end) break;
            end += 2;
        } else {
            continue;
        }
        size_t len = (size_t)(end - p);
        if (strstr(p, "charset=") == p + 6 || strncmp(p, "<meta name=\"viewport\"", 21) == 0 ||
            strncmp(p, "<meta http-equiv=", 17) == 0)
            continue;
        if (n < cap && len < 1024) { memcpy(out[n], p, len); out[n][len] = 0; }
        n++;
    }
    return n;
}

// Every golden, byte for byte and in order.
static void goldens(void) {
    T_CHECK(UNFURL_N_GOLDENS >= 18);
    for (int i = 0; i < UNFURL_N_GOLDENS; i++) {
        const unfurl_golden *g = &UNFURL_GOLDENS[i];
        cb_buf html;
        cb_buf_init(&html);
        cb_unfurl_page(g->payload, strlen(g->payload), &html);
        static char tags[16][1024];
        int n = page_tags(cb_buf_cstr(&html), tags, 16);
        if (n != UNFURL_N_TAGS) fprintf(stderr, "     case %s\n", g->label);
        T_EQ_INT(n, UNFURL_N_TAGS);
        for (int k = 0; k < UNFURL_N_TAGS && k < n; k++) {
            if (strcmp(tags[k], g->tags[k]) != 0) fprintf(stderr, "     case %s, tag %d\n", g->label, k);
            T_EQ_STR(tags[k], g->tags[k]);
        }
        cb_buf_free(&html);
    }
}

// The meta strings the tags are made of, unescaped: the cases that tell the
// three titles and the fallback apart, so a failure names the rule.
static void titles(void) {
    static const struct { const char *label, *title; } want[] = {
        { "live3_turn10", "Ann0 vs Ann1 vs Ann2 - turn 10" },
        { "finished4", "Alex vs Mira vs Jonas vs Priya - a finished Durak game" },
        { "rematch4", "Alex vs Mira vs Jonas vs Priya - a rematch, waiting to start" },
        { "lobby_one_name", "4 players - turn 0" },
        { "names_html", "<b>Ann</b> vs Tom & \"Jerry\" 'J' - turn 12" },
        { "names_bad_utf8", "A\xef\xbf\xbd" "B vs C\xef\xbf\xbd - turn 12" },
        { "replay_refuses", "A Durak game in iMessage" },
        { "bad_escape", "A Durak game in iMessage" },
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        const unfurl_golden *g = NULL;
        for (int k = 0; k < UNFURL_N_GOLDENS; k++)
            if (strcmp(UNFURL_GOLDENS[k].label, want[i].label) == 0) g = &UNFURL_GOLDENS[k];
        T_CHECK(g != NULL);
        if (!g) continue;
        cb_buf t, d;
        cb_buf_init(&t);
        cb_buf_init(&d);
        cb_unfurl_meta(g->payload, strlen(g->payload), &t, &d);
        T_EQ_STR(cb_buf_cstr(&t), want[i].title);
        T_EQ_STR(cb_buf_cstr(&d), strcmp(want[i].title, "A Durak game in iMessage") == 0
                                      ? "Foolish turns a message thread into a card table."
                                      : "A Durak game in iMessage. Foolish turns a message thread into a card table.");
        cb_buf_free(&t);
        cb_buf_free(&d);
    }
}

// ---- through the handler ----------------------------------------------------------

static cb_config g_cfg;
static cb_ctx g_ctx;

typedef struct {
    int    status;
    char   headers[2048];
    char   wire[32768];   // cb_res_serialize's bytes
    size_t body_len;
} reply;

static void request(const char *method, const char *path, reply *out) {
    cb_http_req req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "%s", method);
    req.path = path;
    req.query = "";
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&g_ctx, &req, &res);
    out->status = res.status;
    snprintf(out->headers, sizeof out->headers, "%s", cb_buf_cstr(&res.headers));
    out->body_len = res.body.len;
    cb_buf wire;
    cb_buf_init(&wire);
    cb_res_serialize(&res, true, &wire);
    snprintf(out->wire, sizeof out->wire, "%s", cb_buf_cstr(&wire));
    cb_buf_free(&wire);
    cb_res_free(&res);
}

static void setup(bool unfurl_only) {
    char err[256];
    cb_config_from_env(&g_cfg, err, sizeof err);
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.cfg = &g_cfg;
    g_ctx.mode = CB_MODE_SERVE;
    g_ctx.router = cb_router_new();
    if (unfurl_only) T_EQ_INT(cb_unfurl_routes(&g_ctx), CB_OK);
    else T_EQ_INT(cb_routes_register(&g_ctx), CB_OK);
}

static const char *body_of(const char *wire) {
    const char *b = strstr(wire, "\r\n\r\n");
    return b ? b + 4 : "";
}

static void get_and_head(void) {
    char path[4096];
    snprintf(path, sizeof path, "/m/%s", UNFURL_GOLDENS[0].payload);
    reply get, head;
    request("GET", path, &get);
    T_EQ_INT(get.status, 200);
    T_CHECK(strstr(get.headers, "Content-Type: text/html; charset=utf-8\r\n") != NULL);
    T_CHECK(strstr(get.headers, "Cache-Control: public, max-age=300\r\n") != NULL);
    T_CHECK(strstr(body_of(get.wire), UNFURL_GOLDENS[0].tags[0]) != NULL);
    T_CHECK(strstr(body_of(get.wire), "<meta http-equiv=\"refresh\" content=\"0;url=https://foolish.cards/\"/>") != NULL);
    T_CHECK(strstr(body_of(get.wire), "<a href=\"https://foolish.cards/\">") != NULL);
    T_CHECK(strstr(body_of(get.wire), "<script") == NULL);

    // HEAD: the same status and headers, GET's Content-Length, no body on the wire.
    request("HEAD", path, &head);
    T_EQ_INT(head.status, 200);
    T_EQ_STR(head.headers, get.headers);
    char cl[64];
    snprintf(cl, sizeof cl, "Content-Length: %zu\r\n", get.body_len);
    T_CHECK(get.body_len > 0);
    T_CHECK(strstr(head.wire, cl) != NULL);
    T_EQ_STR(body_of(head.wire), "");

    // Next answered 404 for these; so does the route.
    reply r;
    request("GET", "/m/", &r);
    T_EQ_INT(r.status, 404);
    request("GET", "/m/abc/def", &r);
    T_EQ_INT(r.status, 404);
    request("POST", path, &r);
    T_EQ_INT(r.status, 405);
    cb_router_free(g_ctx.router);
}

// HEAD on any GET route, not only this one: CloudFront and crawlers send it,
// and /health answered 405 to it before.
static void h_get(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { cb_res_text(s, 200, "get"); }
static void h_head(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { cb_res_text(s, 200, "head"); }
static void head_falls_back_to_get(void) {
    cb_router *r = cb_router_new();
    T_EQ_INT(cb_router_add(r, "GET", "/a", CB_ROUTE_LOOP, h_get), CB_OK);
    T_EQ_INT(cb_router_add(r, "GET", "/b", 0, h_get), CB_OK);
    T_EQ_INT(cb_router_add(r, "HEAD", "/b", 0, h_head), CB_OK);
    T_EQ_INT(cb_router_add(r, "POST", "/c", 0, h_get), CB_OK);
    bool known = false;
    const cb_route *m = cb_router_match(r, "HEAD", "/a", &known);
    T_CHECK(m && m->fn == h_get && (m->flags & CB_ROUTE_LOOP));
    m = cb_router_match(r, "HEAD", "/b", &known);
    T_CHECK(m && m->fn == h_head);   // a HEAD route of its own wins
    m = cb_router_match(r, "HEAD", "/c", &known);
    T_CHECK(!m && known);            // POST only: still 405
    m = cb_router_match(r, "HEAD", "/d", &known);
    T_CHECK(!m && !known);
    cb_router_free(r);

    setup(false);   // every module's routes, as boot registers them
    reply h;
    request("HEAD", "/health", &h);
    T_EQ_INT(h.status, 200);
    T_EQ_STR(body_of(h.wire), "");
    cb_router_free(g_ctx.router);
}

int main(void) {
    T_RUN(goldens);
    T_RUN(titles);
    setup(true);
    T_RUN(get_and_head);
    T_RUN(head_falls_back_to_get);
    return T_DONE();
}
