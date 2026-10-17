// fz_sequence.c - the server as a whole, stateful: random sequences of
// requests from several users driven through cb_handle in-process (the
// router, the auth, the functions and /v1 routes, the table operations, a
// real SQLite store), the push stream's frames through cb_nw_on_frame, bot
// drives, a clock that moves, and another writer that races the CAS. After
// every step the invariants a player relies on are checked:
//
//   PRIVACY   no response or push that reaches a user carries another seat's
//             hidden cards: every envelope a user is handed (create, meta,
//             GET /v1/games, GET /v1/games/<id>, a stream VIEW or catch-up)
//             is that user's own seat's view or the spectators', and in every
//             board it holds (and every cast's boards and events) the other
//             seats' hands are masked; the user's own hand is the stored one.
//   AUTH      a request with no token, a malformed one, one signed with
//             another secret or an expired one gets a refusal and nothing
//             else; a connection that never presented a token hears nothing.
//   TOPICS    a connection holds only topics its user may hear.
//   VERSIONS  a game's stored version never goes back; no response or push
//             names a version the store has not reached; each connection
//             hears a game's versions in order.
//   CAS       a game commit writes only over the version it read, exactly
//             once: the stored version before an accepted commit is the
//             expected one and after it the row's; a refused one changes
//             nothing.
//   and no crash, no sanitizer report.
//
// DETERMINISM. The deal seed, the bot pick and every id come from
// cb_random_bytes, which this target replaces with a stream seeded per run
// (src/util/uuid.c is linked out; fuzz.mk FUZZ_EXCLUDE_sequence), so a saved
// input replays the same game.
//
// The input is a program: one byte picks a step, the next bytes its arguments
// (fz.h fz_in). Structure where it pays: a move is the k-th legal move of the
// acting seat on the stored board, an ask is built from parts, a stream frame
// is a real NwFrame; and raw bytes in every one of those places too.
#include "fz.h"

#include "app/app_internal.h"
#include "app/bots.h"
#include "app/fanout.h"
#include "app/functions_http.h"
#include "app/table_ops.h"
#include "app/v1_http.h"
#include "auth/auth.h"
#include "auth/jwt.h"
#include "handler.h"
#include "net/router.h"
#include "realtime/realtime.h"
#include "realtime/stream.h"
#include "store/store.h"
#include "util/log.h"
#include "util/uuid.h"

#include "ask.h"
#include "awire.h"
#include "client_table.h"
#include "legal.h"
#include "netwire.h"
#include "table.h"

#include "rng.h"

// ---- determinism: the process's randomness, replaced -------------------------------------

// The run's stream: rng.h's splitmix64, seeded from the input.
static uint64_t g_rng;

static uint64_t rng_next(void) { return rng_splitmix64(&g_rng); }

void cb_random_bytes(void *out, size_t n) {
    uint8_t *p = out;
    for (size_t i = 0; i < n; i++) {
        if (i % 8 == 0) { uint64_t r = rng_next(); memcpy(p + i, &r, n - i < 8 ? n - i : 8); }
    }
}

void cb_uuid_v4(char out[CB_UUID_LEN + 1]) {
    static const char D[] = "0123456789abcdef";
    unsigned char b[16];
    cb_random_bytes(b, sizeof b);
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = D[b[i] >> 4];
        out[o++] = D[b[i] & 15];
    }
    out[o] = 0;
}

