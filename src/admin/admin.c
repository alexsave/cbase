// admin.c - the c_base console (admin.h): GET /admin serves the embedded
// admin/console.html, and /admin/api/* is its JSON API.
//
// THE GATE. Every route here, the page included, answers 404 when
// CBASE_ADMIN_TOKEN is unset (the console does not exist on that deploy:
// console_on()). Every API call also answers 401 unless the request carries
// "Authorization: Bearer <CBASE_ADMIN_TOKEN>", compared in constant time
// (admin_gate()). The page holds no data and no secret, and a browser cannot
// send a Bearer on a navigation, so the page needs only console_on().
//
// WHAT IT READS. The store's admin_* operations (collections, browse, query)
// and its ordinary reads (users_list, bots_list, game_get,
// games_needing_bots); util/log.h's ring; config.h's cb_config_each (secrets
// already redacted there); ctx->stats and getrusage for the process. Nothing
// here writes but three paths, each one a deliberate act in the console:
//   * POST query with confirm "WRITE": the store's admin_query with
//     allow_write. Without that exact word allow_write is false, and the
//     STORE is the one that refuses a write (SQLite's sqlite3_stmt_readonly,
//     DynamoDB's PartiQL statement kind); this file never parses SQL.
//   * POST users/delete: cb_app_delete_account, the SAME function the app's
//     delete-account route calls, so the redaction from every game, the
//     leaderboard name and the account delete stay one implementation.
//   * POST backups (SQLite only): VACUUM INTO a timestamped file beside the
//     database, through admin_query. DynamoDB has point-in-time recovery on
//     (ops/cbase.yaml) and the console explains it rather than copying.
//
// JSON is tolerated here (DESIGN.md "Where JSON is tolerated", item 4): the
// console is a browser page we own, and JSON is what fetch reads.
#include "admin.h"

#include "app/functions_http.h"
#include "app/table_ops.h"
#include "handler.h"
#include "net/router.h"
#include "roster.h"
#include "store/store.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/hex.h"
#include "util/json.h"
#include "util/log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

// build/gen/console_html.c, from admin/console.html (tools/embed.c).
extern const char cbase_console_html[];
extern const unsigned long cbase_console_html_len;

#define ADMIN_QUERY_ROWS   1000   // a query answers at most this many rows
#define ADMIN_PAGE_MAX     500    // browse and users page size cap
#define ADMIN_LOG_MAX      500    // log entries per poll
#define ADMIN_WRITE_WORD   "WRITE"   // the typed confirmation a write needs

// ---- the gate --------------------------------------------------------------------

static bool const_eq(const char *a, const char *b) {
    size_t an = strlen(a), bn = strlen(b);
    unsigned char d = (unsigned char)(an != bn);
    for (size_t i = 0; i < an; i++) d |= (unsigned char)(a[i] ^ b[i < bn ? i : 0]);
    return d == 0;
}

// The console exists only when CBASE_ADMIN_TOKEN is set; otherwise every
// route here answers 404, as if it were not routed at all.
static bool console_on(const cb_ctx *ctx, cb_http_res *res) {
    if (ctx->cfg->admin_token[0]) return true;
    cb_router_error(res, 404, "not found");
    return false;
}

// true when an API request may proceed; otherwise res holds the refusal.
static bool admin_gate(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!console_on(ctx, res)) return false;
    const char *tok = cb_http_bearer(req);
    if (!tok || !const_eq(tok, ctx->cfg->admin_token)) {
        cb_router_error(res, 401, "admin token required");
        return false;
    }
    return true;
}

// ---- answers ---------------------------------------------------------------------

static void send_json(cb_http_res *res, int status, cb_buf *b, const cb_jw *w) {
    if (!cb_jw_ok(w)) {
        cb_router_error(res, 500, "admin: response did not encode");
        return;
    }
    cb_res_send(res, status, "application/json", b->p, b->len);
    cb_res_header(res, "Cache-Control", "no-store");
}

// A store refusal as an HTTP answer.
static void store_error(cb_ctx *ctx, cb_http_res *res, const char *what, int rc) {
    char m[256];
    int status;
    switch (rc) {
    case CB_E_NOTIMPL:   status = 501; snprintf(m, sizeof m, "%s: not implemented by the %s store", what, ctx->store->ops->name); break;
    case CB_E_NOT_FOUND: status = 404; snprintf(m, sizeof m, "%s: not found", what); break;
    case CB_E_FORBIDDEN: status = 403; snprintf(m, sizeof m, "%s: refused (a write needs the typed confirmation)", what); break;
    case CB_E_INVALID:   status = 400; snprintf(m, sizeof m, "%s: invalid", what); break;
    default:             status = 500; snprintf(m, sizeof m, "%s: %s", what, cb_strerror(rc)); break;
    }
    cb_router_error(res, status, m);
}

