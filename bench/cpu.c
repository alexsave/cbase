// cpu.c - `cbase-bench cpu`: what one action costs in CPU, with no network.
//
// Every fixture is made here, from a real game the kernel plays in-process
// (two humans, dealt, a few moves in, so the state carries a session log):
//
//   event_decode       a Function URL event for POST /v1/action, as
//                      AWS would deliver the browser's request (its headers,
//                      a minted access token, the binary body as base64), into
//                      a cb_http_req (src/aws/event.c)
//   event_encode       the action's 7-byte answer back into the response JSON
//   dynamo_get_parse   a GetItem answer for the game row (the row the kernel
//                      just committed, as json_surface.c writes it) read back into
//                      a cb_game_row: the JSON scan and the base64 of the blobs
//   dynamo_commit_build the commit's UpdateItem body (the CAS on the row,
//                      store/rows_foolish.c cb_rows_cas_write, as a move
//                      between humans sends it)
//   view_push          one player's view push: the stream's NW_WS_VIEW frame
//                      around the envelope (realtime.c rt_publish_view), then
//                      the MQTT PUBLISH the hub sends it as
//   b64_encode_2k,     a 2 KB envelope through base64 each way, the cost a
//   b64_decode_2k      binary body pays in the Lambda envelope
//   jwt_verify         the access token every request carries
//   bcrypt_verify      cost 10: SIGN-IN ONLY, never on a move; for scale
//   kernel_action      the section itself: table_load, table_set_deal_seed,
//                      table_act and table_commit_products (every envelope)
//
// Each is timed in batches long enough for the clock (at least 2 ms), and the
// row reports the median batch's nanoseconds per call. The report's "share of
// 5 ms" is that against the warm request a Lambda is expected to spend.
#include "bench.h"

#include "aws/event.h"
#include "auth/bcrypt.h"
#include "auth/jwt.h"
#include "json_surface.h"
#include "store/rows_foolish.h"
#include "app/pushes.h"
#include "realtime/realtime.h"
#include "../../shared/c/b64/b64.h"
#include "../../shared/c/hex/hex.h"
#include "util/uuid.h"

#include "awire.h"
#include "game.h"
#include "legal.h"
#include "netwire.h"
#include "mqtt/mqtt.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GID "ab12cd"
#define UA "aaaaaaaa-0000-4000-8000-000000000001"
#define UB "aaaaaaaa-0000-4000-8000-000000000002"
#define SECRET "bench-secret-bench-secret-bench-secret"
#define ARENA_BYTES (512 * 1024)

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

typedef void (*op_fn)(void);

// The median nanoseconds per call over batches of at least 2 ms each.
static int64_t time_op(op_fn f, int64_t *iters_out) {
    int64_t k = 1, dt = 0;
    for (;;) {
        int64_t t0 = now_ns();
        for (int64_t i = 0; i < k; i++) f();
        dt = now_ns() - t0;
        if (dt >= 2000000 || k >= (1 << 22)) break;
        k *= 2;
    }
    int batches = dt > 50000000 ? 7 : 41;
    int64_t per[41];
    for (int b = 0; b < batches; b++) {
        int64_t t0 = now_ns();
        for (int64_t i = 0; i < k; i++) f();
        per[b] = (now_ns() - t0) / k;
    }
    *iters_out = k * batches;
    return bs_median(per, batches);
}

// ---- the game the fixtures come from ----------------------------------------------------

static Game *g_game;
static TableSnaps *g_snaps;
static uint8_t *g_arena;
static Table g_t;
static TableCommit g_c;
static uint8_t g_state[64 * 1024], g_roster[ROSTER_BYTES], g_logs[256 * 1024], g_seed[FOOLISH_SEED_LEN];
static int g_state_len, g_roster_len, g_logs_len;
static uint32_t g_version;
static char g_seed_hex[2 * FOOLISH_SEED_LEN + 1];
static bool g_seeded;

static int load(void) {
    table_init(&g_t, g_game, g_snaps);
    int rc = table_load(&g_t, g_state, g_state_len, g_roster, g_roster_len);
    if (rc >= 0 && g_seeded) rc = table_set_deal_seed(&g_t, g_seed_hex, (int)strlen(g_seed_hex));
    return rc;
}