bool cb_uuid_valid(const char *s, size_t n) {
    if (n != CB_UUID_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; continue; }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

// ---- the world ----------------------------------------------------------------------------

#define SECRET  "fz-sequence-secret-fz-sequence-secret"
#define N_USERS 4
#define N_CONNS 6
#define N_GAMES 4

static const char *const UID[N_USERS] = {
    "a0000000-0000-4000-8000-000000000001", "a0000000-0000-4000-8000-000000000002",
    "a0000000-0000-4000-8000-000000000003", "a0000000-0000-4000-8000-000000000004",
};
static const char *const UNAME[N_USERS] = { "ANN", "BOB", "CAT", "DAN" };
static const char *const BOT_ID[2] = { "b0000000-0000-4000-8000-000000000001", "b0000000-0000-4000-8000-000000000002" };

static cb_ctx g_ctx;
static cb_config g_cfg;
static cb_store *g_store;
static cb_store_ops g_ops;               // the store's ops with game_commit wrapped
static const cb_store_ops *g_real;
static int64_t g_now;
static char g_token[N_USERS][CB_JWT_MAX];
// Accounts this run deleted: their tokens still verify, and nothing may answer them.
static bool g_deleted[N_USERS];
// The games a deleted account still sat at when it went: a game in play keeps
// the seat, redacted (functions_http.c run_redact); no other may gain it.
static bool g_kept_seat[N_USERS][N_GAMES];

static int64_t fz_now(void) { return g_now; }
static void fz_sleep(int64_t ms) { g_now += ms; }

// Games this run created, by index (the program names a game by index).
static char g_gid[N_GAMES][CB_GAME_ID_MAX + 1];
static int g_n_games;
static int64_t g_max_version[N_GAMES];   // the highest stored version seen
// A marker this harness planted (case 10), at the version it was planted on,
// -1 none: until a commit or a drive, the scan may kick it, and that kick is
// the harness's doing, not the server's.
static int64_t g_planted[N_GAMES];

// ---- the CAS, watched ---------------------------------------------------------------------

static int g_steal;   // the next game_commit finds the row moved on by another writer

static int64_t stored_version(const char *gid) {
    cb_game_row g;
    memset(&g, 0, sizeof g);
    int rc = g_real->game_get(g_store, gid, &g);
    int64_t v = rc == CB_OK ? g.version : -1;
    cb_game_row_free(&g);
    return v;
}

static int watched_commit(cb_store *s, const cb_game_commit *c) {
    if (g_steal > 0) {
        g_steal--;
        char sql[128];
        snprintf(sql, sizeof sql, "UPDATE games SET version = version + 1 WHERE id = '%s'", c->row->id);
        FZ_CHECK(g_real->admin_query(s, sql, true, NULL, NULL) >= 0);
    }
    int64_t before = stored_version(c->row->id);
    int rc = g_real->game_commit(s, c);
    int64_t after = stored_version(c->row->id);
    if (rc == CB_OK) {
        FZ_CHECK(before == c->expected_version);
        FZ_CHECK(c->row->version == c->expected_version + 1);
        FZ_CHECK(after == c->row->version);
    } else {
        FZ_CHECK(after == before);
    }
    return rc;
}

// ---- reading a stored game the way the oracle needs it ---------------------------------------

static Game *g_game;
static TableSnaps *g_snaps;

// The stored board, unmasked, and the seat `uid` holds (-1 none). false: no game.
static bool stored_table(const char *gid, Table *t, int64_t *version) {
    cb_game_row g;
    memset(&g, 0, sizeof g);
    if (g_real->game_get(g_store, gid, &g) != CB_OK) { cb_game_row_free(&g); return false; }
    table_init(t, g_game, g_snaps);
    int rc = table_load(t, g.state, (int)g.state_len, g.roster, (int)g.roster_len);
    *version = g.version;
    cb_game_row_free(&g);
    FZ_CHECK(rc == TABLE_OK);   // the server never stores a row its kernel refuses
    return true;
}

static int seat_of_user(const char *gid, const char *uid) {
    Table t;
    int64_t v;
    if (!stored_table(gid, &t, &v)) return -1;
    return table_seat_of(&t, uid, (int)strlen(uid));
}

static int game_index(const char *gid, size_t n) {
    for (int i = 0; i < g_n_games; i++)
        if (strlen(g_gid[i]) == n && memcmp(g_gid[i], gid, n) == 0) return i;
    return -1;
}

static void note_version(const char *gid, size_t n, int64_t v) {
    int i = game_index(gid, n);
    if (i < 0) return;
    int64_t now = stored_version(g_gid[i]);
    // Nothing is named before the store holds it (a deleted game holds nothing).
    if (now >= 0) FZ_CHECK(v <= now);
}

// ---- the privacy oracle -----------------------------------------------------------------------

static ClientTable g_c;
static ClientSlot g_slot;

static bool is_hidden(Card c) { return c.suit == 0 && c.value == 1; }
static bool is_back(Card c) { return c.suit == -1 && c.value == -1; }

// The board the slot holds: every seat but `viewer`'s hand is masked.
static void boards_masked(int viewer) {
    const Game *g = g_c.g;
    for (int s = 0; s < g->num_players && s < MAX_PLAYERS; s++) {
        if (s == viewer) continue;
        for (int k = 0; k < g->players[s].hand_count && k < MAX_HAND_SIZE; k++) FZ_CHECK(is_hidden(g->players[s].hand[k]));
    }
}

// An envelope handed to `uid` (NULL: a spectator's asker that is no user).
static void check_envelope(const char *uid, const uint8_t *p, size_t n, const char *gid_hint) {
    if (n == 0) return;
    client_init(&g_c, &g_slot);
    int rc = client_adopt_envelope(&g_c, p, (int)n);
    if (rc != CLIENT_OK && getenv("FZ_TRACE")) {
        fprintf(stderr, "envelope refused %d (detail %d), %zu bytes:", rc, g_c.detail, n);
        for (size_t i = 0; i < n && i < 64; i++) fprintf(stderr, " %02x", p[i]);
        fprintf(stderr, "\n");
    }
    FZ_CHECK(rc == CLIENT_OK);   // the server's own envelope always reads
    const TableView *v = &g_c.view;
    int want = -1;
    for (int s = 0; uid && s < v->num_players; s++)
        if (strlen(uid) == v->seats[s].id_len && memcmp(v->seats[s].id, uid, v->seats[s].id_len) == 0) want = s;
    FZ_CHECK(v->my_seat == want);
    boards_masked(v->my_seat);
    note_version(v->game_id, v->gid_len, v->version);
    // The viewer's own hand is the stored one when this is the stored version.
    if (v->my_seat >= 0) {
        Table t;
        int64_t sv;
        char gid[CB_GAME_ID_MAX + 1];
        snprintf(gid, sizeof gid, "%.*s", (int)v->gid_len, v->game_id);
        if (stored_table(gid, &t, &sv) && sv == (int64_t)v->version) {
            const Player *pl = &t.g->players[v->my_seat];
            FZ_CHECK(v->my_hand_count == pl->hand_count);
            for (int k = 0; k < pl->hand_count; k++)
                FZ_CHECK(v->my_hand[k].suit == pl->hand[k].suit && v->my_hand[k].value == pl->hand[k].value);
        }
    }
    (void)gid_hint;
}

// A cast for `uid` (NULL: the spectators') of game gid.
static void check_cast(const char *uid, const char *gid, const uint8_t *p, size_t n) {
    int want = uid ? seat_of_user(gid, uid) : -1;
    client_init(&g_c, &g_slot);
    int rc = client_cast_open(&g_c, p, (int)n, NULL, 0);
    FZ_CHECK(rc == CLIENT_OK);
    for (int i = 0; i < 512; i++) {
        int r = client_push_next(&g_c);
        FZ_CHECK(r >= 0);
        if (r == 0) break;
        FZ_CHECK(g_c.view.my_seat == want);
        boards_masked(want);
        const PushEvent *e = &g_c.event;
        // A card dealt or drawn to another seat flies face down.
        if ((e->type == EVW_T_DEAL || e->type == EVW_T_REFILL) && e->seat != want && e->seat >= 0)
            for (int k = 0; k < e->n_cards && k < MAX_HAND_SIZE; k++) FZ_CHECK(is_back(e->cards[k]) || is_hidden(e->cards[k]));
    }
    FZ_CHECK(client_push_final(&g_c) == CLIENT_OK);
    FZ_CHECK(g_c.view.my_seat == want);
    boards_masked(want);
}

// ---- the stream ---------------------------------------------------------------------------

typedef struct {
    cb_conn_row row;
    int         user;               // whose token it last presented (-1 none) - the harness's own record
    uint32_t    last_version[N_GAMES];
} conn;

static conn g_conn[N_CONNS];

// A frame the server hands connection `c`: a reply (topic NULL), or a push to
// a topic it holds.
static void conn_hears(conn *c, const uint8_t *msg, size_t len, const char *topic) {
    NwFrame *f = calloc(1, sizeof *f);
    FZ_CHECK(nw_frame_decode(msg, (int)len, f) == NW_OK);
    const char *uid = c->row.user_id[0] ? c->row.user_id : NULL;
    for (int u = 0; uid && u < N_USERS; u++) FZ_CHECK(!(g_deleted[u] && strcmp(uid, UID[u]) == 0));
    if (f->kind == NW_WS_VIEW || f->kind == NW_WS_CATCHUP || f->kind == NW_WS_CAST || f->kind == NW_WS_CHAT) {
        // Nothing about a game reaches a connection that is no one.
        FZ_CHECK(uid != NULL);
        char gid[CB_GAME_ID_MAX + 1];
        snprintf(gid, sizeof gid, "%.*s", (int)f->gid_len, f->gid);
        int gi = game_index(f->gid, f->gid_len);
        if (f->kind != NW_WS_CHAT) {
            note_version(f->gid, f->gid_len, f->version);
            if (getenv("FZ_TRACE")) fprintf(stderr, "  conn %s hears kind %d %s v%u (%u bytes)%s%s\n", c->row.conn_id, f->kind, gid, f->version, f->blob.len, topic ? " on " : "", topic ? topic : "");
            // A seat-gone view (no envelope) carries no version: the seat is gone.
            if (gi >= 0 && !((f->kind == NW_WS_VIEW || f->kind == NW_WS_CATCHUP) && f->blob.len == 0)) {
                FZ_CHECK(f->version >= c->last_version[gi]);
                c->last_version[gi] = f->version;
            }
        }
        if (f->kind == NW_WS_VIEW || f->kind == NW_WS_CATCHUP) check_envelope(uid, f->blob.p, f->blob.len, gid);
        if (f->kind == NW_WS_CAST) {
            // A cast is only ever a push, and its topic says whose it is: the
            // seat's (gu-<game>-<uid>, the connection's own user) or the
            // spectators' (game-<game>), whatever the user's seat is now.
            FZ_CHECK(topic != NULL);
            bool seat_stream = strncmp(topic, "gu-", 3) == 0;
            if (seat_stream) FZ_CHECK(strcmp(topic + 3 + strlen(gid) + 1, uid) == 0);
            check_cast(seat_stream ? uid : NULL, gid, f->blob.p, f->blob.len);
        }
    }
    free(f);
}

static void reply_to(void *ud, const char *frame, size_t len) { conn_hears(ud, (const uint8_t *)frame, len, NULL); }

// What a topic says its listener must be.
static void check_topics(const conn *c) {
    for (int i = 0; i < c->row.n_topics; i++) {
        const char *t = c->row.topics[i];
        FZ_CHECK(c->row.user_id[0]);
        if (strncmp(t, "pv-", 3) == 0) FZ_CHECK(strcmp(t + 3, c->row.user_id) == 0);
        else if (strncmp(t, "gu-", 3) == 0) {
            const char *dash = strchr(t + 3, '-');
            FZ_CHECK(dash && strcmp(dash + 1, c->row.user_id) == 0);
        } else FZ_CHECK(strncmp(t, "game-", 5) == 0 || strncmp(t, "chat:", 5) == 0);
    }
}

// The transport: a publish reaches every connection holding the topic.
static int g_pushes;
static int fz_deliver(cb_rt *rt, const char *topic, const uint8_t *frame, size_t len) {
    g_pushes++;
    // Who the topic is for, from its name, whatever connection holds it.
    if (strncmp(topic, "pv-", 3) == 0 || strncmp(topic, "gu-", 3) == 0 || strncmp(topic, "game-", 5) == 0) {
        NwFrame *f = calloc(1, sizeof *f);
        FZ_CHECK(nw_frame_decode(frame, (int)len, f) == NW_OK);
        char gid[CB_GAME_ID_MAX + 1];
        snprintf(gid, sizeof gid, "%.*s", (int)f->gid_len, f->gid);
        if (topic[0] == 'p') {
            FZ_CHECK(f->kind == NW_WS_VIEW);
            check_envelope(topic + 3, f->blob.p, f->blob.len, gid);
        } else if (topic[0] == 'g' && topic[1] == 'u') {
            FZ_CHECK(f->kind == NW_WS_CAST);
            const char *uid = topic + 3 + strlen(gid) + 1;
            FZ_CHECK(strncmp(topic + 3, gid, strlen(gid)) == 0);
            check_cast(uid, gid, f->blob.p, f->blob.len);
        } else {
            FZ_CHECK(f->kind == NW_WS_CAST);
            check_cast(NULL, gid, f->blob.p, f->blob.len);
        }
        free(f);
    }
    for (int i = 0; i < N_CONNS; i++)
        for (int k = 0; k < g_conn[i].row.n_topics; k++)
            if (strcmp(g_conn[i].row.topics[k], topic) == 0) conn_hears(&g_conn[i], frame, len, topic);
    return 0;
}
// Ends the user's connections, as rt_local closes their sockets.
static void fz_forget(cb_rt *rt, const char *user_id, const char *const *topics, int n) {
    for (int i = 0; i < N_CONNS; i++)
        if (strcmp(g_conn[i].row.user_id, user_id) == 0) {
            g_conn[i].row.user_id[0] = 0;
            g_conn[i].row.n_topics = 0;
        }
}
static const cb_rt_transport FZ_T = { "fuzz", fz_deliver, NULL, fz_forget };
static cb_rt g_rt = { &FZ_T, &g_ctx, NULL };

static int g_kicks;
// While a scan runs, the games it kicks.
static bool g_scanning;
static int g_scan_n;
static char g_scan_kicked[64][CB_GAME_ID_MAX + 1];
static void fz_kick(cb_ctx *ctx, const char *game_id) {
    g_kicks++;
    if (g_scanning && g_scan_n < 64) snprintf(g_scan_kicked[g_scan_n++], CB_GAME_ID_MAX + 1, "%s", game_id);
}

// ---- requests -----------------------------------------------------------------------------

static int g_status;
static uint8_t *g_body;
static size_t g_len;

typedef enum { AUTH_GOOD, AUTH_NONE, AUTH_JUNK, AUTH_OTHER_SECRET, AUTH_EXPIRED, AUTH_NOT_BEARER } auth_kind;

static void send(const char *method, const char *path, int user, auth_kind ak, const uint8_t *body, size_t n,
                 bool octets) {
    char p[256];
    snprintf(p, sizeof p, "%s", path);
    char *q = strchr(p, '?');
    if (q) *q++ = 0;
    cb_http_req r;
    memset(&r, 0, sizeof r);
    snprintf(r.method, sizeof r.method, "%s", method);
    r.path = p;
    r.query = q ? q : "";
    r.minor = 1;
    char authz[CB_JWT_MAX + 32];
    authz[0] = 0;
    switch (ak) {
    case AUTH_GOOD: snprintf(authz, sizeof authz, "Bearer %s", g_token[user]); break;
    case AUTH_NONE: break;
    case AUTH_JUNK: snprintf(authz, sizeof authz, "Bearer %s.x", g_token[user]); break;
    case AUTH_NOT_BEARER: snprintf(authz, sizeof authz, "Basic %s", g_token[user]); break;
    case AUTH_OTHER_SECRET: case AUTH_EXPIRED: {
        cb_jwt_claims c;
        memset(&c, 0, sizeof c);
        snprintf(c.sub, sizeof c.sub, "%s", UID[user]);
        snprintf(c.session_id, sizeof c.session_id, "%s", UID[user]);
        snprintf(c.username, sizeof c.username, "%s", UNAME[user]);
        c.iat = g_now / 1000 - 7200;
        c.exp = ak == AUTH_EXPIRED ? g_now / 1000 - 3600 : g_now / 1000 + 3600;
        char tok[CB_JWT_MAX];
        FZ_CHECK(cb_jwt_mint(ak == AUTH_EXPIRED ? SECRET : "another-secret-another-secret!!", &c, "", tok, sizeof tok) > 0);
        snprintf(authz, sizeof authz, "Bearer %s", tok);
        break;
    }
    }
    if (authz[0]) r.headers[r.n_headers++] = (cb_http_header){ "Authorization", authz };
    if (octets) r.headers[r.n_headers++] = (cb_http_header){ "Content-Type", "application/octet-stream" };
    uint8_t *copy = n ? fz_dup(body, n) : NULL;
    r.body = copy;
    r.body_len = n;
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&g_ctx, &r, &res);
    g_status = res.status;
    // A deleted account's token still verifies until it expires (auth.h), and
    // the server's rule is that it can never SEAT anyone (functions_http.c
    // account_lives): a create is refused, and whatever it asks, it holds no
    // seat afterwards. Reading as a spectator is what any token may do.
    if (ak == AUTH_GOOD && g_deleted[user]) {
        if (strcmp(path, "/v1/create") == 0) FZ_CHECK(res.status >= 400);
        for (int i = 0; i < g_n_games; i++) FZ_CHECK(g_kept_seat[user][i] || seat_of_user(g_gid[i], UID[user]) < 0);
    }
    free(g_body);
    g_len = res.body.len;
    g_body = fz_dup(res.body.p ? res.body.p : (const uint8_t *)"", g_len);
    cb_res_free(&res);
    free(copy);
}

