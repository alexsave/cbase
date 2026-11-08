// unit_storeless.c - the example app as a storeless app (cbase/app.h storeless): no platform store
// is opened, no database file is made, and a storeless app that asks for what lives in the store is
// refused at boot with a sentence.
//
// End to end, as `cbase serve` runs: this program runs ITSELF as the server (lib_proc.h: a scripted
// child is the test's own binary), whose `serve-storeless` mode serves the example app with
// storeless set through the serve ingress (cb_process_main), in an empty directory that is also its
// CBASE_DB's. The directory is empty after it stops.
#include "t.h"
#include "lib_client.h"
#include "lib_proc.h"

#include "boot.h"
#include "cbase/app.h"
#include "cbase/provider.h"
#include "config.h"
#include "ctx.h"
#include "example_app.h"
#include "ingress_process.h"
#include "num_parse.h"
#include "store/store.h"
#include "util/err.h"
#include "util/log.h"

#include <dirent.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef CBASE_LINKS_LOCAL

#define ADMIN "storeless-admin-token-0123456789abcdef"

static const char *g_self;

// The names in dir, but . and .., joined by spaces ("" when it is empty).
static void listing(const char *dir, char *out, size_t cap) {
    out[0] = 0;
    DIR *d = opendir(dir);
    if (!d) { snprintf(out, cap, "(no such directory)"); return; }
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            snprintf(out + strlen(out), cap - strlen(out), "%s%s", out[0] ? " " : "", e->d_name);
    closedir(d);
}

static void rm_tree_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[1200];
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
    closedir(d);
    rmdir(dir);
}

static int get(int port, const char *method, const char *path, const char *headers, const char *body, tc_response *r) {
    memset(r, 0, sizeof *r);
    if (tc_http(port, method, path, headers, body, body ? strlen(body) : 0, r) != 0) return -1;
    return r->status;
}

// The example app, storeless, and the variants a boot must refuse.
static cb_app g_app;
static void app_storeless(void) {
    g_app = *cb_main_app;
    g_app.storeless = true;
    g_app.auth_routes = false;   // the example's accounts live in the store
}

// The server's own schedule, and its own database opened and closed where an app opens its own.
static int tick(cb_ctx *ctx, int64_t now_ms) {
    CB_INFO("storeless", "the schedule ran");
    return 0;
}
static const cb_schedule TICK[] = { { "storeless_tick", 50, tick, true } };
static int g_opened, g_closed;
static int app_open(cb_ctx *ctx, char *err, size_t cap) { g_opened++; return CB_OK; }
static void app_close(cb_ctx *ctx) { g_closed++; }

static int two(cb_ctx *ctx, int64_t now_ms) { return 2; }

