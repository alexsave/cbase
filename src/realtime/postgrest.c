// postgrest.c - the PostgREST reads the clients make, and the chat insert
// (postgrest.h).
//
// One pipeline for every GET: find the table, parse the query, check every
// named column exists, learn who is calling, fetch the rows the caller may see
// (the RLS, in C, per table), then filter, order, page and select generically,
// and write the rows as PostgREST does. Ground truth: seed.sql (columns, types,
// RLS), postgrest-js 2.x (what it sends, how it reads errors), and the client
// call sites listed in postgrest.h.
#include "postgrest.h"

#include "net/router.h"
#include "realtime.h"
#include "rt_deps.h"
#include "rt_rows.h"
#include "util/clock.h"
#include "util/json.h"
#include "util/log.h"
#include "util/uuid.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the query grammar -------------------------------------------------------------

static bool is_ident(const char *s, size_t n) {
    if (n == 0 || n >= 32) return false;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return false;
    return true;
}

static bool parse_uint(const char *s, int *out) {
    if (!*s || strlen(s) > 7) return false;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return true;
}

typedef struct {
    cb_pgrst_query *q;
    bool bad;
    bool seen_select, seen_order, seen_limit, seen_offset;
} parse_st;

static bool parse_select(cb_pgrst_query *q, const char *v) {
    // "*", or a comma list of plain column names (blanks around them allowed).
    // Renames, casts and embedded resources are not something the clients send.
    const char *p = v;
    q->n_cols = 0;
    while (*p == ' ') p++;
    if (strcmp(p, "*") == 0 || *p == 0) return *p == '*' || *v == 0;
    for (;;) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if (!is_ident(p, (size_t)(e - p)) || q->n_cols >= CB_PGRST_MAX_COLS) return false;
        memcpy(q->cols[q->n_cols], p, (size_t)(e - p));
        q->cols[q->n_cols][e - p] = 0;
        q->n_cols++;
        while (*e == ' ') e++;
        if (*e == 0) return true;
        if (*e != ',') return false;
        p = e + 1;
    }
}

static bool parse_order(cb_pgrst_query *q, const char *v) {
    // col[.asc|.desc][.nullsfirst|.nullslast]; one column only.
    const char *dot = strchr(v, '.');
    size_t n = dot ? (size_t)(dot - v) : strlen(v);
    if (!is_ident(v, n)) return false;
    memcpy(q->order_col, v, n);
    q->order_col[n] = 0;
    q->order_desc = false;
    if (!dot) return true;
    const char *rest = dot + 1;
    if (cb_starts_with(rest, "asc")) rest += 3;
    else if (cb_starts_with(rest, "desc")) { q->order_desc = true; rest += 4; }
    else if (!cb_starts_with(rest, "nulls")) return false;
    if (*rest == '.') rest++;
    // The NULLS modifier is accepted and has no effect: no column the clients
    // order by can be null.
    return *rest == 0 || strcmp(rest, "nullsfirst") == 0 || strcmp(rest, "nullslast") == 0;
}

static bool parse_fn(const char *key, const char *value, void *ud) {
    parse_st *st = ud;
    cb_pgrst_query *q = st->q;
    bool ok;
    // Each of these may appear once; a second one is refused rather than guessed at.
    if (strcmp(key, "select") == 0) {
        ok = !st->seen_select && parse_select(q, value);
        st->seen_select = true;
    } else if (strcmp(key, "order") == 0) {
        ok = !st->seen_order && parse_order(q, value);
        st->seen_order = true;
    } else if (strcmp(key, "limit") == 0) {
        ok = !st->seen_limit && parse_uint(value, &q->limit);
        st->seen_limit = true;
    } else if (strcmp(key, "offset") == 0) {
        ok = !st->seen_offset && parse_uint(value, &q->offset);
        st->seen_offset = true;
    } else if (strcmp(key, "columns") == 0 || strcmp(key, "apikey") == 0) ok = true;   // insert's column list; realtime's key
    else {
        // A filter: <col>=<op>.<value>, op eq or gt (the leaderboard's games_played=gt.0).
        const char *dot = strchr(value, '.');
        size_t on = dot ? (size_t)(dot - value) : 0;
        ok = is_ident(key, strlen(key)) && dot && q->n_filters < CB_PGRST_MAX_FILTERS &&
             ((on == 2 && memcmp(value, "eq", 2) == 0) || (on == 2 && memcmp(value, "gt", 2) == 0)) &&
             strlen(dot + 1) < sizeof q->filters[0].value;
        if (ok) {
            cb_strlcpy(q->filters[q->n_filters].col, key, sizeof q->filters[0].col);
            memcpy(q->filters[q->n_filters].op, value, on);
            q->filters[q->n_filters].op[on] = 0;
            cb_strlcpy(q->filters[q->n_filters].value, dot + 1, sizeof q->filters[0].value);
            q->n_filters++;
        }
    }
    if (!ok) st->bad = true;
    return ok;
}

