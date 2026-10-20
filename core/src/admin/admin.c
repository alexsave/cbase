// admin.c - the c_base console (admin.h): GET /admin serves the embedded
// core/src/admin/console.html, and /admin/api/* is its JSON API.
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
//     (providers/aws/ops/cbase.yaml) and the console explains it rather than copying.
//
// JSON is tolerated for this API (DESIGN.md "Where JSON is tolerated", item 4):
// the console is a browser page we own, and JSON is what fetch reads. Every
// answer's shape is written in core_json.c or aws_json.c; this file gathers the values.
#include "admin.h"

#include "rate_limits.h"
#include "util/clock.h"

#include "edge.h"
#include "cbase/app.h"
#include "cbase/provider.h"
#include "handler.h"
#include "core_json.h"
#include "net/router.h"
#include "store/store.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "../../../../shared/c/num_parse.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/sha.h>

// build/gen/console_html.c, from core/src/admin/console.html (tools/embed.c).
extern const char cbase_console_html[];
extern const unsigned long cbase_console_html_len;

#define ADMIN_QUERY_ROWS   1000   // a query answers at most this many rows
#define ADMIN_PAGE_MAX     500    // browse and users page size cap
#define ADMIN_LOG_MAX      500    // log entries per poll
#define ADMIN_WRITE_WORD   "WRITE"   // the typed confirmation a write needs

// ---- the gate --------------------------------------------------------------------

// Both sides are hashed to fixed 32-byte digests and the digests compared
// with CRYPTO_memcmp, so neither the content nor the length of the token
// shows in the time taken.
static bool const_eq(const char *a, const char *b) {
    unsigned char da[SHA256_DIGEST_LENGTH], db[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)a, strlen(a), da);
    SHA256((const unsigned char *)b, strlen(b), db);
    bool eq = CRYPTO_memcmp(da, db, sizeof da) == 0;
    OPENSSL_cleanse(da, sizeof da);
    OPENSSL_cleanse(db, sizeof db);
    return eq;
}

// The console exists only when CBASE_ADMIN_TOKEN is set; otherwise every
// route here answers 404, as if it were not routed at all.
static bool console_on(const cb_ctx *ctx, cb_http_res *res) {
    if (ctx->cfg->admin_token[0]) return true;
    cb_router_error(res, 404, "not found");
    return false;
}

// true when an API request may proceed; otherwise res holds the refusal.
// One bearer guards the whole console, so guessing it is limited (finding
// L7): an address's wrong tokens are counted (rate_limits.h, in memory), each is
// logged, and an address past the limit is refused before its token is even
// compared, the right one included.
static bool admin_gate(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!console_on(ctx, res)) return false;
    char ip[96];
    cb_viewer_key(ctx, req, ip, sizeof ip);
    const int64_t now = cb_now_ms();
    if (!cb_limit_peek(CB_LIMIT_ADMIN_FAIL_IP, ip, now)) {
        cb_router_error(res, 429, "too many wrong admin tokens");
        return false;
    }
    const char *tok = cb_http_bearer(req);
    if (!tok || !const_eq(tok, ctx->cfg->admin_token)) {
        CB_WARN("admin", "wrong or missing admin token from %s for %s %.80s", ip[0] ? ip : "?", req->method, req->path);
        cb_limit_take(CB_LIMIT_ADMIN_FAIL_IP, ip, now, NULL);
        cb_router_error(res, 401, "admin token required");
        return false;
    }
    return true;
}

// ---- answers ---------------------------------------------------------------------

// Sends a document core_json.c or aws_json.c wrote (rc its CB_OK or CB_E_NOMEM).
void cb_admin_send_json(cb_http_res *res, int status, cb_buf *b, int rc) {
    if (rc != CB_OK) {
        cb_router_error(res, 500, "admin: response did not encode");
        return;
    }
    cb_res_send(res, status, "application/json", b->p, b->len);
    cb_res_header(res, "Cache-Control", "no-store");
}

void cb_admin_no_memory(cb_http_res *res) { cb_router_error(res, 500, "out of memory"); }

