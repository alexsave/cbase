// unit_app.c - the app layer (src/app/) against the real kernel and an in-RAM
// store (lib_app_store_mem.c), through cb_handle exactly as a request arrives.
//
// Bearers are faked through the cb_functions_verify seam ("A" and "B" are two
// users); the clock is a fake one the bot loop's sleeps advance, so the
// kernel's pacing costs no wall time; every fan-out is recorded through the
// cb_fanout_observer seam.
#include "t.h"
#include "lib_app_store_mem.h"

#include "app/app_internal.h"
#include "app/bots.h"
#include "app/fanout.h"
#include "app/functions_http.h"
#include "app/table_ops.h"
#include "awire.h"
#include "handler.h"
#include "legal.h"
#include "net/router.h"
#include "table.h"
#include "util/err.h"

#include <stdlib.h>
#include <string.h>

static const char UID_A[] = "aaaaaaaa-0000-4000-8000-000000000001";
static const char UID_B[] = "bbbbbbbb-0000-4000-8000-000000000002";

static cb_config g_cfg;
static cb_ctx g_ctx;
static cb_store *g_store;

// ---- the fake clock ----------------------------------------------------------------

static int64_t g_now = 1760000000000LL;
static int64_t fake_now(void) { return g_now; }
static void fake_sleep(int64_t ms) { g_now += ms; }

// ---- recorders -------------------------------------------------------------------------

static int g_kicks;
static char g_kicked[CB_GAME_ID_MAX + 1];
static void kick_recorder(cb_ctx *ctx, const char *game_id) {
    g_kicks++;
    snprintf(g_kicked, sizeof g_kicked, "%s", game_id);
}

typedef struct {
    int     count;
    int64_t version;
    bool    broadcast;
    int     humans, casts, joined, left;
    bool    spectator_cast;
    cb_uuid first_joined, first_left;
} fan_seen;
static fan_seen g_fan;

static void fan_observer(const cb_fanout *f) {
    fan_seen s;
    memset(&s, 0, sizeof s);
    s.count = g_fan.count + 1;
    s.version = f->version;
    s.broadcast = f->broadcast;
    for (int i = 0; i < f->n_seats; i++) {
        if (f->seats[i].user_id[0]) s.humans++;
        if (f->seats[i].cast) s.casts++;
    }
    s.spectator_cast = f->spectator_cast != NULL;
    s.joined = f->n_joined;
    s.left = f->n_left;
    if (f->n_joined) snprintf(s.first_joined, sizeof s.first_joined, "%s", f->joined[0]);
    if (f->n_left) snprintf(s.first_left, sizeof s.first_left, "%s", f->left[0]);
    g_fan = s;
}

static int fake_verify(cb_ctx *ctx, const struct cb_http_req *req, cb_user *out) {
    const char *b = cb_http_bearer(req);
    memset(out, 0, sizeof *out);
    if (!b) return CB_E_AUTH;
    if (strcmp(b, "A") == 0) { snprintf(out->id, sizeof out->id, "%s", UID_A); snprintf(out->username, sizeof out->username, "ALICE"); return CB_OK; }
    if (strcmp(b, "B") == 0) { snprintf(out->id, sizeof out->id, "%s", UID_B); snprintf(out->username, sizeof out->username, "BOB"); return CB_OK; }
    return CB_E_AUTH;
}

static void setup(int n_bots) {
    char err[256];
    cb_config_from_env(&g_cfg, err, sizeof err);
    if (g_ctx.router) cb_router_free(g_ctx.router);
    if (g_store) cb_store_close(g_store);
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.cfg = &g_cfg;
    g_ctx.mode = CB_MODE_SERVE;
    g_store = tm_store_new(n_bots, "random");
    g_ctx.store = g_store;
    g_ctx.kick_bots = kick_recorder;
    g_ctx.router = cb_router_new();
    cb_functions_routes(&g_ctx);
    tm_add_user(g_store, UID_A, "ALICE");
    tm_add_user(g_store, UID_B, "BOB");
    cb_row_cache_clear();
    cb_app_now = fake_now;
    cb_app_sleep = fake_sleep;
    cb_functions_verify = fake_verify;
    cb_fanout_observer = fan_observer;
    memset(&g_fan, 0, sizeof g_fan);
    g_kicks = 0;
    g_kicked[0] = 0;
}