int cb_pgrst_parse(const char *query, cb_pgrst_query *out) {
    memset(out, 0, sizeof *out);
    parse_st st = { out, false, false, false, false, false };
    int n = cb_http_query_each(query ? query : "", parse_fn, &st);
    if (n < 0 || st.bad) { memset(out, 0, sizeof *out); return CB_E_INVALID; }
    return CB_OK;
}

// ---- tables and rows --------------------------------------------------------------------

enum { V_NULL, V_TEXT, V_INT, V_BOOL, V_BYTEA, V_TS, V_UUIDS };

typedef struct { const char *name; int kind; } pcol;

typedef struct {
    const char *name;
    const pcol *cols;
    int n_cols;
} ptable;

#define COLS(a) a, (int)(sizeof a / sizeof a[0])
static const pcol PV_COLS[] = { { "game_id", V_TEXT }, { "player_id", V_TEXT }, { "view", V_BYTEA },
                                { "version", V_INT }, { "status", V_TEXT }, { "updated_at", V_TS } };
static const pcol SV_COLS[] = { { "game_id", V_TEXT }, { "view", V_BYTEA }, { "version", V_INT },
                                { "status", V_TEXT }, { "updated_at", V_TS } };
static const pcol CHAT_COLS[] = { { "id", V_TEXT }, { "game_id", V_TEXT }, { "user_id", V_TEXT },
                                  { "message", V_TEXT }, { "is_system", V_BOOL }, { "created_at", V_TS } };
static const pcol RATING_COLS[] = { { "user_id", V_TEXT }, { "username", V_TEXT }, { "elo_rating", V_INT },
                                    { "previous_elo", V_INT }, { "games_played", V_INT }, { "created_at", V_TS },
                                    { "updated_at", V_TS } };
static const pcol BOT_COLS[] = { { "id", V_TEXT }, { "nickname", V_TEXT }, { "strategy_key", V_TEXT },
                                 { "elo_rating", V_INT }, { "previous_elo", V_INT }, { "games_played", V_INT },
                                 { "created_at", V_TS }, { "updated_at", V_TS } };
static const pcol SNAP_COLS[] = { { "id", V_TEXT }, { "game_id", V_TEXT }, { "player_ids", V_UUIDS },
                                  { "moves", V_BYTEA }, { "extras", V_BYTEA }, { "created_at", V_TS } };

enum { T_PV, T_SV, T_CHAT, T_RATINGS, T_BOTS, T_SNAPS, T_COUNT };
static const ptable TABLES[T_COUNT] = {
    [T_PV] = { "player_views", COLS(PV_COLS) },
    [T_SV] = { "spectator_views", COLS(SV_COLS) },
    [T_CHAT] = { "chat_messages", COLS(CHAT_COLS) },
    [T_RATINGS] = { "user_elo_ratings", COLS(RATING_COLS) },
    [T_BOTS] = { "bots", COLS(BOT_COLS) },
    [T_SNAPS] = { "game_snapshots", COLS(SNAP_COLS) },
};

static int col_index(const ptable *t, const char *name) {
    for (int i = 0; i < t->n_cols; i++)
        if (strcmp(t->cols[i].name, name) == 0) return i;
    return -1;
}

// One cell. `s` (text, or n uuids of 37 bytes each) and `p` (bytea) are owned.
typedef struct {
    int      kind;   // V_NULL, or the column's kind
    char    *s;
    int64_t  i;
    uint8_t *p;
    size_t   n;
} cell;

typedef struct { cell c[CB_PGRST_MAX_COLS]; } prow;

typedef struct {
    const ptable *t;
    prow *rows;
    size_t n, cap;
    bool oom;
} rowset;

static void rowset_free(rowset *rs) {
    for (size_t r = 0; r < rs->n; r++)
        for (int k = 0; k < rs->t->n_cols; k++) { free(rs->rows[r].c[k].s); free(rs->rows[r].c[k].p); }
    free(rs->rows);
    rs->rows = NULL;
    rs->n = rs->cap = 0;
}

static prow *row_add(rowset *rs) {
    if (rs->oom) return NULL;
    if (rs->n == rs->cap) {
        size_t nc = rs->cap ? rs->cap * 2 : 16;
        prow *nr = realloc(rs->rows, nc * sizeof *nr);
        if (!nr) { rs->oom = true; return NULL; }
        rs->rows = nr;
        rs->cap = nc;
    }
    prow *r = &rs->rows[rs->n++];
    memset(r, 0, sizeof *r);
    return r;
}

