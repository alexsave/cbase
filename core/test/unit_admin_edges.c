// unit_admin_edges.c - the platform console's refusals and its Lambda-only parts:
// every store error mapped to its status, no store at all, the asks it refuses,
// the AWS console links (and CloudWatch's own escaping), the backups listed
// newest first, a backup directory that cannot be made, and the gate a product's
// own routes use. No app is linked into the console here: a product's panels are
// the product's tests.
#include "t.h"
#include "cbase/app.h"
#include "cbase/provider.h"

#include "admin/admin.h"
#include "handler.h"
#include "net/router.h"
#include "store/store.h"
#include "util/err.h"
#include "util/json.h"
#include "util/log.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define ADMIN "admin-edges-token"

static cb_ctx g_ctx;
static cb_config g_cfg;
static cb_buf g_body;

// ---- a store that answers what the test says ----------------------------------------------------

static int g_rc;   // what every call below answers

static int f_users_list(cb_store *s, const char *q, int o, int l, cb_user_fn fn, void *ud) { return g_rc; }
static int f_user_get(cb_store *s, const char *id, cb_user_row *out) {
    if (g_rc != CB_OK) return g_rc;
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "%s", id);
    return CB_OK;
}
static int f_collections(cb_store *s, cb_name_fn fn, void *ud) { return g_rc; }
static int f_browse(cb_store *s, const char *c, const char *p, const char *cur, int l, cb_admin_row_fn fn, void *ud,
                    char *next, size_t cap) {
    if (g_rc != CB_OK) return g_rc;
    // Three rows: two of one item's partition, one of another shape.
    const char *n1[] = { "pk", "sk" }, *v1[] = { "GAME#ab12cd", "ROW" };
    const char *v2[] = { "GAME#ab12cd", "MEMBER#x" };
    const char *n3[] = { "other" }, *v3[] = { "x" };
    fn(2, n1, v1, ud);
    fn(2, n1, v2, ud);
    fn(1, n3, v3, ud);
    return 3;
}
static int f_query(cb_store *s, const char *stmt, bool w, cb_admin_row_fn fn, void *ud) { return g_rc; }
static const cb_store_ops OPS = {
    .name = "sqlite", .users_list = f_users_list, .user_get = f_user_get, .admin_collections = f_collections,
    .admin_browse = f_browse, .admin_query = f_query,
};
static cb_store g_store = { .ops = &OPS };

static void setup(cb_store *store, const cb_provider *mode, const char *db_path) {
    if (g_ctx.router) cb_router_free(g_ctx.router);
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&g_cfg, 0, sizeof g_cfg);
    snprintf(g_cfg.admin_token, sizeof g_cfg.admin_token, ADMIN);
    snprintf(g_cfg.table, sizeof g_cfg.table, "cbase");
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, "%s", db_path);
    snprintf(g_cfg.allowed_origins, sizeof g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = mode;
    g_ctx.app = NULL;   // the platform's console alone
    g_ctx.version = "test";
    g_ctx.router = cb_router_new();
    g_ctx.store = store;
    T_EQ_INT(cb_admin_routes(&g_ctx), CB_OK);
}

static int call(const char *method, const char *path, const char *query, const char *body) {
    cb_http_req q;
    memset(&q, 0, sizeof q);
    snprintf(q.method, sizeof q.method, "%s", method);
    q.path = path;
    q.query = query ? query : "";
    q.minor = 1;
    q.headers[q.n_headers++] = (cb_http_header){ "Authorization", "Bearer " ADMIN };
    if (body) { q.body = (const uint8_t *)body; q.body_len = strlen(body); }
    cb_http_res r;
    cb_res_init(&r);
    cb_handle(&g_ctx, &q, &r);
    cb_buf_reset(&g_body);
    cb_buf_append(&g_body, r.body.p, r.body.len);
    int st = r.status;
    cb_res_free(&r);
    return st;
}

static bool says(const char *text) { return strstr(cb_buf_cstr(&g_body), text) != NULL; }