// A store refusal as an HTTP answer.
void cb_admin_store_error(cb_ctx *ctx, cb_http_res *res, const char *what, int rc) {
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

bool cb_admin_need_store(cb_ctx *ctx, cb_http_res *res) {
    if (ctx->store) return true;
    cb_router_error(res, 503, "store not available");
    return false;
}

int cb_admin_q_int(const cb_http_req *req, const char *key, int dflt, int lo, int hi) {
    char v[32];
    if (!cb_http_query_get(req, key, v, sizeof v) || !v[0]) return dflt;
    // Any number is taken and clamped to lo..hi ("-5" is lo); text that is
    // not one is the default.
    int64_t x;
    if (!num_parse_i64_cstr(v, INT64_MIN, INT64_MAX, &x)) return dflt;
    return x < lo ? lo : x > hi ? hi : (int)x;
}

void cb_admin_q_str(const cb_http_req *req, const char *key, char *out, size_t cap) {
    if (!cb_http_query_get(req, key, out, cap)) out[0] = 0;
}

static const char *mode_label(const cb_ctx *ctx) { return ctx->prov ? ctx->prov->mode : "none"; }
static const char *store_name(const cb_ctx *ctx) { return ctx->store ? ctx->store->ops->name : "none"; }
static bool is_sqlite(const cb_ctx *ctx) { return ctx->store && strcmp(ctx->store->ops->name, "sqlite") == 0; }

static const char *getenv0(const char *k) {
    const char *v = getenv(k);
    return v ? v : "";
}

// ---- links into the provider's console ---------------------------------------------

// The provider's console pages (cbase/provider.h console_links), each NULL
// when it has none. The URLs live in u[4], which the caller frees (links_free).
static void links_of(const cb_ctx *ctx, cb_buf u[4], cb_js_admin_links *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < 4; i++) cb_buf_init(&u[i]);
    if (!ctx->prov || !ctx->prov->console_links) return;
    ctx->prov->console_links(ctx, u);
    const char **slot[4] = { &out->lambda, &out->logs_insights, &out->dynamodb, &out->backups };
    for (int i = 0; i < 4; i++)
        if (u[i].len) *slot[i] = cb_buf_cstr(&u[i]);
}

static void links_free(cb_buf u[4]) {
    for (int i = 0; i < 4; i++) cb_buf_free(&u[i]);
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

static void api_overview(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    int64_t now = cb_now_ms();
    cb_js_admin_overview o;
    memset(&o, 0, sizeof o);
    o.version = ctx->version;
    o.mode = mode_label(ctx);
    o.uptime_s = (now - ctx->started_ms) / 1000;
    o.started_ms = ctx->started_ms;
    o.store = store_name(ctx);
    o.table = ctx->cfg->table;
    o.public_url = ctx->cfg->public_url;
    o.requests = (int64_t)atomic_load_explicit(&ctx->stats.requests, memory_order_relaxed);
    o.errors_5xx = (int64_t)atomic_load_explicit(&ctx->stats.responses_5xx, memory_order_relaxed);
    // Under a broker (IoT Core) this process holds none of the stream's connections.
    o.conns_known = ctx->prov && (ctx->prov->flags & CB_PROV_HELD_SOCKETS);
    if (o.conns_known) {
        o.http_conns = atomic_load_explicit(&ctx->stats.http_conns, memory_order_relaxed);
        o.ws_conns = atomic_load_explicit(&ctx->stats.ws_conns, memory_order_relaxed);
    }
    // The app's one counter line (foolish: the games needing bots).
    o.games_needing_bots = ctx->app && ctx->app->admin_count && ctx->store ? ctx->app->admin_count(ctx, now) : -1;
    // DynamoDB throttling (store.h cb_store_throttles_get): this process's
    // throttled responses, and the requests that failed still throttled.
    // Nonzero means the provisioned capacity is too small for the load.
    cb_store_throttles th;
    cb_store_throttles_get(&th);
    o.throttled = th.throttled;
    o.gave_up = th.gave_up;
    if (is_sqlite(ctx)) {
        backup_file f[1];
        if (list_backups(ctx, f, 1) == 1) o.last_backup_ms = f[0].mtime_ms;
    } else {
        o.continuous_backup = true;
    }
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    o.max_rss_kb = (int64_t)ru.ru_maxrss / 1024;   // bytes on macOS
#else
    o.max_rss_kb = (int64_t)ru.ru_maxrss;          // kilobytes on Linux
#endif
    o.pid = (int64_t)getpid();
    o.cpu_user_ms = (int64_t)ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000;
    o.cpu_sys_ms = (int64_t)ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000;
    cb_buf u[4];
    links_of(ctx, u, &o.links);
    cb_buf b;
    cb_buf_init(&b);
    cb_admin_send_json(res, 200, &b, cb_js_admin_overview_write(&b, &o));
    cb_buf_free(&b);
    links_free(u);
}

// ---- GET /admin/api/collections ---------------------------------------------------

static bool put_name(const char *name, void *ud) { cb_js_admin_collections_add(ud, name); return true; }

static void api_collections(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!cb_admin_need_store(ctx, res)) return;
    cb_buf b;
    cb_buf_init(&b);
    cb_js_stream *s = cb_js_admin_collections_begin(&b, store_name(ctx));
    if (!s) { cb_buf_free(&b); cb_admin_no_memory(res); return; }
    int rc = CB_STORE_CALL(ctx->store, admin_collections, put_name, s);
    int enc = cb_js_admin_collections_end(s);
    if (rc < 0) cb_admin_store_error(ctx, res, "collections", rc);
    else cb_admin_send_json(res, 200, &b, enc);
    cb_buf_free(&b);
}