static void set_text(rowset *rs, prow *r, int k, const char *s) {
    if (!s) { r->c[k].kind = V_NULL; return; }
    r->c[k].kind = V_TEXT;
    r->c[k].s = strdup(s);
    if (!r->c[k].s) rs->oom = true;
}
static void set_int(prow *r, int k, int64_t v) { r->c[k].kind = V_INT; r->c[k].i = v; }
static void set_bool(prow *r, int k, bool v) { r->c[k].kind = V_BOOL; r->c[k].i = v; }
static void set_time(prow *r, int k, int kind, int64_t ms) { r->c[k].kind = kind; r->c[k].i = ms; }
static void set_bytes(rowset *rs, prow *r, int k, const uint8_t *p, size_t n) {
    if (!p) { r->c[k].kind = V_NULL; return; }
    r->c[k].kind = V_BYTEA;
    r->c[k].p = malloc(n ? n : 1);
    if (!r->c[k].p) { rs->oom = true; return; }
    if (n) memcpy(r->c[k].p, p, n);
    r->c[k].n = n;
}
static void set_uuids(rowset *rs, prow *r, int k, const cb_uuid *ids, int n) {
    r->c[k].kind = V_UUIDS;
    r->c[k].s = calloc((size_t)(n ? n : 1), sizeof(cb_uuid));
    if (!r->c[k].s) { rs->oom = true; return; }
    for (int i = 0; i < n; i++) memcpy(r->c[k].s + (size_t)i * sizeof(cb_uuid), ids[i], sizeof(cb_uuid));
    r->c[k].n = (size_t)n;
}

// ---- the row sources, with the RLS of each table -------------------------------------

typedef struct {
    cb_ctx *ctx;
    const cb_pgrst_query *q;
    const char *uid;          // NULL: anon
    rowset *rs;
    int err;                  // CB_E_* from a source
    const char *err_msg;      // a 400 the source raises itself
} fetch;

static void add_view(rowset *rs, const cb_view_row *v, bool player) {
    prow *r = row_add(rs);
    if (!r) return;
    int k = 0;
    set_text(rs, r, k++, v->game_id);
    if (player) set_text(rs, r, k++, v->player_id);
    set_bytes(rs, r, k++, v->view ? v->view : (const uint8_t *)"", v->view_len);
    set_int(r, k++, v->version);
    set_text(rs, r, k++, v->status);
    set_time(r, k++, V_TS, v->updated_at);
}

static bool view_fn(const cb_view_row *v, void *ud) {
    rowset *rs = ud;
    add_view(rs, v, true);
    return !rs->oom;
}

// The first eq filter on `col`, or NULL.
static const char *eq_of(const cb_pgrst_query *q, const char *col) {
    for (int i = 0; i < q->n_filters; i++)
        if (strcmp(q->filters[i].col, col) == 0 && strcmp(q->filters[i].op, "eq") == 0) return q->filters[i].value;
    return NULL;
}

static void fetch_views(fetch *f, bool player) {
    // player_views: only the caller's own rows. spectator_views: any
    // authenticated user, one game at a time (the clients always name it).
    if (!f->uid) return;
    const char *gid = eq_of(f->q, "game_id");
    if (!player && !gid) { f->err_msg = "spectator_views is read one game at a time: add game_id=eq.<id>"; return; }
    if (gid) {
        if (player) {
            const char *pid = eq_of(f->q, "player_id");
            if (pid && strcmp(pid, f->uid) != 0) return;   // someone else's row: invisible
        }
        cb_view_row v;
        memset(&v, 0, sizeof v);
        int rc = cb_rt_deps_g.table_view(f->ctx, gid, player ? f->uid : NULL, &v);
        if (rc == CB_OK) add_view(f->rs, &v, player);
        else if (rc != CB_E_NOT_FOUND) f->err = rc;
        cb_view_row_free(&v);
        return;
    }
    int rc = cb_rt_deps_g.table_views_of_user(f->ctx, f->uid, 0, view_fn, f->rs);
    if (rc < 0) f->err = rc;
}

static bool chat_fn(const cb_chat_row *c, void *ud) {
    rowset *rs = ud;
    prow *r = row_add(rs);
    if (!r) return false;
    set_text(rs, r, 0, c->id);
    set_text(rs, r, 1, c->game_id);
    set_text(rs, r, 2, c->user_id);
    set_text(rs, r, 3, c->message ? c->message : "");
    set_bool(r, 4, c->is_system);
    set_time(r, 5, V_TS, c->created_at);
    return !rs->oom;
}

