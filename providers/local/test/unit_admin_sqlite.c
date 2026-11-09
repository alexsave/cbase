// unit_admin_sqlite.c - the platform console (admin/admin.h) over the real local store, SQLite in
// a fresh file, every request through cb_handle: what core/test/unit_admin_edges.c checks against
// a scripted store, here with the rows SQLite really keeps.
//
// Held here, each as the console meets it:
//   - the gate: a wrong or missing bearer is 401, and with no token configured the console,
//     page and API, is 404 as if not routed;
//   - collections are the store's tables, by name;
//   - browse pages a table: plain, a prefix of its first column (a BLOB uuid's hex), a
//     column=value filter (a uuid by its text or its hex, a TEXT column by its value), a cursor
//     that walks every row once, and a table, column or cursor that is not one refused;
//   - users lists the accounts, searched, and users/delete runs the app's delete_account;
//   - a write statement needs the typed confirmation, and a backup is a file beside the database;
//   - logs tails the warnings the console itself wrote.
#include "t.h"
#include "cbase/app.h"
#include "cbase/provider.h"

#include "admin/admin.h"
#include "auth/auth.h"
#include "handler.h"
#include "net/router.h"
#include "rate_limits.h"
#include "store/store.h"
#include "util/err.h"
#include "util/json.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define ADMIN "unit-admin-sqlite-token"
#define HASH0 "$2b$10$abcdefghijklmnopqrstuuabcdefghijklmnopqrstuvwxyz01234"

static cb_config g_cfg;
static cb_ctx g_ctx;
static char g_dir[300];
static cb_buf g_body;
static int g_deleted;   // delete_account calls

// The app's delete_account: the account's rows go (as a product's would, its own items first).
static int app_delete_account(cb_ctx *ctx, const char *user_id, int *code, int *detail) {
    g_deleted++;
    return cb_auth_delete_user(ctx->auth, user_id);
}
static const cb_app APP = { .name = "unit-admin-sqlite", .delete_account = app_delete_account };

static const char *const IDS[3] = {
    "0a0d0000-0000-4000-8000-000000000001",
    "0b0d0000-0000-4000-8000-000000000002",
    "0c0d0000-0000-4000-8000-000000000003",
};
static const char *const EMAILS[3] = { "anna@accounts.example", "ben@accounts.example", "cora@accounts.example" };

static void setup(void) {
    memset(&g_cfg, 0, sizeof g_cfg);
    snprintf(g_cfg.admin_token, sizeof g_cfg.admin_token, ADMIN);
    strcpy(g_cfg.jwt_secret, "unit-admin-sqlite-secret-with-enough-bytes");
    strcpy(g_cfg.allowed_origins, "*");
    snprintf(g_cfg.table, sizeof g_cfg.table, "cbase");
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/cbase-admin-sqlite-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    mkdir(g_dir, 0700);
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, "%s/cbase.sqlite", g_dir);
    unlink(g_cfg.db_path);
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = &cb_provider_local;
    g_ctx.app = &APP;
    g_ctx.version = "test";
    T_EQ_INT(cb_store_open_sqlite(g_cfg.db_path, &g_ctx.store), CB_OK);
    T_EQ_INT(cb_auth_new(&g_ctx, &g_ctx.auth), CB_OK);
    g_ctx.router = cb_router_new();
    T_EQ_INT(cb_admin_routes(&g_ctx), CB_OK);
    cb_limits_reset();
    // Three accounts, oldest first.
    for (int i = 0; i < 3; i++) {
        cb_user_row u;
        memset(&u, 0, sizeof u);
        strcpy(u.id, IDS[i]);
        strcpy(u.email, EMAILS[i]);
        strcpy(u.password_hash, HASH0);
        u.created_at = 1790000000000LL + i * 1000;
        T_EQ_INT(CB_STORE_CALL(g_ctx.store, user_create, &u), CB_OK);
    }
    g_deleted = 0;
}

static void teardown(void) {
    cb_router_free(g_ctx.router);
    cb_auth_free(g_ctx.auth);
    cb_store_close(g_ctx.store);
    memset(&g_ctx, 0, sizeof g_ctx);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    T_EQ_INT(system(cmd), 0);
}