// Each store refusal is its own status, on every call that reads the store.
static void store_errors_map_to_statuses(void) {
    setup(&g_store, &cb_provider_local, "/nonexistent-cbase/cbase.sqlite");
    static const struct { int rc, status; const char *says; } M[] = {
        { CB_E_NOTIMPL, 501, "not implemented by the sqlite store" },
        { CB_E_NOT_FOUND, 404, "not found" },
        { CB_E_FORBIDDEN, 403, "typed confirmation" },
        { CB_E_INVALID, 400, "invalid" },
        { CB_E_DB, 500, "CB_E_DB" },
    };
    static const struct { const char *m, *path, *query, *body; } CALLS[] = {
        { "GET", "/admin/api/users", NULL, NULL },
        { "GET", "/admin/api/collections", NULL, NULL },
        { "GET", "/admin/api/browse", "c=users", NULL },
        { "POST", "/admin/api/query", NULL, "{\"statement\":\"SELECT 1\"}" },

    };
    for (size_t m = 0; m < sizeof M / sizeof M[0]; m++) {
        g_rc = M[m].rc;
        for (size_t c = 0; c < sizeof CALLS / sizeof CALLS[0]; c++) {
            int st = call(CALLS[c].m, CALLS[c].path, CALLS[c].query, CALLS[c].body);
            if (st != M[m].status) fprintf(stderr, "     %s %s with %s: %d\n", CALLS[c].m, CALLS[c].path, cb_strerror(M[m].rc), st);
            T_EQ_INT(st, M[m].status);
            T_CHECK(says(M[m].says));
        }
    }
    // The overview reads the heartbeat's count, and says -1 when it cannot.
    g_rc = CB_E_DB;
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, NULL), 200);
    T_EQ_INT(cb_json_find(cb_buf_cstr(&g_body), g_body.len, "games_needing_bots", NULL), CB_JSON_NULL);
    // And a backup the store refuses is the store's status.
    char dir[256], path[300];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/cbase-admin-edges-b-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/cbase.sqlite", dir);
    setup(&g_store, &cb_provider_local, path);
    for (size_t m = 0; m < sizeof M / sizeof M[0]; m++) {
        g_rc = M[m].rc;
        T_EQ_INT(call("POST", "/admin/api/backups", NULL, NULL), M[m].status);
    }
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    T_EQ_INT(system(cmd), 0);
}

// No store: 503 from every call that needs one; the ones that do not still answer.
static void no_store_is_503(void) {
    setup(NULL, &cb_provider_local, "/nonexistent-cbase/cbase.sqlite");
    const char *needs[] = { "/admin/api/users", "/admin/api/collections", "/admin/api/browse" };
    for (size_t i = 0; i < sizeof needs / sizeof needs[0]; i++) {
        T_EQ_INT(call("GET", needs[i], "c=users&id=x", NULL), 503);
        T_CHECK(says("store not available"));
    }
    T_EQ_INT(call("POST", "/admin/api/query", NULL, "{\"statement\":\"SELECT 1\"}"), 503);
    T_EQ_INT(call("POST", "/admin/api/users/delete", NULL, "{\"id\":\"u1\"}"), 503);
    T_EQ_INT(call("POST", "/admin/api/backups", NULL, NULL), 503);
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, NULL), 200);
    T_CHECK(says("\"store\":\"none\""));
    T_EQ_INT(call("GET", "/admin/api/config", NULL, NULL), 200);
}

// The bodies and queries each call needs, refused when they are not there.
static void malformed_asks(void) {
    setup(&g_store, &cb_provider_local, "/nonexistent-cbase/cbase.sqlite");
    g_rc = CB_OK;
    T_EQ_INT(call("GET", "/admin/api/browse", "", NULL), 400);
    T_EQ_INT(call("POST", "/admin/api/query", NULL, "not json"), 400);
    T_EQ_INT(call("POST", "/admin/api/query", NULL, "{\"statement\":\"\"}"), 400);
    T_EQ_INT(call("POST", "/admin/api/users/delete", NULL, "{}"), 400);
    T_EQ_INT(call("POST", "/admin/api/users/delete", NULL, "{\"id\":\"\"}"), 400);
    T_EQ_INT(call("GET", "/admin/api/logs", "level=loud", NULL), 400);
    // A limit past the page cap is the cap, and an offset that is not a number the default.
    T_EQ_INT(call("GET", "/admin/api/users", "limit=99999&offset=x", NULL), 200);
}

#ifdef CBASE_LINKS_AWS
static const cb_store_ops OPS_DYNAMO = { .name = "dynamo", .admin_query = f_query };
static cb_store g_store_dy = { .ops = &OPS_DYNAMO };

// Under Lambda the console links to the function's AWS pages; CloudWatch's
// queryDetail wants every byte but [A-Za-z0-9_.-] as '*' and two hex digits.
static void lambda_links(void) {
    setup(&g_store, &cb_provider_aws, "/nonexistent-cbase/cbase.sqlite");
    g_rc = CB_OK;
    setenv("AWS_REGION", "us-east-1", 1);
    setenv("AWS_LAMBDA_FUNCTION_NAME", "cbase-Fn", 1);
    setenv("AWS_LAMBDA_LOG_GROUP_NAME", "/aws/lambda/cbase Fn", 1);
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, NULL), 200);
    char v[1024] = "";
    T_CHECK(cb_json_get_str(cb_buf_cstr(&g_body), g_body.len, "links.lambda", v, sizeof v));
    T_EQ_STR(v, "https://us-east-1.console.aws.amazon.com/lambda/home?region=us-east-1#/functions/cbase-Fn");
    T_CHECK(cb_json_get_str(cb_buf_cstr(&g_body), g_body.len, "links.logs_insights", v, sizeof v));
    // "fields @timestamp, @message\n| sort ..." and the group with its '/' and space.
    T_CHECK(strstr(v, "fields*20*40timestamp*2c*20*40message*0a*7c*20sort") != NULL);
    T_CHECK(strstr(v, "~source~(~'*2faws*2flambda*2fcbase*20Fn))") != NULL);
    T_CHECK(cb_json_get_str(cb_buf_cstr(&g_body), g_body.len, "links.dynamodb", v, sizeof v));
    T_CHECK(strstr(v, "#table?name=cbase") != NULL);
    // No log group: the function's default.
    unsetenv("AWS_LAMBDA_LOG_GROUP_NAME");
    T_EQ_INT(call("GET", "/admin/api/logs", NULL, NULL), 200);
    T_CHECK(says("*2faws*2flambda*2fcbase-Fn"));
    // No function name: no links at all.
    unsetenv("AWS_LAMBDA_FUNCTION_NAME");
    T_EQ_INT(call("GET", "/admin/api/backups", NULL, NULL), 200);
    T_CHECK(!says("console.aws.amazon.com"));
    unsetenv("AWS_REGION");
    // The dynamo store's backups: nothing to trigger.
    setup(&g_store_dy, &cb_provider_aws, "x");
    T_EQ_INT(call("POST", "/admin/api/backups", NULL, NULL), 409);
    T_EQ_INT(call("GET", "/admin/api/backups", NULL, NULL), 200);
    T_CHECK(says("point-in-time recovery"));
}
#endif

