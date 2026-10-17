// functions_http.c - POST /v1/{create,action,meta,account/delete}
// (functions_http.h), ported byte for byte from the old edge functions.
//
// The port of utils.ts wrap400 (auth first, then the content type, the body
// whole, the refusal), packed_action.ts, meta_actions.ts, create/index.ts and
// delete-account/index.ts. The statuses are the TypeScript's, which the header
// comment once summarised differently: wrap400 answered EVERY refusal that had
// a reason with 400 (an unauthenticated move, a game that is not there, a body
// that is not an ask) and one without a reason with 500; only the content type
// check (415) and delete-account (405 for the method, 401 for auth, 500 for a
// failure) chose their own.
#include "functions_http.h"

#include "after.h"

#include "app_internal.h"
#include "edge.h"
#include "limits.h"
#include "v1_http.h"
#include "auth/auth.h"
#include "net/router.h"
#include "realtime/realtime.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/uuid.h"

#include "replay_extras.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OCTETS "application/octet-stream"

static int verify_bearer(cb_ctx *ctx, const cb_http_req *req, cb_user *out) {
    if (!ctx->auth) return CB_E_STATE;
    return cb_auth_verify_bearer(ctx->auth, req, out);
}

int (*cb_functions_verify)(cb_ctx *ctx, const cb_http_req *req, cb_user *out) = verify_bearer;

// ---- replies --------------------------------------------------------------------------

static void refusal(cb_http_res *res, int status, int why, int code, int detail) {
    AskRefusal r = { (uint8_t)why, code, detail };
    uint8_t body[ASK_REFUSAL_BYTES];
    int n = ask_refusal_encode(&r, body, sizeof body);
    if (n < 0) {   // a reason that is not one: the server's own fault
        AskRefusal internal = { ASK_WHY_INTERNAL, 0, 0 };
        n = ask_refusal_encode(&internal, body, sizeof body);
        status = 500;
    }
    cb_res_send(res, status, OCTETS, body, (size_t)n);
}

static void octets(cb_http_res *res, const uint8_t *body, size_t n) {
    cb_res_send(res, 200, OCTETS, body, n);
}

// What a cb_table_run failure says to the client: a game that is not there is
// the client's (NOT_FOUND, 400), anything else the server's (INTERNAL, 500;
// "write contention" included, as runTableOp's thrown Error was).
// A store throttled past its backoff is 503 BUSY (v1_http.h cb_v1_busy); the
// clients send only reads again, since a write may have landed before it.
static void run_failure(cb_http_res *res, int rc) {
    if (rc == CB_E_NOT_FOUND) refusal(res, 400, ASK_WHY_NOT_FOUND, 0, 0);
    else if (rc == CB_E_BUSY) cb_v1_busy(res);
    else refusal(res, 500, ASK_WHY_INTERNAL, 0, 0);
}

static bool octet_body(const cb_http_req *req) {
    const char *ct = cb_http_header_get(req, "content-type");
    return ct && strstr(ct, OCTETS) != NULL;
}

// The bearer, or false with the refusal answered: `status` AUTH for a token
// that does not verify (400 for the writes, 401 for delete-account), 500 when
// the check itself could not run (auth.h): that is not the caller's token.
static bool verified(cb_ctx *ctx, const cb_http_req *req, cb_user *u, cb_http_res *res, int status) {
    int rc = cb_functions_verify(ctx, req, u);
    if (rc == CB_OK) return true;
    if (rc == CB_E_AUTH) refusal(res, status, ASK_WHY_AUTH, 0, 0);
    else refusal(res, rc == CB_E_STATE ? 503 : 500, ASK_WHY_INTERNAL, 0, 0);
    return false;
}

// The prologue every wrap400 endpoint shares: the handles, the bearer, the
// content type. false when the response is already written.
static bool wrap400(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, cb_user *u) {
    if (!ctx->store) { refusal(res, 503, ASK_WHY_INTERNAL, 0, 0); return false; }
    if (!verified(ctx, req, u, res, 400)) return false;
    // A client that still sends JSON must not have it read as an ask.
    if (!octet_body(req)) { refusal(res, 415, ASK_WHY_MALFORMED, 0, 0); return false; }
    return true;
}

// Once the answer is out under Lambda (app/after.h), at once otherwise.
static void kick(cb_ctx *ctx, const char *game_id) { cb_after_kick(ctx, game_id); }