// The last operation's products become the row, as the host's commit does.
static int commit(void) {
    int rc = table_commit_products(&g_t, GID, 6, g_version + 1, 1760000000000 + g_version, &g_c, g_arena, ARENA_BYTES);
    if (rc < 0) return rc;
    memcpy(g_state, g_arena + g_c.state.off, (size_t)g_c.state.len);
    g_state_len = g_c.state.len;
    memcpy(g_roster, g_arena + g_c.roster.off, (size_t)g_c.roster.len);
    g_roster_len = g_c.roster.len;
    if (g_c.logs_reset) g_logs_len = 0;
    if (g_logs_len + g_c.logs.len <= (int)sizeof g_logs) {
        memcpy(g_logs + g_logs_len, g_arena + g_c.logs.off, (size_t)g_c.logs.len);
        g_logs_len += g_c.logs.len;
    }
    g_version++;
    return 0;
}

// A legal move for some seat on the loaded board: its actor and wire. false: none.
static bool pick_move(const char **actor, uint8_t *wire, int *wire_len) {
    static LegalMoves lm;
    for (int seat = 0; seat < 2; seat++) {
        calculate_legal_moves(g_t.g, seat, &lm);
        for (int k = 0; k < lm.n; k++) {
            const LegalMove *m = &lm.moves[k];
            if (m->type == MOVE_WAIT) continue;
            AwireAction a;
            memset(&a, 0, sizeof a);
            a.kind = m->type;
            a.n = m->n_cards;
            for (int i = 0; i < m->n_cards; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
            int n = awire_encode(&a, wire, 64);
            if (n <= 0) continue;
            *actor = seat == 0 ? UA : UB;
            *wire_len = n;
            return true;
        }
    }
    return false;
}

// Two humans, dealt, `moves` moves in. false when the kernel refuses a step.
static bool play_in(int moves) {
    g_game = calloc(1, sizeof *g_game);
    g_snaps = calloc(1, sizeof *g_snaps);
    g_arena = malloc(ARENA_BYTES);
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) g_seed[i] = (uint8_t)(i * 37 + 11);
    hex_encode(g_seed, FOOLISH_SEED_LEN, g_seed_hex, sizeof g_seed_hex);
    table_init(&g_t, g_game, g_snaps);
    if (table_create(&g_t, UA, 36, "alice", 5) < 0 || commit() < 0) return false;
    if (load() < 0 || table_join(&g_t, UB, 36, "bob", 3) < 0 || commit() < 0) return false;
    if (load() < 0 || table_ready(&g_t, UA, 36, g_seed) < 0 || commit() < 0) return false;
    if (load() < 0 || table_ready(&g_t, UB, 36, g_seed) < 0 || commit() < 0) return false;
    g_seeded = true;
    for (int i = 0; i < moves; i++) {
        const char *actor;
        uint8_t wire[64];
        int n;
        if (load() < 0 || !pick_move(&actor, wire, &n)) return false;
        if (table_act(&g_t, actor, 36, wire, n, -1, 0) != TABLE_APPLIED || commit() < 0) return false;
    }
    return load() >= 0;
}

// ---- the operations -----------------------------------------------------------------------

static cb_buf g_event, g_out, g_getitem;
static cb_http_res g_res;
static dw_req g_req;
static cb_game_commit g_commit;
static cb_game_row g_row;
static cb_rows_write g_cas, g_put;
static cb_view_row g_view;
static uint8_t g_env2k[2048], g_back[2048];
static char g_b64[4096], g_token[CB_JWT_MAX], g_hash[CB_BCRYPT_HASH_LEN + 1];
static const char *g_actor;
static uint8_t g_wire[64];
static int g_wire_len;
static volatile int64_t g_sink;

static void op_event_decode(void) {
    cb_event ev;
    if (cb_event_decode((const char *)g_event.p, g_event.len, &ev) == 0) {
        g_sink += (int64_t)ev.http.body_len;
        cb_event_free(&ev);
    }
}

static void op_event_encode(void) {
    cb_buf_reset(&g_out);
    cb_event_encode_http(&g_res, &g_out);
    g_sink += (int64_t)g_out.len;
}

static void op_dynamo_get_parse(void) {
    cb_span item;
    cb_game_row row;
    static cb_item it;
    cb_item_reset(&it);
    if (dw_item((const char *)g_getitem.p, g_getitem.len, &item) && dw_item_read(item, &it) && cb_rows_game_of(&it, &row)) {
        g_sink += (int64_t)row.state_len;
        cb_game_row_free(&row);
    }
}

static void op_dynamo_commit_build(void) {
    cb_rows_cas_write(&g_commit, g_commit.row->updated_at, &g_cas);
    dw_iwrite(&g_req, "cbase", &g_cas.w, true);
    g_sink += (int64_t)g_req.body.len;
}