// One request; `auth` is the whole Authorization header, NULL for none.
static int call_as(const char *auth, const char *method, const char *path, const char *query, const char *body) {
    cb_http_req q;
    memset(&q, 0, sizeof q);
    snprintf(q.method, sizeof q.method, "%s", method);
    q.path = path;
    q.query = query ? query : "";
    q.minor = 1;
    if (auth) q.headers[q.n_headers++] = (cb_http_header){ "Authorization", auth };
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
static int call(const char *method, const char *path, const char *query, const char *body) {
    return call_as("Bearer " ADMIN, method, path, query, body);
}

static const char *B(void) { return cb_buf_cstr(&g_body); }
static int rows(const char *key) { return cb_json_array_len(B(), g_body.len, key); }
static bool str_at(const char *path, const char *want) {
    char v[600] = "";
    if (!cb_json_get_str(B(), g_body.len, path, v, sizeof v)) return false;
    if (strcmp(v, want) != 0) fprintf(stderr, "     %s: got \"%s\", want \"%s\"\n", path, v, want);
    return strcmp(v, want) == 0;
}
static bool says(const char *text) { return strstr(B(), text) != NULL; }

// ---- the cases --------------------------------------------------------------------------------

static void the_gate(void) {
    setup();
    T_EQ_INT(call_as("Bearer wrong", "GET", "/admin/api/collections", NULL, NULL), 401);
    T_CHECK(!says("users"));
    T_EQ_INT(call_as(NULL, "GET", "/admin/api/users", NULL, NULL), 401);
    T_CHECK(!says(EMAILS[0]));
    T_EQ_INT(call_as("Basic " ADMIN, "GET", "/admin/api/users", NULL, NULL), 401);
    T_EQ_INT(call("GET", "/admin/api/users", NULL, NULL), 200);
    T_EQ_INT(call_as(NULL, "GET", "/admin", NULL, NULL), 200);   // the page holds no data
    // No token configured: no console at all, the right bearer included.
    g_cfg.admin_token[0] = 0;
    T_EQ_INT(call("GET", "/admin/api/users", NULL, NULL), 404);
    T_CHECK(!says(EMAILS[0]));
    T_EQ_INT(call_as(NULL, "GET", "/admin", NULL, NULL), 404);
    teardown();
}

static void collections_are_the_tables(void) {
    setup();
    T_EQ_INT(call("GET", "/admin/api/collections", NULL, NULL), 200);
    T_CHECK(str_at("store", "sqlite"));
    int n = rows("collections");
    T_CHECK(n >= 5);
    // By name, and the platform's own tables among them.
    bool users = false, sessions = false, sorted = true;
    char prev[64] = "", v[64], path[32];
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "collections.%d", i);
        T_CHECK(cb_json_get_str(B(), g_body.len, path, v, sizeof v));
        if (strcmp(prev, v) > 0) sorted = false;
        users |= strcmp(v, "users") == 0;
        sessions |= strcmp(v, "sessions") == 0;
        T_CHECK(strncmp(v, "sqlite_", 7) != 0);
        strcpy(prev, v);
    }
    T_CHECK(users);
    T_CHECK(sessions);
    T_CHECK(sorted);
    teardown();
}

static void browse_pages_a_table(void) {
    setup();
    // Plain: every row, the uuid column as its text, the page the last.
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users", NULL), 200);
    T_CHECK(str_at("collection", "users"));
    T_EQ_INT(rows("rows"), 3);
    T_CHECK(says(IDS[0]) && says(IDS[1]) && says(IDS[2]));
    T_CHECK(str_at("next", ""));
    // A cursor walks every row once: 2, then 1, then the end.
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&limit=2", NULL), 200);
    T_EQ_INT(rows("rows"), 2);
    T_CHECK(str_at("next", "2"));
    char seen[2][64] = { "", "" };
    T_CHECK(cb_json_get_str(B(), g_body.len, "rows.0.id", seen[0], sizeof seen[0]));
    T_CHECK(cb_json_get_str(B(), g_body.len, "rows.1.id", seen[1], sizeof seen[1]));
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&limit=2&cursor=2", NULL), 200);
    T_EQ_INT(rows("rows"), 1);
    T_CHECK(str_at("next", ""));
    char last[64] = "";
    T_CHECK(cb_json_get_str(B(), g_body.len, "rows.0.id", last, sizeof last));
    T_CHECK(strcmp(last, seen[0]) != 0 && strcmp(last, seen[1]) != 0);
    // A prefix of the first column, a BLOB uuid: matched on its hex, a dash or case aside.
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&prefix=0b0d", NULL), 200);
    T_EQ_INT(rows("rows"), 1);
    T_CHECK(str_at("rows.0.id", IDS[1]));
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&prefix=0C0D0000-00", NULL), 200);
    T_EQ_INT(rows("rows"), 1);
    T_CHECK(str_at("rows.0.id", IDS[2]));
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&prefix=ff", NULL), 200);
    T_EQ_INT(rows("rows"), 0);
    // column=value on the BLOB uuid: by its text, and by its 32 hex digits.
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&prefix=id%3D0a0d0000-0000-4000-8000-000000000001", NULL), 200);
    T_EQ_INT(rows("rows"), 1);
    T_CHECK(str_at("rows.0.email", EMAILS[0]));
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&prefix=id%3D0c0d0000000040008000000000000003", NULL), 200);
    T_EQ_INT(rows("rows"), 1);
    T_CHECK(str_at("rows.0.email", EMAILS[2]));
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&prefix=id%3Dnot-an-id", NULL), 200);
    T_EQ_INT(rows("rows"), 0);
    // column=value on a TEXT column.
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&prefix=email%3Dben%40accounts.example", NULL), 200);
    T_EQ_INT(rows("rows"), 1);
    T_CHECK(str_at("rows.0.id", IDS[1]));
    // A table, a column or a cursor that is not one: refused, never spliced into SQL.
    T_EQ_INT(call("GET", "/admin/api/browse", "c=nope", NULL), 400);
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users%22%20--", NULL), 400);
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&prefix=nocol%3Dx", NULL), 400);
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&cursor=x", NULL), 400);
    teardown();
}