static bool need_store(cb_ctx *ctx, cb_http_res *res) {
    if (ctx->store) return true;
    cb_router_error(res, 503, "store not available");
    return false;
}

static int q_int(const cb_http_req *req, const char *key, int dflt, int lo, int hi) {
    char v[32];
    if (!cb_http_query_get(req, key, v, sizeof v) || !v[0]) return dflt;
    char *end = NULL;
    long x = strtol(v, &end, 10);
    if (*end) return dflt;
    return x < lo ? lo : x > hi ? hi : (int)x;
}

static void q_str(const cb_http_req *req, const char *key, char *out, size_t cap) {
    if (!cb_http_query_get(req, key, out, cap)) out[0] = 0;
}

static void kiso(cb_jw *w, const char *k, int64_t ms) {
    char t[40];
    if (ms <= 0 || cb_iso8601(ms, t, sizeof t) < 0) cb_jw_knull(w, k);
    else cb_jw_kstr(w, k, t);
}

static const char *store_name(const cb_ctx *ctx) { return ctx->store ? ctx->store->ops->name : "none"; }
static bool is_sqlite(const cb_ctx *ctx) { return ctx->store && strcmp(ctx->store->ops->name, "sqlite") == 0; }

static const char *getenv0(const char *k) {
    const char *v = getenv(k);
    return v ? v : "";
}

// ---- links into the AWS console (lambda mode) -------------------------------------

// The CloudWatch console's own escaping inside a queryDetail: every byte but
// [A-Za-z0-9_.-] as '*' and two lowercase hex digits.
static void aws_star_escape(cb_buf *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')
            cb_buf_putc(b, (char)c);
        else
            cb_buf_printf(b, "*%02x", c);
    }
}

// Writes "links": {...} for the function's AWS console pages, or nothing
// outside Lambda.
static void write_links(const cb_ctx *ctx, cb_jw *w) {
    const char *region = getenv0("AWS_REGION");
    const char *fn = getenv0("AWS_LAMBDA_FUNCTION_NAME");
    const char *group = getenv0("AWS_LAMBDA_LOG_GROUP_NAME");
    if (ctx->mode != CB_MODE_LAMBDA || !region[0] || !fn[0]) { cb_jw_knull(w, "links"); return; }
    cb_buf u;
    cb_buf_init(&u);
    cb_jw_key(w, "links");
    cb_jw_obj(w);
    cb_buf_printf(&u, "https://%s.console.aws.amazon.com/lambda/home?region=%s#/functions/%s", region, region, fn);
    cb_jw_kstr(w, "lambda", cb_buf_cstr(&u));
    cb_buf_reset(&u);
    char lg[300];
    snprintf(lg, sizeof lg, "%s", group[0] ? group : "");
    if (!lg[0]) snprintf(lg, sizeof lg, "/aws/lambda/%s", fn);
    cb_buf_printf(&u, "https://%s.console.aws.amazon.com/cloudwatch/home?region=%s#logsV2:logs-insights"
                      "$3FqueryDetail$3D~(end~0~start~-3600~timeType~'RELATIVE~unit~'seconds~editorString~'",
                  region, region);
    aws_star_escape(&u, "fields @timestamp, @message\n| sort @timestamp desc\n| limit 200");
    cb_buf_puts(&u, "~source~(~'");
    aws_star_escape(&u, lg);
    cb_buf_puts(&u, "))");
    cb_jw_kstr(w, "logs_insights", cb_buf_cstr(&u));
    cb_buf_reset(&u);
    cb_buf_printf(&u, "https://%s.console.aws.amazon.com/dynamodbv2/home?region=%s#table?name=%s", region, region,
                  ctx->cfg->table);
    cb_jw_kstr(w, "dynamodb", cb_buf_cstr(&u));
    cb_buf_reset(&u);
    cb_buf_printf(&u, "https://%s.console.aws.amazon.com/dynamodbv2/home?region=%s#table?name=%s&tab=backups", region,
                  region, ctx->cfg->table);
    cb_jw_kstr(w, "backups", cb_buf_cstr(&u));
    cb_jw_end(w);
    cb_buf_free(&u);
}

// ---- backups (SQLite) -------------------------------------------------------------

// The directory backups go to: <dir of CBASE_DB>/backups.
static void backup_dir(const cb_ctx *ctx, char *out, size_t cap) {
    const char *db = ctx->cfg->db_path;
    const char *slash = strrchr(db, '/');
    if (!slash) snprintf(out, cap, "backups");
    else snprintf(out, cap, "%.*s/backups", (int)(slash - db), db);
}

typedef struct {
    char    name[256];
    int64_t size;
    int64_t mtime_ms;
} backup_file;