static void op_view_push(void) {
    static NwFrame f;
    f.kind = NW_WS_VIEW;
    f.gid_len = (uint8_t)strlen(g_view.game_id);
    memcpy(f.gid, g_view.game_id, f.gid_len);
    f.version = (uint32_t)g_view.version;
    f.blob.p = g_view.view;
    f.blob.len = (uint32_t)g_view.view_len;
    cb_buf_reset(&g_out);
    if (!cb_buf_reserve(&g_out, 3 * (g_view.view_len + 64))) return;
    // The frame, then the hub's PUBLISH of it on the user's views topic.
    int n = nw_frame_encode(&f, g_out.p, (int)(g_view.view_len + 64));
    static const char topic[] = "u/11111111-1111-4111-8111-111111111111/v";
    int t = n > 0 ? mqtt_write_publish(g_out.p + g_view.view_len + 64, (int)(2 * (g_view.view_len + 64)), topic,
                                       (int)sizeof topic - 1, g_out.p, n)
                  : 0;
    g_sink += t;
}

static void op_b64_encode(void) { g_sink += b64_encode(g_env2k, sizeof g_env2k, g_b64, sizeof g_b64); }
static void op_b64_decode(void) { g_sink += b64_decode(g_b64, strlen(g_b64), g_back, sizeof g_back); }

static void op_jwt_verify(void) {
    cb_jwt_claims c;
    g_sink += cb_jwt_verify(SECRET, g_token, 1760000000, &c);
}

static void op_bcrypt_verify(void) { g_sink += cb_bcrypt_verify("bench-pass-123", g_hash); }

static void op_kernel_action(void) {
    if (load() >= 0 && table_act(&g_t, g_actor, 36, g_wire, g_wire_len, -1, 0) == TABLE_APPLIED)
        g_sink += table_commit_products(&g_t, GID, 6, g_version + 1, 1760000999000, &g_c, g_arena, ARENA_BYTES);
}

// ---- fixtures ---------------------------------------------------------------------------

