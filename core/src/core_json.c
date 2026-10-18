// core_json.c - the core's JSON (core_json.h says what and why). With
// providers/aws/src/aws_json.c, the only files that include util/json.h.
#include "core_json.h"

#include "util/err.h"
#include "util/log.h"
#include "../../shared/c/num_parse.h"
#include "util/clock.h"
#include "util/json.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// =====================================================================================
// JWT.

static const char JWT_HEADER_JSON[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

const char *cb_js_jwt_header(size_t *n) {
    *n = sizeof JWT_HEADER_JSON - 1;
    return JWT_HEADER_JSON;
}

static bool jwt_str_is(const char *json, size_t n, const char *path, const char *want) {
    char v[64];
    return cb_json_get_str(json, n, path, v, sizeof v) && strcmp(v, want) == 0;
}

bool cb_js_jwt_header_ok(const char *json, size_t n) {
    return cb_json_valid(json, n) && cb_json_find(json, n, "", NULL) == CB_JSON_OBJ && jwt_str_is(json, n, "alg", "HS256");
}

int cb_js_jwt_claims(cb_buf *out, const cb_jwt_claims *c, const char *issuer) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "aud", "authenticated");
    cb_jw_kint(&w, "exp", c->exp);
    cb_jw_kint(&w, "iat", c->iat);
    if (issuer && issuer[0]) cb_jw_kstr(&w, "iss", issuer);
    cb_jw_kstr(&w, "sub", c->sub);
    cb_jw_kstr(&w, "email", c->email);
    cb_jw_kstr(&w, "role", "authenticated");
    cb_jw_kstr(&w, "session_id", c->session_id);
    cb_jw_key(&w, "user_metadata");
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "username", c->username);
    cb_jw_end(&w);
    cb_jw_key(&w, "app_metadata");
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "provider", "email");
    cb_jw_key(&w, "providers");
    cb_jw_arr(&w);
    cb_jw_str(&w, "email");
    cb_jw_end(&w);
    cb_jw_end(&w);
    cb_jw_kbool(&w, "is_anonymous", false);
    cb_jw_end(&w);
    return cb_jw_ok(&w) ? CB_OK : CB_E_NOMEM;
}

bool cb_js_jwt_claims_decode(const char *body, size_t bl, cb_jwt_claims *out, bool *has_nbf, int64_t *nbf) {
    memset(out, 0, sizeof *out);
    *has_nbf = false;
    *nbf = 0;
    if (!cb_json_valid(body, bl) || cb_json_find(body, bl, "", NULL) != CB_JSON_OBJ) return false;
    cb_jwt_claims c;
    memset(&c, 0, sizeof c);
    if (!jwt_str_is(body, bl, "aud", "authenticated") || !jwt_str_is(body, bl, "role", "authenticated")) return false;
    if (!cb_json_get_i64(body, bl, "exp", &c.exp)) return false;
    if (cb_json_find(body, bl, "nbf", NULL) != CB_JSON_MISSING) {
        if (!cb_json_get_i64(body, bl, "nbf", nbf)) return false;
        *has_nbf = true;
    }
    if (!cb_json_get_i64(body, bl, "iat", &c.iat)) c.iat = 0;
    if (!cb_json_get_str(body, bl, "sub", c.sub, sizeof c.sub)) return false;
    if (!cb_json_get_str(body, bl, "session_id", c.session_id, sizeof c.session_id)) return false;
    if (!cb_json_get_str(body, bl, "email", c.email, sizeof c.email)) c.email[0] = 0;
    if (!cb_json_get_str(body, bl, "user_metadata.username", c.username, sizeof c.username)) c.username[0] = 0;
    *out = c;
    return true;
}

// =====================================================================================
// The admin console's API.

struct cb_js_stream {
    cb_jw w;
};

static cb_js_stream *adm_open(cb_buf *out) {
    cb_js_stream *s = malloc(sizeof *s);
    if (!s) return NULL;
    cb_jw_init(&s->w, out);
    cb_jw_obj(&s->w);
    return s;
}

// Closes the document and frees the stream.
static int adm_close(cb_js_stream *s) {
    if (!s) return CB_E_NOMEM;
    cb_jw_end(&s->w);
    bool ok = cb_jw_ok(&s->w);
    free(s);
    return ok ? CB_OK : CB_E_NOMEM;
}

static void adm_kiso(cb_jw *w, const char *k, int64_t ms) {
    char t[40];
    if (ms <= 0 || cb_iso8601(ms, t, sizeof t) < 0) cb_jw_knull(w, k);
    else cb_jw_kstr(w, k, t);
}

