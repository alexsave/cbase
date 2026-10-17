// unit_admin.c - the admin console's routes, driven through cb_handle with a
// fake store (test/lib_admin_fakestore.c): the gate, paging, the write
// confirmation, redaction, the log tail and the games list.
#include "t.h"
#include "app/limits.h"

#include "admin/admin.h"
#include "app/table_ops.h"
#include "auth/auth.h"
#include "auth/jwt.h"
#include "handler.h"
#include "lib_admin_fakestore.h"
#include "lib_store_backend.h"
#include "net/router.h"
#include "util/clock.h"
#include "util/json.h"
#include "util/log.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define ADMIN_TOKEN "admin-token-0123456789"
#define JWT_SECRET  "jwt-secret-that-is-long-enough-0123456789"

static cb_ctx g_ctx;
static cb_config g_cfg;
static fake_admin_record *g_rec;
static cb_buf g_body;   // the last response's body
static const char *g_peer;   // the caller's address ("" / NULL: none, not limited)

static void setup(const char *admin_token, const char *store_name) {
    if (g_ctx.router) cb_router_free(g_ctx.router);
    if (g_ctx.store) cb_store_close(g_ctx.store);
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&g_cfg, 0, sizeof g_cfg);
    cb_strlcpy(g_cfg.admin_token, admin_token, sizeof g_cfg.admin_token);
    cb_strlcpy(g_cfg.jwt_secret, JWT_SECRET, sizeof g_cfg.jwt_secret);
    cb_strlcpy(g_cfg.allowed_origins, "*", sizeof g_cfg.allowed_origins);
    cb_strlcpy(g_cfg.table, "cbase", sizeof g_cfg.table);
    cb_strlcpy(g_cfg.db_path, "/nonexistent-cbase-test/cbase.sqlite", sizeof g_cfg.db_path);
    cb_strlcpy(g_cfg.public_url, "http://127.0.0.1:8080", sizeof g_cfg.public_url);
    g_cfg.log_level = CB_LOG_INFO;
    g_ctx.cfg = &g_cfg;
    g_ctx.mode = CB_MODE_SERVE;
    g_ctx.version = CB_VERSION;
    g_ctx.started_ms = 1;
    g_ctx.router = cb_router_new();
    g_ctx.store = fake_admin_store_new(store_name, &g_rec);
    T_EQ_INT(cb_admin_routes(&g_ctx), CB_OK);
}

// One request through cb_handle. bearer NULL sends no Authorization. Returns
// the status; the body is in g_body.
static int call(const char *method, const char *path, const char *query, const char *bearer, const char *body) {
    cb_http_req q;
    memset(&q, 0, sizeof q);
    cb_strlcpy(q.method, method, sizeof q.method);
    q.path = path;
    q.query = query ? query : "";
    q.minor = 1;
    char authz[256];
    if (bearer) {
        snprintf(authz, sizeof authz, "Bearer %s", bearer);
        q.headers[q.n_headers++] = (cb_http_header){ "Authorization", authz };
    }
    if (body) {
        q.body = (const uint8_t *)body;
        q.body_len = strlen(body);
    }
    if (g_peer) cb_strlcpy(q.peer, g_peer, sizeof q.peer);
    cb_http_res r;
    cb_res_init(&r);
    cb_handle(&g_ctx, &q, &r);
    cb_buf_reset(&g_body);
    cb_buf_append(&g_body, r.body.p, r.body.len);
    int st = r.status;
    cb_res_free(&r);
    return st;
}

static const char *B(void) { return cb_buf_cstr(&g_body); }
static size_t BN(void) { return g_body.len; }

static void str_at(const char *path, char *out, size_t cap) {
    if (!cb_json_get_str(B(), BN(), path, out, cap)) cb_strlcpy(out, "(missing)", cap);
}

static void unset_token_hides_the_console(void) {
    setup("", "sqlite");
    T_EQ_INT(call("GET", "/admin", NULL, NULL, NULL), 404);
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, NULL, NULL), 404);
    // Even a bearer that would be right on another deploy finds nothing.
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, ADMIN_TOKEN, NULL), 404);
    T_EQ_INT(call("POST", "/admin/api/query", NULL, ADMIN_TOKEN, "{\"statement\":\"SELECT 1\",\"confirm\":\"WRITE\"}"), 404);
    T_EQ_INT(g_rec->query_calls, 0);
}