static int backup_cmp(const void *a, const void *b) {
    return strcmp(((const backup_file *)b)->name, ((const backup_file *)a)->name);   // descending
}

// The backup files, newest first (the names are UTC stamps, so name order is
// time order). Returns how many were written to out, at most cap.
#define BACKUPS_SEEN_MAX 1024
static int list_backups(const cb_ctx *ctx, backup_file *out, int cap) {
    char dir[1100];
    backup_dir(ctx, dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return 0;
    backup_file *all = malloc(BACKUPS_SEEN_MAX * sizeof *all);
    int n = 0;
    struct dirent *e;
    while (all && n < BACKUPS_SEEN_MAX && (e = readdir(d)) != NULL) {
        size_t L = strlen(e->d_name);
        if (L < 8 || strcmp(e->d_name + L - 7, ".sqlite") != 0 || !cb_starts_with(e->d_name, "cbase-")) continue;
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0) continue;
        cb_strlcpy(all[n].name, e->d_name, sizeof all[n].name);
        all[n].size = (int64_t)st.st_size;
        all[n].mtime_ms = (int64_t)st.st_mtime * 1000;
        n++;
    }
    closedir(d);
    if (!all) return 0;
    qsort(all, (size_t)n, sizeof *all, backup_cmp);
    if (n > cap) n = cap;
    memcpy(out, all, (size_t)n * sizeof *all);
    free(all);
    return n;
}

// ---- GET /admin/api/overview ------------------------------------------------------

static bool count_name(const char *name, void *ud) { (*(int *)ud)++; return true; }

static void api_overview(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    int64_t now = cb_now_ms();
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "version", ctx->version);
    cb_jw_kstr(&w, "mode", ctx->mode == CB_MODE_LAMBDA ? "lambda" : "serve");
    cb_jw_kint(&w, "uptime_s", (now - ctx->started_ms) / 1000);
    kiso(&w, "started_at", ctx->started_ms);
    cb_jw_kstr(&w, "store", store_name(ctx));
    cb_jw_kstr(&w, "table", ctx->cfg->table);
    cb_jw_kstr(&w, "public_url", ctx->cfg->public_url);
    cb_jw_kint(&w, "requests", (int64_t)atomic_load_explicit(&ctx->stats.requests, memory_order_relaxed));
    cb_jw_kint(&w, "errors_5xx", (int64_t)atomic_load_explicit(&ctx->stats.responses_5xx, memory_order_relaxed));
    if (ctx->mode == CB_MODE_SERVE) {
        cb_jw_kint(&w, "http_conns", atomic_load_explicit(&ctx->stats.http_conns, memory_order_relaxed));
        cb_jw_kint(&w, "ws_conns", atomic_load_explicit(&ctx->stats.ws_conns, memory_order_relaxed));
    } else {
        // API Gateway holds the sockets; this container sees none of them.
        cb_jw_knull(&w, "http_conns");
        cb_jw_knull(&w, "ws_conns");
    }
    // The heartbeat's own criteria (bots.h cb_bots_scan): untouched for 10 s,
    // active within the hour.
    int due = 0;
    int rc = ctx->store ? CB_STORE_CALL(ctx->store, games_needing_bots, now - 10000, now - 3600000, 1000, count_name, &due)
                        : CB_E_STATE;
    if (rc < 0) cb_jw_knull(&w, "games_needing_bots");
    else cb_jw_kint(&w, "games_needing_bots", due);
    // DynamoDB throttling (store.h cb_store_throttles_get): this process's
    // throttled responses, and the requests that failed still throttled.
    // Nonzero means the provisioned capacity is too small for the load.
    cb_store_throttles th;
    cb_store_throttles_get(&th);
    cb_jw_key(&w, "store_throttles");
    cb_jw_obj(&w);
    cb_jw_kint(&w, "throttled", (int64_t)th.throttled);
    cb_jw_kint(&w, "gave_up", (int64_t)th.gave_up);
    cb_jw_end(&w);
    if (is_sqlite(ctx)) {
        backup_file f[1];
        if (list_backups(ctx, f, 1) == 1) kiso(&w, "last_backup", f[0].mtime_ms);
        else cb_jw_knull(&w, "last_backup");
    } else {
        cb_jw_kstr(&w, "last_backup", "continuous (point-in-time recovery)");
    }
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    int64_t rss_kb = (int64_t)ru.ru_maxrss / 1024;   // bytes on macOS
#else
    int64_t rss_kb = (int64_t)ru.ru_maxrss;          // kilobytes on Linux