// An answer to a request without a valid token: a refusal, nothing else.
static void check_refused(void) {
    FZ_CHECK(g_status >= 400);
    AskRefusal r;
    FZ_CHECK(ask_refusal_decode(g_body, (int)g_len, &r) == ASK_OK);
    FZ_CHECK(r.why == ASK_WHY_AUTH);
}

// A 200 answer whose body is an envelope for `user`.
static void check_envelope_answer(int user) {
    if (g_status != 200) {
        AskRefusal r;
        // Every refusal is the kernel's, or 503 BUSY's.
        FZ_CHECK(ask_refusal_decode(g_body, (int)g_len, &r) == ASK_OK);
        return;
    }
    check_envelope(UID[user], g_body, g_len, NULL);
}

// ---- the steps ----------------------------------------------------------------------------

static const char *game_of(fz_in *in) {
    if (g_n_games == 0) return "zz99zz";
    uint8_t b = fz_u8(in);
    return b == 0xff ? "nosuch" : g_gid[b % g_n_games];
}

static void step_create(int user) {
    send("POST", "/v1/create", user, AUTH_GOOD, NULL, 0, false);
    check_envelope_answer(user);
    if (g_status == 200 && g_n_games < N_GAMES) {
        client_init(&g_c, &g_slot);
        FZ_CHECK(client_adopt_envelope(&g_c, g_body, (int)g_len) == CLIENT_OK);
        snprintf(g_gid[g_n_games++], CB_GAME_ID_MAX + 1, "%.*s", (int)g_c.view.gid_len, g_c.view.game_id);
    }
}