// THE ONE RULE for a request that SEATS its caller (a create, a join): the
// account behind the token must still exist. An access token is verified
// locally and outlives the account it names by up to its TTL (auth.h), so a
// deleted account's token, or a validly signed one for an id that never had
// an account, would otherwise make a seat nobody owns (smoke_matrix.c). One
// read, on the two requests that are rare; a move by a seat that already
// exists is the seat's, and costs nothing more. false with the refusal
// answered: 400 AUTH, as every refused bearer here.
static bool account_lives(cb_ctx *ctx, const cb_user *u, cb_http_res *res) {
    cb_user_row row;
    int rc = CB_STORE_CALL(ctx->store, user_get, u->id, &row);
    if (rc == CB_OK) return true;
    if (rc == CB_E_NOT_FOUND) refusal(res, 400, ASK_WHY_AUTH, 0, 0);
    else run_failure(res, rc);
    return false;
}

// ---- action ------------------------------------------------------------------------------

typedef struct {
    const cb_user *u;
    const uint8_t *wire;
    int            wire_len;
    int64_t        intent;   // -1: the request carried none
} act_ud;

static int run_act(Table *t, const cb_game_row *row, const uint8_t seed[32], void *ud) {
    const act_ud *a = ud;
    return table_act(t, a->u->id, (int)strlen(a->u->id), a->wire, a->wire_len, a->intent, row->round_epoch);
}

static void h_action(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!wrap400(ctx, req, res, &u)) return;
    TableRequest q;
    if (table_request_decode(req->body, (int)req->body_len, &q) != 0) {
        refusal(res, 400, ASK_WHY_MALFORMED, 0, 0);
        return;
    }
    char gid[CB_GAME_ID_MAX + 1];
    if (q.gid_len > CB_GAME_ID_MAX) { refusal(res, 400, ASK_WHY_NOT_FOUND, 0, 0); return; }
    memcpy(gid, req->body + q.gid_off, q.gid_len);
    gid[q.gid_len] = 0;
    act_ud a = { &u, req->body + q.wire_off, q.wire_len, q.has_intent ? (int64_t)q.intent : -1 };
    cb_table_op op = { gid, &u, run_act, &a, false };
    cb_table_result r;
    int rc = cb_table_run(ctx, &op, &r);
    if (rc != CB_OK) { run_failure(res, rc); return; }
    free(r.envelope);
    if (r.result < 0) {
        if (r.result == TABLE_E_WIRE) refusal(res, 400, ASK_WHY_MALFORMED, 0, 0);
        else refusal(res, 400, ASK_WHY_REFUSED, r.result, r.detail);
        return;
    }
    uint8_t body[TABLE_RESP_BYTES];
    int n = table_action_response(r.result, r.reject, (uint32_t)r.version, body, sizeof body);
    if (n < 0) { refusal(res, 500, ASK_WHY_INTERNAL, 0, 0); return; }
    octets(res, body, (size_t)n);
    // A committed move that leaves a bot to act wakes the bot loop.
    if (r.committed && r.needs_bots) kick(ctx, gid);
}

// ---- meta ---------------------------------------------------------------------------------

typedef struct {
    int     n, cap;
    cb_bot_row *rows;
} bot_list;

static bool collect_bot(const cb_bot_row *b, void *ud) {
    bot_list *l = ud;
    if (l->n == l->cap) {
        int cap = l->cap ? 2 * l->cap : 64;
        void *p = realloc(l->rows, (size_t)cap * sizeof *l->rows);
        if (!p) return false;
        l->rows = p;
        l->cap = cap;
    }
    l->rows[l->n++] = *b;
    return true;
}

typedef struct {
    const cb_user  *u;
    const TableAsk *ask;
    const bot_list *bots;   // ADD_BOT only
    bool            no_bot; // ADD_BOT found no row to seat
} ask_ud;

static bool seated(const Table *t, const char *id) {
    return table_seat_of(t, id, (int)strlen(id)) >= 0;
}