#endif
    cb_jw_key(&w, "process");
    cb_jw_obj(&w);
    cb_jw_kint(&w, "pid", (int64_t)getpid());
    cb_jw_kint(&w, "max_rss_kb", rss_kb);
    cb_jw_kint(&w, "cpu_user_ms", (int64_t)ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000);
    cb_jw_kint(&w, "cpu_sys_ms", (int64_t)ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000);
    cb_jw_end(&w);
    write_links(ctx, &w);
    cb_jw_end(&w);
    send_json(res, 200, &b, &w);
    cb_buf_free(&b);
}

// ---- GET /admin/api/collections ---------------------------------------------------

static bool put_name(const char *name, void *ud) { cb_jw_str(ud, name); return true; }

static void api_collections(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!need_store(ctx, res)) return;
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "store", store_name(ctx));
    cb_jw_key(&w, "collections");
    cb_jw_arr(&w);
    int rc = CB_STORE_CALL(ctx->store, admin_collections, put_name, &w);
    cb_jw_end(&w);
    cb_jw_end(&w);
    if (rc < 0) store_error(ctx, res, "collections", rc);
    else send_json(res, 200, &b, &w);
    cb_buf_free(&b);
}

// ---- rows as JSON objects (browse and query) ----------------------------------------

typedef struct {
    cb_jw *w;
    int    n, cap;
    bool   truncated;
} row_sink;

static bool put_row(int n, const char *const *names, const char *const *values, void *ud) {
    row_sink *s = ud;
    if (s->n >= s->cap) { s->truncated = true; return false; }
    s->n++;
    cb_jw_obj(s->w);
    for (int i = 0; i < n; i++) {
        cb_jw_key(s->w, names[i] ? names[i] : "?");
        cb_jw_str(s->w, values[i]);   // NULL writes null
    }
    cb_jw_end(s->w);
    return true;
}

// GET /admin/api/browse?c=<collection>&prefix=&cursor=&limit=
static void api_browse(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!need_store(ctx, res)) return;
    char coll[128], prefix[256], cursor[512], next[512] = "";
    q_str(req, "c", coll, sizeof coll);
    q_str(req, "prefix", prefix, sizeof prefix);
    q_str(req, "cursor", cursor, sizeof cursor);
    int limit = q_int(req, "limit", 50, 1, ADMIN_PAGE_MAX);
    if (!coll[0]) { cb_router_error(res, 400, "browse: c (the collection) is required"); return; }
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "collection", coll);
    cb_jw_key(&w, "rows");
    cb_jw_arr(&w);
    row_sink s = { &w, 0, limit, false };
    int rc = CB_STORE_CALL(ctx->store, admin_browse, coll, prefix, cursor[0] ? cursor : NULL, limit, put_row, &s, next,
                           sizeof next);
    cb_jw_end(&w);
    cb_jw_kstr(&w, "next", next);
    cb_jw_end(&w);
    if (rc < 0) store_error(ctx, res, "browse", rc);
    else send_json(res, 200, &b, &w);
    cb_buf_free(&b);
}

// POST /admin/api/query  {"statement": "...", "confirm": "WRITE"?}
static void api_query(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!need_store(ctx, res)) return;
    const char *body = (const char *)req->body;
    size_t n = req->body_len;
    char *stmt = malloc(n + 1);
    char confirm[32];
    if (!stmt) { cb_router_error(res, 500, "out of memory"); return; }
    if (!body || !cb_json_get_str(body, n, "statement", stmt, n + 1) || !stmt[0]) {
        free(stmt);
        cb_router_error(res, 400, "query: a JSON body with \"statement\" is required");
        return;
    }
    if (!cb_json_get_str(body, n, "confirm", confirm, sizeof confirm)) confirm[0] = 0;
    bool allow_write = strcmp(confirm, ADMIN_WRITE_WORD) == 0;
    if (allow_write) CB_WARN("admin", "write statement confirmed: %.180s", stmt);
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "dialect", is_sqlite(ctx) ? "sql" : "partiql");
    cb_jw_kbool(&w, "write_allowed", allow_write);
    cb_jw_key(&w, "rows");
    cb_jw_arr(&w);
    row_sink s = { &w, 0, ADMIN_QUERY_ROWS, false };
    int rc = CB_STORE_CALL(ctx->store, admin_query, stmt, allow_write, put_row, &s);
    cb_jw_end(&w);
    cb_jw_kbool(&w, "truncated", s.truncated);
    cb_jw_end(&w);
    if (rc < 0) store_error(ctx, res, "query", rc);
    else send_json(res, 200, &b, &w);
    cb_buf_free(&b);
    free(stmt);
}

// ---- users ---------------------------------------------------------------------------

typedef struct {
    cb_jw *w;
    int    n;
} user_sink;