static void step_meta(fz_in *in, int user) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    static const uint8_t KINDS[] = { ASK_JOIN, ASK_START, ASK_ADD_BOT, ASK_EXIT, ASK_CONTINUE, ASK_REARRANGE_HAND,
                                     ASK_REARRANGE_PLAYERS, ASK_UPDATE_NAME, ASK_SET_RULES, ASK_BUMP, ASK_JOIN, ASK_START };
    a.kind = KINDS[fz_u8(in) % sizeof KINDS];
    const char *gid = game_of(in);
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    uint8_t arg = fz_u8(in);
    switch (a.kind) {
    case ASK_ADD_BOT:
        if (arg & 1) { a.bot_len = 36; memcpy(a.bot, BOT_ID[(arg >> 1) & 1], 36); }
        break;
    case ASK_EXIT: {   // leave, or remove another seat (a bot or a player)
        int who = arg % (N_USERS + 3);
        if (who < N_USERS) { a.player_len = 36; memcpy(a.player, UID[who], 36); }
        else if (who < N_USERS + 2) { a.bot_len = 36; memcpy(a.bot, BOT_ID[who - N_USERS], 36); }
        break;
    }
    case ASK_REARRANGE_HAND:
        a.n_idx = (uint8_t)(arg % 8);
        for (int i = 0; i < a.n_idx; i++) a.idx[i] = fz_u8(in) % 8;
        break;
    case ASK_REARRANGE_PLAYERS:
        a.n_order = (uint8_t)(arg % 5);
        for (int i = 0; i < a.n_order; i++) {
            int who = fz_u8(in) % (N_USERS + 2);
            const char *id = who < N_USERS ? UID[who] : BOT_ID[who - N_USERS];
            a.order[i].len = 36;
            memcpy(a.order[i].id, id, 36);
        }
        break;
    case ASK_UPDATE_NAME: {
        size_t got;
        const uint8_t *b = fz_bytes(in, arg % 64, &got);
        a.title_len = (uint16_t)got;
        memcpy(a.title, b, got);
        break;
    }
    case ASK_SET_RULES: a.passing = arg & 1; break;
    }
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    if (n <= 0) return;
    send("POST", "/v1/meta", user, AUTH_GOOD, body, (size_t)n, true);
    if (getenv("FZ_TRACE")) fprintf(stderr, "  meta %d %s by %d: %d\n", a.kind, gid, user, g_status);
    check_envelope_answer(user);
}