// ---- requests ------------------------------------------------------------------------------

typedef struct {
    int     status;
    uint8_t body[16384];
    size_t  len;
    char    ctype[64];
} reply;

static void call(const char *method, const char *fn, const char *bearer, const char *ctype,
                 const void *body, size_t n, reply *out) {
    cb_http_req req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "%s", method);
    char path[64], auth[64];
    snprintf(path, sizeof path, "/functions/v1/%s", fn);
    req.path = path;
    req.query = "";
    if (bearer) {
        snprintf(auth, sizeof auth, "Bearer %s", bearer);
        req.headers[req.n_headers++] = (cb_http_header){ "Authorization", auth };
    }
    if (ctype) req.headers[req.n_headers++] = (cb_http_header){ "Content-Type", ctype };
    req.body = body;
    req.body_len = n;
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&g_ctx, &req, &res);
    out->status = res.status;
    out->len = res.body.len < sizeof out->body ? res.body.len : sizeof out->body;
    if (out->len) memcpy(out->body, res.body.p, out->len);
    out->ctype[0] = 0;
    const char *h = cb_buf_cstr(&res.headers);
    const char *ct = strstr(h, "Content-Type: ");
    if (ct) {
        ct += 14;
        size_t k = strcspn(ct, "\r");
        snprintf(out->ctype, sizeof out->ctype, "%.*s", (int)k, ct);
    }
    cb_res_free(&res);
}

#define OCT "application/octet-stream"

static void meta(const char *bearer, int kind, const char *gid, const char *bot, reply *out) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = (uint8_t)kind;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    if (bot) { a.bot_len = (uint8_t)strlen(bot); memcpy(a.bot, bot, a.bot_len); }
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    call("POST", "meta", bearer, OCT, body, n > 0 ? (size_t)n : 0, out);
}

static void action(const char *bearer, const char *gid, const uint8_t *wire, int wire_len, uint32_t intent, reply *out) {
    uint8_t body[256];
    int gl = (int)strlen(gid);
    memcpy(body, gid, (size_t)gl);
    memcpy(body + gl, wire, (size_t)wire_len);
    int n = table_request_encode(body, sizeof body, gl, wire_len, true, intent);
    call("POST", "action", bearer, OCT, body, n > 0 ? (size_t)n : 0, out);
}

// The refusal a reply carries: why, or -1 when it is not a 10-byte refusal.
static int refusal_why(const reply *r, AskRefusal *out) {
    AskRefusal tmp;
    if (!out) out = &tmp;
    return ask_refusal_decode(r->body, (int)r->len, out) == ASK_OK ? out->why : -1;
}

static cb_game_row stored(const char *gid) {
    cb_game_row row;
    memset(&row, 0, sizeof row);
    CB_STORE_CALL(g_store, game_get, gid, &row);
    return row;
}

static int64_t version_of(const char *gid) {
    cb_game_row row = stored(gid);
    int64_t v = row.version;
    cb_game_row_free(&row);
    return v;
}

static int status_of(const char *gid) {
    cb_game_row row = stored(gid);
    int s = row.status;
    cb_game_row_free(&row);
    return s;
}

static bool first_game(const cb_view_row *row, void *ud) {
    snprintf((char *)ud, CB_GAME_ID_MAX + 1, "%s", row->game_id);
    return false;
}

// A new game created over HTTP by `bearer` (whose user id is uid); its id into gid.
static void create_game_by(const char *bearer, const char *uid, char gid[CB_GAME_ID_MAX + 1], reply *r) {
    call("POST", "create", bearer, NULL, NULL, 0, r);
    gid[0] = 0;
    if (r->status != 200) return;
    // The dashboard read gives the id back: the newest game first.
    cb_table_views_of_user(&g_ctx, uid, 1, first_game, gid);
}

static void create_game(char gid[CB_GAME_ID_MAX + 1], reply *r) {
    create_game_by("A", UID_A, gid, r);
}