// ---- rows (browse and query) --------------------------------------------------------

typedef struct {
    cb_js_stream *s;
    int           n, cap;
    bool          truncated;
} row_sink;

static bool put_row(int n, const char *const *names, const char *const *values, void *ud) {
    row_sink *s = ud;
    if (s->n >= s->cap) { s->truncated = true; return false; }
    s->n++;
    cb_js_admin_row(s->s, n, names, values);
    return true;
}

// GET /admin/api/browse?c=<collection>&prefix=&cursor=&limit=
static void api_browse(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!cb_admin_need_store(ctx, res)) return;
    char coll[128], prefix[256], cursor[512], next[512] = "";
    cb_admin_q_str(req, "c", coll, sizeof coll);
    cb_admin_q_str(req, "prefix", prefix, sizeof prefix);
    cb_admin_q_str(req, "cursor", cursor, sizeof cursor);
    int limit = cb_admin_q_int(req, "limit", 50, 1, ADMIN_PAGE_MAX);
    if (!coll[0]) { cb_router_error(res, 400, "browse: c (the collection) is required"); return; }
    cb_buf b;
    cb_buf_init(&b);
    row_sink s = { cb_js_admin_browse_begin(&b, coll), 0, limit, false };
    if (!s.s) { cb_buf_free(&b); cb_admin_no_memory(res); return; }
    (void)cb_store_settle(ctx->store);   // a write-behind layer's writes are in the file first (store.h)
    int rc = CB_STORE_CALL(ctx->store, admin_browse, coll, prefix, cursor[0] ? cursor : NULL, limit, put_row, &s, next,
                           sizeof next);
    int enc = cb_js_admin_browse_end(s.s, next);
    if (rc < 0) cb_admin_store_error(ctx, res, "browse", rc);
    else cb_admin_send_json(res, 200, &b, enc);
    cb_buf_free(&b);
}

// POST /admin/api/query  {"statement": "...", "confirm": "WRITE"?}
static void api_query(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!cb_admin_need_store(ctx, res)) return;
    const char *body = (const char *)req->body;
    size_t n = req->body_len;
    char *stmt = malloc(n + 1);
    char confirm[32];
    if (!stmt) { cb_router_error(res, 500, "out of memory"); return; }
    if (!cb_js_admin_query_body(body, n, stmt, n + 1, confirm, sizeof confirm) || !stmt[0]) {
        free(stmt);
        cb_router_error(res, 400, "query: a JSON body with \"statement\" is required");
        return;
    }
    bool allow_write = strcmp(confirm, ADMIN_WRITE_WORD) == 0;
    if (allow_write) CB_WARN("admin", "write statement confirmed: %.180s", stmt);
    cb_buf b;
    cb_buf_init(&b);
    row_sink s = { cb_js_admin_query_begin(&b, is_sqlite(ctx) ? "sql" : "partiql", allow_write), 0, ADMIN_QUERY_ROWS, false };
    if (!s.s) { cb_buf_free(&b); free(stmt); cb_admin_no_memory(res); return; }
    int rc = cb_store_admin_query(ctx->store, stmt, allow_write, put_row, &s);
    // A hand edit may have touched a rating or a bot: both edge copies go (app/edge.h).
    if (allow_write) cb_edge_stale(CB_EDGE_ALL);
    // ... or a session or an account: the sessions held in memory are read again (auth.h).
    if (allow_write) cb_auth_forget_sessions(ctx->auth);
    int enc = cb_js_admin_query_end(s.s, s.truncated);
    if (rc < 0) cb_admin_store_error(ctx, res, "query", rc);
    else cb_admin_send_json(res, 200, &b, enc);
    cb_buf_free(&b);
    free(stmt);
}