static void fetch_chat(fetch *f) {
    // Members of the game only (chat_messages SELECT policy).
    if (!f->uid) return;
    const char *gid = eq_of(f->q, "game_id");
    if (!gid) { f->err_msg = "chat_messages is read one game at a time: add game_id=eq.<id>"; return; }
    int m = CB_STORE_CALL(f->ctx->store, is_member, gid, f->uid);
    if (m < 0) { f->err = m; return; }
    if (m != 1) return;
    int rc = CB_STORE_CALL(f->ctx->store, chat_list, gid, 0, chat_fn, f->rs);
    if (rc < 0) f->err = rc;
}

static bool bot_fn(const cb_bot_row *b, void *ud) {
    rowset *rs = ud;
    prow *r = row_add(rs);
    if (!r) return false;
    set_text(rs, r, 0, b->id);
    set_text(rs, r, 1, b->nickname);
    set_text(rs, r, 2, b->strategy_key);
    set_int(r, 3, b->elo_rating);
    if (b->previous_elo >= 0) set_int(r, 4, b->previous_elo);
    set_int(r, 5, b->games_played);
    set_time(r, 6, V_TS, b->created_at);
    set_time(r, 7, V_TS, b->updated_at);
    return !rs->oom;
}

static bool rating_fn(const cb_rating_row *g, void *ud) {
    rowset *rs = ud;
    prow *r = row_add(rs);
    if (!r) return false;
    set_text(rs, r, 0, g->user_id);
    set_text(rs, r, 1, g->username[0] ? g->username : NULL);
    set_int(r, 2, g->elo_rating);
    if (g->previous_elo >= 0) set_int(r, 3, g->previous_elo);
    set_int(r, 4, g->games_played);
    // created_at: the store does not keep it (cb_rating_row); null.
    set_time(r, 6, V_TS, g->updated_at);
    return !rs->oom;
}

static bool snap_fn(const cb_snapshot_row *s, void *ud) {
    rowset *rs = ud;
    prow *r = row_add(rs);
    if (!r) return false;
    set_text(rs, r, 0, s->id);
    set_text(rs, r, 1, s->game_id[0] ? s->game_id : NULL);
    set_uuids(rs, r, 2, s->player_ids, s->n_players);
    set_bytes(rs, r, 3, s->moves ? s->moves : (const uint8_t *)"", s->moves_len);
    set_bytes(rs, r, 4, s->extras, s->extras_len);
    set_time(r, 5, V_TS, s->created_at);
    return !rs->oom;
}

static void fetch_snaps(fetch *f) {
    // Only snapshots whose player_ids contain the caller.
    if (!f->uid) return;
    const cb_pgrst_query *q = f->q;
    cb_snapshot_query sq = { f->uid, eq_of(q, "game_id"), eq_of(q, "id"), 0, q->order_desc };
    // The page can be pushed down to the store only when the store's order is
    // the asked one and every filter is one the store applied.
    bool push = q->order_col[0] == 0 || strcmp(q->order_col, "created_at") == 0;
    for (int i = 0; i < q->n_filters && push; i++)
        push = strcmp(q->filters[i].op, "eq") == 0 &&
               (strcmp(q->filters[i].col, "game_id") == 0 || strcmp(q->filters[i].col, "id") == 0);
    if (push && q->limit > 0) sq.limit = q->offset + q->limit;
    if (q->order_col[0] == 0) sq.newest_first = false;
    int rc = CB_STORE_CALL(f->ctx->store, snapshots_query, &sq, snap_fn, f->rs);
    if (rc < 0) f->err = rc;
}

static void fetch_rows(int table, fetch *f) {
    switch (table) {
    case T_PV:    fetch_views(f, true); break;
    case T_SV:    fetch_views(f, false); break;
    case T_CHAT:  fetch_chat(f); break;
    case T_BOTS:  { int rc = CB_STORE_CALL(f->ctx->store, bots_list, bot_fn, f->rs); if (rc < 0) f->err = rc; } break;
    case T_RATINGS: { int rc = CB_STORE_CALL(f->ctx->store, ratings_list, rating_fn, f->rs); if (rc < 0) f->err = rc; } break;
    case T_SNAPS: fetch_snaps(f); break;
    }
}

// ---- filter, order, page -------------------------------------------------------------

// A filter's value, read once in its column's type.
typedef struct { int col; bool gt; int kind; const char *s; int64_t i; } pfilter;