static bool put_user(const cb_user_row *u, void *ud) {
    user_sink *s = ud;
    cb_jw *w = s->w;
    s->n++;
    cb_jw_obj(w);
    cb_jw_kstr(w, "id", u->id);
    cb_jw_kstr(w, "email", u->email);
    cb_jw_kstr(w, "username", u->username);
    kiso(w, "created_at", u->created_at);
    kiso(w, "last_sign_in_at", u->last_sign_in_at);
    cb_jw_end(w);
    return true;
}

// GET /admin/api/users?search=&offset=&limit=
static void api_users(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!need_store(ctx, res)) return;
    char search[128];
    q_str(req, "search", search, sizeof search);
    int offset = q_int(req, "offset", 0, 0, 1 << 30);
    int limit = q_int(req, "limit", 50, 1, ADMIN_PAGE_MAX);
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_key(&w, "users");
    cb_jw_arr(&w);
    user_sink s = { &w, 0 };
    int rc = CB_STORE_CALL(ctx->store, users_list, search, offset, limit, put_user, &s);
    cb_jw_end(&w);
    cb_jw_kint(&w, "offset", offset);
    // A full page means there may be more.
    if (s.n == limit) cb_jw_kint(&w, "next_offset", offset + limit);
    else cb_jw_knull(&w, "next_offset");
    cb_jw_end(&w);
    if (rc < 0) store_error(ctx, res, "users", rc);
    else send_json(res, 200, &b, &w);
    cb_buf_free(&b);
}

// POST /admin/api/users/delete  {"id": "<uuid>"}: cb_app_delete_account, the
// one implementation delete-account runs (see the header comment).
static void api_user_delete(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!need_store(ctx, res)) return;
    char id[CB_UUID_LEN + 1];
    if (!req->body || !cb_json_get_str((const char *)req->body, req->body_len, "id", id, sizeof id) || !id[0]) {
        cb_router_error(res, 400, "delete: a JSON body with \"id\" is required");
        return;
    }
    cb_user_row u;
    int rc = CB_STORE_CALL(ctx->store, user_get, id, &u);
    if (rc < 0) { store_error(ctx, res, "delete: user", rc); return; }
    int code = 0, detail = 0;
    rc = cb_app_delete_account(ctx, id, &code, &detail);
    CB_WARN("admin", "delete-account for user %s: %s", id, cb_strerror(rc));
    if (rc != CB_OK) {
        char m[160];
        if (code) snprintf(m, sizeof m, "delete: a table refused the redaction (kernel code %d, detail %d)", code, detail);
        else snprintf(m, sizeof m, "delete: %s", cb_strerror(rc));
        cb_router_error(res, code ? 409 : rc == CB_E_STATE ? 503 : 500, m);
        return;
    }
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "deleted", id);
    cb_jw_end(&w);
    send_json(res, 200, &b, &w);
    cb_buf_free(&b);
}

// ---- games ----------------------------------------------------------------------------

// One game's scalar columns and its seats, from the stored row.
static void write_game(cb_jw *w, const cb_game_row *g) {
    cb_jw_kstr(w, "id", g->id);
    cb_jw_kint(w, "status", g->status);
    cb_jw_kstr(w, "status_label", cb_table_status_label(g->status));
    cb_jw_kint(w, "version", g->version);
    cb_jw_kint(w, "round_epoch", g->round_epoch);
    cb_jw_kbool(w, "needs_bots", g->needs_bots);
    kiso(w, "created_at", g->created_at);
    kiso(w, "updated_at", g->updated_at);
    kiso(w, "last_commit_at", g->last_commit_at);
    // The seats, read with the kernel's own decoder (no byte layout here).
    Roster r;
    int rrc = g->roster ? roster_decode(&r, g->roster, (int)g->roster_len) : ROSTER_E_LENGTH;
    cb_jw_key(w, "seats");
    if (rrc != ROSTER_OK) {
        cb_jw_null(w);
        cb_jw_kint(w, "roster_error", rrc);
        return;
    }
    cb_jw_arr(w);
    for (int i = 0; i < r.n; i++) {
        cb_jw_obj(w);
        cb_jw_key(w, "name");
        cb_jw_strn(w, r.seats[i].name, r.seats[i].name_len);
        cb_jw_key(w, "id");
        cb_jw_strn(w, r.seats[i].id, r.seats[i].id_len);
        cb_jw_kbool(w, "bot", r.seats[i].brain_len > 0);
        cb_jw_end(w);
    }
    cb_jw_end(w);
    cb_jw_key(w, "title");
    cb_jw_strn(w, r.title, r.title_len);
}

#define GAMES_PAGE_MAX 100

typedef struct {
    char ids[GAMES_PAGE_MAX][CB_GAME_ID_MAX + 1];
    int  n, cap;
} id_sink;

static const char *col(int n, const char *const *names, const char *const *values, const char *want) {
    for (int i = 0; i < n; i++)
        if (names[i] && strcmp(names[i], want) == 0) return values[i];
    return NULL;
}