// A table whose first column is TEXT: a prefix is the start of its text. Made through the
// console's own write path, which needs the typed confirmation.
static void a_write_needs_the_word_and_a_text_prefix_matches(void) {
    setup();
    const char *create = "{\"statement\":\"CREATE TABLE notes(name TEXT, n INTEGER)\"}";
    T_EQ_INT(call("POST", "/admin/api/query", NULL, create), 403);
    T_EQ_INT(call("GET", "/admin/api/browse", "c=notes", NULL), 400);
    T_EQ_INT(call("POST", "/admin/api/query", NULL,
                  "{\"statement\":\"CREATE TABLE notes(name TEXT, n INTEGER)\",\"confirm\":\"WRITE\"}"), 200);
    T_EQ_INT(call("POST", "/admin/api/query", NULL,
                  "{\"statement\":\"INSERT INTO notes VALUES ('abc', 1), ('abd', 2), ('xyz', 3)\",\"confirm\":\"WRITE\"}"), 200);
    T_EQ_INT(call("GET", "/admin/api/collections", NULL, NULL), 200);
    T_CHECK(says("\"notes\""));
    T_EQ_INT(call("GET", "/admin/api/browse", "c=notes&prefix=ab", NULL), 200);
    T_EQ_INT(rows("rows"), 2);
    T_CHECK(str_at("rows.0.name", "abc"));
    T_CHECK(str_at("rows.1.name", "abd"));
    T_EQ_INT(call("GET", "/admin/api/browse", "c=notes&prefix=n%3D3", NULL), 200);
    T_EQ_INT(rows("rows"), 1);
    T_CHECK(str_at("rows.0.name", "xyz"));
    // A read needs no word, and reads what the write left.
    T_EQ_INT(call("POST", "/admin/api/query", NULL, "{\"statement\":\"SELECT count(*) AS k FROM notes\"}"), 200);
    T_CHECK(str_at("rows.0.k", "3"));
    teardown();
}