static bool fixtures(void) {
    if (!play_in(8)) { fprintf(stderr, "bench: the kernel refused the fixture game\n"); return false; }
    if (!pick_move(&g_actor, g_wire, &g_wire_len)) { fprintf(stderr, "bench: no move in the fixture game\n"); return false; }

    // The access token the browser sends.
    cb_jwt_claims cl;
    memset(&cl, 0, sizeof cl);
    snprintf(cl.sub, sizeof cl.sub, UA);
    snprintf(cl.email, sizeof cl.email, "alice@example.com");
    snprintf(cl.username, sizeof cl.username, "alice");
    snprintf(cl.session_id, sizeof cl.session_id, "cccccccc-0000-4000-8000-000000000001");
    cl.iat = 1760000000 - 60;
    cl.exp = 1760000000 + 3540;
    if (cb_jwt_mint(SECRET, &cl, "https://api.foolish.cards/auth/v1", g_token, sizeof g_token) < 0) return false;

    // The action request's body, as the event carries it (base64).
    uint8_t body[128];
    memcpy(body, GID, 6);
    memcpy(body + 6, g_wire, (size_t)g_wire_len);
    int bn = table_request_encode(body, sizeof body, 6, g_wire_len, true, g_version);
    char b64body[256];
    b64_encode(body, (size_t)bn, b64body, sizeof b64body);
    cb_buf_init(&g_event);
    cb_js_client_function_url_event(&g_event, g_token, bn, b64body);
    cb_event ev;
    if (cb_event_decode((const char *)g_event.p, g_event.len, &ev) != 0 || ev.kind != CB_EVENT_HTTP) {
        fprintf(stderr, "bench: the event fixture does not decode\n");
        return false;
    }
    cb_event_free(&ev);

    // The action's answer, with the headers the router adds.
    uint8_t resp[TABLE_RESP_BYTES];
    table_action_response(TABLE_APPLIED, 0, g_version + 1, resp, sizeof resp);
    cb_res_init(&g_res);
    cb_res_send(&g_res, 200, "application/octet-stream", resp, sizeof resp);
    cb_res_header(&g_res, "Cache-Control", "no-store");
    cb_res_header(&g_res, "Access-Control-Allow-Origin", "*");
    cb_buf_init(&g_out);

    // The stored row, and DynamoDB's GetItem answer for it.
    memset(&g_row, 0, sizeof g_row);
    snprintf(g_row.id, sizeof g_row.id, GID);
    g_row.status = GAME_STATUS_PLAYING;
    g_row.state = g_state;
    g_row.state_len = (size_t)g_state_len;
    g_row.roster = g_roster;
    g_row.roster_len = (size_t)g_roster_len;
    g_row.logs = g_logs;
    g_row.logs_len = (size_t)g_logs_len;
    g_row.has_seed = true;
    memcpy(g_row.seed, g_seed, CB_SEED_LEN);
    g_row.version = g_version;
    g_row.created_at = 1760000000000;
    g_row.updated_at = g_row.last_commit_at = 1760000100000;
    dw_req_init(&g_req);
    cb_rows_game_put_write(&g_row, &g_put);
    dw_iwrite(&g_req, "cbase", &g_put.w, false);
    cb_buf_init(&g_getitem);
    if (!dw_get_answer_of_put(&g_req, &g_getitem)) {
        fprintf(stderr, "bench: no item in the row's put\n");
        return false;
    }

    // The commit of the measured move: the products of kernel_action, once,
    // which must apply (a refused move would time a no-op).
    if (load() < 0 || table_act(&g_t, g_actor, 36, g_wire, g_wire_len, -1, 0) != TABLE_APPLIED ||
        table_commit_products(&g_t, GID, 6, g_version + 1, 1760000999000, &g_c, g_arena, ARENA_BYTES) < 0) {
        fprintf(stderr, "bench: the measured move does not apply\n");
        return false;
    }
    static cb_game_row next;
    next = g_row;
    next.state = g_arena + g_c.state.off;
    next.state_len = (size_t)g_c.state.len;
    next.version = g_version + 1;
    g_commit.row = &next;
    g_commit.expected_version = g_version;
    g_commit.n_humans = 2;
    snprintf(g_commit.humans[0], sizeof(cb_uuid), UA);
    snprintf(g_commit.humans[1], sizeof(cb_uuid), UB);

    // One player's view row, for the pv- push.
    memset(&g_view, 0, sizeof g_view);
    snprintf(g_view.game_id, sizeof g_view.game_id, GID);
    snprintf(g_view.player_id, sizeof g_view.player_id, UB);
    g_view.view = g_arena + g_c.views[1].off;
    g_view.view_len = (size_t)g_c.views[1].len;
    g_view.version = g_version + 1;
    snprintf(g_view.status, sizeof g_view.status, "playing");
    g_view.updated_at = 1760000999000;

    for (size_t i = 0; i < sizeof g_env2k; i++) g_env2k[i] = (uint8_t)(i * 131 + 7);
    b64_encode(g_env2k, sizeof g_env2k, g_b64, sizeof g_b64);
    if (cb_bcrypt_hash("bench-pass-123", CB_BCRYPT_COST, g_hash) != 0) return false;
    fprintf(stderr,
            "bench: fixtures: event %zu B, GetItem %zu B (state %d, logs %d), envelope %d B\n", g_event.len,
            g_getitem.len, g_state_len, g_logs_len, g_c.views[1].len);
    return true;
}

int bench_cpu(const bench_opts *o) {
    if (!fixtures()) return 1;
    static const struct { const char *name; op_fn f; } OPS[] = {
        { "event_decode", op_event_decode },
        { "event_encode", op_event_encode },
        { "dynamo_get_parse", op_dynamo_get_parse },
        { "dynamo_commit_build", op_dynamo_commit_build },
        { "view_push", op_view_push },
        { "b64_encode_2k", op_b64_encode },
        { "b64_decode_2k", op_b64_decode },
        { "jwt_verify", op_jwt_verify },
        { "bcrypt_verify_signin_only", op_bcrypt_verify },
        { "kernel_action", op_kernel_action },
    };
    char date[32], host[64];
    bench_date(date, sizeof date);
    bench_host(host, sizeof host);
    char note[256];
    snprintf(note, sizeof note, "%s", o->note ? o->note : "");
    bench_note_load(note, sizeof note);
    bs_run run;
    bs_run_init(&run, "cpu", date, host, "-", note);
    for (size_t i = 0; i < sizeof OPS / sizeof OPS[0]; i++) {
        int64_t iters = 0;
        int64_t ns = time_op(OPS[i].f, &iters);
        bs_run_cpu(&run, OPS[i].name, ns, iters);
        fprintf(stderr, "bench: %-28s %10lld ns\n", OPS[i].name, (long long)ns);
    }
    op_dynamo_commit_build();
    fprintf(stderr, "bench: the commit transaction is %zu B of JSON\n", g_req.body.len);
    return bench_save(&run, o) == 0 ? 0 : 1;
}