static void adm_links(cb_jw *w, const cb_js_admin_links *l) {
    if (!l->lambda) { cb_jw_knull(w, "links"); return; }
    cb_jw_key(w, "links");
    cb_jw_obj(w);
    cb_jw_kstr(w, "lambda", l->lambda);
    cb_jw_kstr(w, "logs_insights", l->logs_insights);
    cb_jw_kstr(w, "dynamodb", l->dynamodb);
    cb_jw_kstr(w, "backups", l->backups);
    cb_jw_end(w);
}

static int adm_done(cb_jw *w) { return cb_jw_ok(w) ? CB_OK : CB_E_NOMEM; }

int cb_js_admin_overview_write(cb_buf *out, const cb_js_admin_overview *o) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "version", o->version);
    cb_jw_kstr(&w, "mode", o->mode);
    cb_jw_kint(&w, "uptime_s", o->uptime_s);
    adm_kiso(&w, "started_at", o->started_ms);
    cb_jw_kstr(&w, "store", o->store);
    cb_jw_kstr(&w, "table", o->table);
    cb_jw_kstr(&w, "public_url", o->public_url);
    cb_jw_kint(&w, "requests", o->requests);
    cb_jw_kint(&w, "errors_5xx", o->errors_5xx);
    if (o->conns_known) {
        cb_jw_kint(&w, "http_conns", o->http_conns);
        cb_jw_kint(&w, "ws_conns", o->ws_conns);
    } else {
        cb_jw_knull(&w, "http_conns");
        cb_jw_knull(&w, "ws_conns");
    }
    if (o->games_needing_bots < 0) cb_jw_knull(&w, "games_needing_bots");
    else cb_jw_kint(&w, "games_needing_bots", o->games_needing_bots);
    cb_jw_key(&w, "store_throttles");
    cb_jw_obj(&w);
    cb_jw_kint(&w, "throttled", (int64_t)o->throttled);
    cb_jw_kint(&w, "gave_up", (int64_t)o->gave_up);
    cb_jw_end(&w);
    if (o->continuous_backup) cb_jw_kstr(&w, "last_backup", "continuous (point-in-time recovery)");
    else if (o->last_backup_ms) adm_kiso(&w, "last_backup", o->last_backup_ms);
    else cb_jw_knull(&w, "last_backup");
    cb_jw_key(&w, "process");
    cb_jw_obj(&w);
    cb_jw_kint(&w, "pid", o->pid);
    cb_jw_kint(&w, "max_rss_kb", o->max_rss_kb);
    cb_jw_kint(&w, "cpu_user_ms", o->cpu_user_ms);
    cb_jw_kint(&w, "cpu_sys_ms", o->cpu_sys_ms);
    cb_jw_end(&w);
    adm_links(&w, &o->links);
    cb_jw_end(&w);
    return adm_done(&w);
}

cb_js_stream *cb_js_admin_collections_begin(cb_buf *out, const char *store) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_kstr(&s->w, "store", store);
    cb_jw_key(&s->w, "collections");
    cb_jw_arr(&s->w);
    return s;
}

void cb_js_admin_collections_add(cb_js_stream *s, const char *name) { cb_jw_str(&s->w, name); }

int cb_js_admin_collections_end(cb_js_stream *s) {
    if (s) cb_jw_end(&s->w);
    return adm_close(s);
}

void cb_js_admin_row(cb_js_stream *s, int n, const char *const *names, const char *const *values) {
    cb_jw_obj(&s->w);
    for (int i = 0; i < n; i++) {
        cb_jw_key(&s->w, names[i] ? names[i] : "?");
        cb_jw_str(&s->w, values[i]);   // NULL writes null
    }
    cb_jw_end(&s->w);
}

cb_js_stream *cb_js_admin_browse_begin(cb_buf *out, const char *collection) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_kstr(&s->w, "collection", collection);
    cb_jw_key(&s->w, "rows");
    cb_jw_arr(&s->w);
    return s;
}

int cb_js_admin_browse_end(cb_js_stream *s, const char *next) {
    if (s) {
        cb_jw_end(&s->w);
        cb_jw_kstr(&s->w, "next", next);
    }
    return adm_close(s);
}

cb_js_stream *cb_js_admin_query_begin(cb_buf *out, const char *dialect, bool write_allowed) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_kstr(&s->w, "dialect", dialect);
    cb_jw_kbool(&s->w, "write_allowed", write_allowed);
    cb_jw_key(&s->w, "rows");
    cb_jw_arr(&s->w);
    return s;
}

int cb_js_admin_query_end(cb_js_stream *s, bool truncated) {
    if (s) {
        cb_jw_end(&s->w);
        cb_jw_kbool(&s->w, "truncated", truncated);
    }
    return adm_close(s);
}