// ---- users ---------------------------------------------------------------------------

typedef struct {
    cb_js_stream *s;
    int           n;
} user_sink;

static bool put_user(const cb_user_row *u, void *ud) {
    user_sink *s = ud;
    s->n++;
    cb_js_admin_users_add(s->s, u);
    return true;
}

// GET /admin/api/users?search=&offset=&limit=
static void api_users(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!cb_admin_need_store(ctx, res)) return;
    char search[128];
    cb_admin_q_str(req, "search", search, sizeof search);
    int offset = cb_admin_q_int(req, "offset", 0, 0, 1 << 30);
    int limit = cb_admin_q_int(req, "limit", 50, 1, ADMIN_PAGE_MAX);
    cb_buf b;
    cb_buf_init(&b);
    user_sink s = { cb_js_admin_users_begin(&b), 0 };
    if (!s.s) { cb_buf_free(&b); cb_admin_no_memory(res); return; }
    int rc = CB_STORE_CALL(ctx->store, users_list, search, offset, limit, put_user, &s);
    // A full page means there may be more.
    int enc = cb_js_admin_users_end(s.s, offset, s.n == limit ? (int64_t)offset + limit : -1);
    if (rc < 0) cb_admin_store_error(ctx, res, "users", rc);
    else cb_admin_send_json(res, 200, &b, enc);
    cb_buf_free(&b);
}

// POST /admin/api/users/delete  {"id": "<uuid>"}: the app's delete_account,
// the one implementation delete-account runs (see the header comment).
static void api_user_delete(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!cb_admin_need_store(ctx, res)) return;
    char id[CB_UUID_LEN + 1];
    if (!cb_js_admin_id_body((const char *)req->body, req->body_len, id, sizeof id) || !id[0]) {
        cb_router_error(res, 400, "delete: a JSON body with \"id\" is required");
        return;
    }
    cb_user_row u;
    int rc = CB_STORE_CALL(ctx->store, user_get, id, &u);
    if (rc < 0) { cb_admin_store_error(ctx, res, "delete: user", rc); return; }
    if (!ctx->app || !ctx->app->delete_account) { cb_router_error(res, 501, "delete: this app deletes no accounts"); return; }
    int code = 0, detail = 0;
    rc = ctx->app->delete_account(ctx, id, &code, &detail);
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
    cb_admin_send_json(res, 200, &b, cb_js_admin_deleted(&b, id));
    cb_buf_free(&b);
}

// ---- logs -------------------------------------------------------------------------------

// GET /admin/api/logs?after=<seq>&level=<debug|info|warn|error>
static void api_logs(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char v[32];
    int64_t after = 0;
    if (cb_http_query_get(req, "after", v, sizeof v) && v[0] && !num_parse_i64_cstr(v, 0, INT64_MAX, &after)) {
        cb_router_error(res, 400, "logs: after is a sequence number");
        return;
    }
    int level = CB_LOG_DEBUG;
    if (cb_http_query_get(req, "level", v, sizeof v) && v[0]) {
        level = cb_log_level_parse(v);
        if (level < 0) { cb_router_error(res, 400, "logs: level is debug, info, warn or error"); return; }
    }
    cb_log_entry *e = malloc(ADMIN_LOG_MAX * sizeof *e);
    if (!e) { cb_router_error(res, 500, "out of memory"); return; }
    size_t n = cb_log_read((uint64_t)after, level, e, ADMIN_LOG_MAX);
    cb_buf b;
    cb_buf_init(&b);
    // The newest seq overall, so the next poll asks only for what is new even
    // when every line in between was below the level.
    cb_js_stream *s = cb_js_admin_logs_begin(&b, cb_log_last_seq());
    if (!s) { cb_buf_free(&b); free(e); cb_admin_no_memory(res); return; }
    for (size_t i = 0; i < n; i++) {
        // Lowercase, the spelling the level= filter takes.
        char lv[8];
        cb_strlcpy(lv, cb_log_level_name(e[i].level), sizeof lv);
        for (char *p = lv; *p; p++) *p = (char)tolower((unsigned char)*p);
        cb_js_admin_logs_add(s, e[i].seq, e[i].ms, lv, e[i].module, e[i].msg);
    }
    cb_buf u[4];
    cb_js_admin_links links;
    links_of(ctx, u, &links);
    cb_admin_send_json(res, 200, &b, cb_js_admin_logs_end(s, &links));
    links_free(u);
    cb_buf_free(&b);
    free(e);
}