static bool prep_filter(const ptable *t, const cb_pgrst_query *q, int k, pfilter *out, char *err, size_t cap) {
    out->col = col_index(t, q->filters[k].col);
    out->gt = strcmp(q->filters[k].op, "gt") == 0;
    out->kind = t->cols[out->col].kind;
    out->s = q->filters[k].value;
    const char *v = out->s;
    switch (out->kind) {
    case V_TEXT: return true;
    case V_INT: {
        char *e;
        long long x = strtoll(v, &e, 10);
        if (!*v || *e) { snprintf(err, cap, "invalid input syntax for type bigint: \"%s\"", v); return false; }
        out->i = x;
        return true;
    }
    case V_BOOL:
        if (strcmp(v, "true") == 0 || strcmp(v, "t") == 0) out->i = 1;
        else if (strcmp(v, "false") == 0 || strcmp(v, "f") == 0) out->i = 0;
        else { snprintf(err, cap, "invalid input syntax for type boolean: \"%s\"", v); return false; }
        return true;
    case V_TS:
        if (cb_iso8601_parse(v, strlen(v), &out->i) != CB_OK) { snprintf(err, cap, "invalid input syntax for type timestamp: \"%s\"", v); return false; }
        return true;
    default:
        snprintf(err, cap, "column %s cannot be filtered here", q->filters[k].col);
        return false;
    }
}

static bool matches(const prow *r, const pfilter *f) {
    const cell *c = &r->c[f->col];
    if (c->kind == V_NULL) return false;   // NULL = x is not true
    int cmp;
    if (f->kind == V_TEXT) cmp = strcmp(c->s, f->s);
    else cmp = c->i < f->i ? -1 : c->i > f->i;
    return f->gt ? cmp > 0 : cmp == 0;
}

static _Thread_local struct { int col; bool desc; } g_sort;

static int cmp_rows(const void *a, const void *b) {
    const cell *x = &((const prow *)a)->c[g_sort.col], *y = &((const prow *)b)->c[g_sort.col];
    int c;
    // Postgres puts NULLs last ascending and first descending.
    if (x->kind == V_NULL || y->kind == V_NULL) c = (x->kind == V_NULL) - (y->kind == V_NULL);
    else if (x->kind == V_TEXT) c = strcmp(x->s, y->s);
    else c = x->i < y->i ? -1 : x->i > y->i;
    return g_sort.desc ? -c : c;
}

// ---- writing rows ------------------------------------------------------------------------

static void write_cell(cb_jw *w, const cell *c) {
    char ts[48];
    switch (c->kind) {
    case V_TEXT:  cb_jw_str(w, c->s); break;
    case V_INT:   cb_jw_int(w, c->i); break;
    case V_BOOL:  cb_jw_bool(w, c->i != 0); break;
    case V_BYTEA: cb_jw_bytea(w, c->p, c->n); break;
    case V_TS:    cb_rt_ts(c->i, ts, sizeof ts); cb_jw_str(w, ts); break;
    case V_UUIDS:
        cb_jw_arr(w);
        for (size_t i = 0; i < c->n; i++) cb_jw_str(w, c->s + i * sizeof(cb_uuid));
        cb_jw_end(w);
        break;
    default:      cb_jw_null(w); break;
    }
}

static void write_row(cb_jw *w, const ptable *t, const prow *r, const int *sel, int n_sel) {
    cb_jw_obj(w);
    for (int k = 0; k < n_sel; k++) {
        cb_jw_key(w, t->cols[sel[k]].name);
        write_cell(w, &r->c[sel[k]]);
    }
    cb_jw_end(w);
}

// ---- answers --------------------------------------------------------------------------

static void pg_error(cb_http_res *res, int status, const char *code, const char *message, const char *details) {
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "code", code);
    cb_jw_kstr(&w, "details", details);
    cb_jw_knull(&w, "hint");
    cb_jw_kstr(&w, "message", message);
    cb_jw_end(&w);
    cb_res_send(res, status, "application/json; charset=utf-8", b.p, b.len);
    cb_buf_free(&b);
}

static void store_error(cb_http_res *res, int rc) {
    if (rc == CB_E_NOTIMPL || rc == CB_E_STATE) cb_router_error(res, 503, "service unavailable");
    else pg_error(res, 500, "XX000", "internal error", cb_strerror(rc));
}

// Who is calling: 1 a user (*u), 0 anon, or an answer already written (-1).
static int caller(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, cb_user *u) {
    const char *tok = cb_http_bearer(req);
    if (!tok || !*tok) return 0;
    // supabase-js and supabase-swift send the anon key as the bearer when nobody is signed in.
    const char *apikey = cb_http_header_get(req, "apikey");
    if ((ctx->cfg && ctx->cfg->anon_key[0] && strcmp(tok, ctx->cfg->anon_key) == 0) || (apikey && strcmp(tok, apikey) == 0)) return 0;
    int rc = cb_rt_deps_g.verify_token(ctx, tok, u);
    if (rc == CB_OK) return 1;
    if (rc == CB_E_AUTH) pg_error(res, 401, "PGRST301", "JWT expired", NULL);   // PostgREST's answer to any token it will not take
    else cb_router_error(res, 503, "auth is not available");
    return -1;
}