// A legal move for `seat` on the stored board (WAIT excluded), as its action
// wire, chosen by a seeded LCG. Its length, or 0 when the seat has none.
static uint32_t g_lcg = 12345;
static int pick_move(const char *gid, int seat, uint8_t *wire, int cap) {
    cb_game_row row = stored(gid);
    Game *g = calloc(1, sizeof *g);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    LegalMoves *moves = calloc(1, sizeof *moves);
    Table t;
    table_init(&t, g, snaps);
    int n = 0;
    if (table_load(&t, row.state, (int)row.state_len, row.roster, (int)row.roster_len) == TABLE_OK) {
        calculate_legal_moves(g, seat, moves);
        int idx[MAX_LEGAL_MOVES], k = 0;
        for (int i = 0; i < moves->n; i++) if (moves->moves[i].type != MOVE_WAIT) idx[k++] = i;
        if (k > 0) {
            g_lcg = g_lcg * 1103515245u + 12345u;
            const LegalMove *m = &moves->moves[idx[(g_lcg >> 8) % (uint32_t)k]];
            AwireAction a;
            memset(&a, 0, sizeof a);
            a.kind = m->type;
            a.n = m->n_cards;
            for (int i = 0; i < m->n_cards; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
            n = awire_encode(&a, wire, cap);
        }
    }
    free(g);
    free(snaps);
    free(moves);
    cb_game_row_free(&row);
    return n;
}

// The seat a user id holds in the stored roster, or -1.
static int seat_of(const char *gid, const char *uid) {
    cb_game_row row = stored(gid);
    Roster r;
    int seat = -1;
    if (roster_decode(&r, row.roster, (int)row.roster_len) == ROSTER_OK)
        for (int i = 0; i < r.n; i++) if (strlen(uid) == r.seats[i].id_len && memcmp(r.seats[i].id, uid, r.seats[i].id_len) == 0) seat = i;
    cb_game_row_free(&row);
    return seat;
}

// ---- tests ---------------------------------------------------------------------------------

static void create_answers_the_seat_0_envelope(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    T_EQ_INT(r.status, 200);
    T_EQ_STR(r.ctype, OCT);
    T_EQ_INT(strlen(gid), 6);
    EnvHeader h;
    T_CHECK(env_header_read(r.body, (int)r.len, &h));
    T_EQ_INT(h.seat, 0);
    T_EQ_INT(h.version, 0);
    // The answer is exactly the view row computed on demand from the stored row.
    cb_view_row v;
    T_EQ_INT(cb_table_view(&g_ctx, gid, UID_A, &v), CB_OK);
    T_EQ_MEM(r.body, r.len, v.view, v.view_len);
    T_EQ_STR(v.status, "waiting");
    T_EQ_INT(v.version, 0);
    cb_view_row_free(&v);
    // A stranger is not seated; the spectator row exists.
    T_EQ_INT(cb_table_view(&g_ctx, gid, UID_B, &v), CB_E_NOT_FOUND);
    T_EQ_INT(cb_table_view(&g_ctx, gid, NULL, &v), CB_OK);
    T_CHECK(env_header_read(v.view, (int)v.view_len, &h) && h.seat == -1);
    cb_view_row_free(&v);
    // The creator's dashboard hears an INSERT.
    T_EQ_INT(g_fan.count, 1);
    T_EQ_INT(g_fan.joined, 1);
    T_EQ_STR(g_fan.first_joined, UID_A);
    T_EQ_INT(CB_STORE_CALL(g_store, is_member, gid, UID_A), 1);
}

static void join_add_bot_and_start(void) {
    setup(3);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("B", ASK_JOIN, gid, NULL, &r);
    T_EQ_INT(r.status, 200);
    EnvHeader h;
    T_CHECK(env_header_read(r.body, (int)r.len, &h) && h.seat == 1 && h.version == 1);
    T_EQ_INT(version_of(gid), 1);
    T_EQ_INT(CB_STORE_CALL(g_store, is_member, gid, UID_B), 1);
    T_EQ_INT(g_fan.joined, 1);
    T_EQ_STR(g_fan.first_joined, UID_B);
    T_EQ_INT(g_fan.humans, 2);
    // A second join is the kernel's refusal, 400, REFUSED with the roster's reason.
    meta("B", ASK_JOIN, gid, NULL, &r);
    AskRefusal why;
    T_EQ_INT(r.status, 400);
    T_EQ_INT(r.len, ASK_REFUSAL_BYTES);
    T_EQ_INT(refusal_why(&r, &why), ASK_WHY_REFUSED);
    T_EQ_INT(why.code, TABLE_E_ROSTER);
    T_EQ_INT(why.detail, ROSTER_E_DUPLICATE);
    T_EQ_INT(version_of(gid), 1);

    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    T_EQ_INT(r.status, 200);
    T_EQ_INT(version_of(gid), 2);
    T_CHECK(seat_of(gid, tm_bot_id(g_store, 0)) == 2 || seat_of(gid, tm_bot_id(g_store, 1)) == 2 ||
            seat_of(gid, tm_bot_id(g_store, 2)) == 2);
    // A named bot that is not a row: NO_BOT.
    meta("A", ASK_ADD_BOT, gid, "no-such-bot", &r);
    T_EQ_INT(r.status, 400);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_NO_BOT);

    meta("A", ASK_START, gid, NULL, &r);
    T_EQ_INT(r.status, 200);
    T_EQ_INT(status_of(gid), GAME_STATUS_WAITING);
    int kicks = g_kicks;
    meta("B", ASK_START, gid, NULL, &r);
    T_EQ_INT(r.status, 200);
    T_EQ_INT(status_of(gid), GAME_STATUS_PLAYING);
    T_EQ_INT(version_of(gid), 4);
    cb_game_row row = stored(gid);
    T_CHECK(row.has_seed);
    T_CHECK(row.logs_len > 0);
    T_EQ_INT(row.round_epoch, 0);
    // The deal is broadcast: each human's cast and the spectators'.
    T_CHECK(g_fan.broadcast);
    T_EQ_INT(g_fan.casts, 2);
    T_CHECK(g_fan.spectator_cast);
    // The deal wakes the bots exactly when the table has bot work.
    T_EQ_INT(g_kicks - kicks, row.needs_bots ? 1 : 0);
    cb_game_row_free(&row);
}

