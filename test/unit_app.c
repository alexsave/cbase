// unit_app.c - the app layer (src/app/) against the real kernel and an in-RAM
// store (lib_app_store_mem.c), through cb_handle exactly as a request arrives.
//
// Bearers are faked through the cb_functions_verify seam ("A" and "B" are two
// users); the clock is a fake one the bot loop's sleeps advance, so the
// kernel's pacing costs no wall time; every fan-out is recorded through the
// cb_fanout_observer seam.
#include "t.h"
#include "app/limits.h"

#include <pthread.h>
#include <time.h>
#include "lib_app_store_mem.h"

#include "app/after.h"
#include "app/app_internal.h"
#include "app/bots.h"
#include "app/edge.h"
#include "app/fanout.h"
#include "app/functions_http.h"
#include "app/table_ops.h"
#include "awire.h"
#include "handler.h"
#include "legal.h"
#include "net/router.h"
#include "realtime/realtime.h"
#include "replay_extras.h"
#include "auth/auth.h"
#include "table.h"
#include "util/err.h"
#include "util/log.h"
#include "util/uuid.h"

#include <stdlib.h>
#include <string.h>
#include "../../shared/c/rng.h"

static const char UID_A[] = "aaaaaaaa-0000-4000-8000-000000000001";
static const char UID_B[] = "bbbbbbbb-0000-4000-8000-000000000002";
static const char UID_C[] = "cccccccc-0000-4000-8000-000000000003";
static const char UID_D[] = "dddddddd-0000-4000-8000-000000000004";

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
// Everything that left the app, in order: a fan-out's version, or -1 a kick.
static int64_t g_seq[64];
static int     g_nseq;
static void seq_note(int64_t v) {
    if (g_nseq < 64) g_seq[g_nseq++] = v;
}

static void kick_recorder(cb_ctx *ctx, const char *game_id) {
    seq_note(-1);
    g_kicks++;
    snprintf(g_kicked, sizeof g_kicked, "%s", game_id);
}

typedef struct {
    int     count;
    int64_t version;
    int     humans, casts, joined, left;
    bool    spectator_cast;
    bool    casts_name_redaction;   // every cast carries REDACTED_PLAYER_NAME (the roster trailer)
    cb_uuid first_joined, first_left;
} fan_seen;
static fan_seen g_fan;