static int run_ask(Table *t, const cb_game_row *row, const uint8_t seed[32], void *ud) {
    ask_ud *a = ud;
    const char *uid = a->u->id, *name = a->u->username;
    a->no_bot = false;
    if (a->ask->kind != ASK_ADD_BOT)
        return table_ask(t, uid, (int)strlen(uid), name, (int)strlen(name), a->ask, NULL, seed);
    // Which bot row to seat is the request's choice among the rows (the one it
    // names, or any not already seated); whether it may sit is the kernel's.
    int pool[1024], n = 0;
    for (int i = 0; i < a->bots->n && n < (int)(sizeof pool / sizeof pool[0]); i++) {
        const cb_bot_row *b = &a->bots->rows[i];
        bool ok = a->ask->bot_len ? strlen(b->id) == a->ask->bot_len && memcmp(b->id, a->ask->bot, a->ask->bot_len) == 0
                                  : !seated(t, b->id);
        if (ok) pool[n++] = i;
    }
    if (n == 0) { a->no_bot = true; return TABLE_E_FORBIDDEN; }
    uint32_t pick;
    cb_random_bytes(&pick, sizeof pick);
    const cb_bot_row *b = &a->bots->rows[pool[pick % (uint32_t)n]];
    TableAskBot bot = { b->id, (int)strlen(b->id), b->nickname, (int)strlen(b->nickname),
                        b->strategy_key, (int)strlen(b->strategy_key) };
    return table_ask(t, uid, (int)strlen(uid), name, (int)strlen(name), a->ask, &bot, seed);
}

static void h_meta(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!wrap400(ctx, req, res, &u)) return;
    TableAsk ask;
    if (ask_decode(req->body, (int)req->body_len, &ask) != ASK_OK) {
        refusal(res, 400, ASK_WHY_MALFORMED, 0, 0);
        return;
    }
    char gid[CB_GAME_ID_MAX + 1];
    if (ask.gid_len > CB_GAME_ID_MAX) { refusal(res, 400, ASK_WHY_NOT_FOUND, 0, 0); return; }
    memcpy(gid, ask.gid, ask.gid_len);
    gid[ask.gid_len] = 0;
    if (ask.kind == ASK_JOIN && !account_lives(ctx, &u, res)) return;
    // A bump starts a bot run (a Lambda invocation and a lease write): only a
    // seated player may ask (the kernel's rule, TABLE_E_NOT_SEATED), and not
    // often (limits.h).
    int retry;
    if (ask.kind == ASK_BUMP && !cb_limit_take(CB_LIMIT_BUMP_USER, u.id, cb_now_ms(), &retry)) {
        cb_v1_limited(res, retry, false);
        return;
    }
    bot_list bots = { 0, 0, NULL };
    if (ask.kind == ASK_ADD_BOT && CB_STORE_CALL(ctx->store, bots_list, collect_bot, &bots) < 0) {
        CB_ERROR("functions", "meta: failed to fetch bots");
        free(bots.rows);
        refusal(res, 500, ASK_WHY_INTERNAL, 0, 0);
        return;
    }
    ask_ud a = { &u, &ask, &bots, false };
    cb_table_op op = { gid, &u, run_ask, &a, false };
    cb_table_result r;
    int rc = cb_table_run(ctx, &op, &r);
    free(bots.rows);
    if (rc != CB_OK) { run_failure(res, rc); return; }
    if (r.result < 0) {
        free(r.envelope);
        if (a.no_bot) refusal(res, 400, ASK_WHY_NO_BOT, 0, 0);
        else refusal(res, 400, ASK_WHY_REFUSED, r.result, r.detail);
        return;
    }
    octets(res, r.envelope, r.envelope_len);
    free(r.envelope);
    // A bump commits nothing and wakes the bots whenever the table has bot
    // work; an edit wakes them when it committed a table that has (the deal).
    bool wake = ask.kind == ASK_BUMP ? r.needs_bots : r.committed && r.needs_bots;
    if (wake) kick(ctx, gid);
}

// ---- create --------------------------------------------------------------------------------

static void h_create(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ctx->store) { refusal(res, 503, ASK_WHY_INTERNAL, 0, 0); return; }
    cb_user u;
    // No wrap400 here, so no content-type check: create has no body.
    if (!verified(ctx, req, &u, res, 400)) return;
    if (!account_lives(ctx, &u, res)) return;
    int retry;
    if (!cb_limit_take(CB_LIMIT_CREATE_USER, u.id, cb_now_ms(), &retry)) { cb_v1_limited(res, retry, false); return; }
    cb_table_result r;
    int rc = cb_table_create(ctx, &u, &r);
    if (rc != CB_OK) {
        CB_ERROR("functions", "create failed (%s)", cb_strerror(rc));
        run_failure(res, rc == CB_E_NOT_FOUND ? CB_E_DB : rc);
        return;
    }
    octets(res, r.envelope, r.envelope_len);
    free(r.envelope);
}

// ---- delete-account ---------------------------------------------------------------------------