static void the_token_gates_every_call(void) {
    setup(ADMIN_TOKEN, "sqlite");
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, NULL, NULL), 401);
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, "wrong", NULL), 401);
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, "admin-token-012345678", NULL), 401);    // a prefix
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, ADMIN_TOKEN "9", NULL), 401);             // one more
    T_EQ_INT(call("POST", "/admin/api/query", NULL, "wrong", "{\"statement\":\"DELETE FROM x\",\"confirm\":\"WRITE\"}"), 401);
    T_EQ_INT(g_rec->query_calls, 0);
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, ADMIN_TOKEN, NULL), 200);
    // The page needs no bearer (a browser navigation cannot send one) and is the console.
    T_EQ_INT(call("GET", "/admin", NULL, NULL, NULL), 200);
    T_CHECK(strstr(B(), "<title>c_base console</title>") != NULL);
    // Unknown calls and wrong methods, behind the gate.
    T_EQ_INT(call("GET", "/admin/api/nope", NULL, ADMIN_TOKEN, NULL), 404);
    T_EQ_INT(call("GET", "/admin/api/query", NULL, ADMIN_TOKEN, NULL), 405);
    T_EQ_INT(call("GET", "/admin/api/nope", NULL, NULL, NULL), 401);
}

// One bearer guards the console (finding L7): an address's wrong tokens are
// counted, each is logged, and past the limit the address is refused before
// any token is compared, the right one included; another address is not.
static void wrong_tokens_are_limited_per_address(void) {
    setup(ADMIN_TOKEN, "sqlite");
    cb_limits_reset();
    g_peer = "198.51.100.66";
    uint64_t seq = cb_log_last_seq();
    const int b = cb_limit_rules[CB_LIMIT_ADMIN_FAIL_IP].burst;
    for (int i = 0; i < b; i++) T_EQ_INT(call("GET", "/admin/api/overview", NULL, "guess", NULL), 401);
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, "guess", NULL), 429);
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, ADMIN_TOKEN, NULL), 429);   // locked out, right token or not
    cb_log_entry e[64];
    size_t n = cb_log_read(seq, CB_LOG_WARN, e, 64);
    int warned = 0;
    for (size_t i = 0; i < n; i++) warned += strcmp(e[i].module, "admin") == 0 && strstr(e[i].msg, "198.51.100.66") != NULL;
    T_EQ_INT(warned, b);
    g_peer = "198.51.100.67";
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, ADMIN_TOKEN, NULL), 200);
    // The right token spends nothing.
    for (int i = 0; i < 2 * b; i++) T_EQ_INT(call("GET", "/admin/api/overview", NULL, ADMIN_TOKEN, NULL), 200);
    g_peer = NULL;
    cb_limits_reset();
}

static void overview_counts(void) {
    setup(ADMIN_TOKEN, "sqlite");
    atomic_store(&g_ctx.stats.requests, 42);
    T_EQ_INT(call("GET", "/admin/api/overview", NULL, ADMIN_TOKEN, NULL), 200);
    char v[64];
    int64_t n = -1;
    str_at("mode", v, sizeof v);
    T_EQ_STR(v, "serve");
    str_at("store", v, sizeof v);
    T_EQ_STR(v, "sqlite");
    T_CHECK(cb_json_get_i64(B(), BN(), "requests", &n));
    T_EQ_INT(n, 42);
    T_CHECK(cb_json_get_i64(B(), BN(), "games_needing_bots", &n));
    T_EQ_INT(n, 3);
    // The store's throttle counters, as the store keeps them.
    cb_store_throttles th;
    cb_store_throttles_get(&th);
    T_CHECK(cb_json_get_i64(B(), BN(), "store_throttles.throttled", &n));
    T_EQ_INT(n, (int64_t)th.throttled);
    T_CHECK(cb_json_get_i64(B(), BN(), "store_throttles.gave_up", &n));
    T_EQ_INT(n, (int64_t)th.gave_up);
}