// A 2-human game dealt, so every move is a human's.
static void two_humans_dealt(char gid[CB_GAME_ID_MAX + 1]) {
    reply r;
    create_game(gid, &r);
    meta("B", ASK_JOIN, gid, NULL, &r);
    meta("A", ASK_START, gid, NULL, &r);
    meta("B", ASK_START, gid, NULL, &r);
}

// One legal human move over HTTP by whoever has one. The reply, or false.
static bool any_human_move(const char *gid, reply *r) {
    static const char *const uids[2] = { UID_A, UID_B }, *const bearers[2] = { "A", "B" };
    for (int k = 0; k < 2; k++) {
        int seat = seat_of(gid, uids[k]);
        uint8_t wire[64];
        int n = seat >= 0 ? pick_move(gid, seat, wire, sizeof wire) : 0;
        if (n <= 0) continue;
        action(bearers[k], gid, wire, n, (uint32_t)version_of(gid), r);
        return true;
    }
    return false;
}

static void a_legal_action_applies_and_bumps_the_version(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    T_EQ_INT(status_of(gid), GAME_STATUS_PLAYING);
    int64_t v = version_of(gid);
    int commits = tm_hooks_of(g_store)->commits;
    reply r;
    T_CHECK(any_human_move(gid, &r));
    T_EQ_INT(r.status, 200);
    T_EQ_STR(r.ctype, OCT);
    TableResponse resp;
    T_EQ_INT(r.len, TABLE_RESP_BYTES);
    T_EQ_INT(table_response_decode(r.body, (int)r.len, &resp), 0);
    T_EQ_INT(resp.status, TABLE_STATUS_APPLIED);
    T_EQ_INT(resp.version, v + 1);
    T_EQ_INT(version_of(gid), v + 1);
    T_EQ_INT(tm_hooks_of(g_store)->commits, commits + 1);
    // Every commit updates both dashboards, the human move is cast.
    T_EQ_INT(g_fan.version, v + 1);
    T_EQ_INT(g_fan.humans, 2);
    T_CHECK(g_fan.broadcast);
    // A PLAYING row this process committed is cached.
    T_EQ_INT(cb_row_cache_size(), 1);
}