// "bare": an app that declares none of the platform's routes (cbase/app.h health_route, ready_route,
// admin_routes) and has a /ready of its own.
static void h_own_ready(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { cb_res_text(res, 200, "the app's own\n"); }
static const cb_route OWN[] = { { .method = "GET", .pattern = "/ready", .flags = CB_ROUTE_LOOP, .fn = h_own_ready } };
static const cb_route_list OWN_LIST = { OWN, 1 };
static const cb_route_list *g_lists[4];

static int child_main(const char *variant) {
    app_storeless();
    g_app.schedules = TICK;
    g_app.n_schedules = 1;
    g_app.admin_count = two;
    g_app.admin_count_label = "Things";
    if (strcmp(variant, "accounts") == 0) g_app.account_domain = "accounts.example.test";
    if (strcmp(variant, "bare") == 0) {
        g_app.health_route = g_app.ready_route = g_app.admin_routes = false;
        for (int i = 0; i < g_app.n_route_lists && i < 3; i++) g_lists[i] = g_app.routes[i];
        g_lists[g_app.n_route_lists] = &OWN_LIST;
        g_app.routes = g_lists;
        g_app.n_route_lists++;
    }
    const char *dir = getenv("STORELESS_DIR");
    if (!dir || chdir(dir) != 0) return 3;
    char *argv[] = { (char *)g_self, "serve", NULL };
    return cb_process_main(&cb_provider_local, &g_app, 2, argv);
}

// Started as a person runs it on their own machine: not in dev mode, and with neither a JWT secret
// nor a public URL (a storeless app signs no tokens), unless `secret` gives the secret anyway.
static int start(tp_child *c, const char *variant, const char *dir, int *port, bool secret) {
    static char envdir[1100], envdb[1100];
    snprintf(envdir, sizeof envdir, "STORELESS_DIR=%s", dir);
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/example.sqlite", dir);
    char *env[] = { "CBASE_DEV=", "CBASE_PUBLIC_URL=", "CBASE_LISTEN=127.0.0.1:0", envdir, envdb,
                    secret ? "CBASE_JWT_SECRET=storeless-secret-storeless-secret-0" : "CBASE_JWT_SECRET=",
                    "CBASE_ADMIN_TOKEN=" ADMIN, "CBASE_STORE=", "CBASE_BACKUP_BUCKET=storeless-no-such-bucket", NULL };
    char *argv[] = { (char *)g_self, "serve-storeless", (char *)variant, NULL };
    if (tp_start(c, argv, env) != 0) return -1;
    if (!port) return 0;
    char line[512] = "";
    if (secret && tp_wait_line(c, "it is not used", 20000, line, sizeof line) != 0) return -1;
    // A backup bucket is configured, and there is nothing to back up: said, and the start goes on
    // (the restore that runs before a store opens is not run at all).
    if (tp_wait_line(c, "nothing is backed up", 20000, line, sizeof line) != 0) return -1;
    if (tp_wait_line(c, "listening on", 20000, line, sizeof line) != 0) return -1;
    const char *colon = strrchr(line, ':');
    int64_t p = 0;
    if (!colon || !num_parse_i64(colon + 1, strcspn(colon + 1, " \r\n"), 1, 65535, &p)) return -1;
    *port = (int)p;
    return 0;
}

// An app that declares none of the platform's routes carries only its own, under a real serve with
// the admin token set: /health, /admin and /admin/api/* are its 404, /ready is its own route.
static void declares_no_platform_routes(void) {
    char dir[256];
    T_EQ_INT(tp_tempdir(dir, sizeof dir), 0);
    tp_child c;
    int port = 0;
    T_EQ_INT(start(&c, "bare", dir, &port, false), 0);
    tc_response r;
    const char *bearer = "Authorization: Bearer " ADMIN "\r\n";
    static const char *const gone[] = { "/health", "/admin", "/admin/api/overview", "/admin/api/x" };
    for (size_t i = 0; i < sizeof gone / sizeof gone[0]; i++) {
        T_EQ_INT(get(port, "GET", gone[i], bearer, NULL, &r), 404);
        tc_response_free(&r);
    }
    T_EQ_INT(get(port, "POST", "/admin/api/query", bearer, "{}", &r), 404);
    tc_response_free(&r);
    static const char *const auth[] = { "oidc", "signup", "signin", "refresh", "signout", "password" };
    for (size_t i = 0; i < sizeof auth / sizeof auth[0]; i++) {
        char path[64];
        snprintf(path, sizeof path, "/v1/auth/%s", auth[i]);
        T_EQ_INT(get(port, "POST", path, "Content-Type: application/json\r\n", "{}", &r), 404);
        tc_response_free(&r);
    }
    T_EQ_INT(get(port, "GET", "/ready", NULL, NULL, &r), 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "the app's own\n");
    tc_response_free(&r);
    T_EQ_INT(get(port, "GET", "/v1/hello", NULL, NULL, &r), 200);   // its own routes, as ever
    tc_response_free(&r);
    T_EQ_INT(tp_stop(&c, SIGTERM, 15000), 0);
    rm_tree_files(dir);
}

static void serves_with_no_store(void) {
    char dir[256], seen[512];
    T_EQ_INT(tp_tempdir(dir, sizeof dir), 0);
    tp_child c;
    int port = 0;
    T_EQ_INT(start(&c, "plain", dir, &port, false), 0);
    T_CHECK(port > 0);
    char line[512];
    T_EQ_INT(tp_wait_line(&c, "the schedule ran", 20000, line, sizeof line), 0);   // schedules run with no store
    tc_response r;
    T_EQ_INT(get(port, "GET", "/v1/hello", NULL, NULL, &r), 200);
    T_EQ_STR(cb_buf_cstr(&r.body), EXAMPLE_HELLO);
    tc_response_free(&r);
    T_EQ_INT(get(port, "POST", "/v1/echo", "Content-Type: application/octet-stream\r\n", "no store", &r), 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "no store");
    tc_response_free(&r);
    T_EQ_INT(get(port, "GET", "/health", NULL, NULL, &r), 200);
    tc_response_free(&r);
    T_EQ_INT(get(port, "GET", "/ready", NULL, NULL, &r), 200);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "store=none") != NULL);
    tc_response_free(&r);
    // Accounts live in the store: a storeless app has no auth routes at all.
    T_EQ_INT(get(port, "POST", "/v1/auth/signup", "Content-Type: application/json\r\n", "{\"email\":\"a@b.test\",\"password\":\"pw-pw-pw-pw\"}", &r), 404);
    tc_response_free(&r);
    // The console: the app's counter answers (it keeps its own data); the store's views are 503.
    const char *bearer = "Authorization: Bearer " ADMIN "\r\n";
    T_EQ_INT(get(port, "GET", "/admin/api/overview", bearer, NULL, &r), 200);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "\"app_count\":2") != NULL);
    tc_response_free(&r);
    T_EQ_INT(get(port, "GET", "/admin/api/collections", bearer, NULL, &r), 503);
    tc_response_free(&r);
    T_EQ_INT(tp_stop(&c, SIGTERM, 15000), 0);
    listing(dir, seen, sizeof seen);
    T_EQ_STR(seen, "");   // no database, no -wal or -shm, no .serve-lock
    rm_tree_files(dir);
}

