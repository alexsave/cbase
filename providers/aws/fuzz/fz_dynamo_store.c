// fz_dynamo_store.c - the DynamoDB backend (store/store_dynamo.c) end to end
// against an AWS that answers anything: each store call it makes is answered
// with a status and a body from the input, through the same seam the unit
// tests record against (store_priv.h cb_store_open_dynamo_with). This reaches
// the item decoders where they are used (a row with a missing or mistyped
// attribute, a list that pages forever, a transaction cancelled for a reason
// that is not one), the retry and pagination loops, and what each store
// function hands back to its caller.
//
// Input: a program of store calls (one byte each, with arguments), and the
// answers they consume in order: [status pick][u16 length][body].
#include "fz.h"

#include "store/rows_foolish.h"
#include "store/store_priv.h"
#include "util/log.h"

static fz_in g_in;
static int g_sends;
static int64_t g_clock;

static int64_t fake_mono(void) { return g_clock; }
static void fake_sleep(int64_t ms) { g_clock += ms; }

static int fake_send(void *ud, const char *target, const uint8_t *body, size_t len, int *status, cb_buf *resp) {
    (void)ud; (void)target; (void)body; (void)len;
    // Bounded: a loop that never stops asking is a hang, and libFuzzer's
    // timeout reports it; the cap keeps each run short when it does stop.
    if (fz_left(&g_in) == 0 || ++g_sends > 200) return CB_E_IO;
    static const int ST[] = { 200, 200, 200, 200, 400, 400, 500, 503 };
    *status = ST[fz_u8(&g_in) % 8];
    size_t n;
    const uint8_t *b = fz_bytes(&g_in, fz_u16(&g_in) % 8192, &n);
    cb_buf_append(resp, b, n);
    return CB_OK;
}

static bool on_user(const cb_user_row *u, void *ud) { (void)strlen(u->email); return true; }
static bool on_member(const cb_member_row *m, void *ud) { return true; }
static bool on_game(const cb_game_row *g, void *ud) { FZ_CHECK(!g->state_len || g->state); return true; }
static bool on_name(const char *n, void *ud) { (void)strlen(n); return true; }
static bool on_bot(const cb_bot_row *b, void *ud) { return true; }
static bool on_rating(const cb_rating_row *r, void *ud) { return true; }
static bool on_snap(const cb_snapshot_row *s, void *ud) { FZ_CHECK(s->n_players >= 0 && s->n_players <= CB_MAX_SEATS); return true; }
static bool on_chat(const cb_chat_row *c, void *ud) { return true; }
static bool on_row(int n, const char *const *names, const char *const *values, void *ud) { return true; }

#define U1 "11111111-1111-4111-8111-111111111111"
#define U2 "22222222-2222-4222-8222-222222222222"
#define GID "ab12cd"

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // The first byte says where the program ends and the answers begin.
    if (size < 1) return 0;
    size_t split = 1 + (size_t)data[0] * (size - 1) / 256;
    fz_in prog = fz_in_of(data + 1, split - 1);
    g_in = fz_in_of(data + split, size - split);
    g_sends = 0;
    g_clock = 1;
    cb_store *s = NULL;
    if (cb_store_open_dynamo_with("cbase", fake_send, NULL, &s) != 0) abort();
    cb_store_dynamo_clock(s, fake_mono, fake_sleep);
    cb_store_set_deadline(0);
    for (int step = 0; step < 16 && fz_left(&prog); step++) {
        int op = fz_u8(&prog) % 24;
        cb_user_row u; cb_game_row g; cb_refresh_row rr; cb_uuid tok;
        memset(&g, 0, sizeof g);
        switch (op) {
        case 0: (void)CB_STORE_CALL(s, user_get, U1, &u); break;
        case 1: (void)CB_STORE_CALL(s, user_by_email, "a@b.io", &u); break;
        case 2: if (CB_ROWS_CALL(s, game_get, GID, &g) == 0) FZ_CHECK(!g.state_len || g.state); cb_game_row_free(&g); break;
        case 3: (void)CB_ROWS_CALL(s, games_of_user, U1, 20, on_member, NULL); break;
        case 4: { const char *ids[2] = { GID, "ff00ee" }; (void)CB_ROWS_CALL(s, games_get_views, ids, 2, on_game, NULL); break; }
        case 5: (void)CB_ROWS_CALL(s, is_member, GID, U1); break;
        case 6: (void)CB_ROWS_CALL(s, games_needing_bots, 0, 0, 10, on_name, NULL); break;
        case 7: (void)CB_ROWS_CALL(s, lease_acquire, GID, 1000, 25000, tok); break;
        case 8: (void)CB_ROWS_CALL(s, bots_list, on_bot, NULL); break;
        case 9: (void)CB_ROWS_CALL(s, ratings_top, 20, on_rating, NULL); break;
        case 10: { const char *ids[2] = { U1, U2 }; int32_t out[2]; (void)CB_ROWS_CALL(s, ratings_for, ids, 2, out); break; }
        case 11: { cb_snapshot_query q = { U1, NULL, NULL, 10, true }; (void)CB_ROWS_CALL(s, snapshots_query, &q, on_snap, NULL); break; }
        case 12: (void)CB_ROWS_CALL(s, chat_list, GID, 50, on_chat, NULL); break;
        // The chat's members (the realtime publish's targets: one per member's own topic).
        case 13: (void)CB_ROWS_CALL(s, game_members, GID, on_name, NULL); break;
        case 14: (void)CB_ROWS_CALL(s, is_member, GID, U2); break;
        case 15: (void)CB_STORE_CALL(s, refresh_take, "00", 1000, 60000, &rr); break;
        case 16: (void)CB_STORE_CALL(s, users_list, "", 0, 10, on_user, NULL); break;
        case 17: (void)CB_STORE_CALL(s, admin_collections, on_name, NULL); break;
        case 18: { char next[256]; (void)CB_STORE_CALL(s, admin_browse, "games", "", "", 10, on_row, NULL, next, sizeof next); break; }
        case 19: {
            cb_game_row row;
            memset(&row, 0, sizeof row);
            snprintf(row.id, sizeof row.id, GID);
            static uint8_t state[8] = { 1 }, roster[8] = { 1 };
            row.state = state; row.state_len = sizeof state; row.roster = roster; row.roster_len = sizeof roster;
            row.version = 4;
            cb_game_commit c;
            memset(&c, 0, sizeof c);
            c.row = &row; c.expected_version = 3;
            c.n_humans = 1; snprintf(c.humans[0], sizeof c.humans[0], U1);
            c.n_rated = fz_u8(&prog) & 1;
            snprintf(c.rated_ids[0], sizeof c.rated_ids[0], U1);
            c.rating_delta[0] = 12;
            (void)CB_ROWS_CALL(s, game_commit, &c);
            break;
        }
        case 20: (void)CB_ROWS_CALL(s, game_delete, GID, 3); break;
        case 21: (void)CB_STORE_CALL(s, admin_query, "SELECT * FROM cbase", false, on_row, NULL); break;
        // The readers the security fixes added (H1's sign-in failures, M1's sessions).
        case 22: { cb_session_row se; (void)CB_STORE_CALL(s, session_get, "s1", 1000, &se); break; }
        case 23: { cb_signin_fails sf; (void)CB_STORE_CALL(s, signin_fails_get, "a@b.io", 1000, &sf); break; }
        }
    }
    cb_store_close(s);
    return 0;
}