// ---- config ---------------------------------------------------------------------------

static void put_setting(const char *name, const char *value, bool secret, void *ud) {
    cb_js_admin_config_add(ud, name, value, secret);   // cb_config_each has already redacted a secret
}

static void api_config(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_js_admin_config c = {
        .mode = mode_label(ctx),
        .store = store_name(ctx),
        .table = ctx->cfg->table,
        .allowed_origins = ctx->cfg->allowed_origins,
        .aws_region = getenv0("AWS_REGION"),
        .function_name = getenv0("AWS_LAMBDA_FUNCTION_NAME"),
    };
    cb_buf b;
    cb_buf_init(&b);
    cb_js_stream *s = cb_js_admin_config_begin(&b, &c);
    if (!s) { cb_buf_free(&b); cb_admin_no_memory(res); return; }
    cb_config_each(ctx->cfg, put_setting, s);
    cb_admin_send_json(res, 200, &b, cb_js_admin_config_end(s));
    cb_buf_free(&b);
}

// ---- backups --------------------------------------------------------------------------

static void api_backups(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_js_admin_backups bk;
    memset(&bk, 0, sizeof bk);
    bk.store = store_name(ctx);
    bk.sqlite = is_sqlite(ctx);
    char dir[1100];
    backup_file f[64];
    cb_js_admin_backup files[64];
    if (bk.sqlite) {
        backup_dir(ctx, dir, sizeof dir);
        bk.dir = dir;
        bk.n_files = list_backups(ctx, f, 64);
        for (int i = 0; i < bk.n_files; i++) files[i] = (cb_js_admin_backup){ f[i].name, f[i].size, f[i].mtime_ms };
        bk.files = files;
    } else {
        bk.explain = "DynamoDB point-in-time recovery is on for this table (ops/cbase.yaml): AWS keeps a continuous "
                     "backup and can restore the table as it was at any second of the last 35 days, into a NEW table. "
                     "There is nothing to trigger. To restore, open the table's Backups tab, choose Restore to point in "
                     "time, then point CBASE_TABLE at the restored table.";
    }
    cb_buf u[4];
    links_of(ctx, u, &bk.links);
    cb_buf b;
    cb_buf_init(&b);
    cb_admin_send_json(res, 200, &b, cb_js_admin_backups_write(&b, &bk));
    cb_buf_free(&b);
    links_free(u);
}

static bool ignore_row(int n, const char *const *names, const char *const *values, void *ud) { return true; }

// POST /admin/api/backups: SQLite's VACUUM INTO <dir>/cbase-<UTC stamp>.sqlite.
static void api_backup_now(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!cb_admin_need_store(ctx, res)) return;
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
    if (rc < 0) { cb_admin_store_error(ctx, res, "backup", rc); return; }
    CB_INFO("admin", "backup written: %s", path);
    cb_buf b;
    cb_buf_init(&b);
    cb_admin_send_json(res, 200, &b, cb_js_admin_written(&b, path));
    cb_buf_free(&b);
}

// ---- dispatch -------------------------------------------------------------------------

static const cb_admin_call API[] = {
    { "GET", "overview", api_overview },
    { "GET", "collections", api_collections },
    { "GET", "browse", api_browse },
    { "POST", "query", api_query },
    { "GET", "users", api_users },
    { "POST", "users/delete", api_user_delete },
    { "GET", "logs", api_logs },
    { "GET", "config", api_config },
    { "GET", "backups", api_backups },
    { "POST", "backups", api_backup_now },
};

static void h_api(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!admin_gate(ctx, req, res)) return;
    const char *name = req->path + strlen("/admin/api/");
    bool known = false;
    // The platform's calls, then the app's panels (cbase/app.h admin_calls).
    for (int t = 0; t < 2; t++) {
        const cb_admin_call *calls = t == 0 ? API : ctx->app ? ctx->app->admin_calls : NULL;
        int n = t == 0 ? (int)(sizeof API / sizeof API[0]) : ctx->app ? ctx->app->n_admin_calls : 0;
        for (int i = 0; calls && i < n; i++) {
            if (strcmp(calls[i].name, name) != 0) continue;
            known = true;
            if (strcmp(calls[i].method, req->method) == 0) { calls[i].fn(ctx, req, res); return; }
        }
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