static void users_and_their_delete(void) {
    setup();
    T_EQ_INT(call("GET", "/admin/api/users", NULL, NULL), 200);
    T_EQ_INT(rows("users"), 3);
    T_CHECK(str_at("users.0.id", IDS[2]));   // newest first
    T_CHECK(str_at("users.2.email", EMAILS[0]));
    T_EQ_INT(call("GET", "/admin/api/users", "search=ben", NULL), 200);
    T_EQ_INT(rows("users"), 1);
    T_CHECK(str_at("users.0.id", IDS[1]));
    // A full page says where the next begins; the last does not.
    T_EQ_INT(call("GET", "/admin/api/users", "limit=2", NULL), 200);
    T_EQ_INT(rows("users"), 2);
    int64_t next = 0;
    T_CHECK(cb_json_get_i64(B(), g_body.len, "next_offset", &next));
    T_EQ_INT(next, 2);
    T_EQ_INT(call("GET", "/admin/api/users", "limit=2&offset=2", NULL), 200);
    T_EQ_INT(rows("users"), 1);
    T_EQ_INT(cb_json_find(B(), g_body.len, "next_offset", NULL), CB_JSON_NULL);
    // Delete: the app's delete_account, and the account gone; another untouched.
    char body[128];
    snprintf(body, sizeof body, "{\"id\":\"%s\"}", IDS[1]);
    T_EQ_INT(call("POST", "/admin/api/users/delete", NULL, body), 200);
    T_CHECK(str_at("deleted", IDS[1]));
    T_EQ_INT(g_deleted, 1);
    cb_user_row u;
    T_EQ_INT(CB_STORE_CALL(g_ctx.store, user_get, IDS[1], &u), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(g_ctx.store, user_get, IDS[0], &u), CB_OK);
    // Again: not found, and the app not asked.
    T_EQ_INT(call("POST", "/admin/api/users/delete", NULL, body), 404);
    T_EQ_INT(g_deleted, 1);
    T_EQ_INT(call("GET", "/admin/api/users", NULL, NULL), 200);
    T_EQ_INT(rows("users"), 2);
    // An app with no delete_account deletes nothing.
    static const cb_app NO_DELETE = { .name = "no-delete" };
    g_ctx.app = &NO_DELETE;
    snprintf(body, sizeof body, "{\"id\":\"%s\"}", IDS[0]);
    T_EQ_INT(call("POST", "/admin/api/users/delete", NULL, body), 501);
    T_EQ_INT(CB_STORE_CALL(g_ctx.store, user_get, IDS[0], &u), CB_OK);
    teardown();
}

static void a_backup_is_a_file_beside_the_database(void) {
    setup();
    T_EQ_INT(call("GET", "/admin/api/backups", NULL, NULL), 200);
    T_EQ_INT(rows("files"), 0);
    T_EQ_INT(call("POST", "/admin/api/backups", NULL, NULL), 200);
    char written[600] = "", want_dir[400];
    T_CHECK(cb_json_get_str(B(), g_body.len, "written", written, sizeof written));
    snprintf(want_dir, sizeof want_dir, "%s/backups/cbase-", g_dir);
    T_CHECK(strncmp(written, want_dir, strlen(want_dir)) == 0);
    // The copy is a database with the accounts in it.
    cb_store *copy = NULL;
    T_EQ_INT(cb_store_open_sqlite(written, &copy), CB_OK);
    cb_user_row u;
    if (copy) {
        T_EQ_INT(CB_STORE_CALL(copy, user_by_email, EMAILS[2], &u), CB_OK);
        T_EQ_STR(u.id, IDS[2]);
        cb_store_close(copy);
    }
    T_EQ_INT(call("GET", "/admin/api/backups", NULL, NULL), 200);
    T_EQ_INT(rows("files"), 1);
    T_CHECK(str_at("files.0.name", strrchr(written, '/') + 1));
    teardown();
}

static void logs_tail_the_consoles_warnings(void) {
    setup();
    T_EQ_INT(call("GET", "/admin/api/logs", "level=warn", NULL), 200);
    int64_t last = -1;
    T_CHECK(cb_json_get_i64(B(), g_body.len, "last_seq", &last));
    T_EQ_INT(call_as("Bearer wrong", "GET", "/admin/api/users", NULL, NULL), 401);
    char after[32];
    snprintf(after, sizeof after, "after=%lld&level=warn", (long long)last);
    T_EQ_INT(call("GET", "/admin/api/logs", after, NULL), 200);
    T_EQ_INT(rows("entries"), 1);
    T_CHECK(str_at("entries.0.level", "warn"));
    T_CHECK(str_at("entries.0.module", "admin"));
    T_CHECK(says("wrong or missing admin token"));
    // Above warn: none of them.
    T_EQ_INT(call("GET", "/admin/api/logs", "level=error", NULL), 200);
    T_CHECK(!says("wrong or missing admin token"));
    T_EQ_INT(call("GET", "/admin/api/logs", "after=x", NULL), 400);
    teardown();
}

int main(void) {
    cb_log_init(CB_LOG_WARN, 0);
    T_RUN(the_gate);
    T_RUN(collections_are_the_tables);
    T_RUN(browse_pages_a_table);
    T_RUN(a_write_needs_the_word_and_a_text_prefix_matches);
    T_RUN(users_and_their_delete);
    T_RUN(a_backup_is_a_file_beside_the_database);
    T_RUN(logs_tail_the_consoles_warnings);
    cb_buf_free(&g_body);
    return T_DONE();
}