static void browse_pages_by_cursor(void) {
    setup(ADMIN_TOKEN, "sqlite");
    char v[64];
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&limit=3", ADMIN_TOKEN, NULL), 200);
    T_EQ_INT(cb_json_array_len(B(), BN(), "rows"), 3);
    str_at("rows.0.id", v, sizeof v);
    T_EQ_STR(v, "u0");
    str_at("rows.2.email", v, sizeof v);
    T_EQ_STR(v, "u2@example.test");
    str_at("next", v, sizeof v);
    T_EQ_STR(v, "3");
    T_EQ_INT(g_rec->last_limit, 3);
    // The cursor the page answered is the one the next call hands the store.
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&limit=3&cursor=3", ADMIN_TOKEN, NULL), 200);
    T_EQ_STR(g_rec->last_cursor, "3");
    str_at("rows.0.id", v, sizeof v);
    T_EQ_STR(v, "u3");
    str_at("next", v, sizeof v);
    T_EQ_STR(v, "6");
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&limit=3&cursor=6", ADMIN_TOKEN, NULL), 200);
    T_EQ_INT(cb_json_array_len(B(), BN(), "rows"), 1);
    str_at("next", v, sizeof v);
    T_EQ_STR(v, "");
    // A prefix, and the page size cap.
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&prefix=u4", ADMIN_TOKEN, NULL), 200);
    T_EQ_INT(cb_json_array_len(B(), BN(), "rows"), 1);
    T_EQ_INT(call("GET", "/admin/api/browse", "c=users&limit=100000", ADMIN_TOKEN, NULL), 200);
    T_EQ_INT(g_rec->last_limit, 500);
    // Errors: no collection, an unknown one.
    T_EQ_INT(call("GET", "/admin/api/browse", "", ADMIN_TOKEN, NULL), 400);
    T_EQ_INT(call("GET", "/admin/api/browse", "c=nope", ADMIN_TOKEN, NULL), 404);
    T_EQ_INT(call("GET", "/admin/api/collections", NULL, ADMIN_TOKEN, NULL), 200);
    str_at("collections.1", v, sizeof v);
    T_EQ_STR(v, "games");
}

static void query_needs_the_typed_confirmation_to_write(void) {
    setup(ADMIN_TOKEN, "sqlite");
    // A write with no confirmation: the store is asked read-only and refuses.
    T_EQ_INT(call("POST", "/admin/api/query", NULL, ADMIN_TOKEN, "{\"statement\":\"DELETE FROM users\"}"), 403);
    T_EQ_INT(g_rec->query_calls, 1);
    T_CHECK(!g_rec->last_allow_write);
    // Anything but the exact word is no confirmation.
    T_EQ_INT(call("POST", "/admin/api/query", NULL, ADMIN_TOKEN, "{\"statement\":\"DELETE FROM users\",\"confirm\":\"write\"}"), 403);
    T_CHECK(!g_rec->last_allow_write);
    T_EQ_INT(call("POST", "/admin/api/query", NULL, ADMIN_TOKEN, "{\"statement\":\"DELETE FROM users\",\"confirm\":true}"), 403);
    T_CHECK(!g_rec->last_allow_write);
    // The word: allowed, and said so.
    T_EQ_INT(call("POST", "/admin/api/query", NULL, ADMIN_TOKEN, "{\"statement\":\"DELETE FROM users\",\"confirm\":\"WRITE\"}"), 200);
    T_CHECK(g_rec->last_allow_write);
    T_EQ_STR(g_rec->last_statement, "DELETE FROM users");
    bool wa = false;
    T_CHECK(cb_json_get_bool(B(), BN(), "write_allowed", &wa));
    T_CHECK(wa);
    // A read needs none, and is still asked read-only.
    T_EQ_INT(call("POST", "/admin/api/query", NULL, ADMIN_TOKEN, "{\"statement\":\"SELECT 1\"}"), 200);
    T_CHECK(!g_rec->last_allow_write);
    char v[16];
    str_at("rows.0.n", v, sizeof v);
    T_EQ_STR(v, "1");
    // No statement.
    T_EQ_INT(call("POST", "/admin/api/query", NULL, ADMIN_TOKEN, "{}"), 400);
    T_EQ_INT(call("POST", "/admin/api/query", NULL, ADMIN_TOKEN, NULL), 400);
}

static void config_never_shows_a_secret(void) {
    setup(ADMIN_TOKEN, "dynamo");
    T_EQ_INT(call("GET", "/admin/api/config", NULL, ADMIN_TOKEN, NULL), 200);
    T_CHECK(strstr(B(), ADMIN_TOKEN) == NULL);
    T_CHECK(strstr(B(), JWT_SECRET) == NULL);
    T_CHECK(strstr(B(), "\"CBASE_ADMIN_TOKEN\"") != NULL);
    char v[64];
    str_at("store", v, sizeof v);
    T_EQ_STR(v, "dynamo");
    str_at("allowed_origins", v, sizeof v);
    T_EQ_STR(v, "*");
    // Under DynamoDB the backups panel explains point-in-time recovery and
    // refuses a trigger.
    T_EQ_INT(call("GET", "/admin/api/backups", NULL, ADMIN_TOKEN, NULL), 200);
    str_at("kind", v, sizeof v);
    T_EQ_STR(v, "pitr");
    T_EQ_INT(call("POST", "/admin/api/backups", NULL, ADMIN_TOKEN, NULL), 409);
}