static void an_illegal_action_is_rejected_and_commits_nothing(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    int64_t v = version_of(gid);
    int commits = tm_hooks_of(g_store)->commits;
    int fans = g_fan.count;
    // A pickup by both seats: at most one of them can be the defender, so at
    // least one of these is a move the rules refuse.
    const uint8_t pickup[2] = { AWIRE_PICKUP, 0 };
    reply ra, rb;
    action("A", gid, pickup, 2, (uint32_t)v, &ra);
    TableResponse resp;
    int rejected = 0;
    T_EQ_INT(ra.status, 200);
    if (table_response_decode(ra.body, (int)ra.len, &resp) == 0 && resp.status == TABLE_STATUS_REJECTED) {
        rejected++;
        T_CHECK(resp.reject_code != 0 && resp.reject_code != TABLE_REJECT_STALE_ROUND);
        T_EQ_INT(resp.version, v);
        T_EQ_INT(version_of(gid), v);
        T_EQ_INT(tm_hooks_of(g_store)->commits, commits);
        T_EQ_INT(g_fan.count, fans);
    }
    v = version_of(gid);
    commits = tm_hooks_of(g_store)->commits;
    action("B", gid, pickup, 2, (uint32_t)v, &rb);
    if (table_response_decode(rb.body, (int)rb.len, &resp) == 0 && resp.status == TABLE_STATUS_REJECTED) {
        rejected++;
        T_EQ_INT(resp.version, v);
        T_EQ_INT(tm_hooks_of(g_store)->commits, commits);
    }
    T_CHECK(rejected >= 1);
    // A wire that is not a move is MALFORMED, 400.
    const uint8_t junk[3] = { 9, 9, 9 };
    reply r;
    action("A", gid, junk, 3, (uint32_t)v, &r);
    T_EQ_INT(r.status, 400);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_MALFORMED);
}

// Plays random legal human moves until the stored round_epoch moves (a round
// closed) or the game ends. true when an epoch > 0 was reached.
static bool play_until_epoch(const char *gid) {
    for (int i = 0; i < 400; i++) {
        cb_game_row row = stored(gid);
        int64_t e = row.round_epoch;
        int st = row.status;
        cb_game_row_free(&row);
        if (e > 0) return true;
        if (st != GAME_STATUS_PLAYING) return false;
        reply r;
        if (!any_human_move(gid, &r)) return false;
    }
    return false;
}

static void a_stale_intent_is_stale_round(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    T_CHECK(play_until_epoch(gid));
    cb_game_row row = stored(gid);
    int64_t epoch = row.round_epoch, v = row.version;
    T_EQ_INT(epoch, v);   // the commit that closed the round stamped its own version
    cb_game_row_free(&row);
    int commits = tm_hooks_of(g_store)->commits;
    static const char *const uids[2] = { UID_A, UID_B }, *const bearers[2] = { "A", "B" };
    int k = 0;
    uint8_t wire[64];
    int n = 0;
    for (; k < 2 && n <= 0; k++) n = pick_move(gid, seat_of(gid, uids[k]), wire, sizeof wire);
    k--;
    T_CHECK(n > 0);
    // Composed against the version before the round began.
    reply r;
    action(bearers[k], gid, wire, n, (uint32_t)(epoch - 1), &r);
    TableResponse resp;
    T_EQ_INT(table_response_decode(r.body, (int)r.len, &resp), 0);
    T_EQ_INT(resp.status, TABLE_STATUS_REJECTED);
    T_EQ_INT(resp.reject_code, TABLE_REJECT_STALE_ROUND);
    T_EQ_INT(resp.version, v);
    T_EQ_INT(version_of(gid), v);
    T_EQ_INT(tm_hooks_of(g_store)->commits, commits);
    // The same move against the current version applies.
    action(bearers[k], gid, wire, n, (uint32_t)v, &r);
    T_EQ_INT(table_response_decode(r.body, (int)r.len, &resp), 0);
    T_EQ_INT(resp.status, TABLE_STATUS_APPLIED);
}