static int table_of(const char *path) {
    const char *name = path + strlen("/rest/v1/");
    for (int i = 0; i < T_COUNT; i++)
        if (strcmp(TABLES[i].name, name) == 0) return i;
    return -1;
}

static void no_table(cb_http_res *res, const char *path) {
    char msg[256];
    snprintf(msg, sizeof msg, "Could not find the table 'public.%.100s' in the schema cache", path + strlen("/rest/v1/"));
    pg_error(res, 404, "PGRST205", msg, NULL);
}

static bool wants_object(const cb_http_req *req) {
    const char *a = cb_http_header_get(req, "accept");
    return a && strstr(a, "application/vnd.pgrst.object") != NULL;
}

// The columns `q` selects, as indices (all of them for select=*). false (and
// a 400 written) when one does not exist.
static bool resolve_select(const ptable *t, const cb_pgrst_query *q, int *sel, int *n_sel, cb_http_res *res) {
    if (q->n_cols == 0) {
        for (int i = 0; i < t->n_cols; i++) sel[i] = i;
        *n_sel = t->n_cols;
        return true;
    }
    for (int i = 0; i < q->n_cols; i++) {
        sel[i] = col_index(t, q->cols[i]);
        if (sel[i] < 0) {
            char msg[128];
            snprintf(msg, sizeof msg, "column %s.%s does not exist", t->name, q->cols[i]);
            pg_error(res, 400, "42703", msg, NULL);
            return false;
        }
    }
    *n_sel = q->n_cols;
    return true;
}

// Writes rows[from, from+n) of rs, as an array or (object) the one row.
static void send_rows(cb_http_res *res, int status, const rowset *rs, size_t from, size_t n, const int *sel, int n_sel,
                      bool object, bool content_range) {
    if (object && n != 1) {
        char details[64];
        snprintf(details, sizeof details, "The result contains %zu rows", n);
        pg_error(res, 406, "PGRST116", "JSON object requested, multiple (or no) rows returned", details);
        return;
    }
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    if (object) write_row(&w, rs->t, &rs->rows[from], sel, n_sel);
    else {
        cb_jw_arr(&w);
        for (size_t i = 0; i < n; i++) write_row(&w, rs->t, &rs->rows[from + i], sel, n_sel);
        cb_jw_end(&w);
    }
    if (!cb_jw_ok(&w)) { cb_buf_free(&b); pg_error(res, 500, "XX000", "internal error", "row encoding failed"); return; }
    cb_res_send(res, status, object ? "application/vnd.pgrst.object+json; charset=utf-8" : "application/json; charset=utf-8", b.p, b.len);
    if (content_range) {
        char cr[64];
        if (n == 0) snprintf(cr, sizeof cr, "*/*");
        else snprintf(cr, sizeof cr, "%zu-%zu/*", from, from + n - 1);
        cb_res_header(res, "Content-Range", cr);
    }
    cb_buf_free(&b);
}