bool cb_js_admin_query_body(const char *body, size_t n, char *stmt, size_t stmt_cap, char *confirm, size_t confirm_cap) {
    if (!body || !cb_json_get_str(body, n, "statement", stmt, stmt_cap)) return false;
    if (!cb_json_get_str(body, n, "confirm", confirm, confirm_cap)) confirm[0] = 0;
    return true;
}

cb_js_stream *cb_js_admin_users_begin(cb_buf *out) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_key(&s->w, "users");
    cb_jw_arr(&s->w);
    return s;
}

void cb_js_admin_users_add(cb_js_stream *s, const cb_user_row *u) {
    cb_jw *w = &s->w;
    cb_jw_obj(w);
    cb_jw_kstr(w, "id", u->id);
    cb_jw_kstr(w, "email", u->email);
    cb_jw_kstr(w, "username", u->username);
    adm_kiso(w, "created_at", u->created_at);
    adm_kiso(w, "last_sign_in_at", u->last_sign_in_at);
    cb_jw_end(w);
}

int cb_js_admin_users_end(cb_js_stream *s, int64_t offset, int64_t next_offset) {
    if (s) {
        cb_jw_end(&s->w);
        cb_jw_kint(&s->w, "offset", offset);
        if (next_offset >= 0) cb_jw_kint(&s->w, "next_offset", next_offset);
        else cb_jw_knull(&s->w, "next_offset");
    }
    return adm_close(s);
}

bool cb_js_admin_id_body(const char *body, size_t n, char *id, size_t cap) {
    return body && cb_json_get_str(body, n, "id", id, cap);
}

static int adm_one(cb_buf *out, const char *key, const char *value) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, key, value);
    cb_jw_end(&w);
    return adm_done(&w);
}

int cb_js_admin_deleted(cb_buf *out, const char *id) { return adm_one(out, "deleted", id); }
int cb_js_admin_written(cb_buf *out, const char *path) { return adm_one(out, "written", path); }

// One game's scalar columns and its seats, into an open object.
static void adm_game(cb_jw *w, const cb_js_admin_game *a) {
    const cb_game_row *g = a->g;
    cb_jw_kstr(w, "id", g->id);
    cb_jw_kint(w, "status", g->status);
    cb_jw_kstr(w, "status_label", a->status_label);
    cb_jw_kint(w, "version", g->version);
    cb_jw_kint(w, "round_epoch", g->round_epoch);
    cb_jw_kbool(w, "needs_bots", g->needs_bots);
    adm_kiso(w, "created_at", g->created_at);
    adm_kiso(w, "updated_at", g->updated_at);
    adm_kiso(w, "last_commit_at", g->last_commit_at);
    cb_jw_key(w, "seats");
    if (a->roster_rc != ROSTER_OK) {
        cb_jw_null(w);
        cb_jw_kint(w, "roster_error", a->roster_rc);
        return;
    }
    const Roster *r = a->roster;
    cb_jw_arr(w);
    for (int i = 0; i < r->n; i++) {
        cb_jw_obj(w);
        cb_jw_key(w, "name");
        cb_jw_strn(w, r->seats[i].name, r->seats[i].name_len);
        cb_jw_key(w, "id");
        cb_jw_strn(w, r->seats[i].id, r->seats[i].id_len);
        cb_jw_kbool(w, "bot", r->seats[i].brain_len > 0);
        cb_jw_end(w);
    }
    cb_jw_end(w);
    cb_jw_key(w, "title");
    cb_jw_strn(w, r->title, r->title_len);
}

cb_js_stream *cb_js_admin_games_begin(cb_buf *out) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_key(&s->w, "games");
    cb_jw_arr(&s->w);
    return s;
}

void cb_js_admin_games_add(cb_js_stream *s, const cb_js_admin_game *g) {
    cb_jw_obj(&s->w);
    adm_game(&s->w, g);
    cb_jw_end(&s->w);
}

int cb_js_admin_games_end(cb_js_stream *s, const char *next) {
    if (s) {
        cb_jw_end(&s->w);
        cb_jw_kstr(&s->w, "next", next);
    }
    return adm_close(s);
}

int cb_js_admin_game_write(cb_buf *out, const cb_js_admin_game *a, const cb_js_admin_envelope *e) {
    const cb_game_row *g = a->g;
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    adm_game(&w, a);
    cb_jw_kint(&w, "state_len", (int64_t)g->state_len);
    cb_jw_kint(&w, "roster_len", (int64_t)g->roster_len);
    cb_jw_kint(&w, "logs_len", (int64_t)g->logs_len);
    cb_jw_kbool(&w, "has_seed", g->has_seed);
    if (e->view_ok) {
        if (e->envelope_hex) cb_jw_kstr(&w, "envelope_hex", e->envelope_hex);
        else cb_jw_knull(&w, "envelope_hex");
        cb_jw_kint(&w, "envelope_version", e->envelope_version);
    } else {
        cb_jw_knull(&w, "envelope_hex");
        cb_jw_kstr(&w, "envelope_error", e->envelope_error);
    }
    cb_jw_end(&w);
    return adm_done(&w);
}