static void a_lost_cas_reloads_and_wins(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    int64_t v = version_of(gid);
    tm_hooks *h = tm_hooks_of(g_store);
    h->steal_commits = 1;
    int conflicts = h->conflicts, commits = h->commits;
    reply r;
    T_CHECK(any_human_move(gid, &r));
    TableResponse resp;
    T_EQ_INT(table_response_decode(r.body, (int)r.len, &resp), 0);
    T_EQ_INT(resp.status, TABLE_STATUS_APPLIED);
    // The thief's version, then ours on top of it.
    T_EQ_INT(resp.version, v + 2);
    T_EQ_INT(version_of(gid), v + 2);
    T_EQ_INT(h->conflicts, conflicts + 1);
    T_EQ_INT(h->commits, commits + 1);
    // Five losses in a row: write contention, INTERNAL 500, nothing written.
    h->steal_commits = CB_TABLE_ATTEMPTS;
    v = version_of(gid);
    T_CHECK(any_human_move(gid, &r));
    T_EQ_INT(r.status, 500);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_INTERNAL);
    T_EQ_INT(h->commits, commits + 1);
}

typedef struct { int n; size_t moves; } snap_seen;
static bool count_snap(const cb_snapshot_row *row, void *ud) {
    snap_seen *seen = ud;
    seen->n++;
    seen->moves = row->moves_len;
    return true;
}

static void a_finished_game_writes_a_snapshot_and_ratings(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    int moves = 0;
    reply r;
    while (status_of(gid) == GAME_STATUS_PLAYING && moves < 3000) {
        if (!any_human_move(gid, &r)) break;
        moves++;
    }
    T_EQ_INT(status_of(gid), GAME_STATUS_GAME_OVER);
    T_EQ_INT(tm_hooks_of(g_store)->snapshots, 1);
    cb_rating_row ra, rb;
    T_EQ_INT(tm_rating(g_store, UID_A, &ra), CB_OK);
    T_EQ_INT(tm_rating(g_store, UID_B, &rb), CB_OK);
    T_EQ_INT(ra.games_played, 1);
    T_EQ_INT(rb.games_played, 1);
    T_CHECK(ra.elo_rating != TABLE_RATING_START);
    T_EQ_INT((ra.elo_rating - TABLE_RATING_START) + (rb.elo_rating - TABLE_RATING_START), 0);
    // The snapshot is the players', with a verified replay code.
    snap_seen seen = { 0, 0 };
    cb_snapshot_query q = { UID_B, gid, NULL, 0, false };
    T_EQ_INT(CB_STORE_CALL(g_store, snapshots_query, &q, count_snap, &seen), 1);
    T_CHECK(seen.moves > 0);
    // The finished game is evicted from the cache: it is read fresh.
    T_EQ_INT(cb_row_cache_size(), 0);
    // A move on a finished game is MOOT and commits nothing.
    int64_t v = version_of(gid);
    const uint8_t pickup[2] = { AWIRE_PICKUP, 0 };
    action("A", gid, pickup, 2, (uint32_t)v, &r);
    TableResponse resp;
    T_EQ_INT(table_response_decode(r.body, (int)r.len, &resp), 0);
    T_EQ_INT(resp.status, TABLE_STATUS_MOOT);
    T_EQ_INT(version_of(gid), v);
    // Rated once: a later commit on the finished table rates nobody.
    meta("A", ASK_UPDATE_NAME, gid, NULL, &r);
    T_EQ_INT(tm_rating(g_store, UID_A, &ra), CB_OK);
    T_EQ_INT(ra.games_played, 1);
}

static void bump_is_moot_and_commits_nothing(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    int commits = tm_hooks_of(g_store)->commits, fans = g_fan.count;
    // A spectator may bump: the answer is the spectator envelope.
    meta("B", ASK_BUMP, gid, NULL, &r);
    T_EQ_INT(r.status, 200);
    EnvHeader h;
    T_CHECK(env_header_read(r.body, (int)r.len, &h) && h.seat == -1 && h.version == 0);
    T_EQ_INT(version_of(gid), 0);
    T_EQ_INT(tm_hooks_of(g_store)->commits, commits);
    T_EQ_INT(g_fan.count, fans);
    T_EQ_INT(g_kicks, 0);
}