// A browse row of the "games" collection names a game: SQLite's games.id, or
// DynamoDB's pk "GAME#<id>" on the sk "ROW" item (MEMBER and BOT items under
// the same pk are skipped).
static bool take_game_id(int n, const char *const *names, const char *const *values, void *ud) {
    id_sink *s = ud;
    const char *id = col(n, names, values, "id");
    if (!id) {
        const char *pk = col(n, names, values, "pk"), *sk = col(n, names, values, "sk");
        if (pk && cb_starts_with(pk, "GAME#") && (!sk || strcmp(sk, "ROW") == 0)) id = pk + 5;
    }
    if (id && s->n < s->cap && strlen(id) <= CB_GAME_ID_MAX) cb_strlcpy(s->ids[s->n++], id, sizeof s->ids[0]);
    return s->n < s->cap;
}

// GET /admin/api/games?cursor=&limit=&prefix=
static void api_games(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!need_store(ctx, res)) return;
    char prefix[64], cursor[512], next[512] = "";
    q_str(req, "prefix", prefix, sizeof prefix);
    q_str(req, "cursor", cursor, sizeof cursor);
    id_sink *s = calloc(1, sizeof *s);
    if (!s) { cb_router_error(res, 500, "out of memory"); return; }
    s->cap = q_int(req, "limit", 25, 1, GAMES_PAGE_MAX);
    int rc = CB_STORE_CALL(ctx->store, admin_browse, CB_ADMIN_GAMES, prefix, cursor[0] ? cursor : NULL, s->cap, take_game_id, s,
                           next, sizeof next);
    if (rc < 0) { free(s); store_error(ctx, res, "games", rc); return; }
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_key(&w, "games");
    cb_jw_arr(&w);
    for (int i = 0; i < s->n; i++) {
        cb_game_row g;
        memset(&g, 0, sizeof g);
        if (CB_STORE_CALL(ctx->store, game_get, s->ids[i], &g) != CB_OK) continue;   // deleted since the browse
        cb_jw_obj(&w);
        write_game(&w, &g);
        cb_jw_end(&w);
        cb_game_row_free(&g);
    }
    cb_jw_end(&w);
    cb_jw_kstr(&w, "next", next);
    cb_jw_end(&w);
    send_json(res, 200, &b, &w);
    cb_buf_free(&b);
    free(s);
}

// GET /admin/api/game?id=: the row, its byte sizes, and the spectator envelope
// (app/table_ops.h cb_table_view, the same bytes spectator_views answers) as hex.
static void api_game(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!need_store(ctx, res)) return;
    char id[CB_GAME_ID_MAX + 1];
    q_str(req, "id", id, sizeof id);
    if (!id[0]) { cb_router_error(res, 400, "game: id is required"); return; }
    cb_game_row g;
    memset(&g, 0, sizeof g);
    int rc = CB_STORE_CALL(ctx->store, game_get, id, &g);
    if (rc < 0) { store_error(ctx, res, "game", rc); return; }
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    write_game(&w, &g);
    cb_jw_kint(&w, "state_len", (int64_t)g.state_len);
    cb_jw_kint(&w, "roster_len", (int64_t)g.roster_len);
    cb_jw_kint(&w, "logs_len", (int64_t)g.logs_len);
    cb_jw_kbool(&w, "has_seed", g.has_seed);
    cb_view_row v;
    memset(&v, 0, sizeof v);
    int vrc = cb_table_view(ctx, id, NULL, &v);
    if (vrc == CB_OK) {
        char *hex = malloc(v.view_len * 2 + 1);
        if (hex && cb_hex_encode(v.view, v.view_len, hex, v.view_len * 2 + 1) >= 0) cb_jw_kstr(&w, "envelope_hex", hex);
        else cb_jw_knull(&w, "envelope_hex");
        cb_jw_kint(&w, "envelope_version", v.version);
        free(hex);
        cb_view_row_free(&v);
    } else {
        cb_jw_knull(&w, "envelope_hex");
        cb_jw_kstr(&w, "envelope_error", cb_strerror(vrc));
    }
    cb_jw_end(&w);
    send_json(res, 200, &b, &w);
    cb_buf_free(&b);
    cb_game_row_free(&g);
}

// ---- bots -----------------------------------------------------------------------------

static bool put_bot(const cb_bot_row *r, void *ud) {
    cb_jw *w = ud;
    cb_jw_obj(w);
    cb_jw_kstr(w, "id", r->id);
    cb_jw_kstr(w, "nickname", r->nickname);
    cb_jw_kstr(w, "strategy_key", r->strategy_key);
    cb_jw_kint(w, "elo_rating", r->elo_rating);
    if (r->previous_elo < 0) cb_jw_knull(w, "previous_elo");
    else cb_jw_kint(w, "previous_elo", r->previous_elo);
    cb_jw_kint(w, "games_played", r->games_played);
    kiso(w, "updated_at", r->updated_at);
    cb_jw_end(w);
    return true;
}