static void h_get(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    int table = table_of(req->path);
    if (table < 0) { no_table(res, req->path); return; }
    const ptable *t = &TABLES[table];
    cb_pgrst_query q;
    if (cb_pgrst_parse(req->query, &q) != CB_OK) { pg_error(res, 400, "PGRST100", "failed to parse the query string", req->query); return; }
    int sel[CB_PGRST_MAX_COLS], n_sel;
    if (!resolve_select(t, &q, sel, &n_sel, res)) return;
    char msg[160];
    const char *bad = NULL;
    for (int i = 0; i < q.n_filters && !bad; i++)
        if (col_index(t, q.filters[i].col) < 0) bad = q.filters[i].col;
    if (!bad && q.order_col[0] && col_index(t, q.order_col) < 0) bad = q.order_col;
    if (bad) {
        snprintf(msg, sizeof msg, "column %s.%s does not exist", t->name, bad);
        pg_error(res, 400, "42703", msg, NULL);
        return;
    }
    pfilter pf[CB_PGRST_MAX_FILTERS];
    for (int i = 0; i < q.n_filters; i++)
        if (!prep_filter(t, &q, i, &pf[i], msg, sizeof msg)) { pg_error(res, 400, "22P02", msg, NULL); return; }
    if (!ctx->store) { cb_router_error(res, 503, "store is not available"); return; }
    cb_user u;
    memset(&u, 0, sizeof u);
    int who = caller(ctx, req, res, &u);
    if (who < 0) return;

    rowset rs = { t, NULL, 0, 0, false };
    fetch f = { ctx, &q, who == 1 ? u.id : NULL, &rs, CB_OK, NULL };
    fetch_rows(table, &f);
    if (f.err_msg) { pg_error(res, 400, "PGRST100", f.err_msg, NULL); rowset_free(&rs); return; }
    if (f.err) { store_error(res, f.err); rowset_free(&rs); return; }
    if (rs.oom) { pg_error(res, 500, "XX000", "internal error", "out of memory"); rowset_free(&rs); return; }

    // Filter in place (a dropped row's cells are freed as it goes).
    size_t keep = 0;
    for (size_t r = 0; r < rs.n; r++) {
        bool ok = true;
        for (int i = 0; i < q.n_filters && ok; i++) ok = matches(&rs.rows[r], &pf[i]);
        if (ok) rs.rows[keep++] = rs.rows[r];
        else for (int k = 0; k < t->n_cols; k++) { free(rs.rows[r].c[k].s); free(rs.rows[r].c[k].p); }
    }
    rs.n = keep;
    if (q.order_col[0] && rs.n > 1) {
        g_sort.col = col_index(t, q.order_col);
        g_sort.desc = q.order_desc;
        qsort(rs.rows, rs.n, sizeof *rs.rows, cmp_rows);
    }
    size_t from = (size_t)q.offset < rs.n ? (size_t)q.offset : rs.n;
    size_t n = rs.n - from;
    if (q.limit > 0 && (size_t)q.limit < n) n = (size_t)q.limit;
    send_rows(res, 200, &rs, from, n, sel, n_sel, wants_object(req), true);
    rowset_free(&rs);
}

// ---- POST /rest/v1/chat_messages ------------------------------------------------------

// One object of the insert body, read and checked. false and a written answer on a refusal.
static bool read_chat(cb_ctx *ctx, const char *body, size_t n, const char *base, const char *uid, cb_chat_row *out,
                      cb_http_res *res) {
    char path[64], uuidbuf[64];
    memset(out, 0, sizeof *out);
#define P(k) (snprintf(path, sizeof path, "%s%s", base, k), path)
    static const char *const required[3] = { "game_id", "user_id", "message" };
    for (int i = 0; i < 3; i++) {
        if (cb_json_find(body, n, P(required[i]), NULL) != CB_JSON_STR) {
            char msg[160];
            snprintf(msg, sizeof msg, "null value in column \"%s\" of relation \"chat_messages\" violates not-null constraint", required[i]);
            pg_error(res, 400, "23502", msg, NULL);
            return false;
        }
    }
    if (!cb_json_get_str(body, n, P("game_id"), out->game_id, sizeof out->game_id) ||
        !cb_json_get_str(body, n, P("user_id"), uuidbuf, sizeof uuidbuf) || !cb_uuid_valid(uuidbuf, strlen(uuidbuf))) {
        pg_error(res, 400, "22P02", "invalid input syntax for type uuid", NULL);
        return false;
    }
    for (char *p = uuidbuf; *p; p++) *p = (char)tolower((unsigned char)*p);
    cb_strlcpy(out->user_id, uuidbuf, sizeof out->user_id);
    cb_span tok;
    cb_json_find(body, n, P("message"), &tok);
    out->message = malloc(tok.n + 1);
    if (!out->message) { pg_error(res, 500, "XX000", "internal error", "out of memory"); return false; }
    int ml = cb_json_unescape(tok, out->message, tok.n + 1);
    if (ml < 0 || strlen(out->message) != (size_t)ml) {
        // Postgres TEXT cannot hold a NUL.
        pg_error(res, 400, "22P05", "unsupported Unicode escape sequence", NULL);
        return false;
    }
    cb_json_type st = cb_json_find(body, n, P("is_system"), NULL);
    if (st == CB_JSON_BOOL) cb_json_get_bool(body, n, P("is_system"), &out->is_system);
    else if (st != CB_JSON_MISSING && st != CB_JSON_NULL) { pg_error(res, 400, "22P02", "invalid input syntax for type boolean", NULL); return false; }
    if (cb_json_find(body, n, P("id"), NULL) == CB_JSON_STR) {
        if (!cb_json_get_str(body, n, P("id"), uuidbuf, sizeof uuidbuf) || !cb_uuid_valid(uuidbuf, strlen(uuidbuf))) {
            pg_error(res, 400, "22P02", "invalid input syntax for type uuid", NULL);
            return false;
        }
        for (char *p = uuidbuf; *p; p++) *p = (char)tolower((unsigned char)*p);
        cb_strlcpy(out->id, uuidbuf, sizeof out->id);
    } else {
        cb_uuid_v4(out->id);
    }
#undef P
    // The INSERT policy: the caller's own user_id, in a game they sit at.
    int m = strcmp(out->user_id, uid) == 0 ? CB_STORE_CALL(ctx->store, is_member, out->game_id, uid) : 0;
    if (m < 0) { store_error(res, m); return false; }
    if (m != 1) {
        pg_error(res, 403, "42501", "new row violates row-level security policy for table \"chat_messages\"", NULL);
        return false;
    }
    return true;
}