// The backups directory beside the database: listed newest first, only the
// console's own files; and one that cannot be made is a 500.
static void backups_newest_first(void) {
    char dir[256], path[700], b[400];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/cbase-admin-edges-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    mkdir(dir, 0700);
    snprintf(b, sizeof b, "%s/backups", dir);
    mkdir(b, 0700);
    const char *names[] = { "cbase-20261001T000000Z.sqlite", "cbase-20261003T000000Z.sqlite", "cbase-20261002T000000Z.sqlite",
                            "other.sqlite", "cbase-x.txt" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", b, names[i]);
        FILE *f = fopen(path, "w");
        if (f) { fputs("x", f); fclose(f); }
    }
    snprintf(path, sizeof path, "%s/cbase.sqlite", dir);
    setup(&g_store, &cb_provider_local, path);
    T_EQ_INT(call("GET", "/admin/api/backups", NULL, NULL), 200);
    char n0[64] = "", n1[64] = "", n2[64] = "";
    cb_json_get_str(cb_buf_cstr(&g_body), g_body.len, "files.0.name", n0, sizeof n0);
    cb_json_get_str(cb_buf_cstr(&g_body), g_body.len, "files.1.name", n1, sizeof n1);
    cb_json_get_str(cb_buf_cstr(&g_body), g_body.len, "files.2.name", n2, sizeof n2);
    T_EQ_STR(n0, "cbase-20261003T000000Z.sqlite");
    T_EQ_STR(n1, "cbase-20261002T000000Z.sqlite");
    T_EQ_STR(n2, "cbase-20261001T000000Z.sqlite");
    T_EQ_INT(cb_json_array_len(cb_buf_cstr(&g_body), g_body.len, "files"), 3);
    // The overview's last backup is the newest.
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, NULL), 200);
    T_EQ_INT(cb_json_find(cb_buf_cstr(&g_body), g_body.len, "last_backup", NULL), CB_JSON_STR);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    T_EQ_INT(system(cmd), 0);
    // A database under a file: the backups directory cannot be made.
    setup(&g_store, &cb_provider_local, "/dev/null/cbase.sqlite");
    g_rc = CB_OK;
    T_EQ_INT(call("POST", "/admin/api/backups", NULL, NULL), 500);
    T_CHECK(says("cannot create the backups directory"));
}

// THE EXPORTED GATE (admin.h cb_admin_authorized): an app's own dev route asks the console's
// gate itself; the right bearer passes, a wrong or missing one is 401 and counted toward the
// address's limit, and with no console token configured the route is 404, as if not routed.
static bool gate_with(const char *auth, int *status) {
    cb_http_req q;
    memset(&q, 0, sizeof q);
    snprintf(q.method, sizeof q.method, "GET");
    q.path = "/dev/thing";
    q.query = "";
    q.minor = 1;
    if (auth) q.headers[q.n_headers++] = (cb_http_header){ "Authorization", auth };
    cb_http_res r;
    cb_res_init(&r);
    bool ok = cb_admin_authorized(&g_ctx, &q, &r);
    *status = r.status;
    cb_res_free(&r);
    return ok;
}

static void the_exported_gate(void) {
    setup(&g_store, &cb_provider_local, "/nonexistent-cbase/cbase.sqlite");
    int st = 0;
    T_CHECK(gate_with("Bearer " ADMIN, &st));
    T_CHECK(!gate_with("Bearer wrong", &st));
    T_EQ_INT(st, 401);
    T_CHECK(!gate_with(NULL, &st));
    T_EQ_INT(st, 401);
    g_cfg.admin_token[0] = 0;
    T_CHECK(!gate_with("Bearer " ADMIN, &st));
    T_EQ_INT(st, 404);
}

int main(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    T_RUN(store_errors_map_to_statuses);
    T_RUN(no_store_is_503);
    T_RUN(malformed_asks);
#ifdef CBASE_LINKS_AWS
    T_RUN(lambda_links);
#endif
    T_RUN(backups_newest_first);
    T_RUN(the_exported_gate);
    cb_router_free(g_ctx.router);
    cb_buf_free(&g_body);
    return T_DONE();
}