static void api_bots(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!need_store(ctx, res)) return;
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_key(&w, "bots");
    cb_jw_arr(&w);
    int rc = CB_STORE_CALL(ctx->store, bots_list, put_bot, &w);
    cb_jw_end(&w);
    cb_jw_end(&w);
    if (rc < 0) store_error(ctx, res, "bots", rc);
    else send_json(res, 200, &b, &w);
    cb_buf_free(&b);
}

// ---- logs -------------------------------------------------------------------------------

// GET /admin/api/logs?after=<seq>&level=<debug|info|warn|error>
static void api_logs(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char v[32];
    uint64_t after = 0;
    if (cb_http_query_get(req, "after", v, sizeof v)) after = strtoull(v, NULL, 10);
    int level = CB_LOG_DEBUG;
    if (cb_http_query_get(req, "level", v, sizeof v) && v[0]) {
        level = cb_log_level_parse(v);
        if (level < 0) { cb_router_error(res, 400, "logs: level is debug, info, warn or error"); return; }
    }
    cb_log_entry *e = malloc(ADMIN_LOG_MAX * sizeof *e);
    if (!e) { cb_router_error(res, 500, "out of memory"); return; }
    size_t n = cb_log_read(after, level, e, ADMIN_LOG_MAX);
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    // The newest seq overall, so the next poll asks only for what is new even
    // when every line in between was below the level.
    cb_jw_kint(&w, "last_seq", (int64_t)cb_log_last_seq());
    cb_jw_key(&w, "entries");
    cb_jw_arr(&w);
    for (size_t i = 0; i < n; i++) {
        cb_jw_obj(&w);
        cb_jw_kint(&w, "seq", (int64_t)e[i].seq);
        kiso(&w, "at", e[i].ms);
        // Lowercase, the spelling the level= filter takes.
        char lv[8];
        cb_strlcpy(lv, cb_log_level_name(e[i].level), sizeof lv);
        for (char *p = lv; *p; p++) *p = (char)tolower((unsigned char)*p);
        cb_jw_kstr(&w, "level", lv);
        cb_jw_kstr(&w, "module", e[i].module);
        cb_jw_kstr(&w, "msg", e[i].msg);
        cb_jw_end(&w);
    }
    cb_jw_end(&w);
    write_links(ctx, &w);
    cb_jw_end(&w);
    send_json(res, 200, &b, &w);
    cb_buf_free(&b);
    free(e);
}

// ---- config ---------------------------------------------------------------------------

static void put_setting(const char *name, const char *value, bool secret, void *ud) {
    cb_jw *w = ud;
    cb_jw_obj(w);
    cb_jw_kstr(w, "name", name);
    cb_jw_kstr(w, "value", value);   // cb_config_each has already redacted a secret
    cb_jw_kbool(w, "secret", secret);
    cb_jw_end(w);
}

static void api_config(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "mode", ctx->mode == CB_MODE_LAMBDA ? "lambda" : "serve");
    cb_jw_kstr(&w, "store", store_name(ctx));
    cb_jw_kstr(&w, "table", ctx->cfg->table);
    cb_jw_kstr(&w, "allowed_origins", ctx->cfg->allowed_origins);
    cb_jw_kstr(&w, "aws_region", getenv0("AWS_REGION"));
    cb_jw_kstr(&w, "function_name", getenv0("AWS_LAMBDA_FUNCTION_NAME"));
    cb_jw_key(&w, "settings");
    cb_jw_arr(&w);
    cb_config_each(ctx->cfg, put_setting, &w);
    cb_jw_end(&w);
    cb_jw_end(&w);
    send_json(res, 200, &b, &w);
    cb_buf_free(&b);
}

// ---- backups --------------------------------------------------------------------------

static void api_backups(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "store", store_name(ctx));
    if (is_sqlite(ctx)) {
        char dir[1100];
        backup_dir(ctx, dir, sizeof dir);
        cb_jw_kstr(&w, "kind", "vacuum_into");
        cb_jw_kstr(&w, "dir", dir);
        backup_file f[64];
        int n = list_backups(ctx, f, 64);
        cb_jw_key(&w, "files");
        cb_jw_arr(&w);
        for (int i = 0; i < n; i++) {
            cb_jw_obj(&w);
            cb_jw_kstr(&w, "name", f[i].name);
            cb_jw_kint(&w, "size", f[i].size);
            kiso(&w, "at", f[i].mtime_ms);
            cb_jw_end(&w);
        }
        cb_jw_end(&w);
    } else {
        cb_jw_kstr(&w, "kind", "pitr");
        cb_jw_kstr(&w, "explain",
                   "DynamoDB point-in-time recovery is on for this table (ops/cbase.yaml): AWS keeps a continuous "
                   "backup and can restore the table as it was at any second of the last 35 days, into a NEW table. "
                   "There is nothing to trigger. To restore, open the table's Backups tab, choose Restore to point in "
                   "time, then point CBASE_TABLE at the restored table.");
    }
    write_links(ctx, &w);
    cb_jw_end(&w);
    send_json(res, 200, &b, &w);
    cb_buf_free(&b);
}