static void fan_observer(const cb_fanout *f) {
    seq_note(f->version);
    fan_seen s;
    memset(&s, 0, sizeof s);
    s.count = g_fan.count + 1;
    s.version = f->version;
    for (int i = 0; i < f->n_seats; i++) {
        if (f->seats[i].user_id[0]) s.humans++;
        if (f->seats[i].cast) s.casts++;
    }
    s.spectator_cast = f->spectator_cast != NULL;
    s.casts_name_redaction = s.casts > 0 && s.spectator_cast &&
        memmem(f->spectator_cast, f->spectator_cast_len, REDACTED_PLAYER_NAME, strlen(REDACTED_PLAYER_NAME)) != NULL;
    for (int i = 0; i < f->n_seats; i++)
        if (f->seats[i].cast && !memmem(f->seats[i].cast, f->seats[i].cast_len, REDACTED_PLAYER_NAME, strlen(REDACTED_PLAYER_NAME)))
            s.casts_name_redaction = false;
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
    if (strcmp(b, "C") == 0) { snprintf(out->id, sizeof out->id, "%s", UID_C); snprintf(out->username, sizeof out->username, "CAROL"); return CB_OK; }
    if (strcmp(b, "D") == 0) { snprintf(out->id, sizeof out->id, "%s", UID_D); snprintf(out->username, sizeof out->username, "DAVE"); return CB_OK; }
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
    tm_add_user(g_store, UID_C, "CAROL");
    tm_add_user(g_store, UID_D, "DAVE");
    cb_app_game_id = cb_game_id_random;
    cb_bots_cycle_observer = NULL;
    cb_row_cache_clear();
    cb_app_now = fake_now;
    cb_app_sleep = fake_sleep;
    cb_functions_verify = fake_verify;
    cb_fanout_observer = fan_observer;
    memset(&g_fan, 0, sizeof g_fan);
    g_kicks = 0;
    cb_limits_reset();
    g_kicked[0] = 0;
    g_nseq = 0;
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
    snprintf(path, sizeof path, "/v1/%s", fn);
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
// wire, chosen by a seeded rng.h stream. Its length, or 0 when the seat has none.
static _Thread_local uint64_t g_rng = 12345;
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
            const LegalMove *m = &moves->moves[idx[rng_u32(&g_rng) % (uint32_t)k]];
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

// What the kernel says of the stored board: a bot can act on it (table_bots_due).
static bool bots_due(const char *gid) {
    cb_game_row row = stored(gid);
    cb_section *s = cb_section_new();
    bool due = s && cb_section_load(s, &row) >= 0 && table_bots_due(&s->t);
    cb_section_free(s);
    cb_game_row_free(&row);
    return due;
}

// A (seat 0) against bots: A moves until a bot can act. true when one can.
static bool until_a_bot_is_due(const char *gid) {
    reply r;
    for (int i = 0; i < 200 && !bots_due(gid) && status_of(gid) == GAME_STATUS_PLAYING; i++) {
        uint8_t w[64];
        int n = pick_move(gid, 0, w, sizeof w);
        if (n <= 0) return false;
        action("A", gid, w, n, (uint32_t)version_of(gid), &r);
    }
    return bots_due(gid);
}

// The bots drive until A owes the move and no bot can act. true when so.
static bool until_the_human_owes(const char *gid) {
    for (int i = 0; i < 200 && bots_due(gid); i++) cb_bots_drive(&g_ctx, gid);
    uint8_t w[64];
    return !bots_due(gid) && status_of(gid) == GAME_STATUS_PLAYING && pick_move(gid, 0, w, sizeof w) > 0;
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

// Whether the kernel can encode the stored session as a replay, asked of the
// kernel itself on the committed row. Two kernel limits make its round-trip
// gate refuse one, and the end of the game then logs and swallows that, as
// finalize.ts did: a log longer than MAX_LOGS records, and a pickup of more
// than MAX_LOG_PAIRS (16) cards, whose record keeps 16 while the replay has
// them all (a three-seat passing game reaches it).
static bool log_encodable(const char *gid) {
    cb_game_row row = stored(gid);
    Game *g = calloc(1, sizeof *g);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    uint8_t *code = malloc(1 << 21), *scratch = malloc(1 << 20);
    Table t;
    table_init(&t, g, snaps);
    int n = table_load(&t, row.state, (int)row.state_len, row.roster, (int)row.roster_len);
    if (n == TABLE_OK)
        n = table_replay_code(&t, row.seed, CB_SEED_LEN, row.logs, (int)row.logs_len, code, 1 << 21, scratch, 1 << 20);
    free(g);
    free(snaps);
    free(code);
    free(scratch);
    cb_game_row_free(&row);
    return n > 0;
}

// ---- tests ---------------------------------------------------------------------------------

static void create_answers_the_seat_0_envelope(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    T_EQ_INT(r.status, 200);
    T_EQ_STR(r.ctype, OCT);
    T_EQ_INT(strlen(gid), 12);   // app_internal.h CB_GAME_ID_HEX (finding L3)
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
    T_EQ_INT(g_fan.casts, 2);
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

// What `wire` by `uid` comes to on a stored board: table_act on a scratch table.
static int act_on(const cb_game_row *row, const char *uid, const uint8_t *wire, int n) {
    Game *g = calloc(1, sizeof *g);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    Table t;
    table_init(&t, g, snaps);
    int rc = table_load(&t, row->state, (int)row->state_len, row->roster, (int)row->roster_len);
    if (rc == TABLE_OK) rc = table_act(&t, uid, (int)strlen(uid), wire, n, -1, row->round_epoch);
    free(g);
    free(snaps);
    return rc;
}

static void a_refusal_from_a_stale_cache_is_rechecked_fresh(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    static const char *const uids[2] = { UID_A, UID_B }, *const bearers[2] = { "A", "B" };
    bool found = false;
    for (int i = 0; i < 300 && !found && status_of(gid) == GAME_STATUS_PLAYING; i++) {
        cb_game_row old = stored(gid);
        reply r;
        if (!any_human_move(gid, &r)) { cb_game_row_free(&old); break; }
        for (int k = 0; k < 2 && !found && old.status == GAME_STATUS_PLAYING; k++) {
            uint8_t wire[64];
            int n = pick_move(gid, seat_of(gid, uids[k]), wire, sizeof wire);
            if (n <= 0 || act_on(&old, uids[k], wire, n) != TABLE_REJECTED) continue;
            // A move the current board allows and the older one refuses: put
            // the older row in the cache, as a process that missed a commit has it.
            found = true;
            int64_t v = version_of(gid);
            cb_row_cache_note(&old);
            action(bearers[k], gid, wire, n, (uint32_t)v, &r);
            TableResponse resp;
            T_EQ_INT(table_response_decode(r.body, (int)r.len, &resp), 0);
            T_EQ_INT(resp.status, TABLE_STATUS_APPLIED);
            T_EQ_INT(resp.version, v + 1);
        }
        cb_game_row_free(&old);
    }
    T_CHECK(found);
}

// Rating farming (M6): a game with a seat whose account is under three days
// old ends unrated for everyone at it, and the leaderboard is not touched.
static void a_new_account_plays_unrated(void) {
    setup(0);
    tm_set_user_created(g_store, UID_B, fake_now() - 3600 * 1000);   // an hour old
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    int moves = 0;
    reply r;
    uint64_t board0 = cb_edge_marks(CB_EDGE_BOARD);
    while (status_of(gid) == GAME_STATUS_PLAYING && moves < 3000) {
        if (!any_human_move(gid, &r)) break;
        moves++;
    }
    T_EQ_INT(status_of(gid), GAME_STATUS_GAME_OVER);
    cb_rating_row ra, rb;
    T_EQ_INT(tm_rating(g_store, UID_A, &ra), CB_OK);
    T_EQ_INT(tm_rating(g_store, UID_B, &rb), CB_OK);
    T_EQ_INT(ra.games_played, 0);
    T_EQ_INT(rb.games_played, 0);
    T_EQ_INT(ra.elo_rating, TABLE_RATING_START);
    T_EQ_INT((int)(cb_edge_marks(CB_EDGE_BOARD) - board0), 0);
    T_EQ_INT(tm_hooks_of(g_store)->snapshots, 1);   // the replay is still kept
    cb_edge_flush(g_ctx.cfg);
}

static void a_finished_game_writes_a_snapshot_and_ratings(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    int moves = 0;
    reply r;
    // The edge's leaderboard copy is marked stale by the commit that rates,
    // exactly once, and by no other move (app/edge.h); the bots never.
    uint64_t board0 = cb_edge_marks(CB_EDGE_BOARD), bots0 = cb_edge_marks(CB_EDGE_BOTS);
    while (status_of(gid) == GAME_STATUS_PLAYING && moves < 3000) {
        if (!any_human_move(gid, &r)) break;
        moves++;
    }
    T_EQ_INT(status_of(gid), GAME_STATUS_GAME_OVER);
    T_CHECK(moves > 1);
    T_EQ_INT((int)(cb_edge_marks(CB_EDGE_BOARD) - board0), 1);
    T_EQ_INT((int)(cb_edge_marks(CB_EDGE_BOTS) - bots0), 0);
    T_EQ_INT(cb_edge_pending(), CB_EDGE_BOARD);
    T_EQ_INT(cb_edge_flush(g_ctx.cfg), CB_OK);   // no distribution in a test: cleared, nothing sent
    T_EQ_INT(cb_edge_pending(), 0);
    T_CHECK(log_encodable(gid));
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
    T_EQ_INT((int)(cb_edge_marks(CB_EDGE_BOARD) - board0), 1);
}

static void add_bot_never_picks_a_seated_bot(void) {
    setup(1);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    T_EQ_INT(r.status, 200);
    T_EQ_INT(seat_of(gid, tm_bot_id(g_store, 0)), 1);
    // The only bot is seated: none is free.
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    T_EQ_INT(r.status, 400);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_NO_BOT);
    T_EQ_INT(version_of(gid), 1);
}

// limits.h: an account's game creations are limited; past the burst, 429 BUSY.
static void create_is_limited(void) {
    setup(0);
    reply r;
    for (int i = 0; i < cb_limit_rules[CB_LIMIT_CREATE_USER].burst; i++) {
        call("POST", "create", "C", NULL, NULL, 0, &r);
        T_EQ_INT(r.status, 200);
    }
    call("POST", "create", "C", NULL, NULL, 0, &r);
    T_EQ_INT(r.status, 429);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_BUSY);
    call("POST", "create", "D", NULL, NULL, 0, &r);   // another account may
    T_EQ_INT(r.status, 200);
}

// A bump starts a bot run (a Lambda invocation and a lease write): only a
// seated player may ask (security finding M8), and not without limit.
static void bump_is_moot_and_seated_only(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    int commits = tm_hooks_of(g_store)->commits, fans = g_fan.count;
    // Someone who does not sit there is refused, and wakes nothing.
    meta("B", ASK_BUMP, gid, NULL, &r);
    T_EQ_INT(r.status, 400);
    AskRefusal why;
    T_EQ_INT(refusal_why(&r, &why), ASK_WHY_REFUSED);
    T_EQ_INT(why.code, TABLE_E_NOT_SEATED);
    T_EQ_INT(g_kicks, 0);
    // The seated creator: moot, the creator's own envelope, nothing committed.
    meta("A", ASK_BUMP, gid, NULL, &r);
    T_EQ_INT(r.status, 200);
    EnvHeader h;
    T_CHECK(env_header_read(r.body, (int)r.len, &h) && h.seat == 0 && h.version == 0);
    T_EQ_INT(version_of(gid), 0);
    T_EQ_INT(tm_hooks_of(g_store)->commits, commits);
    T_EQ_INT(g_fan.count, fans);
    T_EQ_INT(g_kicks, 0);
    // limits.h: an account's bumps are counted; past the burst, 429 BUSY.
    for (int i = 1; i < cb_limit_rules[CB_LIMIT_BUMP_USER].burst; i++) {
        meta("A", ASK_BUMP, gid, NULL, &r);
        T_EQ_INT(r.status, 200);
    }
    meta("A", ASK_BUMP, gid, NULL, &r);
    T_EQ_INT(r.status, 429);
    T_EQ_INT(refusal_why(&r, &why), ASK_WHY_BUSY);
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
    call("GET", "account/delete", "A", NULL, NULL, 0, &r);
    T_EQ_INT(r.status, 405);
    T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_METHOD);
    call("POST", "account/delete", NULL, NULL, NULL, 0, &r);
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
    // The game is gone, so the answer carries no board: an envelope of a
    // table with no seats is one client_adopt_envelope refuses (it was, until
    // the sequence fuzzer, fuzz/fz_sequence.c, found it).
    T_EQ_INT((int)r.len, 0);
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
    uint64_t board0 = cb_edge_marks(CB_EDGE_BOARD);
    T_EQ_INT(cb_app_delete_account(&g_ctx, UID_B, &code, &detail), CB_E_STATE);
    // Their name left the leaderboard: its edge copy is stale.
    T_EQ_INT((int)(cb_edge_marks(CB_EDGE_BOARD) - board0), 1);
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

// A redaction on a dealt table flies no card and sets no good, but the open
// game pages must hear it: the commit casts to every human seat and to the
// spectators, and each cast carries the renamed roster.
static void delete_account_casts_the_rename_to_open_pages(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    T_EQ_INT(status_of(gid), GAME_STATUS_PLAYING);
    int64_t v = version_of(gid);
    memset(&g_fan, 0, sizeof g_fan);
    int code, detail;
    uint64_t board0 = cb_edge_marks(CB_EDGE_BOARD);
    T_EQ_INT(cb_app_delete_account(&g_ctx, UID_B, &code, &detail), CB_E_STATE);
    // Their name left the leaderboard: its edge copy is stale.
    T_EQ_INT((int)(cb_edge_marks(CB_EDGE_BOARD) - board0), 1);   // no auth handle: the account step
    T_EQ_INT(version_of(gid), v + 1);
    T_EQ_INT(g_fan.count, 1);
    T_EQ_INT(g_fan.version, v + 1);
    T_EQ_INT(g_fan.casts, 2);
    T_CHECK(g_fan.spectator_cast);
    T_CHECK(g_fan.casts_name_redaction);
}

// The names a stored replay's extras give its seats, and where the names
// section ends (the times section follows it byte for byte).
typedef struct { int n; char names[CB_MAX_SEATS][64]; size_t names_end; } extras_names;

static bool read_extras_names(const uint8_t *blob, size_t len, int n_players, extras_names *out) {
    memset(out, 0, sizeof *out);
    uint8_t ans[4096];
    int m = replay_extras_decode(blob, (int)len, n_players, 0, ans, (int)sizeof ans);
    if (m < 2 || !(ans[0] & REPLAY_EXTRAS_FLAG_NAMES)) return false;
    out->n = ans[1];
    size_t p = 2, end = 2;
    for (int i = 0; i < out->n; i++) {
        int l = ans[p] | (ans[p + 1] << 8);
        p += 2;
        snprintf(out->names[i], sizeof out->names[i], "%.*s", l, (const char *)ans + p);
        p += (size_t)l;
        end += (size_t)l + 1;
    }
    out->names_end = end;
    return true;
}

typedef struct { cb_snapshot_row row; int n; } snap_copy;
static bool copy_snap(const cb_snapshot_row *r, void *ud) {
    snap_copy *c = ud;
    c->n++;
    c->row = *r;
    c->row.moves = malloc(r->moves_len);
    memcpy(c->row.moves, r->moves, r->moves_len);
    c->row.extras = r->extras_len ? malloc(r->extras_len) : NULL;
    if (r->extras_len) memcpy(c->row.extras, r->extras, r->extras_len);
    return true;
}

// delete-account through cb_handle: the stored replay of a finished game keeps
// the game and loses the name. Its extras name the deleted seat "Deleted
// player", the other seat as it was, and the times section and the moves are
// the same bytes as before.
static void delete_account_anonymizes_stored_replays(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    reply r;
    for (int moves = 0; status_of(gid) == GAME_STATUS_PLAYING && moves < 3000; moves++)
        if (!any_human_move(gid, &r)) break;
    T_EQ_INT(status_of(gid), GAME_STATUS_GAME_OVER);
    T_EQ_INT(tm_hooks_of(g_store)->snapshots, 1);
    snap_copy before = { .n = 0 }, after = { .n = 0 };
    cb_snapshot_query q = { UID_A, gid, NULL, 0, false };
    T_EQ_INT(CB_STORE_CALL(g_store, snapshots_query, &q, copy_snap, &before), 1);
    extras_names nb, na;
    T_CHECK(read_extras_names(before.row.extras, before.row.extras_len, before.row.n_players, &nb));
    T_EQ_INT(nb.n, 2);
    T_EQ_STR(nb.names[0], "ALICE");
    T_EQ_STR(nb.names[1], "BOB");

    // A real auth handle over the same store, so the route runs to its 204.
    snprintf(g_cfg.jwt_secret, sizeof g_cfg.jwt_secret, "unit-app-secret-0123456789abcdef");
    T_EQ_INT(cb_auth_new(&g_ctx, &g_ctx.auth), CB_OK);
    call("POST", "account/delete", "B", NULL, NULL, 0, &r);
    T_EQ_INT(r.status, 204);
    cb_auth_free(g_ctx.auth);
    g_ctx.auth = NULL;

    T_EQ_INT(CB_STORE_CALL(g_store, snapshots_query, &q, copy_snap, &after), 1);
    T_CHECK(read_extras_names(after.row.extras, after.row.extras_len, after.row.n_players, &na));
    T_EQ_INT(na.n, 2);
    T_EQ_STR(na.names[0], "ALICE");
    T_EQ_STR(na.names[1], REDACTED_PLAYER_NAME);
    T_CHECK(memmem(after.row.extras, after.row.extras_len, "BOB", 3) == NULL);
    // Everything else as it was: the times section, the moves, who played.
    T_EQ_INT(after.row.extras_len - na.names_end, before.row.extras_len - nb.names_end);
    T_CHECK(memcmp(after.row.extras + na.names_end, before.row.extras + nb.names_end,
                   before.row.extras_len - nb.names_end) == 0);
    T_EQ_INT(after.row.moves_len, before.row.moves_len);
    T_CHECK(memcmp(after.row.moves, before.row.moves, before.row.moves_len) == 0);
    T_EQ_INT(after.row.n_players, 2);
    T_EQ_STR(after.row.player_ids[1], UID_B);
    cb_snapshot_row_free(&before.row);
    cb_snapshot_row_free(&after.row);
}

// ---- push order -------------------------------------------------------------------------

// Two commits of one game publish in their commit order. Found as a flake of
// the game smoke under Linux ASan: a bot drive and a human's action, each on
// its own pool thread, committed v10 and v11, and v11's casts went out first.
// Here the first commit is held inside its publish; the second commit must not
// overtake it (with no ordering it commits and publishes meanwhile).
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool armed, arrived, released;
    int n;
    int64_t versions[8];
} g_order = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, false, false, 0, { 0 } };