static void logs_tail_after_a_seq_at_a_level(void) {
    setup(ADMIN_TOKEN, "sqlite");
    uint64_t before = cb_log_last_seq();
    CB_INFO("t", "admin-tail info line");
    CB_WARN("t", "admin-tail warn line");
    CB_ERROR("t", "admin-tail error line");
    char q[64];
    snprintf(q, sizeof q, "after=%llu&level=warn", (unsigned long long)before);
    T_EQ_INT(call("GET", "/admin/api/logs", q, ADMIN_TOKEN, NULL), 200);
    T_EQ_INT(cb_json_array_len(B(), BN(), "entries"), 2);
    char v[64];
    str_at("entries.0.msg", v, sizeof v);
    T_EQ_STR(v, "admin-tail warn line");
    str_at("entries.1.level", v, sizeof v);
    T_EQ_STR(v, "error");
    int64_t last = 0;
    T_CHECK(cb_json_get_i64(B(), BN(), "last_seq", &last));
    T_EQ_INT(last, (int64_t)before + 3);
    // Nothing new after the last seq.
    snprintf(q, sizeof q, "after=%lld", (long long)last);
    T_EQ_INT(call("GET", "/admin/api/logs", q, ADMIN_TOKEN, NULL), 200);
    T_EQ_INT(cb_json_array_len(B(), BN(), "entries"), 0);
    T_EQ_INT(call("GET", "/admin/api/logs", "level=loud", ADMIN_TOKEN, NULL), 400);
}

static void games_list_reads_each_row(void) {
    setup(ADMIN_TOKEN, "sqlite");
    T_EQ_INT(call("GET", "/admin/api/games", NULL, ADMIN_TOKEN, NULL), 200);
    // bbb222 is browsed but no longer there: skipped, not an error.
    T_EQ_INT(cb_json_array_len(B(), BN(), "games"), 1);
    char v[64];
    int64_t n = 0;
    bool nb = false;
    str_at("games.0.id", v, sizeof v);
    T_EQ_STR(v, "aaa111");
    str_at("games.0.status_label", v, sizeof v);
    T_EQ_STR(v, "playing");
    T_CHECK(cb_json_get_i64(B(), BN(), "games.0.version", &n));
    T_EQ_INT(n, 7);
    T_CHECK(cb_json_get_bool(B(), BN(), "games.0.needs_bots", &nb));
    T_CHECK(nb);
    T_EQ_INT(call("GET", "/admin/api/game", "id=zzz999", ADMIN_TOKEN, NULL), 404);
    T_EQ_INT(call("GET", "/admin/api/game", "id=aaa111", ADMIN_TOKEN, NULL), 200);
    T_EQ_INT(call("GET", "/admin/api/bots", NULL, ADMIN_TOKEN, NULL), 200);
    str_at("bots.0.nickname", v, sizeof v);
    T_EQ_STR(v, "%Cordite");
    T_EQ_INT(call("GET", "/admin/api/users", "search=player4", ADMIN_TOKEN, NULL), 200);
    T_EQ_INT(cb_json_array_len(B(), BN(), "users"), 1);
}

static void sqlite_backup_is_vacuum_into(void) {
    setup(ADMIN_TOKEN, "sqlite");
    char tmpl[] = "/tmp/cbase-admin-XXXXXX";
    char *dir = mkdtemp(tmpl);
    T_CHECK(dir != NULL);
    if (!dir) return;
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, "%s/it's.sqlite", dir);   // a quote in the path
    T_EQ_INT(call("POST", "/admin/api/backups", NULL, ADMIN_TOKEN, NULL), 200);
    T_CHECK(g_rec->last_allow_write);
    char want[600];
    snprintf(want, sizeof want, "VACUUM INTO '%s/backups/cbase-", dir);
    T_CHECK(strncmp(g_rec->last_statement, want, strlen(want)) == 0);
    size_t L = strlen(g_rec->last_statement);
    T_CHECK(L > 8 && strcmp(g_rec->last_statement + L - 8, ".sqlite'") == 0);
    // The backups directory sits beside the database; a quote in it is doubled.
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, "%s/q'd/x.sqlite", dir);
    char qd[600];
    snprintf(qd, sizeof qd, "%s/q'd", dir);
    T_EQ_INT(mkdir(qd, 0700), 0);
    T_EQ_INT(call("POST", "/admin/api/backups", NULL, ADMIN_TOKEN, NULL), 200);
    T_CHECK(strstr(g_rec->last_statement, "/q''d/backups/cbase-") != NULL);
    T_EQ_INT(call("GET", "/admin/api/backups", NULL, ADMIN_TOKEN, NULL), 200);
    char v[32];
    str_at("kind", v, sizeof v);
    T_EQ_STR(v, "vacuum_into");
    snprintf(qd, sizeof qd, "%s/q'd/backups", dir);
    rmdir(qd);
    snprintf(qd, sizeof qd, "%s/q'd", dir);
    rmdir(qd);
    snprintf(qd, sizeof qd, "%s/backups", dir);
    rmdir(qd);
    rmdir(dir);
}