static bool ignore_row(int n, const char *const *names, const char *const *values, void *ud) { return true; }

// POST /admin/api/backups: SQLite's VACUUM INTO <dir>/cbase-<UTC stamp>.sqlite.
static void api_backup_now(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!need_store(ctx, res)) return;
    if (!is_sqlite(ctx)) {
        cb_router_error(res, 409, "this store backs itself up continuously (point-in-time recovery); nothing to trigger");
        return;
    }
    char dir[1100], path[1300], stamp[40];
    backup_dir(ctx, dir, sizeof dir);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        cb_router_error(res, 500, "backup: cannot create the backups directory");
        return;
    }
    cb_iso8601(cb_now_ms(), stamp, sizeof stamp);   // 2026-10-13T12:34:56.789000+00:00
    // cbase-20261013T123456Z.sqlite: sortable, and no character SQL must quote.
    char s[24];
    snprintf(s, sizeof s, "%.4s%.2s%.2sT%.2s%.2s%.2sZ", stamp, stamp + 5, stamp + 8, stamp + 11, stamp + 14, stamp + 17);
    snprintf(path, sizeof path, "%s/cbase-%s.sqlite", dir, s);
    // The path is quoted as an SQL string: a single quote is doubled.
    cb_buf sql;
    cb_buf_init(&sql);
    cb_buf_puts(&sql, "VACUUM INTO '");
    for (const char *p = path; *p; p++) {
        if (*p == '\'') cb_buf_putc(&sql, '\'');
        cb_buf_putc(&sql, *p);
    }
    cb_buf_putc(&sql, '\'');
    int rc = CB_STORE_CALL(ctx->store, admin_query, cb_buf_cstr(&sql), true, ignore_row, NULL);
    cb_buf_free(&sql);
    if (rc < 0) { store_error(ctx, res, "backup", rc); return; }
    CB_INFO("admin", "backup written: %s", path);
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "written", path);
    cb_jw_end(&w);
    send_json(res, 200, &b, &w);
    cb_buf_free(&b);
}

// ---- dispatch -------------------------------------------------------------------------

typedef struct {
    const char *method;
    const char *name;   // the path after /admin/api/
    cb_handler  fn;
} api_route;

static const api_route API[] = {
    { "GET", "overview", api_overview },
    { "GET", "collections", api_collections },
    { "GET", "browse", api_browse },
    { "POST", "query", api_query },
    { "GET", "users", api_users },
    { "POST", "users/delete", api_user_delete },
    { "GET", "games", api_games },
    { "GET", "game", api_game },
    { "GET", "bots", api_bots },
    { "GET", "logs", api_logs },
    { "GET", "config", api_config },
    { "GET", "backups", api_backups },
    { "POST", "backups", api_backup_now },
};

static void h_api(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!admin_gate(ctx, req, res)) return;
    const char *name = req->path + strlen("/admin/api/");
    bool known = false;
    for (size_t i = 0; i < sizeof API / sizeof API[0]; i++) {
        if (strcmp(API[i].name, name) != 0) continue;
        known = true;
        if (strcmp(API[i].method, req->method) == 0) { API[i].fn(ctx, req, res); return; }
    }
    cb_router_error(res, known ? 405 : 404, known ? "method not allowed" : "no such admin call");
}

static void h_page(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    // The page carries no data and a browser cannot send a Bearer on a
    // navigation, so the token guards the API, not this. It is still refused
    // when the console is off, so an unset token leaves no surface.
    if (!console_on(ctx, res)) return;
    cb_res_send(res, 200, "text/html; charset=utf-8", cbase_console_html, cbase_console_html_len);
    cb_res_header(res, "Cache-Control", "no-store");
    cb_res_header(res, "X-Frame-Options", "DENY");
    cb_res_header(res, "Content-Security-Policy",
                  "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; "
                  "img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
}

int cb_admin_routes(cb_ctx *ctx) {
    int rc = cb_router_add(ctx->router, "GET", "/admin", CB_ROUTE_LOOP, h_page);
    if (rc == CB_OK) rc = cb_router_add(ctx->router, "*", "/admin/api/*", CB_ROUTE_POOL, h_api);
    return rc;
}