static void the_wire_surface(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = ASK_BUMP;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    uint8_t body[64];
    int n = ask_encode(&a, body, sizeof body);
    // JSON (or anything not octets) is 415, a MALFORMED refusal.
    call("POST", "meta", "A", "application/json", body, (size_t)n, &r);
    T_EQ_INT(r.status, 415);
    T_EQ_STR(r.ctype, OCT);
    T_EQ_INT(r.len, ASK_REFUSAL_BYTES);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_MALFORMED);
    // No bearer: AUTH, 400 (wrap400 gave every reason 400).
    call("POST", "meta", NULL, OCT, body, (size_t)n, &r);
    T_EQ_INT(r.status, 400);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_AUTH);
    call("POST", "create", "nobody", NULL, NULL, 0, &r);
    T_EQ_INT(r.status, 400);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_AUTH);
    // A body that is not an ask.
    call("POST", "meta", "A", OCT, "\x01", 1, &r);
    T_EQ_INT(r.status, 400);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_MALFORMED);
    // A game that is not there.
    meta("A", ASK_JOIN, "ffffff", NULL, &r);
    T_EQ_INT(r.status, 400);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_NOT_FOUND);
    // delete-account: POST only, 401 without a bearer.
    call("GET", "delete-account", "A", NULL, NULL, 0, &r);
    T_EQ_INT(r.status, 405);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_METHOD);
    call("POST", "delete-account", NULL, NULL, NULL, 0, &r);
    T_EQ_INT(r.status, 401);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_AUTH);
}

static void the_last_seat_leaving_deletes_the_game(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_EXIT, gid, NULL, &r);
    T_EQ_INT(r.status, 200);
    cb_game_row row;
    T_EQ_INT(CB_STORE_CALL(g_store, game_get, gid, &row), CB_E_NOT_FOUND);
    T_EQ_INT(g_fan.left, 1);
    T_EQ_STR(g_fan.first_left, UID_A);
}

static void delete_account_redacts(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("B", ASK_JOIN, gid, NULL, &r);
    int64_t v = version_of(gid);
    int code, detail;
    // There is no auth handle here, so the last step (the account itself)
    // refuses; the redaction before it is what this test is about.
    T_EQ_INT(cb_app_delete_account(&g_ctx, UID_B, &code, &detail), CB_E_STATE);
    T_EQ_INT(version_of(gid), v + 1);
    cb_game_row row = stored(gid);
    Roster ro;
    T_EQ_INT(roster_decode(&ro, row.roster, (int)row.roster_len), ROSTER_OK);
    T_EQ_INT(ro.seats[1].name_len, strlen("Deleted player"));
    T_CHECK(memcmp(ro.seats[1].name, "Deleted player", ro.seats[1].name_len) == 0);
    T_CHECK(memcmp(ro.seats[0].name, "ALICE", 5) == 0);
    cb_game_row_free(&row);
    cb_rating_row rb;
    T_EQ_INT(tm_rating(g_store, UID_B, &rb), CB_OK);
    T_EQ_STR(rb.username, "");
}

// ---- bots --------------------------------------------------------------------------------------