// A JWT secret given to a storeless app is not used, and said so; the app serves as without one.
static void a_secret_is_ignored(void) {
    char dir[256];
    T_EQ_INT(tp_tempdir(dir, sizeof dir), 0);
    tp_child c;
    int port = 0;
    T_EQ_INT(start(&c, "plain", dir, &port, true), 0);   // waits for "it is not used"
    tc_response r;
    T_EQ_INT(get(port, "GET", "/v1/hello", NULL, NULL, &r), 200);
    tc_response_free(&r);
    T_EQ_INT(tp_stop(&c, SIGTERM, 15000), 0);
    rm_tree_files(dir);
}

// A storeless app that names an account domain does not start: the sentence, and exit 1.
static void refused_end_to_end(void) {
    char dir[256], seen[512], line[512] = "";
    T_EQ_INT(tp_tempdir(dir, sizeof dir), 0);
    tp_child c;
    T_EQ_INT(start(&c, "accounts", dir, NULL, false), 0);
    T_EQ_INT(tp_wait_line(&c, "keeps no platform store", 20000, line, sizeof line), 0);
    T_CHECK(strstr(line, "account domain") != NULL);
    T_EQ_INT(tp_stop(&c, 0, 15000), 1);
    listing(dir, seen, sizeof seen);
    T_EQ_STR(seen, "");
    rm_tree_files(dir);
}

// cb_boot, in this process, over each refusal; and the example as it is (not storeless), whose
// store's file appears, so the empty listings above can see a file when there is one.
static int boot_in(const cb_app *app, const char *dir, const char *store, const char *apple, char *err, size_t cap) {
    static cb_config cfg;
    T_EQ_INT(cb_config_from_env(&cfg, err, cap), CB_OK);
    strcpy(cfg.jwt_secret, "a-unit-test-secret-with-enough-bytes");
    snprintf(cfg.db_path, sizeof cfg.db_path, "%s/example.sqlite", dir);
    snprintf(cfg.store, sizeof cfg.store, "%s", store);
    snprintf(cfg.apple_client_ids, sizeof cfg.apple_client_ids, "%s", apple);
    static cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.prov = &cb_provider_local;
    ctx.app = app;
    err[0] = 0;
    int rc = cb_boot(&ctx, err, cap);
    cb_unboot(&ctx);
    return rc;
}

static int fake_delete(cb_ctx *ctx, const char *user_id, int *code, int *detail) { return CB_OK; }
static const cb_store_app SOME_ROWS;   // rows declared (none: a storeless boot never reads them)

static void refused_in_boot(void) {
    char dir[256], seen[512], err[512];
    T_EQ_INT(tp_tempdir(dir, sizeof dir), 0);
    struct { const char *what, *store, *apple, *needle; int field; } cases[] = {
        { "store rows", "", "", "declares store rows", 1 },
        { "account domain", "", "", "names an account domain", 2 },
        { "delete account", "", "", "deletes accounts", 3 },
        { "auth routes", "", "", "serves the account routes (cb_app.auth_routes)", 4 },
        { "CBASE_STORE", "sqlite", "", "CBASE_STORE=sqlite names a store", 0 },
        { "Apple", "", "app.example.test", "Sign in with Apple", 0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        app_storeless();
        if (cases[i].field == 1) g_app.store = &SOME_ROWS;
        if (cases[i].field == 2) g_app.account_domain = "accounts.example.test";
        if (cases[i].field == 3) g_app.delete_account = fake_delete;
        if (cases[i].field == 4) g_app.auth_routes = true;
        int rc = boot_in(&g_app, dir, cases[i].store, cases[i].apple, err, sizeof err);
        T_EQ_INT(rc, CB_E_INVALID);
        if (!strstr(err, cases[i].needle)) fprintf(stderr, "     %s: \"%s\"\n", cases[i].what, err);
        T_CHECK(strstr(err, cases[i].needle) != NULL);
        listing(dir, seen, sizeof seen);
        T_EQ_STR(seen, "");
    }
    // Storeless and asking for nothing of the store: it boots, its own open and close run, and it
    // still makes no file.
    app_storeless();
    g_app.serve_open = app_open;
    g_app.serve_close = app_close;
    g_opened = g_closed = 0;
    T_EQ_INT(boot_in(&g_app, dir, "", "", err, sizeof err), CB_OK);
    T_EQ_INT(g_opened, 1);
    T_EQ_INT(g_closed, 1);
    listing(dir, seen, sizeof seen);
    T_EQ_STR(seen, "");
    // The example as it ships: its store's file is made.
    T_EQ_INT(boot_in(cb_main_app, dir, "", "", err, sizeof err), CB_OK);
    listing(dir, seen, sizeof seen);
    T_CHECK(strstr(seen, "example.sqlite") != NULL);
    rm_tree_files(dir);
}

int main(int argc, char **argv) {
    g_self = argv[0];
    if (argc == 3 && strcmp(argv[1], "serve-storeless") == 0) return child_main(argv[2]);
    T_RUN(serves_with_no_store);
    T_RUN(declares_no_platform_routes);
    T_RUN(a_secret_is_ignored);
    T_RUN(refused_end_to_end);
    T_RUN(refused_in_boot);
    return T_DONE();
}

#else

int main(void) {
    T_CHECK(1);   // the local provider is not linked: there is no serve to run storeless
    return T_DONE();
}

#endif