cb_js_stream *cb_js_admin_bots_begin(cb_buf *out) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_key(&s->w, "bots");
    cb_jw_arr(&s->w);
    return s;
}

void cb_js_admin_bots_add(cb_js_stream *s, const cb_bot_row *r) {
    cb_jw *w = &s->w;
    cb_jw_obj(w);
    cb_jw_kstr(w, "id", r->id);
    cb_jw_kstr(w, "nickname", r->nickname);
    cb_jw_kstr(w, "strategy_key", r->strategy_key);
    cb_jw_kint(w, "elo_rating", r->elo_rating);
    if (r->previous_elo < 0) cb_jw_knull(w, "previous_elo");
    else cb_jw_kint(w, "previous_elo", r->previous_elo);
    cb_jw_kint(w, "games_played", r->games_played);
    adm_kiso(w, "updated_at", r->updated_at);
    cb_jw_end(w);
}

int cb_js_admin_bots_end(cb_js_stream *s) {
    if (s) cb_jw_end(&s->w);
    return adm_close(s);
}

cb_js_stream *cb_js_admin_logs_begin(cb_buf *out, uint64_t last_seq) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_kint(&s->w, "last_seq", (int64_t)last_seq);
    cb_jw_key(&s->w, "entries");
    cb_jw_arr(&s->w);
    return s;
}

void cb_js_admin_logs_add(cb_js_stream *s, uint64_t seq, int64_t at_ms, const char *level, const char *module,
                          const char *msg) {
    cb_jw *w = &s->w;
    cb_jw_obj(w);
    cb_jw_kint(w, "seq", (int64_t)seq);
    adm_kiso(w, "at", at_ms);
    cb_jw_kstr(w, "level", level);
    cb_jw_kstr(w, "module", module);
    cb_jw_kstr(w, "msg", msg);
    cb_jw_end(w);
}

int cb_js_admin_logs_end(cb_js_stream *s, const cb_js_admin_links *links) {
    if (s) {
        cb_jw_end(&s->w);
        adm_links(&s->w, links);
    }
    return adm_close(s);
}

cb_js_stream *cb_js_admin_config_begin(cb_buf *out, const cb_js_admin_config *c) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_kstr(&s->w, "mode", c->mode);
    cb_jw_kstr(&s->w, "store", c->store);
    cb_jw_kstr(&s->w, "table", c->table);
    cb_jw_kstr(&s->w, "allowed_origins", c->allowed_origins);
    cb_jw_kstr(&s->w, "aws_region", c->aws_region);
    cb_jw_kstr(&s->w, "function_name", c->function_name);
    cb_jw_key(&s->w, "settings");
    cb_jw_arr(&s->w);
    return s;
}

void cb_js_admin_config_add(cb_js_stream *s, const char *name, const char *value, bool secret) {
    cb_jw *w = &s->w;
    cb_jw_obj(w);
    cb_jw_kstr(w, "name", name);
    cb_jw_kstr(w, "value", value);   // cb_config_each has already redacted a secret
    cb_jw_kbool(w, "secret", secret);
    cb_jw_end(w);
}

int cb_js_admin_config_end(cb_js_stream *s) {
    if (s) cb_jw_end(&s->w);
    return adm_close(s);
}

int cb_js_admin_backups_write(cb_buf *out, const cb_js_admin_backups *b) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "store", b->store);
    if (b->sqlite) {
        cb_jw_kstr(&w, "kind", "vacuum_into");
        cb_jw_kstr(&w, "dir", b->dir);
        cb_jw_key(&w, "files");
        cb_jw_arr(&w);
        for (int i = 0; i < b->n_files; i++) {
            cb_jw_obj(&w);
            cb_jw_kstr(&w, "name", b->files[i].name);
            cb_jw_kint(&w, "size", b->files[i].size);
            adm_kiso(&w, "at", b->files[i].mtime_ms);
            cb_jw_end(&w);
        }
        cb_jw_end(&w);
    } else {
        cb_jw_kstr(&w, "kind", "pitr");
        cb_jw_kstr(&w, "explain", b->explain);
    }
    adm_links(&w, &b->links);
    cb_jw_end(&w);
    return adm_done(&w);
}