static void the_bot_loop_drives_until_a_human_owes_a_move(void) {
    setup(1);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    meta("A", ASK_START, gid, NULL, &r);
    T_EQ_INT(status_of(gid), GAME_STATUS_PLAYING);
    // A moves whenever A has a move; otherwise the bots owe one, and a drive
    // must make it (needs_bots alone is "a bot is still in", not "a bot's turn").
    int rounds = 0, drives = 0;
    const int64_t t0 = g_now;
    while (status_of(gid) == GAME_STATUS_PLAYING && rounds < 3000) {
        int64_t v = version_of(gid);
        uint8_t wire[64];
        int n = pick_move(gid, 0, wire, sizeof wire);
        if (n > 0) {
            action("A", gid, wire, n, (uint32_t)v, &r);
        } else {
            T_EQ_INT(cb_bots_drive(&g_ctx, gid), CB_OK);
            drives++;
            // The drive moved the game on, and let go of its lease.
            T_CHECK(version_of(gid) > v);
            char token[CB_UUID_LEN + 1];
            int64_t until = 1;
            tm_lease(g_store, gid, token, &until);
            T_EQ_INT(until, 0);
        }
        rounds++;
    }
    T_CHECK(drives > 0);
    // The drives waited out the kernel's pacing (on the fake clock).
    T_CHECK(g_now > t0);
    T_EQ_INT(status_of(gid), GAME_STATUS_GAME_OVER);
    // The bot's rating moved too: a bot row is rated like a user's.
    cb_rating_row rb;
    T_EQ_INT(tm_rating(g_store, tm_bot_id(g_store, 0), &rb), CB_OK);
    T_EQ_INT(rb.games_played, 1);
    T_EQ_INT(tm_hooks_of(g_store)->snapshots, 1);
}

static void a_held_lease_skips_the_drive(void) {
    setup(1);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    meta("A", ASK_START, gid, NULL, &r);
    cb_uuid other;
    T_EQ_INT(CB_STORE_CALL(g_store, lease_acquire, gid, g_now, CB_BOT_LEASE_MS, other), CB_OK);
    int64_t v = version_of(gid);
    T_EQ_INT(cb_bots_drive(&g_ctx, gid), CB_OK);
    T_EQ_INT(version_of(gid), v);
    // The holder's lease is untouched.
    char token[CB_UUID_LEN + 1];
    int64_t until = 0;
    tm_lease(g_store, gid, token, &until);
    T_EQ_STR(token, other);
    T_EQ_INT(until, g_now + CB_BOT_LEASE_MS);
}

static void the_scan_kicks_stale_active_bot_games(void) {
    setup(0);
    char g1[CB_GAME_ID_MAX + 1], g2[CB_GAME_ID_MAX + 1], g3[CB_GAME_ID_MAX + 1];
    reply r;
    create_game_by("A", UID_A, g1, &r);
    g_now += 10;
    create_game_by("B", UID_B, g2, &r);
    g_now += 10;
    create_game_by("A", UID_A, g3, &r);
    int64_t now = g_now;
    // Stale and active: kicked. Abandoned (no commit within the hour): not.
    // Fresh (touched within STALE_MS): not. No bot work: not.
    tm_set_scan(g_store, g1, true, now - CB_BOT_STALE_MS - 1, now - 1000);
    tm_set_scan(g_store, g2, true, now - CB_BOT_STALE_MS - 1, now - CB_BOT_ABANDON_MS - 1);
    tm_set_scan(g_store, g3, true, now - 5, now - 5);
    g_kicks = 0;
    T_EQ_INT(cb_bots_scan(&g_ctx, now), 1);
    T_EQ_INT(g_kicks, 1);
    T_EQ_STR(g_kicked, g1);
    tm_set_scan(g_store, g1, false, now - CB_BOT_STALE_MS - 1, now - 1000);
    g_kicks = 0;
    T_EQ_INT(cb_bots_scan(&g_ctx, now), 0);
    T_EQ_INT(g_kicks, 0);
}

int main(void) {
    T_RUN(create_answers_the_seat_0_envelope);
    T_RUN(join_add_bot_and_start);
    T_RUN(a_legal_action_applies_and_bumps_the_version);
    T_RUN(an_illegal_action_is_rejected_and_commits_nothing);
    T_RUN(a_stale_intent_is_stale_round);
    T_RUN(a_lost_cas_reloads_and_wins);
    T_RUN(a_finished_game_writes_a_snapshot_and_ratings);
    T_RUN(bump_is_moot_and_commits_nothing);
    T_RUN(the_wire_surface);
    T_RUN(the_last_seat_leaving_deletes_the_game);
    T_RUN(delete_account_redacts);
    T_RUN(the_bot_loop_drives_until_a_human_owes_a_move);
    T_RUN(a_held_lease_skips_the_drive);
    T_RUN(the_scan_kicks_stale_active_bot_games);
    cb_row_cache_clear();
    cb_router_free(g_ctx.router);
    cb_store_close(g_store);
    return T_DONE();
}