static void order_observer(const cb_fanout *f) {
    pthread_mutex_lock(&g_order.mu);
    if (g_order.armed) {
        g_order.armed = false;
        g_order.arrived = true;
        pthread_cond_broadcast(&g_order.cv);
        while (!g_order.released) pthread_cond_wait(&g_order.cv, &g_order.mu);
    }
    if (g_order.n < 8) g_order.versions[g_order.n++] = f->version;
    pthread_cond_broadcast(&g_order.cv);
    pthread_mutex_unlock(&g_order.mu);
}

static void *order_move(void *arg) {
    reply r;
    any_human_move(arg, &r);
    return NULL;
}

static void pushes_go_out_in_commit_order(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    cb_fanout_observer = order_observer;
    g_order.n = 0;
    g_order.arrived = g_order.released = false;
    g_order.armed = true;
    pthread_t a, b;
    pthread_create(&a, NULL, order_move, gid);
    pthread_mutex_lock(&g_order.mu);
    while (!g_order.arrived) pthread_cond_wait(&g_order.cv, &g_order.mu);
    pthread_mutex_unlock(&g_order.mu);
    // The first move is committed and held in its publish. The next one, by
    // whoever has a move now, starts.
    int64_t first = version_of(gid);
    pthread_create(&b, NULL, order_move, gid);
    // Give it the time it needs to commit and publish if nothing stops it.
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_nsec += 300 * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_order.mu);
    while (g_order.n == 0 && pthread_cond_timedwait(&g_order.cv, &g_order.mu, &dl) == 0) {}
    g_order.released = true;
    pthread_cond_broadcast(&g_order.cv);
    pthread_mutex_unlock(&g_order.mu);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    T_EQ_INT(g_order.n, 2);
    T_EQ_INT(g_order.versions[0], first);
    T_CHECK(g_order.versions[1] > g_order.versions[0]);
    cb_fanout_observer = fan_observer;
}

// ---- ported from the TypeScript e2e (stage 4) ----------------------------------------------

// create_game_id.test.ts: create never hands out a game it did not store. A
// draw can name a stored game: the store refuses it, and create draws again,
// CB_TABLE_ID_DRAWS times.
static char g_taken[CB_GAME_ID_MAX + 1];
static int g_taken_draws, g_draws;
static void colliding_id(char out[CB_GAME_ID_MAX + 1]) {
    if (g_draws++ < g_taken_draws) snprintf(out, CB_GAME_ID_MAX + 1, "%s", g_taken);
    else snprintf(out, CB_GAME_ID_MAX + 1, "fedcba987654");
}