// Deleting a user is cb_app_delete_account, delete-account's own function, run
// here against a real store (SQLite, or DynamoDB Local under
// CBASE_STORE=dynamo) and real auth: a user seated at a lobby
// is redacted from it and the account is gone.
static bool count_game(const cb_member_row *m, void *ud) { (void)m; (*(int *)ud)++; return true; }

static void user_delete_is_the_delete_account_path(void) {
    setup(ADMIN_TOKEN, "sqlite");
    cb_store_close(g_ctx.store);
    char db[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(db, sizeof db, "%s/cbase-admin-delete-%d.sqlite", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    unlink(db);
    g_ctx.store = NULL;
    g_ctx.store = tsb_open_fresh(db);
    T_CHECK(g_ctx.store != NULL);
    if (!g_ctx.store) return;
    T_EQ_INT(cb_auth_new(&g_ctx, &g_ctx.auth), CB_OK);
    cb_session s;
    T_EQ_INT(cb_auth_signup(g_ctx.auth, "gone@foolish.cards", "secret-11", "GONE", &s), CB_OK);
    cb_table_result made;
    T_EQ_INT(cb_table_create(&g_ctx, &s.user, &made), CB_OK);
    free(made.envelope);
    int games = 0;
    T_EQ_INT(CB_STORE_CALL(g_ctx.store, games_of_user, s.user.id, 10, count_game, &games), 1);

    T_EQ_INT(call("POST", "/admin/api/users/delete", NULL, ADMIN_TOKEN, "{\"id\":\"nobody\"}"), 404);
    T_EQ_INT(call("POST", "/admin/api/users/delete", NULL, ADMIN_TOKEN, "{}"), 400);
    char body[96];
    snprintf(body, sizeof body, "{\"id\":\"%s\"}", s.user.id);
    T_EQ_INT(call("POST", "/admin/api/users/delete", NULL, "not-the-token", body), 401);
    cb_user_row row;
    T_EQ_INT(CB_STORE_CALL(g_ctx.store, user_get, s.user.id, &row), CB_OK);
    T_EQ_INT(call("POST", "/admin/api/users/delete", NULL, ADMIN_TOKEN, body), 200);
    T_CHECK(strstr(B(), s.user.id) != NULL);
    T_EQ_INT(CB_STORE_CALL(g_ctx.store, user_get, s.user.id, &row), CB_E_NOT_FOUND);
    games = 0;
    CB_STORE_CALL(g_ctx.store, games_of_user, s.user.id, 10, count_game, &games);
    T_EQ_INT(games, 0);
    cb_auth_free(g_ctx.auth);
    g_ctx.auth = NULL;
    cb_store_close(g_ctx.store);
    g_ctx.store = NULL;
    unlink(db);
}

int main(void) {
    cb_log_init(CB_LOG_DEBUG, 64);
    cb_buf_init(&g_body);
    T_RUN(unset_token_hides_the_console);
    T_RUN(the_token_gates_every_call);
    T_RUN(wrong_tokens_are_limited_per_address);
    T_RUN(overview_counts);
    T_RUN(browse_pages_by_cursor);
    T_RUN(query_needs_the_typed_confirmation_to_write);
    T_RUN(config_never_shows_a_secret);
    T_RUN(logs_tail_after_a_seq_at_a_level);
    T_RUN(games_list_reads_each_row);
    T_RUN(sqlite_backup_is_vacuum_into);
    T_RUN(user_delete_is_the_delete_account_path);
    cb_router_free(g_ctx.router);
    if (g_ctx.store) cb_store_close(g_ctx.store);
    cb_buf_free(&g_body);
    return T_DONE();
}