#define CHAT_INSERT_MAX 16

static void h_post(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    int table = table_of(req->path);
    if (table < 0) { no_table(res, req->path); return; }
    cb_pgrst_query q;
    if (cb_pgrst_parse(req->query, &q) != CB_OK || q.n_filters) { pg_error(res, 400, "PGRST100", "failed to parse the query string", req->query); return; }
    if (!ctx->store) { cb_router_error(res, 503, "store is not available"); return; }
    cb_user u;
    memset(&u, 0, sizeof u);
    int who = caller(ctx, req, res, &u);
    if (who < 0) return;
    if (table != T_CHAT || who == 0) {
        // Only chat is writable, and only by a signed-in user (seed.sql's grants).
        char msg[96];
        snprintf(msg, sizeof msg, "permission denied for table %s", TABLES[table].name);
        pg_error(res, who == 0 ? 401 : 403, "42501", msg, NULL);
        return;
    }
    const ptable *t = &TABLES[T_CHAT];
    int sel[CB_PGRST_MAX_COLS], n_sel;
    if (!resolve_select(t, &q, sel, &n_sel, res)) return;
    const char *body = (const char *)req->body;
    size_t n = req->body_len;
    if (!body || !cb_json_valid(body, n)) { pg_error(res, 400, "PGRST102", "Empty or invalid json", NULL); return; }
    cb_json_type top = cb_json_find(body, n, "", NULL);
    int count = top == CB_JSON_OBJ ? 1 : top == CB_JSON_ARR ? cb_json_array_len(body, n, "") : -1;
    if (count < 0 || count > CHAT_INSERT_MAX) { pg_error(res, 400, "PGRST102", "Empty or invalid json", NULL); return; }
    cb_chat_row rows[CHAT_INSERT_MAX];
    memset(rows, 0, sizeof rows);
    int ok = 0;
    int64_t now = cb_now_ms();
    // Every row is checked before any is written: one statement, all or nothing.
    for (; ok < count; ok++) {
        char base[16];
        if (top == CB_JSON_ARR) {
            snprintf(base, sizeof base, "%d", ok);
            if (cb_json_find(body, n, base, NULL) != CB_JSON_OBJ) { pg_error(res, 400, "PGRST102", "Empty or invalid json", NULL); break; }
            snprintf(base, sizeof base, "%d.", ok);
        } else base[0] = 0;
        if (!read_chat(ctx, body, n, base, u.id, &rows[ok], res)) { cb_chat_row_free(&rows[ok]); break; }
        rows[ok].created_at = now;
    }
    if (ok == count) {
        int rc = CB_OK;
        for (int i = 0; i < count && rc == CB_OK; i++) rc = CB_STORE_CALL(ctx->store, chat_put, &rows[i]);
        if (rc == CB_E_CONFLICT) pg_error(res, 409, "23505", "duplicate key value violates unique constraint \"chat_messages_pkey\"", NULL);
        else if (rc != CB_OK) store_error(res, rc);
        else {
            // The chat_messages_changes trigger's broadcast.
            for (int i = 0; i < count; i++) rt_publish_chat(ctx->rt, rows[i].game_id, &rows[i]);
            const char *prefer = cb_http_header_get(req, "prefer");
            if (prefer && strstr(prefer, "return=representation")) {
                rowset rs = { t, NULL, 0, 0, false };
                for (int i = 0; i < count; i++) chat_fn(&rows[i], &rs);
                if (rs.oom) pg_error(res, 500, "XX000", "internal error", "out of memory");
                else send_rows(res, 201, &rs, 0, rs.n, sel, n_sel, wants_object(req), false);
                rowset_free(&rs);
            } else {
                cb_res_status(res, 201);
                cb_res_header(res, "Content-Range", "*/*");
            }
        }
    }
    for (int i = 0; i < ok && i < count; i++) cb_chat_row_free(&rows[i]);
}

int cb_postgrest_routes(cb_ctx *ctx) {
    int rc = cb_router_add(ctx->router, "GET", "/rest/v1/*", CB_ROUTE_APIKEY, h_get);
    if (rc == CB_OK) rc = cb_router_add(ctx->router, "POST", "/rest/v1/*", CB_ROUTE_APIKEY, h_post);
    return rc;
}