// A move: the k-th legal move of the user's seat on the stored board, or raw bytes.
static void step_action(fz_in *in, int user) {
    const char *gid = game_of(in);
    uint8_t how = fz_u8(in);
    uint8_t wire[2 + 2 * AWIRE_MAX_CARDS];
    int wn = 0;
    Table t;
    int64_t v = 0;
    if (how % 4 != 3 && stored_table(gid, &t, &v)) {
        // The user's own legal moves; when they have none, the next user's
        // who has some acts instead, so a program of moves keeps a game going.
        static LegalMove moves[512];
        int n = 0;
        for (int k = 0; k < N_USERS && n == 0; k++) {
            int u = (user + k) % N_USERS;
            int seat = table_seat_of(&t, UID[u], (int)strlen(UID[u]));
            if (seat >= 0 && t.g->status == GAME_STATUS_PLAYING) {
                int all = legal_moves_into(t.g, seat, moves, 512);
                for (int i = 0; i < all; i++)
                    if (moves[i].type != MOVE_WAIT) moves[n++] = moves[i];   // a wait is no wire
            }
            if (n > 0) user = u;
            if (how & 16) break;   // only the named user
        }
        if (n > 0) {
            const LegalMove *m = &moves[fz_u16(in) % n];
            AwireAction a;
            memset(&a, 0, sizeof a);
            a.kind = m->type;
            a.n = m->n_cards;
            for (int i = 0; i < m->n_cards && i < AWIRE_MAX_CARDS; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
            wn = awire_encode(&a, wire, sizeof wire);
        }
    }
    if (wn <= 0) {
        size_t got;
        const uint8_t *b = fz_bytes(in, fz_u8(in) % sizeof wire, &got);
        memcpy(wire, b, got);
        wn = (int)got;
    }
    uint8_t buf[64 + sizeof wire];
    size_t gl = strlen(gid);
    memcpy(buf, gid, gl);
    memcpy(buf + gl, wire, (size_t)wn);
    // The intent: none, the stored version, or a stale one.
    bool has_intent = how & 4;
    uint32_t intent = (uint32_t)(how & 8 ? v : (v > 0 ? v - 1 : 0));
    int n = table_request_encode(buf, sizeof buf, (int)gl, wn, has_intent, intent);
    if (n <= 0) return;
    send("POST", "/v1/action", user, AUTH_GOOD, buf, (size_t)n, true);
    if (getenv("FZ_TRACE")) fprintf(stderr, "  action %s by %d (%d bytes of wire): %d\n", gid, user, wn, g_status);
    if (g_status == 200) {
        TableResponse r;
        FZ_CHECK(table_response_decode(g_body, (int)g_len, &r) == 0);
        note_version(gid, strlen(gid), r.version);
    } else {
        AskRefusal r;
        FZ_CHECK(ask_refusal_decode(g_body, (int)g_len, &r) == ASK_OK);
    }
}

static void step_reads(fz_in *in, int user) {
    char path[128];
    switch (fz_u8(in) % 7) {
    case 0: {
        send("GET", "/v1/games", user, AUTH_GOOD, NULL, 0, false);
        if (g_status != 200) break;
        int n = nw_list_open(g_body, (int)g_len, NW_GAMES);
        FZ_CHECK(n >= 0);
        for (int i = 0; i < n; i++) {
            uint32_t ver;
            NwBlob e;
            FZ_CHECK(nw_get_game(g_body, (int)g_len, i, &ver, &e) == NW_OK);
            uint8_t *copy = fz_dup(e.p, e.len);
            check_envelope(UID[user], copy, e.len, NULL);
            free(copy);
        }
        break;
    }
    case 1: {
        snprintf(path, sizeof path, "/v1/games/%s", game_of(in));
        send("GET", path, user, AUTH_GOOD, NULL, 0, false);
        if (g_status != 200) break;
        uint32_t ver;
        NwBlob e;
        FZ_CHECK(nw_view_decode(g_body, (int)g_len, &ver, &e) == NW_OK);
        uint8_t *copy = fz_dup(e.p, e.len);
        check_envelope(UID[user], copy, e.len, NULL);
        free(copy);
        break;
    }
    case 2: {
        snprintf(path, sizeof path, "/v1/chat/%s", game_of(in));
        send("GET", path, user, AUTH_GOOD, NULL, 0, false);
        break;
    }
    case 3: {
        NwRequest q;
        memset(&q, 0, sizeof q);
        q.kind = NW_CHAT_SEND;
        size_t got;
        const uint8_t *b = fz_bytes(in, fz_u8(in) % 64, &got);
        q.message_len = (uint16_t)got;
        memcpy(q.message, b, got);
        uint8_t body[NW_CHAT_MAX + 16];
        int n = nw_request_encode(&q, body, sizeof body);
        snprintf(path, sizeof path, "/v1/chat/%s", game_of(in));
        if (n > 0) send("POST", path, user, AUTH_GOOD, body, (size_t)n, true);
        break;
    }
    case 4: send("GET", "/v1/history", user, AUTH_GOOD, NULL, 0, false); break;
    case 5:
        snprintf(path, sizeof path, "/v1/replay/%s", game_of(in));
        send("GET", path, user, AUTH_GOOD, NULL, 0, false);
        break;
    case 6: send("GET", fz_u8(in) & 1 ? "/v1/bots" : "/v1/leaderboard", user, AUTH_GOOD, NULL, 0, false); break;
    }
}

// Every protected route without a valid token.
static void step_unauthorised(fz_in *in, int user) {
    static const struct { const char *method, *path; bool octets; } R[] = {
        { "POST", "/v1/create", false }, { "POST", "/v1/action", true }, { "POST", "/v1/meta", true },
        { "POST", "/v1/account/delete", false }, { "GET", "/v1/games", false }, { "GET", "/v1/games/%s", false },
        { "GET", "/v1/chat/%s", false }, { "POST", "/v1/chat/%s", true }, { "GET", "/v1/history", false },
        { "GET", "/v1/replay/%s", false }, { "POST", "/v1/auth/signout", true }, { "POST", "/v1/auth/password", true },
    };
    int k = fz_u8(in) % (int)(sizeof R / sizeof R[0]);
    auth_kind ak = (auth_kind)(1 + fz_u8(in) % 5);
    char path[128];
    snprintf(path, sizeof path, R[k].path, game_of(in));
    size_t got;
    const uint8_t *b = fz_bytes(in, fz_u8(in) % 128, &got);
    send(R[k].method, path, user, ak, b, got, R[k].octets);
    if (getenv("FZ_TRACE")) fprintf(stderr, "  unauthorised %s %s (%d): %d\n", R[k].method, path, (int)ak, g_status);
    check_refused();
}

// Any route, any body, a valid token: nothing may crash, and a 200 that is
// an envelope route is held to the envelope rules.
static void step_raw(fz_in *in, int user) {
    static const char *const P[] = { "/v1/create", "/v1/action", "/v1/meta", "/v1/chat/%s", "/v1/auth/signout",
                                     "/v1/games/%s", "/v1/replay/%s" };
    int k = fz_u8(in) % (int)(sizeof P / sizeof P[0]);
    char path[128];
    snprintf(path, sizeof path, P[k], game_of(in));
    size_t got;
    const uint8_t *b = fz_bytes(in, fz_u16(in) % 2048, &got);
    send(fz_u8(in) & 1 ? "POST" : "GET", path, user, AUTH_GOOD, b, got, true);
    if (g_status == 200 && (k == 0 || k == 2)) check_envelope(UID[user], g_body, g_len, NULL);
}

static void step_stream(fz_in *in) {
    conn *c = &g_conn[fz_u8(in) % N_CONNS];
    NwFrame *f = calloc(1, sizeof *f);
    uint8_t kind = fz_u8(in) % 6;
    int user = fz_u8(in) % N_USERS;
    bool good = fz_u8(in) % 4 != 0;
    switch (kind) {
    case 0: case 4:
        f->kind = kind == 0 ? NW_WS_AUTH : NW_WS_SUB_AS;
        snprintf(f->token, sizeof f->token, "%s", good ? g_token[user] : "not.a.token");
        f->token_len = (uint16_t)strlen(f->token);
        if (kind == 0) break;
        /* fallthrough */
    case 1: {
        if (kind == 1) f->kind = NW_WS_SUB;
        f->ref = fz_u16(in);
        f->what = (uint8_t)(1 + fz_u8(in) % 3);
        if (f->what != NW_SUB_GAMES || fz_u8(in) == 0) {
            const char *gid = game_of(in);
            f->gid_len = (uint8_t)strlen(gid);
            memcpy(f->gid, gid, f->gid_len);
        }
        f->version = fz_u8(in) % 8;
        break;
    }
    case 2: {
        f->kind = NW_WS_UNSUB;
        f->what = (uint8_t)(1 + fz_u8(in) % 3);
        const char *gid = game_of(in);
        f->gid_len = (uint8_t)strlen(gid);
        memcpy(f->gid, gid, f->gid_len);
        break;
    }
    case 3: f->kind = NW_WS_PING; break;
    }
    uint8_t buf[4096];
    int n;
    if (kind == 5) {
        size_t got;
        const uint8_t *b = fz_bytes(in, fz_u8(in), &got);
        memcpy(buf, b, got);
        n = (int)got;
    } else {
        n = nw_frame_encode(f, buf, sizeof buf);
    }
    free(f);
    if (n < 0) return;
    uint8_t *exact = fz_dup(buf, (size_t)n);
    cb_conn_row before = c->row;
    (void)cb_nw_on_frame(&g_ctx, &c->row, exact, (size_t)n, reply_to, c);
    free(exact);
    FZ_CHECK(c->row.n_topics >= 0 && c->row.n_topics <= CB_CONN_TOPICS);
    check_topics(c);
    // A topic this frame added is one its user may hear now: a seat's stream
    // and a game's chat only for a member of the game.
    for (int i = 0; i < c->row.n_topics; i++) {
        const char *t = c->row.topics[i];
        bool had = false;
        for (int k = 0; k < before.n_topics && !had; k++) had = strcmp(before.topics[k], t) == 0;
        if (had) continue;
        char gid[CB_GAME_ID_MAX + 1] = "";
        if (strncmp(t, "gu-", 3) == 0) snprintf(gid, sizeof gid, "%.*s", (int)(strchr(t + 3, '-') - (t + 3)), t + 3);
        if (strncmp(t, "chat:", 5) == 0) snprintf(gid, sizeof gid, "%s", t + 5);
        if (gid[0]) FZ_CHECK(g_real->is_member(g_store, gid, c->row.user_id) == 1);
    }
}

// ---- the bot scan ---------------------------------------------------------------------------

// Whether the scan partition lists `gid` at all, whatever its windows.
static bool listed_cb_found;
static const char *listed_cb_want;
static bool listed_cb(const char *name, void *ud) {
    if (strcmp(name, listed_cb_want) == 0) listed_cb_found = true;
    return true;
}
static bool in_scan(const char *gid) {
    listed_cb_found = false;
    listed_cb_want = gid;
    FZ_CHECK(g_real->games_needing_bots(g_store, INT64_MAX, INT64_MIN, 0, listed_cb, NULL) >= 0);
    return listed_cb_found;
}

// The heartbeat: every game it kicks is one a bot can act on now (table.h
// table_bots_due on the stored board).
static void step_scan(void) {
    g_scanning = true;
    g_scan_n = 0;
    int rc = cb_bots_scan(&g_ctx, g_now);
    g_scanning = false;
    FZ_CHECK(rc >= 0 && rc == g_scan_n);
    for (int i = 0; i < g_scan_n; i++) {
        Table t;
        int64_t v;
        if (!stored_table(g_scan_kicked[i], &t, &v)) continue;   // a kicked game since deleted
        int gi = game_index(g_scan_kicked[i], strlen(g_scan_kicked[i]));
        if (gi >= 0 && g_planted[gi] == v) continue;
        FZ_CHECK(table_bots_due(&t));
    }
}

// A drive: one that finds nothing to do (commits nothing) leaves the game out
// of the scan, unless a newer commit arrived (none can here: one thread).
static void step_drive(const char *gid) {
    int64_t before = stored_version(gid);
    int rc = cb_bots_drive(&g_ctx, gid);
    int64_t after = stored_version(gid);
    if (rc == CB_OK && before >= 0 && after == before) FZ_CHECK(!in_scan(gid));
    int gi = game_index(gid, strlen(gid));
    if (gi >= 0) g_planted[gi] = -1;
}

// ---- one run ------------------------------------------------------------------------------

static bool collect_nothing(int n, const char *const *names, const char *const *values, void *ud) { return true; }

static void reset_world(uint64_t seed) {
    g_rng = rng_seed64(seed);
    g_now = 1790000000000LL;
    g_n_games = 0;
    memset(g_gid, 0, sizeof g_gid);
    memset(g_max_version, 0, sizeof g_max_version);
    for (int i = 0; i < N_GAMES; i++) g_planted[i] = -1;
    memset(g_deleted, 0, sizeof g_deleted);
    memset(g_kept_seat, 0, sizeof g_kept_seat);
    memset(g_conn, 0, sizeof g_conn);
    for (int i = 0; i < N_CONNS; i++) {
        snprintf(g_conn[i].row.conn_id, sizeof g_conn[i].row.conn_id, "conn%d", i);
        g_conn[i].user = -1;
    }
    g_steal = 0;
    cb_row_cache_clear();
    static const char *const EMPTY[] = {
        "DELETE FROM chat_messages", "DELETE FROM game_snapshots", "DELETE FROM game_members", "DELETE FROM game_bots",
        "DELETE FROM games",         "DELETE FROM ratings",        "DELETE FROM refresh_tokens", "DELETE FROM users",
        "DELETE FROM rt_conn_topics", "DELETE FROM rt_conns",
    };
    for (size_t i = 0; i < sizeof EMPTY / sizeof EMPTY[0]; i++)
        FZ_CHECK(g_real->admin_query(g_store, EMPTY[i], true, collect_nothing, NULL) >= 0);
    for (int i = 0; i < N_USERS; i++) {
        cb_user_row u;
        memset(&u, 0, sizeof u);
        snprintf(u.id, sizeof u.id, "%s", UID[i]);
        snprintf(u.email, sizeof u.email, "user%d@fuzz.test", i);
        snprintf(u.password_hash, sizeof u.password_hash, "$2b$04$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
        snprintf(u.username, sizeof u.username, "%s", UNAME[i]);
        u.created_at = g_now;
        FZ_CHECK(g_real->user_create(g_store, &u) == CB_OK);
        cb_jwt_claims c;
        memset(&c, 0, sizeof c);
        snprintf(c.sub, sizeof c.sub, "%s", UID[i]);
        snprintf(c.session_id, sizeof c.session_id, "%s", UID[i]);
        snprintf(c.email, sizeof c.email, "%s", u.email);
        snprintf(c.username, sizeof c.username, "%s", UNAME[i]);
        c.iat = g_now / 1000;
        c.exp = g_now / 1000 + 365 * 86400;   // outlives any clock the program moves
        FZ_CHECK(cb_jwt_mint(SECRET, &c, "", g_token[i], sizeof g_token[i]) > 0);
    }
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&g_cfg, 0, sizeof g_cfg);
    snprintf(g_cfg.jwt_secret, sizeof g_cfg.jwt_secret, "%s", SECRET);
    snprintf(g_cfg.allowed_origins, sizeof g_cfg.allowed_origins, "*");
    snprintf(g_cfg.public_url, sizeof g_cfg.public_url, "http://127.0.0.1:8080");
    g_cfg.jwt_ttl_s = 3600;
    g_ctx.cfg = &g_cfg;
    g_ctx.mode = CB_MODE_SERVE;
    g_ctx.version = "fuzz";
    if (cb_store_open_sqlite(":memory:", &g_store) != CB_OK) abort();
    g_real = g_store->ops;
    g_ops = *g_real;
    g_ops.game_commit = watched_commit;
    g_store->ops = &g_ops;
    g_ctx.store = g_store;
    // Two bots that play at once.
    cb_bot_row bots[2];
    memset(bots, 0, sizeof bots);
    for (int i = 0; i < 2; i++) {
        snprintf(bots[i].id, sizeof bots[i].id, "%s", BOT_ID[i]);
        snprintf(bots[i].nickname, sizeof bots[i].nickname, "%%Fuzz %d", i + 1);
        snprintf(bots[i].strategy_key, sizeof bots[i].strategy_key, "random");
        bots[i].elo_rating = 1000;
        bots[i].previous_elo = -1;
    }
    if (g_real->bots_replace(g_store, bots, 2) != CB_OK) abort();
    if (cb_auth_new(&g_ctx, &g_ctx.auth) != CB_OK) abort();
    g_ctx.rt = &g_rt;
    g_ctx.kick_bots = fz_kick;
    g_ctx.router = cb_router_new();
    if (cb_functions_routes(&g_ctx) != CB_OK || cb_v1_routes(&g_ctx) != CB_OK) abort();
    cb_auth_now = fz_now;
    cb_app_now = fz_now;
    cb_app_sleep = fz_sleep;
    g_game = calloc(1, sizeof *g_game);
    g_snaps = calloc(1, sizeof *g_snaps);
    if (!g_game || !g_snaps) abort();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!g_store) LLVMFuzzerInitialize(NULL, NULL);
    fz_in in = fz_in_of(data, size);
    uint64_t seed = fz_u32(&in);
    reset_world(seed);
    for (int step = 0; step < 48 && fz_left(&in); step++) {
        int user = 0;
        uint8_t op = fz_u8(&in);
        user = (op / 12) % N_USERS;   // op = 12 * user + step
        switch (op % 12) {
        case 0: step_create(user); break;
        case 1: case 2: step_meta(&in, user); break;
        case 3: case 4: step_action(&in, user); break;
        case 5: step_reads(&in, user); break;
        case 6:
            if (fz_u8(&in) == 0xff) {   // the account goes
                send("POST", "/v1/account/delete", user, AUTH_GOOD, NULL, 0, false);
                if (g_status == 204) {
                    g_deleted[user] = true;
                    for (int i = 0; i < g_n_games; i++) g_kept_seat[user][i] = seat_of_user(g_gid[i], UID[user]) >= 0;
                }
            } else {
                step_unauthorised(&in, user);
            }
            break;
        case 7: step_stream(&in); break;
        case 8:
            if (fz_u8(&in) % 4 == 0) step_scan();
            else if (g_n_games) step_drive(game_of(&in));
            break;
        case 9: g_now += (int64_t)fz_u16(&in) * 50; break;
        case 10: {
            uint8_t b = fz_u8(&in);
            g_steal = b % 3;
            // A scan marker with no bot work behind it: what a commit racing
            // a drive's idle, or DynamoDB's lagging marker, leaves. The next
            // drive must find nothing and take the game out.
            if ((b & 4) && g_n_games) {
                char sql[128];
                snprintf(sql, sizeof sql, "UPDATE games SET needs_bots = 1 WHERE id = '%s'", game_of(&in));
                FZ_CHECK(g_real->admin_query(g_store, sql, true, NULL, NULL) >= 0);
                int gi = game_index(sql + strlen(sql) - 7, 6);
                if (gi >= 0) g_planted[gi] = stored_version(g_gid[gi]);
            }
            break;
        }
        case 11: step_raw(&in, user); break;
        }
        // VERSIONS: a stored version never goes back.
        for (int i = 0; i < g_n_games; i++) {
            int64_t v = stored_version(g_gid[i]);
            if (v < 0) continue;   // the last seat left: the row is gone
            FZ_CHECK(v >= g_max_version[i]);
            g_max_version[i] = v;
        }
    }
    if (getenv("FZ_TRACE")) {
        fprintf(stderr, "sequence: %d games, %d pushes, %d kicks; versions", g_n_games, g_pushes, g_kicks);
        for (int i = 0; i < g_n_games; i++) fprintf(stderr, " %s:%lld", g_gid[i], (long long)stored_version(g_gid[i]));
        fprintf(stderr, "\n");
    }
    free(g_body);
    g_body = NULL;
    return 0;
}