// A game id is CB_GAME_ID_HEX lowercase hex (48 bits, finding L3), fresh
// every draw; a six-character id from before still names its game.
static void game_ids_are_wide_and_old_ones_work(void) {
    setup(0);
    char seen[64][CB_GAME_ID_MAX + 1];
    for (int i = 0; i < 64; i++) {
        cb_game_id_random(seen[i]);
        T_EQ_INT(strlen(seen[i]), CB_GAME_ID_HEX);
        for (const char *p = seen[i]; *p; p++) T_CHECK((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'));
        for (int j = 0; j < i; j++) T_CHECK(strcmp(seen[i], seen[j]) != 0);
    }
    reply r;
    char gid[CB_GAME_ID_MAX + 1];
    create_game(gid, &r);
    T_EQ_INT(r.status, 200);
    T_EQ_INT(strlen(gid), CB_GAME_ID_HEX);
    // An old six-character game: made under the old width, it still loads and takes asks.
    g_draws = 0;
    g_taken_draws = 0;
    cb_app_game_id = colliding_id;
    snprintf(g_taken, sizeof g_taken, "abc123");
    g_taken_draws = 1;
    call("POST", "create", "C", NULL, NULL, 0, &r);
    T_EQ_INT(r.status, 200);
    cb_view_row v;
    T_EQ_INT(cb_table_view(&g_ctx, "abc123", UID_C, &v), CB_OK);
    cb_view_row_free(&v);
    meta("D", ASK_JOIN, "abc123", NULL, &r);
    T_EQ_INT(r.status, 200);
    cb_app_game_id = cb_game_id_random;
}

static int games_of(const char *uid) {
    char gid[CB_GAME_ID_MAX + 1] = "";
    return cb_table_views_of_user(&g_ctx, uid, 100, first_game, gid);
}

static void a_taken_game_id_is_drawn_again(void) {
    setup(0);
    reply r;
    create_game(g_taken, &r);
    T_EQ_INT(r.status, 200);
    // Two draws name A's game, the third is free: B gets the free one.
    cb_app_game_id = colliding_id;
    g_draws = 0;
    g_taken_draws = 2;
    call("POST", "create", "B", NULL, NULL, 0, &r);
    T_EQ_INT(r.status, 200);
    T_EQ_INT(g_draws, 3);
    char gid[CB_GAME_ID_MAX + 1];
    cb_table_views_of_user(&g_ctx, UID_B, 1, first_game, gid);
    T_EQ_STR(gid, "fedcba987654");
    // The answer names the stored game, and A's game is untouched.
    cb_view_row v;
    T_EQ_INT(cb_table_view(&g_ctx, "fedcba987654", UID_B, &v), CB_OK);
    T_EQ_MEM(r.body, r.len, v.view, v.view_len);
    cb_view_row_free(&v);
    T_EQ_INT(CB_STORE_CALL(g_store, is_member, g_taken, UID_B), 0);
    T_EQ_INT(version_of(g_taken), 0);
    // Every draw taken: a failure, and nothing stored for C.
    g_draws = 0;
    g_taken_draws = 1000;
    call("POST", "create", "C", NULL, NULL, 0, &r);
    T_CHECK(r.status != 200);
    T_EQ_INT(g_draws, CB_TABLE_ID_DRAWS);
    T_EQ_INT(games_of(UID_C), 0);
}

// tests.c test_table_a_bots_good_is_a_move: an operation whose only effect is a Good (no card
// event) still goes out, or an open page never sees the badge.
static void a_goods_only_commit_is_broadcast(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("B", ASK_JOIN, gid, NULL, &r);
    meta("C", ASK_JOIN, gid, NULL, &r);
    meta("A", ASK_START, gid, NULL, &r);
    meta("B", ASK_START, gid, NULL, &r);
    meta("C", ASK_START, gid, NULL, &r);
    T_EQ_INT(status_of(gid), GAME_STATUS_PLAYING);
    static const char *const uids[3] = { UID_A, UID_B, UID_C }, *const bearers[3] = { "A", "B", "C" };
    cb_section *s = cb_section_new();
    int found = 0;
    for (int step = 0; step < 4000 && found < 3 && status_of(gid) == GAME_STATUS_PLAYING; step++) {
        int k = step % 3;
        int seat = seat_of(gid, uids[k]);
        uint8_t wire[64];
        int n = pick_move(gid, seat, wire, sizeof wire);
        if (n <= 0) continue;
        // What the kernel will make of it, on a scratch copy of the stored row.
        cb_game_row row = stored(gid);
        bool goods_only = false;
        if (cb_section_load(s, &row) >= 0 &&
            table_act(&s->t, uids[k], (int)strlen(uids[k]), wire, n, -1, row.round_epoch) == TABLE_OK &&
            table_commit_products(&s->t, gid, (int)strlen(gid), (uint32_t)row.version + 1, g_now, &s->c, s->arena,
                                  sizeof s->arena) >= 0)
            goods_only = s->c.n_events == 0 && s->c.goods_changed && !s->c.roster_changed;
        int64_t v = row.version;
        cb_game_row_free(&row);
        int fans = g_fan.count;
        action(bearers[k], gid, wire, n, (uint32_t)v, &r);
        if (!goods_only || version_of(gid) != v + 1) continue;
        found++;
        T_EQ_INT(g_fan.count, fans + 1);
        T_EQ_INT(g_fan.version, v + 1);
        T_CHECK(g_fan.casts > 0 && g_fan.spectator_cast);
    }
    cb_section_free(s);
    T_CHECK(found > 0);
}

// What the kernel makes of an operation on a scratch copy of the stored row:
// whether its products have nothing a viewer is shown (no event, no shown
// good, no roster change). These are the commits a "cast only when something
// moved" rule leaves without a cast.
static bool shows_nothing(cb_section *s, const char *gid, const char *uid, const uint8_t *wire, int n,
                          const TableAsk *ask) {
    cb_game_row row = stored(gid);
    bool quiet = false;
    if (cb_section_load(s, &row) >= 0) {
        int rc = ask ? table_ask(&s->t, uid, (int)strlen(uid), "X", 1, ask, NULL, NULL)
                     : table_act(&s->t, uid, (int)strlen(uid), wire, n, -1, row.round_epoch);
        if (rc == TABLE_OK &&
            table_commit_products(&s->t, gid, (int)strlen(gid), (uint32_t)row.version + 1, g_now, &s->c, s->arena,
                                  sizeof s->arena) >= 0)
            quiet = s->c.n_events == 0 && !s->c.goods_changed && !s->c.roster_changed;
    }
    cb_game_row_free(&row);
    return quiet;
}

// The seat's hand, reversed, as an ASK_REARRANGE_HAND; false when it holds
// fewer than two cards (a reversal would be the same order).
static bool reversed_hand_ask(const char *gid, int seat, TableAsk *a) {
    cb_game_row row = stored(gid);
    Game *g = calloc(1, sizeof *g);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    Table t;
    table_init(&t, g, snaps);
    int n = 0;
    if (table_load(&t, row.state, (int)row.state_len, row.roster, (int)row.roster_len) == TABLE_OK) n = g->players[seat].hand_count;
    free(g);
    free(snaps);
    cb_game_row_free(&row);
    memset(a, 0, sizeof *a);
    a->kind = ASK_REARRANGE_HAND;
    a->gid_len = (uint8_t)strlen(gid);
    memcpy(a->gid, gid, a->gid_len);
    a->n_idx = (uint8_t)(n > 0 ? n : 0);
    for (int i = 0; i < n; i++) a->idx[i] = (uint8_t)(n - 1 - i);
    return n >= 2;
}

static void meta_ask(const char *bearer, const TableAsk *a, reply *out) {
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(a, body, sizeof body);
    call("POST", "meta", bearer, OCT, body, n > 0 ? (size_t)n : 0, out);
}

// EVERY COMMIT IS CAST (table_ops.c). A client gates casts on the version and
// fetches the game whole when one is skipped (netclient.c NW_WS_CAST), so a
// commit that moved the version and cast nothing made every open page refetch
// at its next push: a third of each client's requests in a two-player game at
// full speed. The commits that slipped through were the ones that show nothing:
// a good said over an uncovered table (game_shown_good_mask) and a hand
// rearranged. A whole two-player game is played here through the HTTP
// surface, and every version it commits must reach both seats and the
// spectators as a cast at that version, the quiet ones by name included.
static void every_commit_is_cast(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("B", ASK_JOIN, gid, NULL, &r);
    meta("A", ASK_START, gid, NULL, &r);
    meta("B", ASK_START, gid, NULL, &r);
    T_EQ_INT(status_of(gid), GAME_STATUS_PLAYING);
    static const char *const uids[2] = { UID_A, UID_B }, *const bearers[2] = { "A", "B" };
    cb_section *s = cb_section_new();
    int commits = 0, quiet_goods = 0, rearranged = 0;
    for (int step = 0; step < 4000 && status_of(gid) == GAME_STATUS_PLAYING; step++) {
        const int k = step % 2, seat = seat_of(gid, uids[k]);
        const int64_t v = version_of(gid);
        const int fans = g_fan.count;
        bool quiet;
        TableAsk ask;
        if (step % 7 == 3 && reversed_hand_ask(gid, seat, &ask)) {
            quiet = shows_nothing(s, gid, uids[k], NULL, 0, &ask);
            meta_ask(bearers[k], &ask, &r);
            if (version_of(gid) == v + 1) rearranged += quiet;
        } else {
            uint8_t wire[64];
            int n = pick_move(gid, seat, wire, sizeof wire);
            if (n <= 0) continue;
            quiet = shows_nothing(s, gid, uids[k], wire, n, NULL);
            action(bearers[k], gid, wire, n, (uint32_t)v, &r);
            if (version_of(gid) == v + 1) quiet_goods += quiet && AWIRE_GOOD == wire[0];
        }
        if (version_of(gid) != v + 1) continue;
        commits++;
        T_EQ_INT(g_fan.count, fans + 1);
        T_EQ_INT(g_fan.version, v + 1);
        T_EQ_INT(g_fan.humans, 2);
        T_EQ_INT(g_fan.casts, 2);
        T_CHECK(g_fan.spectator_cast);
    }
    cb_section_free(s);
    T_EQ_INT(status_of(gid), GAME_STATUS_GAME_OVER);
    T_CHECK(commits > 20);
    // The game must have held the commits the old rule left uncast, or this
    // proves nothing about them.
    T_CHECK(quiet_goods > 0);
    T_CHECK(rearranged > 0);
}

// ---- answer first (app/after.h) ---------------------------------------------------------------

// Under Lambda an HTTP invocation answers before it publishes: with the window
// open, the commits' pushes and the bots' kicks wait for cb_after_run, then
// go out in the order they were made, the pushes in version order.
static void the_answer_goes_before_its_pushes(void) {
    setup(1);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    int64_t v0 = version_of(gid);
    g_nseq = 0;
    cb_after_begin();
    meta("A", ASK_ADD_BOT, gid, NULL, &r);   // commits v0+1
    T_EQ_INT(r.status, 200);
    meta("A", ASK_START, gid, NULL, &r);     // commits v0+2, a kick when a bot is to act
    T_EQ_INT(r.status, 200);
    meta("A", ASK_BUMP, gid, NULL, &r);      // commits nothing, a kick when a bot is to act
    T_EQ_INT(r.status, 200);
    T_EQ_INT(version_of(gid), v0 + 2);
    const bool due = bots_due(gid);   // the deal decides who opens
    // Answered, and nothing has left yet.
    T_EQ_INT(g_nseq, 0);
    T_CHECK(cb_after_open());
    cb_after_run(&g_ctx);
    T_CHECK(!cb_after_open());
    // The two commits' pushes in version order, the bump's kick last.
    int64_t last = v0;
    int fans = 0, kicks = 0;
    for (int i = 0; i < g_nseq; i++) {
        if (g_seq[i] < 0) { kicks++; continue; }
        T_EQ_INT(g_seq[i], last + 1);
        last = g_seq[i];
        fans++;
    }
    T_EQ_INT(fans, 2);
    T_EQ_INT(kicks, due ? 2 : 0);
    T_CHECK(g_nseq > 0 && (g_seq[g_nseq - 1] == -1) == due);
    // Run again: nothing is left to do, and with the window closed a commit
    // publishes at once, as a bot drive or `cbase serve` does.
    g_nseq = 0;
    cb_after_run(&g_ctx);
    T_EQ_INT(g_nseq, 0);
    T_CHECK(until_a_bot_is_due(gid));
    g_nseq = 0;
    meta("A", ASK_BUMP, gid, NULL, &r);
    T_EQ_INT(g_nseq, 1);
    T_EQ_INT(g_seq[0], -1);
}

// A transport that reaches nobody: every delivery fails.
static int g_failed_deliveries;
static int failing_deliver(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len) {
    g_failed_deliveries++;
    return 1;
}
static const cb_rt_transport FAILING_T = { .name = "failing", .deliver = failing_deliver };

// Once the answer is out a failed push cannot fail the invocation any more:
// it is logged at WARN with its game and version.
static void a_push_that_fails_after_the_answer_is_logged(void) {
    setup(1);
    cb_log_init(CB_LOG_WARN, 0);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    cb_rt rt = { &FAILING_T, &g_ctx, NULL, NULL };
    g_ctx.rt = &rt;
    g_failed_deliveries = 0;
    uint64_t seq = cb_log_last_seq();
    cb_after_begin();
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    T_EQ_INT(r.status, 200);
    int64_t v = version_of(gid);
    T_EQ_INT(g_failed_deliveries, 0);
    cb_after_run(&g_ctx);
    T_CHECK(g_failed_deliveries > 0);
    cb_log_entry e[16];
    size_t n = cb_log_read(seq, CB_LOG_WARN, e, 16);
    char want[96];
    snprintf(want, sizeof want, "game %s v%lld: %d push(es) failed after the answer was sent", gid, (long long)v,
             g_failed_deliveries);
    bool found = false;
    for (size_t i = 0; i < n; i++) found |= strcmp(e[i].module, "after") == 0 && strcmp(e[i].msg, want) == 0;
    T_CHECK(found);
    g_ctx.rt = NULL;
}

// heartbeat_needs_bots.test.ts: a bump wakes the bots when one can act, and
// only then: a bump on a table waiting on its human would start a drive that
// finds nothing (every page load of a game against a bot did).
static void a_bump_wakes_the_bots(void) {
    setup(1);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    meta("A", ASK_START, gid, NULL, &r);
    T_EQ_INT(status_of(gid), GAME_STATUS_PLAYING);
    T_CHECK(until_the_human_owes(gid));
    int kicks = g_kicks;
    meta("A", ASK_BUMP, gid, NULL, &r);
    T_EQ_INT(r.status, 200);
    T_EQ_INT(g_kicks, kicks);
    T_CHECK(until_a_bot_is_due(gid));
    kicks = g_kicks;
    int64_t v = version_of(gid);
    meta("A", ASK_BUMP, gid, NULL, &r);
    T_EQ_INT(r.status, 200);
    T_EQ_INT(g_kicks, kicks + 1);
    T_EQ_STR(g_kicked, gid);
    T_EQ_INT(version_of(gid), v);
    // A stranger's bump is refused and wakes nothing (security finding M8:
    // each kick is a Lambda run; the minute's heartbeat scan still wakes a
    // table nobody seated is watching).
    meta("D", ASK_BUMP, gid, NULL, &r);
    T_EQ_INT(r.status, 400);
    T_EQ_INT(g_kicks, kicks + 1);
}

// waiting_stale_blob.test.ts and meta.test.ts: continue takes a finished game
// back to a lobby that carries no seed and no log, and that deals afresh.
static void continue_returns_a_clean_lobby(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    reply r;
    for (int i = 0; i < 3000 && status_of(gid) == GAME_STATUS_PLAYING; i++)
        if (!any_human_move(gid, &r)) break;
    T_EQ_INT(status_of(gid), GAME_STATUS_GAME_OVER);
    // A stranger may not continue it.
    int64_t v = version_of(gid);
    meta("D", ASK_CONTINUE, gid, NULL, &r);
    T_EQ_INT(r.status, 400);
    T_EQ_INT(version_of(gid), v);
    meta("A", ASK_CONTINUE, gid, NULL, &r);
    T_EQ_INT(r.status, 200);
    cb_game_row row = stored(gid);
    T_EQ_INT(row.status, GAME_STATUS_WAITING);
    T_CHECK(!row.has_seed);
    T_EQ_INT(row.logs_len, 0);
    T_EQ_INT(row.version, v + 1);
    cb_game_row_free(&row);
    meta("A", ASK_START, gid, NULL, &r);
    meta("B", ASK_START, gid, NULL, &r);
    row = stored(gid);
    T_EQ_INT(row.status, GAME_STATUS_PLAYING);
    T_CHECK(row.has_seed);
    T_CHECK(row.logs_len > 0);
    cb_game_row_free(&row);
    T_CHECK(any_human_move(gid, &r));
    TableResponse resp;
    T_EQ_INT(table_response_decode(r.body, (int)r.len, &resp), 0);
    T_EQ_INT(resp.status, TABLE_STATUS_APPLIED);
}

// security_seat_from_auth.test.ts, table_server_seat.test.ts: the seat a request
// acts as is the bearer's, so a stranger's action and lobby edits commit nothing.
static void a_stranger_changes_nothing(void) {
    setup(1);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("B", ASK_JOIN, gid, NULL, &r);
    int commits = tm_hooks_of(g_store)->commits;
    int64_t v = version_of(gid);
    static const int kinds[] = { ASK_ADD_BOT, ASK_EXIT, ASK_START, ASK_UPDATE_NAME, ASK_REARRANGE_PLAYERS, ASK_SET_RULES };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        meta("D", kinds[i], gid, NULL, &r);
        T_EQ_INT(r.status, 400);
        T_EQ_INT(version_of(gid), v);
    }
    T_EQ_INT(tm_hooks_of(g_store)->commits, commits);
    meta("A", ASK_START, gid, NULL, &r);
    meta("B", ASK_START, gid, NULL, &r);
    T_EQ_INT(status_of(gid), GAME_STATUS_PLAYING);
    // Each seated player's own legal move, sent by the stranger: refused.
    commits = tm_hooks_of(g_store)->commits;
    v = version_of(gid);
    for (int seat = 0; seat < 2; seat++) {
        uint8_t wire[64];
        int n = pick_move(gid, seat, wire, sizeof wire);
        if (n <= 0) continue;
        action("D", gid, wire, n, (uint32_t)v, &r);
        T_EQ_INT(r.status, 400);
        T_EQ_INT(refusal_why(&r, NULL), ASK_WHY_REFUSED);
    }
    meta("D", ASK_EXIT, gid, NULL, &r);
    T_EQ_INT(r.status, 400);
    meta("D", ASK_REARRANGE_HAND, gid, NULL, &r);
    T_EQ_INT(r.status, 400);
    T_EQ_INT(version_of(gid), v);
    T_EQ_INT(tm_hooks_of(g_store)->commits, commits);
}

// adversarial_ts_layer.test.ts, concurrent_games.test.ts: both players submit
// moves from their own threads at once, on stale versions as often as not.
// Every APPLIED answer is exactly one stored version, and the board stays one
// the kernel loads.
typedef struct { const char *gid, *uid, *bearer; int applied; } hammer_arg;
static void *hammer(void *p) {
    hammer_arg *a = p;
    g_rng = (uint64_t)(uintptr_t)a->uid;
    for (int i = 0; i < 150; i++) {
        int seat = seat_of(a->gid, a->uid);
        uint8_t wire[64];
        int n = seat >= 0 ? pick_move(a->gid, seat, wire, sizeof wire) : 0;
        if (n <= 0) continue;
        reply r;
        action(a->bearer, a->gid, wire, n, (uint32_t)version_of(a->gid), &r);
        TableResponse resp;
        if (r.status == 200 && table_response_decode(r.body, (int)r.len, &resp) == 0 &&
            resp.status == TABLE_STATUS_APPLIED)
            a->applied++;
    }
    return NULL;
}

static void concurrent_moves_lose_and_duplicate_nothing(void) {
    setup(0);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    cb_fanout_observer = NULL;   // g_fan is one struct; two threads would tear it
    int64_t v0 = version_of(gid);
    hammer_arg a = { gid, UID_A, "A", 0 }, b = { gid, UID_B, "B", 0 };
    pthread_t ta, tb;
    pthread_create(&ta, NULL, hammer, &a);
    pthread_create(&tb, NULL, hammer, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    cb_fanout_observer = fan_observer;
    T_CHECK(a.applied + b.applied > 0);
    T_EQ_INT(version_of(gid), v0 + a.applied + b.applied);
    cb_game_row row = stored(gid);
    cb_section *s = cb_section_new();
    T_CHECK(cb_section_load(s, &row) >= 0);   // table_load validates the board (state_import)
    cb_section_free(s);
    cb_game_row_free(&row);
}

// security_hidden_info_{2p,4p,6p}.test.ts: every envelope goes to the user
// whose seat it was cut for, and is exactly that seat's view; every HTTP
// answer is the asker's seat or the spectators'.
static int g_hidden_bad, g_hidden_seen;
static const char *g_hidden_gid;
static void hidden_observer(const cb_fanout *f) {
    fan_observer(f);
    for (int i = 0; i < f->n_seats; i++) {
        if (!f->seats[i].envelope) continue;
        g_hidden_seen++;
        EnvHeader h;
        if (!f->seats[i].user_id[0] || !env_header_read(f->seats[i].envelope, (int)f->seats[i].envelope_len, &h)) {
            g_hidden_bad++;
            continue;
        }
        if (h.seat != seat_of(f->game_id, f->seats[i].user_id)) { g_hidden_bad++; continue; }
        cb_view_row v;
        if (cb_table_view(&g_ctx, f->game_id, f->seats[i].user_id, &v) != CB_OK) { g_hidden_bad++; continue; }
        if (v.view_len != f->seats[i].envelope_len || memcmp(v.view, f->seats[i].envelope, v.view_len) != 0) g_hidden_bad++;
        cb_view_row_free(&v);
    }
}

static void hidden_info_at(int n_humans, int n_bots) {
    static const char *const uids[3] = { UID_A, UID_B, UID_C }, *const bearers[3] = { "A", "B", "C" };
    static const char *const asker_ids[4] = { UID_A, UID_B, UID_C, UID_D };
    setup(n_bots);
    cb_fanout_observer = hidden_observer;
    g_hidden_bad = g_hidden_seen = 0;
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    g_hidden_gid = gid;
    // Seats interleaved: a human, a bot, a human, ...
    int humans = 1, bots = 0;
    for (int i = 1; i < n_humans + n_bots; i++) {
        if ((i % 2 == 1 && bots < n_bots) || humans == n_humans) { meta("A", ASK_ADD_BOT, gid, NULL, &r); bots++; }
        else meta(bearers[humans++], ASK_JOIN, gid, NULL, &r);
        T_EQ_INT(r.status, 200);
    }
    for (int k = 0; k < n_humans; k++) meta(bearers[k], ASK_START, gid, NULL, &r);
    T_EQ_INT(status_of(gid), GAME_STATUS_PLAYING);
    int steps = 0;
    while (status_of(gid) == GAME_STATUS_PLAYING && steps++ < 3000) {
        bool moved = false;
        for (int k = 0; k < n_humans && !moved; k++) {
            int seat = seat_of(gid, uids[k]);
            uint8_t wire[64];
            int n = seat >= 0 ? pick_move(gid, seat, wire, sizeof wire) : 0;
            if (n <= 0) continue;
            action(bearers[k], gid, wire, n, (uint32_t)version_of(gid), &r);
            moved = true;
        }
        if (!moved) T_EQ_INT(cb_bots_drive(&g_ctx, gid), CB_OK);
        // Every asker's view (the read the view routes make): their own
        // seat, or the spectators' (-1). Read directly, not by a bump: a bump
        // is a seated player's only, and limited (limits.h).
        for (int k = 0; k < 4; k++) {
            int seat = seat_of(gid, asker_ids[k]);
            cb_view_row v;
            EnvHeader h;
            if (cb_table_view(&g_ctx, gid, seat >= 0 ? asker_ids[k] : NULL, &v) != CB_OK) { g_hidden_bad++; continue; }
            if (!env_header_read(v.view, (int)v.view_len, &h) || h.seat != seat) g_hidden_bad++;
            cb_view_row_free(&v);
        }
    }
    T_EQ_INT(status_of(gid), GAME_STATUS_GAME_OVER);
    T_CHECK(g_hidden_seen > 0);
    T_EQ_INT(g_hidden_bad, 0);
    cb_fanout_observer = fan_observer;
}

static void every_view_goes_to_its_own_seat(void) {
    hidden_info_at(2, 0);
    hidden_info_at(2, 2);
    hidden_info_at(3, 3);
}

// belief_logs_wiring.test.ts, belief_human_freshness.test.ts: every bot cycle
// is handed the stored session log whole (octogen reads it), fresh after a
// human's commit; resilience.test.ts: a cycle that lost its CAS offers back the
// moves it already chose.
static struct { int calls, short_log, retries_with_prefs, retries; } g_cyc;
static void cycle_observer(const char *game_id, int attempt, int logs_len, int prefs_len) {
    g_cyc.calls++;
    cb_game_row row = stored(game_id);
    if (logs_len <= 0 || (size_t)logs_len != row.logs_len) g_cyc.short_log++;
    cb_game_row_free(&row);
    if (attempt > 1) {
        g_cyc.retries++;
        if (prefs_len > 0) g_cyc.retries_with_prefs++;
    }
}

static void every_bot_cycle_gets_the_whole_log_and_its_choices_back(void) {
    setup(1);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    meta("A", ASK_START, gid, NULL, &r);
    memset(&g_cyc, 0, sizeof g_cyc);
    cb_bots_cycle_observer = cycle_observer;
    tm_hooks *hk = tm_hooks_of(g_store);
    int steals = 0;
    for (int i = 0; i < 3000 && status_of(gid) == GAME_STATUS_PLAYING; i++) {
        uint8_t wire[64];
        int n = pick_move(gid, 0, wire, sizeof wire);
        if (n > 0) { action("A", gid, wire, n, (uint32_t)version_of(gid), &r); continue; }
        // Now and then the drive's first commit loses to "another writer".
        if (steals < 5) { hk->steal_commits = 1; steals++; }
        T_EQ_INT(cb_bots_drive(&g_ctx, gid), CB_OK);
        hk->steal_commits = 0;
    }
    cb_bots_cycle_observer = NULL;
    T_EQ_INT(status_of(gid), GAME_STATUS_GAME_OVER);
    T_CHECK(g_cyc.calls > 0);
    T_EQ_INT(g_cyc.short_log, 0);
    T_CHECK(g_cyc.retries > 0);
    T_EQ_INT(g_cyc.retries_with_prefs, g_cyc.retries);
}

// web_bot_first_move_pace.test.ts, web_throwin_vs_bot.test.ts,
// win_screen_rating_is_the_games.test.ts: a bot move lands no sooner than the
// kernel's wait after the commit before it (the deal's settle and the pace, or
// a human's move still playing), and a game the bot loop ends is rated once.
static struct { int n; int64_t at[4096]; int64_t version[4096]; } g_pace;
static void pace_observer(const cb_fanout *f) {
    fan_observer(f);
    if (g_pace.n < 4096) { g_pace.at[g_pace.n] = g_now; g_pace.version[g_pace.n] = f->version; g_pace.n++; }
}

static void bot_moves_wait_for_the_table_to_settle(void) {
    setup(1);
    uint64_t board0 = cb_edge_marks(CB_EDGE_BOARD);   // a game the bots end is marked once too
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    memset(&g_pace, 0, sizeof g_pace);
    cb_fanout_observer = pace_observer;
    meta("A", ASK_START, gid, NULL, &r);
    T_EQ_INT(status_of(gid), GAME_STATUS_PLAYING);
    cb_section *s = cb_section_new();
    int drives = 0, waited = 0;
    for (int i = 0; i < 3000 && status_of(gid) == GAME_STATUS_PLAYING; i++) {
        uint8_t wire[64];
        int n = pick_move(gid, 0, wire, sizeof wire);
        if (n > 0) { action("A", gid, wire, n, (uint32_t)version_of(gid), &r); continue; }
        // The kernel's wait as of the last commit, which happened at g_now.
        cb_game_row row = stored(gid);
        int wait = cb_section_load(s, &row) >= 0 ? table_bot_wait_ms(&s->t, g_now) : -1;
        int64_t v = row.version, t_last = g_now;
        cb_game_row_free(&row);
        int before = g_pace.n;
        T_EQ_INT(cb_bots_drive(&g_ctx, gid), CB_OK);
        drives++;
        // The drive's first commit.
        for (int k = before; k < g_pace.n; k++) {
            if (g_pace.version[k] != v + 1) continue;
            T_CHECK(g_pace.at[k] >= t_last + (wait > 0 ? wait : 0));
            if (wait >= BOT_PACE_MS_WITH_HUMANS) waited++;
            break;
        }
    }
    cb_section_free(s);
    cb_fanout_observer = fan_observer;
    T_CHECK(drives > 0);
    T_CHECK(waited > 0);
    T_EQ_INT(status_of(gid), GAME_STATUS_GAME_OVER);
    cb_rating_row ra, rb;
    T_EQ_INT(tm_rating(g_store, UID_A, &ra), CB_OK);
    T_EQ_INT(tm_rating(g_store, tm_bot_id(g_store, 0), &rb), CB_OK);
    T_EQ_INT(ra.games_played, 1);
    T_EQ_INT(rb.games_played, 1);
    // A drive after the end rates nobody again: a finished game is out of the
    // scan, so there is no lease to take.
    T_EQ_INT(cb_bots_drive(&g_ctx, gid), CB_E_NOT_FOUND);
    T_EQ_INT(tm_rating(g_store, UID_A, &ra), CB_OK);
    T_EQ_INT(ra.games_played, 1);
    T_EQ_INT((int)(cb_edge_marks(CB_EDGE_BOARD) - board0), 1);
}

// ---- bots --------------------------------------------------------------------------------------

static void the_bot_loop_drives_until_a_human_owes_a_move(void) {
    setup(2);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
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
            // One drive runs cycle after cycle: it returns when a human owes
            // the next move (or the game is over), not after one bot's move.
            T_CHECK(status_of(gid) != GAME_STATUS_PLAYING || pick_move(gid, 0, wire, sizeof wire) > 0);
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
    // The snapshot, whenever the kernel can encode this session (log_encodable).
    T_EQ_INT(tm_hooks_of(g_store)->snapshots, log_encodable(gid) ? 1 : 0);
}

static void a_held_lease_skips_the_drive(void) {
    setup(1);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    meta("A", ASK_START, gid, NULL, &r);
    T_CHECK(until_a_bot_is_due(gid));
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

// Production, 2026-10-17: 1,037 of 1,149 bot cycles in a week drove nothing.
// The row said "needs bots" whenever a bot was IN, so a game whose human sat
// on a move was kicked by every minute's scan for the whole abandon hour, and
// each drive found no bot could act. The row now says what the kernel says
// (table_bots_due), so the scan never lists such a game.
static void the_scan_never_kicks_a_game_waiting_on_its_human(void) {
    setup(1);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    meta("A", ASK_START, gid, NULL, &r);
    const int idle0 = tm_hooks_of(g_store)->bots_idle;
    T_CHECK(until_the_human_owes(gid));
    cb_game_row row = stored(gid);
    T_CHECK(!row.needs_bots);
    cb_game_row_free(&row);
    // The commits wrote that answer themselves: no drive had to correct one.
    T_EQ_INT(tm_hooks_of(g_store)->bots_idle, idle0);
    // The human walks away: every scan of the abandon hour passes it by.
    g_kicks = 0;
    int kicked = 0;
    for (int64_t t = g_now + CB_BOT_STALE_MS + 1; t < g_now + CB_BOT_ABANDON_MS; t += 60000)
        kicked += cb_bots_scan(&g_ctx, t);
    T_EQ_INT(kicked, 0);
    T_EQ_INT(g_kicks, 0);
    // The human moves: a bot can act, the row says so, and a stale row is kicked.
    T_CHECK(until_a_bot_is_due(gid));
    row = stored(gid);
    T_CHECK(row.needs_bots);
    cb_game_row_free(&row);
    T_EQ_INT(cb_bots_scan(&g_ctx, g_now + CB_BOT_STALE_MS + 1), 1);
    T_EQ_STR(g_kicked, gid);
}

// A row the scan holds although no bot can act (written before the row asked
// the kernel's question, as production's were): the first drive that finds
// nothing takes it out of the scan, so the next minute's scan does not kick it
// again; and only at the version it read, so a commit in between wins.
static void a_drive_that_finds_nothing_takes_the_game_out_of_the_scan(void) {
    setup(1);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    meta("A", ASK_START, gid, NULL, &r);
    T_CHECK(until_the_human_owes(gid));
    const int64_t now = g_now + CB_BOT_STALE_MS + 1;
    T_EQ_INT(tm_set_scan(g_store, gid, true, g_now - 1, g_now - 1), CB_OK);
    T_EQ_INT(cb_bots_scan(&g_ctx, now), 1);   // the old row, kicked
    const int64_t v = version_of(gid);
    const int idle0 = tm_hooks_of(g_store)->bots_idle;
    T_EQ_INT(cb_bots_drive(&g_ctx, gid), CB_OK);
    T_EQ_INT(version_of(gid), v);
    T_EQ_INT(tm_hooks_of(g_store)->bots_idle, idle0 + 1);
    cb_game_row row = stored(gid);
    T_CHECK(!row.needs_bots);
    cb_game_row_free(&row);
    T_EQ_INT(cb_bots_scan(&g_ctx, now + 60000), 0);
    // The lease went with it: the next drive has none to take.
    T_EQ_INT(cb_bots_drive(&g_ctx, gid), CB_E_NOT_FOUND);
    // The fence: a commit after the read leaves its own answer alone.
    T_EQ_INT(tm_set_scan(g_store, gid, true, g_now - 1, g_now - 1), CB_OK);
    T_EQ_INT(CB_STORE_CALL(g_store, bots_idle, gid, v - 1), CB_E_CONFLICT);
    row = stored(gid);
    T_CHECK(row.needs_bots);
    cb_game_row_free(&row);
}

// Production, 2026-10-17: about one drive per human move in a game against a
// bot started only to find the lease taken (24 of 34 drives in one game): the
// human moved while a drive was waiting out its pace. A kick now looks at the
// lease after its commit and leaves the move to the drive that holds it.
static void a_kick_leaves_the_move_to_the_drive_holding_the_lease(void) {
    setup(1);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    meta("A", ASK_START, gid, NULL, &r);
    T_CHECK(until_the_human_owes(gid));
    T_EQ_INT(tm_hold_lease(g_store, gid, g_now + CB_BOT_LEASE_MS), CB_OK);   // a drive elsewhere
    int kicks = g_kicks;
    T_CHECK(until_a_bot_is_due(gid));
    T_EQ_INT(g_kicks, kicks);
    meta("A", ASK_BUMP, gid, NULL, &r);
    T_EQ_INT(g_kicks, kicks);
    // The drive let go (or its lease ran out): a kick goes again.
    T_EQ_INT(tm_hold_lease(g_store, gid, g_now - 1), CB_OK);
    meta("A", ASK_BUMP, gid, NULL, &r);
    T_EQ_INT(g_kicks, kicks + 1);
}

// The other half: the drive that held the lease looks once more after it lets
// go, so a move whose kick it swallowed is driven by it, not left to the scan.
static const char *g_release_gid;
static int g_release_moves;
static int64_t g_release_version;
static bool g_release_left_due;
static void move_at_release(const char *game_id) {
    if (!g_release_gid || strcmp(game_id, g_release_gid) != 0 || g_release_moves > 0) return;
    if (bots_due(game_id)) return;   // a release mid-drive: not the case under test
    uint8_t w[64];
    int n = pick_move(game_id, 0, w, sizeof w);
    if (n <= 0) return;
    reply r;
    action("A", game_id, w, n, (uint32_t)version_of(game_id), &r);
    g_release_moves++;
    g_release_version = version_of(game_id);
    g_release_left_due = bots_due(game_id);
}

static void a_move_made_while_the_lease_is_held_is_driven(void) {
    setup(1);
    char gid[CB_GAME_ID_MAX + 1];
    reply r;
    create_game(gid, &r);
    meta("A", ASK_ADD_BOT, gid, NULL, &r);
    meta("A", ASK_START, gid, NULL, &r);
    bool tested = false;
    for (int i = 0; i < 40 && !tested && status_of(gid) == GAME_STATUS_PLAYING; i++) {
        T_CHECK(until_a_bot_is_due(gid));
        g_release_gid = gid;
        g_release_moves = 0;
        tm_on_release = move_at_release;
        int kicks = g_kicks;
        T_EQ_INT(cb_bots_drive(&g_ctx, gid), CB_OK);
        tm_on_release = NULL;
        if (g_release_moves == 0 || !g_release_left_due) continue;
        // A's move left a bot to act, its kick was left to the drive, and the
        // drive drove it before returning.
        tested = true;
        T_EQ_INT(g_kicks, kicks);
        T_CHECK(version_of(gid) > g_release_version);
        T_CHECK(!bots_due(gid));
    }
    T_CHECK(tested);
}

int main(void) {
    T_RUN(create_answers_the_seat_0_envelope);
    T_RUN(the_answer_goes_before_its_pushes);
    T_RUN(a_push_that_fails_after_the_answer_is_logged);
    T_RUN(join_add_bot_and_start);
    T_RUN(a_legal_action_applies_and_bumps_the_version);
    T_RUN(an_illegal_action_is_rejected_and_commits_nothing);
    T_RUN(a_stale_intent_is_stale_round);
    T_RUN(a_lost_cas_reloads_and_wins);
    T_RUN(a_refusal_from_a_stale_cache_is_rechecked_fresh);
    T_RUN(a_finished_game_writes_a_snapshot_and_ratings);
    T_RUN(a_new_account_plays_unrated);
    T_RUN(add_bot_never_picks_a_seated_bot);
    T_RUN(bump_is_moot_and_seated_only);
    T_RUN(create_is_limited);
    T_RUN(the_wire_surface);
    T_RUN(the_last_seat_leaving_deletes_the_game);
    T_RUN(delete_account_redacts);
    T_RUN(delete_account_casts_the_rename_to_open_pages);
    T_RUN(delete_account_anonymizes_stored_replays);
    T_RUN(pushes_go_out_in_commit_order);
    T_RUN(the_bot_loop_drives_until_a_human_owes_a_move);
    T_RUN(a_held_lease_skips_the_drive);
    T_RUN(the_scan_kicks_stale_active_bot_games);
    T_RUN(the_scan_never_kicks_a_game_waiting_on_its_human);
    T_RUN(a_drive_that_finds_nothing_takes_the_game_out_of_the_scan);
    T_RUN(a_kick_leaves_the_move_to_the_drive_holding_the_lease);
    T_RUN(a_move_made_while_the_lease_is_held_is_driven);
    T_RUN(a_taken_game_id_is_drawn_again);
    T_RUN(game_ids_are_wide_and_old_ones_work);
    T_RUN(a_goods_only_commit_is_broadcast);
    T_RUN(every_commit_is_cast);
    T_RUN(a_bump_wakes_the_bots);
    T_RUN(continue_returns_a_clean_lobby);
    T_RUN(a_stranger_changes_nothing);
    T_RUN(concurrent_moves_lose_and_duplicate_nothing);
    T_RUN(every_view_goes_to_its_own_seat);
    T_RUN(every_bot_cycle_gets_the_whole_log_and_its_choices_back);
    T_RUN(bot_moves_wait_for_the_table_to_settle);
    cb_row_cache_clear();
    cb_router_free(g_ctx.router);
    cb_store_close(g_store);
    return T_DONE();
}
