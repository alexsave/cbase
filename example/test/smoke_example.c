// smoke_example.c - the example app end to end, as a person runs it: `cbase serve` on a port the
// kernel picks, with no cloud account and nothing but a build.
//
//   build/test/smoke_example build/cbase
//
// The platform's own routes it declares answer (/health, /admin, the account routes), the app's two routes answer, an unknown path
// is 404 and a known one under another method 405, and the server stops on SIGTERM with status 0.
#include "t.h"
#include "lib_client.h"
#include "lib_proc.h"

#include "example_app.h"
#include "num_parse.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

#define ADMIN "example-admin-token-0123456789abcdef"

static const char *g_bin;
static tp_child g_child;
static int g_port;

static int get(const char *method, const char *path, const char *headers, const char *body, tc_response *r) {
    memset(r, 0, sizeof *r);
    if (tc_http(g_port, method, path, headers, body, body ? strlen(body) : 0, r) != 0) return -1;
    return r->status;
}

static void serves_the_app(void) {
    static char envdb[512];
    char dir[256];
    T_EQ_INT(tp_tempdir(dir, sizeof dir), 0);
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/example.sqlite", dir);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", envdb, "CBASE_JWT_SECRET=example-secret-example-secret-example",
                    "CBASE_ADMIN_TOKEN=" ADMIN, NULL };
    char *argv[] = { (char *)g_bin, "serve", NULL };
    T_EQ_INT(tp_start(&g_child, argv, env), 0);
    char line[512] = "";
    T_EQ_INT(tp_wait_line(&g_child, "listening on", 20000, line, sizeof line), 0);
    const char *colon = strrchr(line, ':');
    int64_t port = 0;
    T_CHECK(colon && num_parse_i64(colon + 1, strcspn(colon + 1, " \r\n"), 1, 65535, &port));
    g_port = (int)port;
    T_CHECK(g_port > 0);

    tc_response r;
    T_EQ_INT(get("GET", "/health", NULL, NULL, &r), 200);
    tc_response_free(&r);
    T_EQ_INT(get("GET", "/admin", NULL, NULL, &r), 200);
    tc_response_free(&r);
    // The account routes it declares (cb_app.auth_routes) are there, where an undeclared route is
    // 404: a refresh not in the auth wire is its 400, and sign-up and sign-in are 503, as the
    // example names no account domain (cb_app.account_domain).
    static const struct { const char *path; int status; } auth[] = {
        { "/v1/auth/refresh", 400 }, { "/v1/auth/signup", 503 }, { "/v1/auth/signin", 503 } };
    for (size_t i = 0; i < sizeof auth / sizeof auth[0]; i++) {
        T_EQ_INT(get("POST", auth[i].path, "Content-Type: application/octet-stream\r\n", "not the wire", &r), auth[i].status);
        tc_response_free(&r);
    }

    T_EQ_INT(get("GET", "/v1/hello", NULL, NULL, &r), 200);
    T_EQ_STR(cb_buf_cstr(&r.body), EXAMPLE_HELLO);
    tc_response_free(&r);
    T_EQ_INT(get("POST", "/v1/echo", "Content-Type: application/octet-stream\r\n", "round trip", &r), 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "round trip");
    tc_response_free(&r);

    T_EQ_INT(get("GET", "/v1/nothing-here", NULL, NULL, &r), 404);
    tc_response_free(&r);
    T_EQ_INT(get("DELETE", "/v1/hello", NULL, NULL, &r), 405);
    tc_response_free(&r);

    T_EQ_INT(tp_stop(&g_child, SIGTERM, 15000), 0);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: smoke_example <cbase>\n");
        return 2;
    }
    g_bin = argv[1];
    T_RUN(serves_the_app);
    return T_DONE();
}