typedef struct {
    int n, cap;
    char (*ids)[CB_GAME_ID_MAX + 1];
} game_ids;

static bool collect_game(const cb_member_row *m, void *ud) {
    game_ids *l = ud;
    if (l->n == l->cap) {
        int cap = l->cap ? 2 * l->cap : 16;
        void *p = realloc(l->ids, (size_t)cap * sizeof *l->ids);
        if (!p) return false;
        l->ids = p;
        l->cap = cap;
    }
    snprintf(l->ids[l->n++], sizeof *l->ids, "%s", m->game_id);
    return true;
}

static int run_redact(Table *t, const cb_game_row *row, const uint8_t seed[32], void *ud) {
    const char *uid = ud;
    return table_redact(t, uid, (int)strlen(uid), REDACTED_PLAYER_NAME, (int)strlen(REDACTED_PLAYER_NAME));
}

// The user's stored replays, collected first and rewritten after: a store's
// list callback may not write (SQLite's runs after its lock, DynamoDB's
// between pages), and the rewrite reads nothing the list did not hand over.
typedef struct {
    cb_uuid  id;
    int      n_players, seat;
    uint8_t *extras;
    size_t   extras_len;
} snap_redaction;

typedef struct {
    const char     *user_id;
    snap_redaction *v;
    int             n, cap;
    bool            oom;
} snap_list;

static bool collect_snapshot(const cb_snapshot_row *r, void *ud) {
    snap_list *l = ud;
    int seat = -1;
    for (int i = 0; i < r->n_players && seat < 0; i++)
        if (strcmp(r->player_ids[i], l->user_id) == 0) seat = i;
    if (seat < 0 || !r->extras_len) return true;   // nobody to rename, or no names at all
    if (l->n == l->cap) {
        int cap = l->cap ? 2 * l->cap : 16;
        void *p = realloc(l->v, (size_t)cap * sizeof *l->v);
        if (!p) { l->oom = true; return false; }
        l->v = p;
        l->cap = cap;
    }
    snap_redaction *e = &l->v[l->n];
    e->extras = malloc(r->extras_len);
    if (!e->extras) { l->oom = true; return false; }
    memcpy(e->extras, r->extras, r->extras_len);
    e->extras_len = r->extras_len;
    snprintf(e->id, sizeof e->id, "%s", r->id);
    e->n_players = r->n_players;
    e->seat = seat;
    l->n++;
    return true;
}

// Every finished game's replay keeps the game and loses the person: the seat's
// name in its extras becomes REDACTED_PLAYER_NAME (the kernel's
// replay_extras_redact, which leaves every other byte as it was). An extras
// blob the kernel cannot read is cleared instead, because the promise is that
// no stored replay names a deleted player; a replay without extras still plays,
// its seats called P1, P2. A rerun (a retried delete) finds the name already
// gone and writes nothing.
static int redact_snapshots(cb_ctx *ctx, const char *user_id) {
    snap_list l = { .user_id = user_id };
    cb_snapshot_query q = { .viewer_id = user_id };
    int rc = CB_STORE_CALL(ctx->store, snapshots_query, &q, collect_snapshot, &l);
    if (rc >= 0) rc = l.oom ? CB_E_NOMEM : CB_OK;
    const int name_len = (int)strlen(REDACTED_PLAYER_NAME);
    for (int i = 0; i < l.n && rc == CB_OK; i++) {
        snap_redaction *e = &l.v[i];
        int cap = (int)e->extras_len + name_len + 1;
        uint8_t *out = malloc((size_t)cap);
        if (!out) { rc = CB_E_NOMEM; break; }
        int n = replay_extras_redact(e->extras, (int)e->extras_len, e->n_players, e->seat,
                                     (const unsigned char *)REDACTED_PLAYER_NAME, name_len, out, cap);
        if (n < 0) {
            CB_WARN("functions", "delete-account: replay %s has extras the kernel cannot read (%d); clearing them", e->id, n);
            n = 0;
        }
        if ((size_t)n != e->extras_len || memcmp(out, e->extras, (size_t)n) != 0) {
            int w = CB_STORE_CALL(ctx->store, snapshot_set_extras, e->id, out, (size_t)n);
            if (w != CB_OK && w != CB_E_NOT_FOUND) rc = w;   // a replay gone meanwhile names nobody
        }
        free(out);
    }
    for (int i = 0; i < l.n; i++) free(l.v[i].extras);
    free(l.v);
    return rc;
}

int cb_app_delete_account(cb_ctx *ctx, const char *user_id, int *refused_code, int *refused_detail) {
    *refused_code = *refused_detail = 0;
    if (!ctx->store) return CB_E_STATE;
    // 1. Every table the user sits at, before the account delete takes the
    //    membership rows with it.
    game_ids l = { 0, 0, NULL };
    int rc = CB_STORE_CALL(ctx->store, games_of_user, user_id, 1 << 20, collect_game, &l);
    if (rc < 0) { free(l.ids); return rc; }
    // 2. The name redacted in each, one fresh CAS commit per game.
    rc = CB_OK;
    for (int i = 0; i < l.n && rc == CB_OK; i++) {
        cb_table_op op = { l.ids[i], NULL, run_redact, (void *)user_id, true };
        cb_table_result r;
        int g = cb_table_run(ctx, &op, &r);
        free(r.envelope);
        // A stale membership (no game, or no seat) has no name to scrub.
        if (g == CB_E_NOT_FOUND) continue;
        if (g != CB_OK) { rc = g; break; }
        if (r.result == TABLE_E_NOT_SEATED) continue;
        if (r.result < 0) { *refused_code = r.result; *refused_detail = r.detail; rc = CB_E_FORBIDDEN; }
    }
    if (rc != CB_OK) { free(l.ids); return rc; }
    // 2b. The stored replays of their finished games, the same name in each.
    if ((rc = redact_snapshots(ctx, user_id)) != CB_OK) { free(l.ids); return rc; }
    // 3. The leaderboard name, then 4. the account (sessions, memberships,
    //    the rating row and chat go with it).
    rc = CB_STORE_CALL(ctx->store, rating_set_username, user_id, NULL);
    if (rc != CB_OK && rc != CB_E_NOT_FOUND) { free(l.ids); return rc; }
    cb_edge_stale(CB_EDGE_BOARD);   // their name and row leave the leaderboard
    if (!ctx->auth) { free(l.ids); return CB_E_STATE; }
    // A retry of a delete that already removed the account (its answer was
    // lost, or a step after it failed) finds nothing left to remove: done, as
    // the first attempt meant. It was a 500 forever (unit_oom.c, smoke_matrix.c).
    rc = cb_auth_delete_user(ctx->auth, user_id);
    if (rc == CB_E_NOT_FOUND) rc = CB_OK;
    // 5. Their open streams end: their seats keep the id, so a page left open
    //    would go on hearing those games (smoke_matrix.c).
    if (rc == CB_OK) cb_rt_forget_user(ctx->rt, user_id, (const char (*)[CB_GAME_ID_MAX + 1])l.ids, l.n);
    free(l.ids);
    return rc;
}

static void h_delete_account(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (strcmp(req->method, "POST") != 0) { refusal(res, 405, ASK_WHY_METHOD, 0, 0); return; }
    if (!ctx->store) { refusal(res, 503, ASK_WHY_INTERNAL, 0, 0); return; }
    cb_user u;
    // No or an invalid token: the client reads 401 as "already deleted".
    if (!verified(ctx, req, &u, res, 401)) return;
    int code, detail;
    int rc = cb_app_delete_account(ctx, u.id, &code, &detail);
    if (rc != CB_OK) {
        CB_ERROR("functions", "delete-account failed for %s (%s)", u.id, cb_strerror(rc));
        // Whatever stopped it, the account is still there: a 500, with the
        // kernel's code when a table refused the redaction.
        if (code) refusal(res, 500, ASK_WHY_REFUSED, code, detail);
        else refusal(res, 500, ASK_WHY_INTERNAL, 0, 0);
        return;
    }
    cb_res_status(res, 204);
}

// ---- routes --------------------------------------------------------------------------------

int cb_functions_routes(cb_ctx *ctx) {
    // Every method reaches the handler, as it reached the edge function: the
    // method rules are the functions' own (delete-account's 405), and the
    // router answers OPTIONS (DESIGN.md "Binary surface").
    static const struct { const char *path; cb_handler fn; } routes[] = {
        { "/v1/create", h_create },
        { "/v1/action", h_action },
        { "/v1/meta", h_meta },
        { "/v1/account/delete", h_delete_account },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        int rc = cb_router_add(ctx->router, "*", routes[i].path, CB_ROUTE_POOL, routes[i].fn);
        if (rc != CB_OK) return rc;
    }
    return CB_OK;
}
