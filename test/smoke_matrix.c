// smoke_matrix.c - who may read and write what, against a real `cbase serve`.
//
//   build/test/smoke_matrix build/cbase
//
// smoke_authz.c pins one row per authorization rule as each is made; this one
// is exhaustive the other way: every identity against every route and frame
// kind, with every body decoded, plus a whole game and an account deletion.
//
// The store is the in-RAM one (CBASE_DB=:memory:), or the server's DynamoDB
// table under CBASE_STORE=dynamo (`make check-dynamo`). Three parts:
//
// THE AUTHORIZATION MATRIX. Every route and every stream frame kind, asked by
// every identity: no token, a token that is not one, an expired one, one
// signed with another secret, one with alg "none", one whose payload was
// swapped for another user's under the first's signature, the admin token sent
// as a user's, a validly signed token for a user that does not exist, another
// user (signed in, never at the game), a spectator (signed in, watching the
// game's stream), a seated player, the host, and for the console the admin
// token, a wrong one and a user's. Each cell asserts the exact status (and
// for a refusal the exact AskRefusal), and that no answer carries another
// user's data: a refusal is ten bytes, a list holds only the caller's rows,
// and every envelope is decoded with the kernel and must be the caller's own
// seat (or the spectators') with every other hand hidden.
//
// THE HIDDEN-CARD GAME. Three humans play a whole game over HTTP while each
// holds a stream socket, with a spectator and a fourth user listening too.
// EVERY answer and EVERY push any of them receives is decoded with the
// kernel: an envelope's other hands and the deck are hidden (wire 0xFE), a
// cast's deals and refills to other seats are hidden and each step's board
// hides as the envelope does, and at each shared version no card a viewer can
// see is in another seat's hand as that seat's own envelope shows it.
//
// ACCOUNT DELETION. A player deletes their account after the game: their
// refresh token, sign-in and console row are gone, every collection the
// console can browse is searched for their name (raw, hex and base64 in each
// alignment) and for their id outside the games' and replays' own rows, the
// other players' replays no longer name them, and their open stream hears
// nothing more.
#include "../../shared/c/rng.h"
#include "t.h"
#include "lib_client.h"
#include "lib_proc.h"
#include "lib_store_backend.h"
#include "fixture_unfurl.h"

#include "auth/jwt.h"
#include "net/ws.h"
#include "util/b64.h"
#include "util/clock.h"
#include "util/hex.h"
#include "util/json.h"

#include "ask.h"
#include "awire.h"
#include "client_table.h"
#include "evwire.h"
#include "legal.h"
#include "netwire.h"
#include "table.h"
#include "view.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SECRET "authz-secret-authz-secret-authz-secret-0"
#define OTHER_SECRET "not-the-servers-secret-not-the-servers-secret"
#define ADMIN "authz-admin-token-0123456789abcdef"
#define WIRE_HIDDEN 0xFE
#define PASSWORD "authz-pass-123"

static const char *g_bin;
static tp_child g_child;
static int g_port;
static int g_cells;          // matrix cells asserted
static int g_bodies_checked; // envelopes and casts decoded and judged

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void start_server(void) {
    char line[512];
    char *mem[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_JWT_SECRET=" SECRET, "CBASE_ADMIN_TOKEN=" ADMIN,
                    "CBASE_DB=:memory:", "CBASE_REQ_WORKERS=4", NULL };
    // Under dynamo the store and table come from the environment the lane exported.
    char *dyn[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_JWT_SECRET=" SECRET, "CBASE_ADMIN_TOKEN=" ADMIN,
                    "CBASE_REQ_WORKERS=4", NULL };
    char *argv[] = { (char *)g_bin, "serve", NULL };
    T_EQ_INT(tp_start(&g_child, argv, tsb_dynamo() ? dyn : mem), 0);
    T_EQ_INT(tp_wait_line(&g_child, "listening on", 20000, line, sizeof line), 0);
    const char *colon = strrchr(line, ':');
    g_port = colon ? atoi(colon + 1) : 0;
    T_CHECK(g_port > 0);
}

// ---- HTTP --------------------------------------------------------------------------------

typedef struct {
    int     status;
    uint8_t body[1 << 20];
    int     len;
    char    cache[64];
} answer;
static answer g_a;

// One request. `auth` is the whole Authorization value ("Bearer x"), or NULL.
static int http(const char *method, const char *path, const char *auth, const void *body, size_t n, const char *ctype) {
    char headers[4096];
    int h = 0;
    headers[0] = 0;
    if (auth) h += snprintf(headers + h, sizeof headers - (size_t)h, "Authorization: %s\r\n", auth);
    if (ctype) h += snprintf(headers + h, sizeof headers - (size_t)h, "Content-Type: %s\r\n", ctype);
    tc_response r;
    g_a.status = -1;
    g_a.len = 0;
    g_a.cache[0] = 0;
    if (tc_http(g_port, method, path, h ? headers : NULL, body, n, &r) != 0) return -1;
    g_a.status = r.status;
    g_a.len = r.body.len < sizeof g_a.body ? (int)r.body.len : 0;
    if (g_a.len) memcpy(g_a.body, r.body.p, (size_t)g_a.len);
    if (!tc_header(&r, "cache-control", g_a.cache, sizeof g_a.cache)) g_a.cache[0] = 0;
    tc_response_free(&r);
    return g_a.status;
}

#define OCTETS "application/octet-stream"

// The last answer is exactly the refusal (status, why, code).
static bool refused_as(int status, int why, int code) {
    if (g_a.status != status) return false;
    AskRefusal r;
    if (g_a.len != ASK_REFUSAL_BYTES || ask_refusal_decode(g_a.body, g_a.len, &r) != ASK_OK) return false;
    return r.why == why && (code == -1 || r.code == code);
}

// One matrix cell: the last answer must be the refusal; prints the cell when it is not.
#define CELL_REFUSED(label, who, st_, why_, code_) do { \
    g_cells++; \
    bool ok_ = refused_as((st_), (why_), (code_)); \
    if (!ok_) fprintf(stderr, "cell %s as %s: got %d (%d bytes), want %d why %d\n", (label), (who), g_a.status, g_a.len, (st_), (why_)); \
    T_CHECK(ok_); \
} while (0)

#define CELL_STATUS(label, who, st_) do { \
    g_cells++; \
    if (g_a.status != (st_)) fprintf(stderr, "cell %s as %s: got %d, want %d\n", (label), (who), g_a.status, (st_)); \
    T_EQ_INT(g_a.status, (st_)); \
} while (0)

// ---- identities --------------------------------------------------------------------------

typedef struct {
    char name[24];
    char id[64];
    char auth[NW_ACCESS_MAX + 16];   // "Bearer <access token>"
    char refresh[NW_REFRESH_MAX + 1];
} user;

static int account(user *u, int kind, const char *name, const char *password) {
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = (uint8_t)kind;
    q.name_len = (uint8_t)strlen(name);
    memcpy(q.name, name, q.name_len);
    q.password_len = (uint8_t)strlen(password);
    memcpy(q.password, password, q.password_len);
    uint8_t body[256];
    int n = nw_request_encode(&q, body, sizeof body);
    if (n < 0 || http("POST", kind == NW_SIGNUP ? "/v1/auth/signup" : "/v1/auth/signin", NULL, body, (size_t)n, OCTETS) != 200) return -1;
    static NwSession s;
    if (nw_session_decode(g_a.body, g_a.len, &s) != NW_OK) return -1;
    snprintf(u->name, sizeof u->name, "%s", name);
    snprintf(u->id, sizeof u->id, "%.*s", (int)s.user_id_len, s.user_id);
    snprintf(u->auth, sizeof u->auth, "Bearer %.*s", (int)s.access_len, s.access);
    snprintf(u->refresh, sizeof u->refresh, "%.*s", (int)s.refresh_len, s.refresh);
    return 0;
}

static int refresh_status(const char *token) {
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_REFRESH;
    q.refresh_len = (uint8_t)strlen(token);
    memcpy(q.refresh, token, q.refresh_len);
    uint8_t body[128];
    int n = nw_request_encode(&q, body, sizeof body);
    return http("POST", "/v1/auth/refresh", NULL, body, (size_t)n, OCTETS);
}

// A token minted here: `secret` signs claims for `sub`, expiring at `exp`.
static void mint(char *auth, size_t cap, const char *secret, const char *sub, const char *name, int64_t iat, int64_t exp) {
    cb_jwt_claims c;
    memset(&c, 0, sizeof c);
    snprintf(c.sub, sizeof c.sub, "%s", sub);
    snprintf(c.email, sizeof c.email, "x@foolish.cards");
    snprintf(c.username, sizeof c.username, "%s", name);
    snprintf(c.session_id, sizeof c.session_id, "00000000-0000-4000-8000-000000000001");
    c.iat = iat;
    c.exp = exp;
    char tok[CB_JWT_MAX];
    T_CHECK(cb_jwt_mint(secret, &c, "http://127.0.0.1/auth/v1", tok, sizeof tok) > 0);
    snprintf(auth, cap, "Bearer %s", tok);
}

// The bad identities: each must be refused as nobody wherever a caller is needed.
enum { BAD_NONE, BAD_GARBAGE, BAD_EMPTY, BAD_EXPIRED, BAD_FORGED, BAD_ALG_NONE, BAD_SWAPPED, BAD_ADMIN, BAD_BASIC, N_BAD };
static const char *const BAD_NAMES[N_BAD] = { "no token", "garbage", "empty bearer", "expired", "forged", "alg none",
                                              "payload swapped", "admin token", "basic auth" };
static char g_bad[N_BAD][NW_ACCESS_MAX + 16];

static void make_bad(const user *victim, const user *other) {
    int64_t now = cb_now_ms() / 1000;
    g_bad[BAD_NONE][0] = 0;
    snprintf(g_bad[BAD_GARBAGE], sizeof g_bad[0], "Bearer not.a.token");
    snprintf(g_bad[BAD_EMPTY], sizeof g_bad[0], "Bearer ");
    // An hour past expiry: far outside the verifier's 30 s of skew.
    mint(g_bad[BAD_EXPIRED], sizeof g_bad[0], SECRET, victim->id, victim->name, now - 7200, now - 3600);
    mint(g_bad[BAD_FORGED], sizeof g_bad[0], OTHER_SECRET, victim->id, victim->name, now, now + 3600);
    // alg none: the victim's own payload, an unsigned header, no signature.
    const char *tok = victim->auth + 7;
    const char *dot1 = strchr(tok, '.'), *dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;
    static const char none_hdr[] = "{\"alg\":\"none\",\"typ\":\"JWT\"}";
    char h64[128];
    cb_b64url_encode((const uint8_t *)none_hdr, strlen(none_hdr), h64, sizeof h64);
    if (dot1 && dot2) snprintf(g_bad[BAD_ALG_NONE], sizeof g_bad[0], "Bearer %s.%.*s.", h64, (int)(dot2 - dot1 - 1), dot1 + 1);
    // Swapped: the other user's payload under the victim's header and signature.
    const char *ot = other->auth + 7;
    const char *o1 = strchr(ot, '.'), *o2 = o1 ? strchr(o1 + 1, '.') : NULL;
    if (dot1 && dot2 && o1 && o2)
        snprintf(g_bad[BAD_SWAPPED], sizeof g_bad[0], "Bearer %.*s.%.*s%s", (int)(dot1 - tok), tok, (int)(o2 - o1 - 1), o1 + 1, dot2);
    snprintf(g_bad[BAD_ADMIN], sizeof g_bad[0], "Bearer " ADMIN);
    snprintf(g_bad[BAD_BASIC], sizeof g_bad[0], "Basic %s", victim->auth + 7);
}

static const char *bad_auth(int i) { return g_bad[i][0] ? g_bad[i] : NULL; }

// ---- what an answer may show: the kernel decodes it ----------------------------------------

typedef struct { int n; uint8_t ids[64]; } cardset;   // card ids 0..51

static void cardset_add(cardset *s, Card c) {
    if (c.suit < 0 || c.value < 1 || c.value > 13 || s->n >= 64) return;
    s->ids[s->n++] = (uint8_t)card_to_id(c);
}

static bool cardset_has(const cardset *s, int id) {
    for (int i = 0; i < s->n; i++) if (s->ids[i] == id) return true;
    return false;
}

static bool is_real(Card c) { return c.suit >= 0; }

// A masked board as `viewer` (a seat, or -1 for the spectators) was sent it:
// every other seat's hand and every deck card but the face-up trump are
// hidden, and the viewer's own hand is all real. `own` (optional) receives the
// viewer's hand, `seen` (optional) every real card the board shows.
static bool board_hides(const uint8_t *p, int len, int viewer, cardset *own, cardset *seen, const char *what) {
    static Game g;
    if (len <= 0 || state_get(&g, p, len, 0) < 0) {
        fprintf(stderr, "%s: the board does not decode\n", what);
        return false;
    }
    g_bodies_checked++;
    bool ok = true;
    for (int s = 0; s < g.num_players; s++) {
        const Player *pl = &g.players[s];
        for (int i = 0; i < pl->hand_count && i < MAX_HAND_SIZE; i++) {
            bool real = is_real(pl->hand[i]);
            if (s == viewer && !real) { fprintf(stderr, "%s: the viewer's own card %d is hidden\n", what, i); ok = false; }
            if (s != viewer && real) {
                fprintf(stderr, "%s: seat %d's card %d (id %d) is shown to viewer %d\n", what, s, i, card_to_id(pl->hand[i]), viewer);
                ok = false;
            }
            if (s == viewer && own) cardset_add(own, pl->hand[i]);
            if (seen) cardset_add(seen, pl->hand[i]);
        }
    }
    for (int i = 0; i < g.deck_count && i < MAX_DECK; i++) {
        if (!is_real(g.deck[i])) continue;
        if (!(g.has_flipped && card_eq(g.deck[i], g.flipped))) {
            fprintf(stderr, "%s: deck card %d (id %d) is shown\n", what, i, card_to_id(g.deck[i]));
            ok = false;
        }
        if (seen) cardset_add(seen, g.deck[i]);
    }
    if (seen) {
        if (g.has_flipped) cardset_add(seen, g.flipped);
        for (int b = 0; b < g.num_battles && b < MAX_BATTLES; b++) {
            cardset_add(seen, g.table_battles[b].attack);
            cardset_add(seen, g.table_battles[b].defense);
        }
    }
    return ok;
}

// An envelope for `want_seat` (-1: the spectators'): its header names that
// seat, and its board hides as board_hides says.
static bool envelope_hides(const uint8_t *p, int len, int want_seat, cardset *own, cardset *seen, uint32_t *version, const char *what) {
    EnvHeader h;
    if (!env_header_read(p, len, &h)) { fprintf(stderr, "%s: the envelope does not read\n", what); return false; }
    if (h.seat != want_seat) { fprintf(stderr, "%s: an envelope for seat %d, want %d\n", what, h.seat, want_seat); return false; }
    if (version) *version = h.version;
    return board_hides(p + h.state_at, h.state_len, h.seat, own, seen, what);
}

typedef struct { int viewer; bool ok; const char *what; } cast_check;

static void cast_event(void *ud, int index, const EvwRead *ev) {
    cast_check *c = ud;
    if ((ev->type == EVW_T_DEAL || ev->type == EVW_T_REFILL) && ev->seat != c->viewer)
        for (int i = 0; i < ev->n_cards; i++)
            if (ev->cards_wire[i] != WIRE_HIDDEN) {
                fprintf(stderr, "%s: event %d deals card %d to seat %d, shown to viewer %d\n", c->what, index, ev->cards_wire[i], ev->seat, c->viewer);
                c->ok = false;
            }
    if (ev->snap_len && !board_hides(ev->snap, ev->snap_len, c->viewer, NULL, NULL, c->what)) c->ok = false;
}

// A cast (NW_WS_CAST's blob) for `viewer`.
static bool cast_hides(const uint8_t *p, int len, int viewer, const char *what) {
    uint32_t v;
    int off = evwire_cast_header_get(p, len, &v);
    if (off < 0) { fprintf(stderr, "%s: no cast header\n", what); return false; }
    int seq_len, flags, block;
    if (evwire_as3_split(p + off, len - off, &seq_len, &flags, &block) != 0) { fprintf(stderr, "%s: not an as3 push\n", what); return false; }
    cast_check c = { viewer, true, what };
    EvwHeader h;
    const unsigned char *fin = NULL;
    int fin_len = 0;
    if (evwire_read(p + off, seq_len, &h, &fin, &fin_len, cast_event, &c) < 0) { fprintf(stderr, "%s: the events do not read\n", what); return false; }
    if (h.viewer != viewer) { fprintf(stderr, "%s: a cast for viewer %d, want %d\n", what, h.viewer, viewer); return false; }
    if (fin_len && !board_hides(fin, fin_len, viewer, NULL, NULL, what)) c.ok = false;
    return c.ok;
}

// ---- the stream ---------------------------------------------------------------------------

typedef struct {
    const char *who;
    int  fd;
    int  seat;            // what an envelope or cast to this socket must be for (-1 spectator)
    char gid[16];         // the game it may hear about ("" none)
    bool may_chat;
    int  n_ack, n_ack_refused, last_why, last_seated;
    int  n_view, n_cast, n_chat, n_pong, n_other_game;
    bool leak;            // a frame it should never have had
    bool closed;          // the server closed it (a CLOSE frame)
    int  close_code;
    uint32_t last_cast_v;
} sock;

static void ws_frame(sock *s, const NwFrame *f) {
    uint8_t raw[NW_ACCESS_MAX + 128];
    int n = nw_frame_encode(f, raw, sizeof raw);
    T_CHECK(n > 0);
    if (n > 0) T_EQ_INT(tc_ws_send(s->fd, CB_WS_OP_BIN, raw, (size_t)n), 0);
}

static void ws_sub(sock *s, int kind, const char *token, int what, const char *gid, uint16_t ref) {
    static NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = (uint8_t)kind;
    f.ref = ref;
    f.what = (uint8_t)what;
    if (gid) { f.gid_len = (uint8_t)strlen(gid); memcpy(f.gid, gid, f.gid_len); }
    if (token) { f.token_len = (uint16_t)strlen(token); memcpy(f.token, token, f.token_len); }
    ws_frame(s, &f);
}

// Reads what arrives within timeout_ms (then 50 ms between frames), judging each.
static void ws_pump(sock *s, int timeout_ms) {
    cb_buf b;
    cb_buf_init(&b);
    static NwFrame f;
    int op;
    while (s->fd >= 0 && (op = tc_ws_recv(s->fd, timeout_ms, &b)) >= 0) {
        timeout_ms = 50;
        if (op == CB_WS_OP_CLOSE) {
            s->closed = true;
            s->close_code = b.len >= 2 ? (b.p[0] << 8 | b.p[1]) : 0;
            break;
        }
        if (nw_frame_decode(b.p, (int)b.len, &f) != NW_OK) { s->leak = true; cb_buf_reset(&b); continue; }
        char gid[NW_GID_MAX + 1];
        snprintf(gid, sizeof gid, "%.*s", (int)f.gid_len, f.gid);
        char what[96];
        snprintf(what, sizeof what, "%s's %s", s->who, f.kind == NW_WS_VIEW ? "view push" : "cast");
        switch (f.kind) {
        case NW_WS_ACK:
            s->n_ack++;
            s->last_why = f.why;
            s->last_seated = f.seated;
            if (f.why) s->n_ack_refused++;
            break;
        case NW_WS_PONG: s->n_pong++; break;
        case NW_WS_CATCHUP:   // a subscription's catch-up: the caller's view, as a VIEW is
        case NW_WS_VIEW:
            s->n_view++;
            if (!s->gid[0] || strcmp(gid, s->gid) != 0) { s->n_other_game++; s->leak = true; break; }
            if (f.blob.len && !envelope_hides(f.blob.p, (int)f.blob.len, s->seat, NULL, NULL, NULL, what)) s->leak = true;
            break;
        case NW_WS_CAST:
            s->n_cast++;
            if (!s->gid[0] || strcmp(gid, s->gid) != 0) { s->n_other_game++; s->leak = true; break; }
            if (!cast_hides(f.blob.p, (int)f.blob.len, s->seat, what)) s->leak = true;
            s->last_cast_v = f.version;
            break;
        case NW_WS_CHAT:
            s->n_chat++;
            if (!s->may_chat || strcmp(gid, s->gid) != 0) s->leak = true;
            break;
        default: s->leak = true; break;
        }
        cb_buf_reset(&b);
    }
    cb_buf_free(&b);
}

static void ws_open(sock *s, const char *who, int seat, const char *gid, bool may_chat) {
    memset(s, 0, sizeof *s);
    s->who = who;
    s->seat = seat;
    snprintf(s->gid, sizeof s->gid, "%s", gid ? gid : "");
    s->may_chat = may_chat;
    s->fd = tc_ws_open(g_port, "/v1/stream?proto=nw1", NULL);
    T_CHECK(s->fd >= 0);
}

// Sends one subscription and waits for its ACK (other frames are judged on the way).
static void ws_ask(sock *s, int kind, const char *token, int what, const char *gid) {
    int before = s->n_ack;
    ws_sub(s, kind, token, what, gid, (uint16_t)(before + 1));
    int64_t t0 = mono_ms();
    while (s->n_ack == before && mono_ms() - t0 < 5000) ws_pump(s, 200);
}

// ---- the game ------------------------------------------------------------------------------

static int meta(const user *u, int kind, const char *gid, void (*fill)(TableAsk *, const void *), const void *ud) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = (uint8_t)kind;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    if (fill) fill(&a, ud);
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    T_CHECK(n > 0);
    return http("POST", "/v1/meta", u ? u->auth : NULL, body, (size_t)(n > 0 ? n : 0), OCTETS);
}

static int act(const char *auth, const char *gid, const uint8_t *wire, int wn, uint32_t intent) {
    uint8_t buf[256];
    int gl = (int)strlen(gid);
    memcpy(buf, gid, (size_t)gl);
    memcpy(buf + gl, wire, (size_t)wn);
    int n = table_request_encode(buf, sizeof buf, gl, wn, true, intent);
    T_CHECK(n > 0);
    return http("POST", "/v1/action", auth, buf, (size_t)(n > 0 ? n : 0), OCTETS);
}

typedef struct { ClientSlot slot; ClientTable c; LegalMoves moves; } mover;
static mover g_mv;

// A legal move for the envelope's seat, judged again by client_validate; 0 when none.
static int choose(const uint8_t *env, int len, uint8_t *wire, int cap, uint32_t *rng, int *status, uint32_t *version) {
    client_init(&g_mv.c, &g_mv.slot);
    if (client_adopt_envelope(&g_mv.c, env, len) < 0) return 0;
    *status = g_mv.c.view.status;
    *version = g_mv.c.view.version;
    int seat = g_mv.c.view.my_seat;
    if (seat < 0 || g_mv.c.view.status != GAME_STATUS_PLAYING) return 0;
    calculate_legal_moves(g_mv.c.g, seat, &g_mv.moves);
    int start = g_mv.moves.n ? (int)((rng_lcg32(rng) >> 8) % (uint32_t)g_mv.moves.n) : 0;
    for (int k = 0; k < g_mv.moves.n; k++) {
        const LegalMove *m = &g_mv.moves.moves[(start + k) % g_mv.moves.n];
        if (m->type == MOVE_WAIT) continue;
        AwireAction a;
        memset(&a, 0, sizeof a);
        a.kind = m->type;
        a.n = m->n_cards;
        for (int i = 0; i < m->n_cards; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
        int n = awire_encode(&a, wire, cap);
        if (n > 0 && client_validate(&g_mv.c, &g_mv.c.view, wire, n) == 0) return n;
    }
    return 0;
}

// The game id an envelope names.
static void gid_of_envelope(const uint8_t *env, int len, char *gid, size_t cap) {
    client_init(&g_mv.c, &g_mv.slot);
    gid[0] = 0;
    if (client_adopt_envelope(&g_mv.c, env, len) >= 0) snprintf(gid, cap, "%.*s", (int)g_mv.c.gid_len, g_mv.c.gid);
}

// u's view of gid over GET /v1/games/<gid>, judged as seat `seat`.
static bool view_of(const user *u, const char *gid, int seat, cardset *own, cardset *seen, uint32_t *version) {
    char path[64];
    snprintf(path, sizeof path, "/v1/games/%s", gid);
    if (http("GET", path, u->auth, NULL, 0, NULL) != 200) return false;
    uint32_t v;
    NwBlob env;
    if (nw_view_decode(g_a.body, g_a.len, &v, &env) != NW_OK) return false;
    char what[64];
    snprintf(what, sizeof what, "%s's GET game", u->name);
    return envelope_hides(env.p, (int)env.len, seat, own, seen, version, what);
}

// Every game in u's list is one u sits at (judged as `seat` for `gid`, which
// must be there when want_gid) and nothing else.
static bool games_list_is_own(const user *u, const char *gid, int seat, bool want_gid) {
    if (http("GET", "/v1/games", u->auth, NULL, 0, NULL) != 200) return false;
    int n = nw_list_open(g_a.body, g_a.len, NW_GAMES);
    if (n < 0) return false;
    bool found = false, ok = true;
    for (int i = 0; i < n; i++) {
        uint32_t v;
        NwBlob env;
        if (nw_get_game(g_a.body, g_a.len, i, &v, &env) != NW_OK) return false;
        char g[16];
        static uint8_t copy[1 << 16];
        int el = env.len < sizeof copy ? (int)env.len : 0;
        memcpy(copy, env.p, (size_t)el);
        gid_of_envelope(copy, el, g, sizeof g);
        if (strcmp(g, gid) == 0) {
            found = true;
            char what[64];
            snprintf(what, sizeof what, "%s's games list", u->name);
            EnvHeader h;
            if (!env_header_read(copy, el, &h) || h.seat < 0) ok = false;
            else if (!envelope_hides(copy, el, seat, NULL, NULL, NULL, what)) ok = false;
        } else if (!want_gid) {
            ok = false;   // a game this user was never at
        }
    }
    return ok && found == want_gid;
}

// ---- the scenario --------------------------------------------------------------------------

static sock g_deleted_sock;  // E's stream socket, kept open across E's deletion
static user A, B, E, C, D;   // A hosts, B and E sit, C watches, D is elsewhere
static char G[16];           // the game
static char G2[16];          // D's own game, for the "another user" reads

static void fill_exit_player(TableAsk *a, const void *ud) {
    const char *id = ud;
    a->player_len = (uint8_t)strlen(id);
    memcpy(a->player, id, a->player_len);
}

static void fill_title(TableAsk *a, const void *ud) {
    const char *t = ud;
    a->title_len = (uint16_t)strlen(t);
    memcpy(a->title, t, a->title_len);
}

static void setup_users(void) {
    T_EQ_INT(account(&A, NW_SIGNUP, "AUTHZ-HOST", PASSWORD), 0);
    T_EQ_INT(account(&B, NW_SIGNUP, "AUTHZ-SEAT", PASSWORD), 0);
    T_EQ_INT(account(&E, NW_SIGNUP, "AUTHZ-GONE", PASSWORD), 0);
    T_EQ_INT(account(&C, NW_SIGNUP, "AUTHZ-WATCH", PASSWORD), 0);
    T_EQ_INT(account(&D, NW_SIGNUP, "AUTHZ-ELSE", PASSWORD), 0);
    make_bad(&A, &D);
    // D's own lobby: what D's reads may show, and what nobody else's may.
    T_EQ_INT(http("POST", "/v1/create", D.auth, NULL, 0, OCTETS), 200);
    gid_of_envelope(g_a.body, g_a.len, G2, sizeof G2);
    T_EQ_INT(strlen(G2), 6);
    // A's game: B and E join, nobody has started.
    T_EQ_INT(http("POST", "/v1/create", A.auth, NULL, 0, OCTETS), 200);
    gid_of_envelope(g_a.body, g_a.len, G, sizeof G);
    T_EQ_INT(strlen(G), 6);
    T_EQ_INT(meta(&B, ASK_JOIN, G, NULL, NULL), 200);
    T_EQ_INT(meta(&E, ASK_JOIN, G, NULL, NULL), 200);
}

// Every route that needs a caller, asked by every bad identity: refused as
// nobody, in exactly ten bytes, and nothing changed.
static void bad_identities_everywhere(void) {
    char p_game[48], p_chat[48], p_replay[48];
    snprintf(p_game, sizeof p_game, "/v1/games/%s", G);
    snprintf(p_chat, sizeof p_chat, "/v1/chat/%s", G);
    snprintf(p_replay, sizeof p_replay, "/v1/replay/%s", G);
    NwRequest q;
    uint8_t chat[64], signout[16], pw[96];
    memset(&q, 0, sizeof q); q.kind = NW_CHAT_SEND; q.message_len = 2; memcpy(q.message, "hi", 2);
    int chat_n = nw_request_encode(&q, chat, sizeof chat);
    memset(&q, 0, sizeof q); q.kind = NW_SIGNOUT; q.scope = NW_SCOPE_GLOBAL;
    int signout_n = nw_request_encode(&q, signout, sizeof signout);
    memset(&q, 0, sizeof q); q.kind = NW_PASSWORD; q.password_len = 12; memcpy(q.password, "stolen-pass1", 12);
    int pw_n = nw_request_encode(&q, pw, sizeof pw);
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = ASK_START;
    a.gid_len = 6;
    memcpy(a.gid, G, 6);
    uint8_t ask[ASK_BYTES_MAX];
    int ask_n = ask_encode(&a, ask, sizeof ask);
    uint8_t wire[8] = { 0 };
    for (int i = 0; i < N_BAD; i++) {
        const char *who = BAD_NAMES[i], *au = bad_auth(i);
        // The binary surface's reads and its account calls: 401 AUTH.
        http("GET", "/v1/games", au, NULL, 0, NULL);               CELL_REFUSED("GET /v1/games", who, 401, ASK_WHY_AUTH, 0);
        http("GET", p_game, au, NULL, 0, NULL);                    CELL_REFUSED("GET /v1/games/<id>", who, 401, ASK_WHY_AUTH, 0);
        http("GET", p_chat, au, NULL, 0, NULL);                    CELL_REFUSED("GET /v1/chat/<id>", who, 401, ASK_WHY_AUTH, 0);
        http("POST", p_chat, au, chat, (size_t)chat_n, OCTETS);    CELL_REFUSED("POST /v1/chat/<id>", who, 401, ASK_WHY_AUTH, 0);
        http("GET", "/v1/history", au, NULL, 0, NULL);             CELL_REFUSED("GET /v1/history", who, 401, ASK_WHY_AUTH, 0);
        http("GET", p_replay, au, NULL, 0, NULL);                  CELL_REFUSED("GET /v1/replay/<id>", who, 401, ASK_WHY_AUTH, 0);
        http("POST", "/v1/auth/signout", au, signout, (size_t)signout_n, OCTETS); CELL_REFUSED("POST /v1/auth/signout", who, 401, ASK_WHY_AUTH, 0);
        http("POST", "/v1/auth/password", au, pw, (size_t)pw_n, OCTETS);         CELL_REFUSED("POST /v1/auth/password", who, 401, ASK_WHY_AUTH, 0);
        http("POST", "/v1/account/delete", au, NULL, 0, OCTETS);   CELL_REFUSED("POST /v1/account/delete", who, 401, ASK_WHY_AUTH, 0);
        // The writes kept the edge functions' statuses: 400 AUTH.
        http("POST", "/v1/create", au, NULL, 0, OCTETS);           CELL_REFUSED("POST /v1/create", who, 400, ASK_WHY_AUTH, 0);
        http("POST", "/v1/meta", au, ask, (size_t)ask_n, OCTETS);  CELL_REFUSED("POST /v1/meta", who, 400, ASK_WHY_AUTH, 0);
        act(au, G, wire, 2, 1);                                    CELL_REFUSED("POST /v1/action", who, 400, ASK_WHY_AUTH, 0);
        // The console's API: a user's token, a bad one or none is 401 (the
        // admin token is the console's own: console_takes_only_the_admin_token).
        if (i == BAD_ADMIN) continue;
        http("GET", "/admin/api/overview", au, NULL, 0, NULL);     CELL_STATUS("GET /admin/api/overview", who, 401);
        T_CHECK(g_a.len < 200 && !memmem(g_a.body, (size_t)g_a.len, A.id, strlen(A.id)));
    }
    // Nothing a refused call asked for happened: A's password and session stand.
    user again;
    T_EQ_INT(account(&again, NW_SIGNIN, A.name, PASSWORD), 0);
    T_EQ_INT(refresh_status(A.refresh), 200);   // global sign-out was refused, so the token still turns
    static NwSession s;
    T_EQ_INT(nw_session_decode(g_a.body, g_a.len, &s), NW_OK);
    snprintf(A.refresh, sizeof A.refresh, "%.*s", (int)s.refresh_len, s.refresh);
    snprintf(A.auth, sizeof A.auth, "Bearer %.*s", (int)s.access_len, s.access);
    T_EQ_INT(http("GET", "/v1/games", A.auth, NULL, 0, NULL), 200);
    T_EQ_INT(nw_list_open(g_a.body, g_a.len, NW_GAMES), 1);   // A's game still there, nothing created as A
}

// The public reads answer every identity alike: they never read the caller.
static void public_reads_ignore_the_caller(void) {
    char unfurl[4096];
    snprintf(unfurl, sizeof unfurl, "/m/%s", UNFURL_GOLDENS[0].payload);
    const char *paths[] = { "/health", "/v1/bots", "/v1/leaderboard", unfurl, "/admin" };
    for (size_t p = 0; p < sizeof paths / sizeof paths[0]; p++) {
        static uint8_t first[1 << 17];
        int first_len = -1;
        const char *auths[N_BAD + 3];
        const char *names[N_BAD + 3];
        int na = 0;
        for (int i = 0; i < N_BAD; i++) { auths[na] = bad_auth(i); names[na++] = BAD_NAMES[i]; }
        auths[na] = A.auth; names[na++] = "host";
        auths[na] = D.auth; names[na++] = "another user";
        auths[na] = "Bearer " ADMIN; names[na++] = "admin";
        for (int i = 0; i < na; i++) {
            http("GET", paths[p], auths[i], NULL, 0, NULL);
            CELL_STATUS(paths[p], names[i], 200);
            if (strcmp(paths[p], "/health") == 0) continue;   // carries uptime
            if (first_len < 0) { T_CHECK(g_a.len < (int)sizeof first); first_len = g_a.len < (int)sizeof first ? g_a.len : 0; memcpy(first, g_a.body, (size_t)first_len); }
            T_EQ_MEM(g_a.body, g_a.len, first, first_len);
        }
    }
}

// A signed-in user who is not at the game: reads show nothing of anyone else's,
// writes are refused, and the game reads as the spectators'.
static void another_user_sees_only_their_own(void) {
    char path[64];
    const char *who = "another user";
    // The games list: D's own lobby only.
    T_CHECK(games_list_is_own(&D, G2, 0, true));
    T_EQ_INT(http("GET", "/v1/games", D.auth, NULL, 0, NULL), 200);
    T_EQ_INT(nw_list_open(g_a.body, g_a.len, NW_GAMES), 1);
    g_cells++;
    // A's game, as the spectators see it.
    uint32_t v;
    T_CHECK(view_of(&D, G, -1, NULL, NULL, &v));
    g_cells++;
    // Its chat: empty to D, after A wrote one.
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_CHAT_SEND;
    q.message_len = 13;
    memcpy(q.message, "seats-only-hi", 13);
    uint8_t body[64];
    int n = nw_request_encode(&q, body, sizeof body);
    snprintf(path, sizeof path, "/v1/chat/%s", G);
    T_EQ_INT(http("POST", path, A.auth, body, (size_t)n, OCTETS), 204);
    T_EQ_INT(http("GET", path, D.auth, NULL, 0, NULL), 200);
    CELL_STATUS("GET /v1/chat/<id>", who, 200);
    T_EQ_INT(nw_list_open(g_a.body, g_a.len, NW_CHATS), 0);
    T_CHECK(!memmem(g_a.body, (size_t)g_a.len, "seats-only-hi", 13));
    http("POST", path, D.auth, body, (size_t)n, OCTETS);
    CELL_REFUSED("POST /v1/chat/<id>", who, 403, ASK_WHY_FORBIDDEN, 0);
    T_EQ_INT(http("GET", path, B.auth, NULL, 0, NULL), 200);
    T_EQ_INT(nw_list_open(g_a.body, g_a.len, NW_CHATS), 1);   // the seat sees it; D's was never stored
    // History and replays: none of A's.
    http("GET", "/v1/history", D.auth, NULL, 0, NULL);
    CELL_STATUS("GET /v1/history", who, 200);
    T_EQ_INT(nw_list_open(g_a.body, g_a.len, NW_HISTORY), 0);
    snprintf(path, sizeof path, "/v1/replay/%s", G);
    http("GET", path, D.auth, NULL, 0, NULL);
    CELL_REFUSED("GET /v1/replay/<id>", who, 404, ASK_WHY_NOT_FOUND, 0);
    // Every lobby edit of A's table: refused by the kernel, nothing committed.
    const int asks[] = { ASK_START, ASK_ADD_BOT, ASK_SET_RULES, ASK_CONTINUE, ASK_REARRANGE_HAND, ASK_REARRANGE_PLAYERS };
    for (size_t i = 0; i < sizeof asks / sizeof asks[0]; i++) {
        meta(&D, asks[i], G, NULL, NULL);
        g_cells++;
        T_CHECK(g_a.status == 400);
        T_CHECK(g_a.len == ASK_REFUSAL_BYTES);
    }
    meta(&D, ASK_UPDATE_NAME, G, fill_title, "pwned");
    g_cells++;
    T_CHECK(g_a.status == 400 && g_a.len == ASK_REFUSAL_BYTES);
    meta(&D, ASK_EXIT, G, fill_exit_player, B.id);
    g_cells++;
    T_CHECK(g_a.status == 400 && g_a.len == ASK_REFUSAL_BYTES);
    // A bump is a read: the spectators' envelope.
    T_EQ_INT(meta(&D, ASK_BUMP, G, NULL, NULL), 200);
    T_CHECK(envelope_hides(g_a.body, g_a.len, -1, NULL, NULL, NULL, "D's bump"));
    g_cells++;
    // (A seated player MAY remove another human and retitle the table: the
    // kernel's lobby policy, table.h "lobby edits". Not exercised here.)
    // The table is as it was: three seats, still in its lobby, A's title.
    T_CHECK(view_of(&A, G, 0, NULL, NULL, &v));
    client_init(&g_mv.c, &g_mv.slot);
    uint32_t vv;
    NwBlob env;
    T_EQ_INT(nw_view_decode(g_a.body, g_a.len, &vv, &env), NW_OK);
    T_CHECK(client_adopt_envelope(&g_mv.c, env.p, (int)env.len) >= 0);
    T_EQ_INT(g_mv.c.view.num_players, 3);
    T_EQ_INT(g_mv.c.view.status, GAME_STATUS_WAITING);
    // D's own lobby is D's: A cannot edit it either.
    meta(&A, ASK_START, G2, NULL, NULL);
    g_cells++;
    T_CHECK(g_a.status == 400 && g_a.len == ASK_REFUSAL_BYTES);
}

// Admin: the console's every call with the admin token, a wrong one, a user's.
static void console_takes_only_the_admin_token(void) {
    static const struct { const char *method, *path; } api[] = {
        { "GET", "/admin/api/overview" }, { "GET", "/admin/api/collections" }, { "GET", "/admin/api/browse?c=games" },
        { "POST", "/admin/api/query" }, { "GET", "/admin/api/users" }, { "POST", "/admin/api/users/delete" },
        { "GET", "/admin/api/games" }, { "GET", "/admin/api/game?id=000000" }, { "GET", "/admin/api/bots" },
        { "GET", "/admin/api/logs" }, { "GET", "/admin/api/config" }, { "GET", "/admin/api/backups" },
        { "POST", "/admin/api/backups" }, { "GET", "/admin/api/nope" },
    };
    const char *wrong[] = { "Bearer " ADMIN "x", "Bearer authz-admin-token", "Bearer", "Basic " ADMIN, NULL, A.auth };
    const char *wrong_names[] = { "admin token plus a byte", "a prefix of it", "empty bearer", "basic", "no token", "a user" };
    for (size_t r = 0; r < sizeof api / sizeof api[0]; r++) {
        for (size_t w = 0; w < sizeof wrong / sizeof wrong[0]; w++) {
            http(api[r].method, api[r].path, wrong[w], "{}", 2, "application/json");
            CELL_STATUS(api[r].path, wrong_names[w], 401);
            T_CHECK(!memmem(g_a.body, (size_t)g_a.len, A.id, strlen(A.id)));
        }
        // The admin token: never a 401. (Not the backup: it would write a file
        // beside the server's working directory.)
        if (strcmp(api[r].method, "POST") == 0 && strstr(api[r].path, "backups")) continue;
        http(api[r].method, api[r].path, "Bearer " ADMIN, "{}", 2, "application/json");
        g_cells++;
        T_CHECK(g_a.status != 401 && g_a.status > 0);
    }
    http("GET", "/admin/api/users", "Bearer " ADMIN, NULL, 0, NULL);
    T_EQ_INT(g_a.status, 200);
    T_CHECK(memmem(g_a.body, (size_t)g_a.len, A.id, strlen(A.id)) != NULL);
}

// The stream: every frame kind, from every identity.
static void stream_frames(void) {
    sock s;
    // A bare SUB before any AUTH: refused AUTH.
    for (int what = NW_SUB_GAMES; what <= NW_SUB_CHAT; what++) {
        ws_open(&s, "nobody", -1, "", false);
        ws_ask(&s, NW_WS_SUB, NULL, what, what == NW_SUB_GAMES ? NULL : G);
        g_cells++;
        T_EQ_INT(s.last_why, ASK_WHY_AUTH);
        close(s.fd);
    }
    // SUB_AS and AUTH-then-SUB with each bad token: AUTH, for every subscription kind.
    for (int i = 0; i < N_BAD; i++) {
        const char *tok = g_bad[i][0] && strncmp(g_bad[i], "Bearer ", 7) == 0 ? g_bad[i] + 7 : g_bad[i];
        for (int what = NW_SUB_GAMES; what <= NW_SUB_CHAT; what++) {
            ws_open(&s, BAD_NAMES[i], -1, "", false);
            ws_ask(&s, NW_WS_SUB_AS, tok, what, what == NW_SUB_GAMES ? NULL : G);
            g_cells++;
            if (s.last_why != ASK_WHY_AUTH) fprintf(stderr, "stream SUB_AS %d as %s: why %d\n", what, BAD_NAMES[i], s.last_why);
            T_EQ_INT(s.last_why, ASK_WHY_AUTH);
            ws_sub(&s, NW_WS_AUTH, tok, 0, NULL, 0);
            ws_ask(&s, NW_WS_SUB, NULL, what, what == NW_SUB_GAMES ? NULL : G);
            g_cells++;
            T_EQ_INT(s.last_why, ASK_WHY_AUTH);
            close(s.fd);
        }
    }
    // Valid users: games feed always theirs; a game: seated hears its seat, anyone else the spectators';
    // chat: members only.
    struct { user *u; const char *who; int seat; bool member; } ids[] = {
        { &A, "host", 0, true }, { &B, "seated", 1, true }, { &C, "spectator", -1, false }, { &D, "another user", -1, false },
    };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        const char *tok = ids[i].u->auth + 7;
        ws_open(&s, ids[i].who, ids[i].seat, G, ids[i].member);
        ws_ask(&s, NW_WS_SUB_AS, tok, NW_SUB_GAMES, NULL);
        g_cells++;
        T_EQ_INT(s.last_why, 0);
        ws_ask(&s, NW_WS_SUB_AS, tok, NW_SUB_GAME, G);
        g_cells++;
        T_EQ_INT(s.last_why, 0);
        T_EQ_INT(s.last_seated, ids[i].member ? 1 : 0);
        ws_ask(&s, NW_WS_SUB_AS, tok, NW_SUB_CHAT, G);
        g_cells++;
        T_EQ_INT(s.last_why, ids[i].member ? 0 : ASK_WHY_FORBIDDEN);
        // A malformed game id and a games feed with a game id: MALFORMED.
        ws_ask(&s, NW_WS_SUB_AS, tok, NW_SUB_GAME, "../x");
        g_cells++;
        T_EQ_INT(s.last_why, ASK_WHY_MALFORMED);
        ws_ask(&s, NW_WS_SUB_AS, tok, NW_SUB_GAMES, G);
        g_cells++;
        T_EQ_INT(s.last_why, ASK_WHY_MALFORMED);
        // PING: PONG. UNSUB: nothing back.
        ws_sub(&s, NW_WS_PING, NULL, 0, NULL, 0);
        ws_sub(&s, NW_WS_UNSUB, NULL, NW_SUB_GAME, G, 0);
        ws_pump(&s, 500);
        g_cells++;
        T_EQ_INT(s.n_pong, 1);
        T_CHECK(!s.leak);
        close(s.fd);
    }
    // A server's frame sent by a client ends the socket, and nothing is answered.
    ws_open(&s, "server frame", -1, "", false);
    static NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_ACK;
    ws_frame(&s, &f);
    ws_pump(&s, 1000);
    g_cells++;
    T_EQ_INT(s.n_ack + s.n_pong + s.n_view + s.n_cast + s.n_chat, 0);
    close(s.fd);
    // After AUTH as one user, another's token in AUTH ends the first's subscriptions.
    ws_open(&s, "switch", 0, G, true);
    ws_ask(&s, NW_WS_SUB_AS, A.auth + 7, NW_SUB_CHAT, G);
    T_EQ_INT(s.last_why, 0);
    ws_sub(&s, NW_WS_AUTH, D.auth + 7, 0, NULL, 0);
    ws_pump(&s, 300);
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_CHAT_SEND;
    q.message_len = 5;
    memcpy(q.message, "after", 5);
    uint8_t body[32];
    int n = nw_request_encode(&q, body, sizeof body);
    char path[48];
    snprintf(path, sizeof path, "/v1/chat/%s", G);
    T_EQ_INT(http("POST", path, B.auth, body, (size_t)n, OCTETS), 204);
    ws_pump(&s, 1000);
    g_cells++;
    T_EQ_INT(s.n_chat, 0);   // the chat subscription went with A
    close(s.fd);
}

// Three humans play to the end; every body anyone receives is judged.
static void hidden_card_game(void) {
    sock sA, sB, sE, sC, sD;
    ws_open(&sA, "host", 0, G, true);
    ws_open(&sB, "seated B", 1, G, true);
    ws_open(&sE, "seated E", 2, G, true);
    ws_open(&sC, "spectator", -1, G, false);
    ws_open(&sD, "another user", -1, "", false);
    ws_ask(&sA, NW_WS_SUB_AS, A.auth + 7, NW_SUB_GAME, G);
    ws_ask(&sB, NW_WS_SUB_AS, B.auth + 7, NW_SUB_GAMES, NULL);
    ws_ask(&sE, NW_WS_SUB_AS, E.auth + 7, NW_SUB_GAME, G);
    ws_ask(&sE, NW_WS_SUB_AS, E.auth + 7, NW_SUB_CHAT, G);
    ws_ask(&sC, NW_WS_SUB_AS, C.auth + 7, NW_SUB_GAME, G);
    ws_ask(&sD, NW_WS_SUB_AS, D.auth + 7, NW_SUB_GAMES, NULL);
    T_EQ_INT(sA.n_ack_refused + sB.n_ack_refused + sE.n_ack_refused + sC.n_ack_refused + sD.n_ack_refused, 0);
    T_EQ_INT(meta(&A, ASK_START, G, NULL, NULL), 200);
    T_EQ_INT(meta(&B, ASK_START, G, NULL, NULL), 200);
    T_EQ_INT(meta(&E, ASK_START, G, NULL, NULL), 200);
    T_CHECK(envelope_hides(g_a.body, g_a.len, 2, NULL, NULL, NULL, "E's start"));
    // A move by someone not seated, while the game plays: refused, ten bytes.
    {
        static uint8_t wire[4] = { 0 };
        act(D.auth, G, wire, 2, 1);
        CELL_REFUSED("POST /v1/action (playing)", "another user", 400, ASK_WHY_REFUSED, TABLE_E_NOT_SEATED);
        act(C.auth, G, wire, 2, 1);
        CELL_REFUSED("POST /v1/action (playing)", "spectator", 400, ASK_WHY_REFUSED, TABLE_E_NOT_SEATED);
    }

    user *who[3] = { &A, &B, &E };
    int seats[3] = { 0, 1, 2 };
    uint32_t rng = 11;
    int status = GAME_STATUS_PLAYING, moves = 0, crosschecks = 0;
    const int64_t t0 = mono_ms();
    int64_t last_move = t0;
    while (status == GAME_STATUS_PLAYING && mono_ms() - last_move < 30000) {
        bool moved = false;
        for (int h = 0; h < 3 && status == GAME_STATUS_PLAYING; h++) {
            // The seat's view, read as a client reads it (GET, not a BUMP: a
            // bump is a seat's nudge to the bots, and limited per account).
            char gpath[48];
            snprintf(gpath, sizeof gpath, "/v1/games/%s", G);
            if (http("GET", gpath, who[h]->auth, NULL, 0, NULL) != 200) continue;
            uint32_t gv;
            NwBlob genv;
            if (nw_view_decode(g_a.body, g_a.len, &gv, &genv) != NW_OK) continue;
            char what[48];
            snprintf(what, sizeof what, "%s's view", who[h]->name);
            static uint8_t env[1 << 16];
            int el = genv.len < sizeof env ? (int)genv.len : 0;
            memcpy(env, genv.p, (size_t)el);
            T_CHECK(envelope_hides(env, el, seats[h], NULL, NULL, NULL, what));
            uint8_t wire[64];
            uint32_t version = 0;
            int n = choose(env, el, wire, sizeof wire, &rng, &status, &version);
            if (n <= 0) continue;
            T_EQ_INT(act(who[h]->auth, G, wire, n, version), 200);
            T_EQ_INT(g_a.len, TABLE_RESP_BYTES);   // a move's answer is seven bytes, no board
            moved = true;
            moves++;
            last_move = mono_ms();
        }
        // The three seats' own views at one version: no card one sees is in another's hand.
        cardset own[3], seen[3];
        uint32_t ver[3];
        memset(own, 0, sizeof own);
        memset(seen, 0, sizeof seen);
        bool ok = true;
        for (int h = 0; h < 3; h++) ok = view_of(who[h], G, seats[h], &own[h], &seen[h], &ver[h]) && ok;
        T_CHECK(ok);
        if (ok && ver[0] == ver[1] && ver[1] == ver[2]) {
            crosschecks++;
            for (int x = 0; x < 3; x++)
                for (int y = 0; y < 3; y++) {
                    if (x == y) continue;
                    for (int i = 0; i < own[y].n; i++)
                        if (cardset_has(&seen[x], own[y].ids[i])) {
                            fprintf(stderr, "seat %d sees card %d, which is in seat %d's hand\n", x, own[y].ids[i], y);
                            T_CHECK(0);
                        }
                }
            cardset spec_seen;
            memset(&spec_seen, 0, sizeof spec_seen);
            uint32_t sv;
            T_CHECK(view_of(&C, G, -1, NULL, &spec_seen, &sv));
            for (int y = 0; y < 3; y++)
                for (int i = 0; i < own[y].n; i++) T_CHECK(!cardset_has(&spec_seen, own[y].ids[i]));
        }
        ws_pump(&sA, 0); ws_pump(&sB, 0); ws_pump(&sE, 0); ws_pump(&sC, 0); ws_pump(&sD, 0);
        if (!moved) { struct timespec ts = { 0, 20 * 1000000 }; nanosleep(&ts, NULL); }
    }
    ws_pump(&sA, 1000); ws_pump(&sB, 300); ws_pump(&sE, 300); ws_pump(&sC, 300); ws_pump(&sD, 300);
    fprintf(stderr, "matrix: game over=%d after %d moves in %lld ms; %d cross-checks; casts A %d E %d C %d, views B %d; %d bodies judged\n",
            status == GAME_STATUS_GAME_OVER, moves, (long long)(mono_ms() - t0), crosschecks, sA.n_cast, sE.n_cast, sC.n_cast,
            sB.n_view, g_bodies_checked);
    T_EQ_INT(status, GAME_STATUS_GAME_OVER);
    T_CHECK(crosschecks > 10);
    T_CHECK(sA.n_cast > 0 && sE.n_cast > 0 && sC.n_cast > 0 && sB.n_view > 0);
    T_CHECK(!sA.leak && !sB.leak && !sE.leak && !sC.leak && !sD.leak);
    // D sits at none of it and heard none of it.
    T_EQ_INT(sD.n_view + sD.n_cast + sD.n_chat, 0);
    // The finished game: its replay is the seats', not D's.
    char path[48];
    snprintf(path, sizeof path, "/v1/replay/%s", G);
    T_EQ_INT(http("GET", path, B.auth, NULL, 0, NULL), 200);
    http("GET", path, D.auth, NULL, 0, NULL);
    CELL_REFUSED("GET /v1/replay/<id> (finished)", "another user", 404, ASK_WHY_NOT_FOUND, 0);
    http("GET", path, C.auth, NULL, 0, NULL);
    CELL_REFUSED("GET /v1/replay/<id> (finished)", "spectator", 404, ASK_WHY_NOT_FOUND, 0);
    T_EQ_INT(http("GET", "/v1/history", D.auth, NULL, 0, NULL), 200);
    T_EQ_INT(nw_list_open(g_a.body, g_a.len, NW_HISTORY), 0);
    // A move on a finished game is moot for anyone (table_act answers MOOT
    // before it looks for the seat): seven bytes, the version, no board.
    uint8_t wire[4] = { 0 };
    act(D.auth, G, wire, 2, 1);
    g_cells++;
    TableResponse tr;
    T_CHECK(g_a.status == 200 && g_a.len == TABLE_RESP_BYTES && table_response_decode(g_a.body, g_a.len, &tr) == 0 &&
            tr.status == TABLE_STATUS_MOOT);
    close(sA.fd); close(sB.fd); close(sC.fd); close(sD.fd);
    // E keeps a socket open across the deletion below.
    g_deleted_sock = sE;
}

// ---- account deletion ----------------------------------------------------------------------

// The forms a name can take in what the console shows: as text, as SQLite's
// "\x" hex of a blob, and as base64 (DynamoDB's binary attributes) in each of
// the three alignments a run of bytes can start at.
typedef struct { int n; char form[5][160]; } forms;

static void forms_of(const char *name, forms *f) {
    size_t n = strlen(name);
    f->n = 0;
    snprintf(f->form[f->n++], sizeof f->form[0], "%s", name);
    cb_hex_encode((const uint8_t *)name, n, f->form[f->n++], sizeof f->form[0]);
    for (int k = 0; k < 3; k++) {
        uint8_t in[128] = { 0 };
        memcpy(in + k, name, n);
        char b64[200];
        cb_b64_encode(in, n + (size_t)k, b64, sizeof b64);
        int i0 = (8 * k + 5) / 6, i1 = (int)(8 * (k + n)) / 6;   // the characters only the name's bits make
        snprintf(f->form[f->n++], sizeof f->form[0], "%.*s", i1 - i0, b64 + i0);
    }
}

static bool has_any(const uint8_t *p, int len, const forms *f, int *which) {
    for (int i = 0; i < f->n; i++)
        if (memmem(p, (size_t)len, f->form[i], strlen(f->form[i]))) { if (which) *which = i; return true; }
    return false;
}

// Every row of every collection the console can browse, through the console.
// For each collection: whether the name (in any form) and the id appear.
typedef struct { char name[64]; bool has_name, has_id; int rows_bytes; } coll_hit;

static int scan_collections(const forms *name, const char *id, coll_hit *out, int cap) {
    if (http("GET", "/admin/api/collections", "Bearer " ADMIN, NULL, 0, NULL) != 200) return -1;
    static char list[1 << 14];
    int ll = g_a.len < (int)sizeof list - 1 ? g_a.len : 0;
    memcpy(list, g_a.body, (size_t)ll);
    list[ll] = 0;
    int n = cb_json_array_len(list, (size_t)ll, "collections");
    int k = 0;
    for (int i = 0; i < n && k < cap; i++) {
        char key[32], coll[64];
        snprintf(key, sizeof key, "collections.%d", i);
        if (!cb_json_get_str(list, (size_t)ll, key, coll, sizeof coll)) continue;
        coll_hit *h = &out[k++];
        memset(h, 0, sizeof *h);
        snprintf(h->name, sizeof h->name, "%s", coll);
        char cursor[600] = "";
        for (int page = 0; page < 100; page++) {
            char path[1024];
            snprintf(path, sizeof path, "/admin/api/browse?c=%s&limit=500%s%s", coll, cursor[0] ? "&cursor=" : "", cursor);
            if (http("GET", path, "Bearer " ADMIN, NULL, 0, NULL) != 200) { fprintf(stderr, "browse %s: %d\n", coll, g_a.status); return -1; }
            h->rows_bytes += g_a.len;
            if (has_any(g_a.body, g_a.len, name, NULL)) h->has_name = true;
            if (memmem(g_a.body, (size_t)g_a.len, id, strlen(id))) h->has_id = true;
            char next[600] = "";
            if (!cb_json_get_str((const char *)g_a.body, (size_t)g_a.len, "next", next, sizeof next) || !next[0]) break;
            // The cursor goes into a query: percent-encode what is not unreserved.
            int o = 0;
            for (const char *c = next; *c && o < (int)sizeof cursor - 4; c++) {
                unsigned char ch = (unsigned char)*c;
                if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')
                    cursor[o++] = (char)ch;
                else o += snprintf(cursor + o, sizeof cursor - (size_t)o, "%%%02X", ch);
            }
            cursor[o] = 0;
        }
    }
    return k;
}

// The collections whose rows are a game's or a replay's own, where a seat's
// id stays after its account goes (store.h user_delete; the roster keeps the id).
static bool shared_collection(const char *c) {
    return strcmp(c, "games") == 0 || strcmp(c, "game_snapshots") == 0 || strcmp(c, "GAME") == 0 || strcmp(c, "SNAP") == 0;
}

// Whether u's replay of G (the newest of their history) names `name` in its extras.
static int replay_names(const user *u, const forms *name) {
    if (http("GET", "/v1/history", u->auth, NULL, 0, NULL) != 200) return -1;
    int n = nw_list_open(g_a.body, g_a.len, NW_HISTORY);
    int found = 0;
    for (int i = 0; i < n; i++) {
        static NwHistoryRow r;
        if (nw_get_history(g_a.body, g_a.len, i, &r) != NW_OK) return -1;
        if (r.extras.len && memmem(r.extras.p, r.extras.len, name->form[0], strlen(name->form[0]))) found++;
    }
    return found;
}

static void account_deletion(void) {
    forms name;
    forms_of(E.name, &name);
    // The positive controls: before the delete the scan finds E everywhere it should.
    coll_hit before[32];
    int nb = scan_collections(&name, E.id, before, 32);
    T_CHECK(nb > 0);
    bool name_in_games = false, id_in_users = false;
    for (int i = 0; i < nb; i++) {
        if (shared_collection(before[i].name) && before[i].has_name) name_in_games = true;
        if (!shared_collection(before[i].name) && before[i].has_id) id_in_users = true;
    }
    T_CHECK(name_in_games);   // the roster's bytes are found through their encoding
    T_CHECK(id_in_users);
    T_EQ_INT(replay_names(&A, &name), 1);
    // A lobby E sits at, with E's socket on its seat and its chat.
    char G3[16];
    T_EQ_INT(http("POST", "/v1/create", A.auth, NULL, 0, OCTETS), 200);
    gid_of_envelope(g_a.body, g_a.len, G3, sizeof G3);
    T_EQ_INT(meta(&E, ASK_JOIN, G3, NULL, NULL), 200);
    sock *sE = &g_deleted_sock;
    snprintf(sE->gid, sizeof sE->gid, "%s", G3);
    sE->seat = 1;
    sE->may_chat = true;
    ws_ask(sE, NW_WS_SUB_AS, E.auth + 7, NW_SUB_GAME, G3);
    T_EQ_INT(sE->last_seated, 1);
    ws_ask(sE, NW_WS_SUB_AS, E.auth + 7, NW_SUB_CHAT, G3);
    T_EQ_INT(sE->last_why, 0);
    ws_pump(sE, 300);
    int heard_before = sE->n_cast + sE->n_view + sE->n_chat;

    // E's own token deletes E. The deletion's own commits (E's seats renamed)
    // may still reach E's socket; then the server ends it (1008).
    T_EQ_INT(http("POST", "/v1/account/delete", E.auth, NULL, 0, OCTETS), 204);
    ws_pump(sE, 1500);
    T_CHECK(sE->closed);
    T_EQ_INT(sE->close_code, 1008);
    heard_before = sE->n_cast + sE->n_view + sE->n_chat;

    // Sessions: the refresh token names nobody (not "used"), the password signs nobody in.
    refresh_status(E.refresh);
    CELL_REFUSED("POST /v1/auth/refresh", "deleted user", 400, ASK_WHY_ACCOUNT, NW_ACCOUNT_E_BAD_REFRESH);
    user again;
    T_EQ_INT(account(&again, NW_SIGNIN, E.name, PASSWORD), -1);
    T_CHECK(refused_as(400, ASK_WHY_ACCOUNT, NW_ACCOUNT_E_BAD_LOGIN));
    // The rows: E's name nowhere, E's id only inside games and replays.
    coll_hit after[32];
    int na = scan_collections(&name, E.id, after, 32);
    T_EQ_INT(na, nb);
    for (int i = 0; i < na; i++) {
        if (after[i].has_name) fprintf(stderr, "deletion: %s still holds the name\n", after[i].name);
        T_CHECK(!after[i].has_name);
        if (!shared_collection(after[i].name)) {
            if (after[i].has_id) fprintf(stderr, "deletion: %s still holds the id\n", after[i].name);
            T_CHECK(!after[i].has_id);
        }
    }
    http("GET", "/admin/api/users?search=AUTHZ-GONE", "Bearer " ADMIN, NULL, 0, NULL);
    T_EQ_INT(g_a.status, 200);
    T_CHECK(!memmem(g_a.body, (size_t)g_a.len, E.id, strlen(E.id)));
    // The replays: A's still plays, and no longer names E.
    T_EQ_INT(replay_names(&A, &name), 0);
    char path[48];
    snprintf(path, sizeof path, "/v1/replay/%s", G);
    T_EQ_INT(http("GET", path, A.auth, NULL, 0, NULL), 200);
    T_CHECK(!has_any(g_a.body, g_a.len, &name, NULL));
    // Nothing the other users read names E.
    T_EQ_INT(http("GET", "/v1/leaderboard", NULL, NULL, 0, NULL), 200);
    T_CHECK(!has_any(g_a.body, g_a.len, &name, NULL));
    T_CHECK(!memmem(g_a.body, (size_t)g_a.len, E.id, strlen(E.id)));
    int which = -1;
    T_EQ_INT(http("GET", "/v1/games", A.auth, NULL, 0, NULL), 200);
    T_CHECK(!has_any(g_a.body, g_a.len, &name, &which));
    // E's lobby seat is gone from the lobby's push: B joins, E's socket hears nothing of it.
    T_EQ_INT(meta(&B, ASK_JOIN, G3, NULL, NULL), 200);
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_CHAT_SEND;
    q.message_len = 9;
    memcpy(q.message, "after-del", 9);
    uint8_t body[32];
    int n = nw_request_encode(&q, body, sizeof body);
    snprintf(path, sizeof path, "/v1/chat/%s", G3);
    T_EQ_INT(http("POST", path, A.auth, body, (size_t)n, OCTETS), 204);
    ws_pump(sE, 1500);
    int heard_after = sE->n_cast + sE->n_view + sE->n_chat - heard_before;
    if (heard_after) fprintf(stderr, "deletion: the deleted user's open stream heard %d more frames\n", heard_after);
    T_EQ_INT(heard_after, 0);
    close(sE->fd);
    // A new socket with the same, still unexpired, token subscribes to nothing.
    sock again_s;
    ws_open(&again_s, "deleted user", 1, G3, true);
    ws_ask(&again_s, NW_WS_SUB_AS, E.auth + 7, NW_SUB_GAMES, NULL);
    g_cells++;
    T_EQ_INT(again_s.last_why, ASK_WHY_AUTH);
    ws_ask(&again_s, NW_WS_SUB_AS, E.auth + 7, NW_SUB_GAME, G3);
    T_EQ_INT(again_s.last_why, ASK_WHY_AUTH);
    close(again_s.fd);
    // The deleted user's access token, still unexpired, reaches nothing that was theirs.
    T_EQ_INT(http("GET", "/v1/games", E.auth, NULL, 0, NULL), 200);
    T_EQ_INT(nw_list_open(g_a.body, g_a.len, NW_GAMES), 0);
    // History: a replay keeps its seats' ids (DESIGN.md "Open defects carried
    // over"), so the token still lists the games it played, every name in
    // them redacted, its own included; and no rating.
    T_EQ_INT(http("GET", "/v1/history", E.auth, NULL, 0, NULL), 200);
    T_CHECK(!has_any(g_a.body, g_a.len, &name, NULL));
    int has_rating = 1;
    uint32_t rating = 0;
    T_EQ_INT(nw_history_rating(g_a.body, g_a.len, &has_rating, &rating), NW_OK);
    T_EQ_INT(has_rating, 0);
    NwRequest pq;
    memset(&pq, 0, sizeof pq);
    pq.kind = NW_PASSWORD;
    pq.password_len = 12;
    memcpy(pq.password, "a-new-pass-1", 12);
    uint8_t pw[96];
    int pn = nw_request_encode(&pq, pw, sizeof pw);
    http("POST", "/v1/auth/password", E.auth, pw, (size_t)pn, OCTETS);
    CELL_REFUSED("POST /v1/auth/password", "deleted user", 401, ASK_WHY_AUTH, 0);
    http("POST", "/v1/create", E.auth, NULL, 0, OCTETS);
    CELL_REFUSED("POST /v1/create", "deleted user", 400, ASK_WHY_AUTH, 0);
    meta(&E, ASK_JOIN, G3, NULL, NULL);
    CELL_REFUSED("POST /v1/meta join", "deleted user", 400, ASK_WHY_AUTH, 0);
    snprintf(path, sizeof path, "/v1/chat/%s", G);
    http("POST", path, E.auth, body, (size_t)n, OCTETS);
    CELL_REFUSED("POST /v1/chat/<id>", "deleted user", 403, ASK_WHY_FORBIDDEN, 0);
    // A second delete (a retry whose first answer was lost): done, as the first was.
    http("POST", "/v1/account/delete", E.auth, NULL, 0, OCTETS);
    CELL_STATUS("POST /v1/account/delete (again)", "deleted user", 204);
}

// A validly signed token for a user who never existed: a nobody with no rows.
static void a_token_for_nobody(void) {
    char ghost[NW_ACCESS_MAX + 16];
    int64_t now = cb_now_ms() / 1000;
    mint(ghost, sizeof ghost, SECRET, "00000000-0000-4000-8000-00000000dead", "GHOST", now, now + 600);
    T_EQ_INT(http("GET", "/v1/games", ghost, NULL, 0, NULL), 200);
    g_cells++;
    T_EQ_INT(nw_list_open(g_a.body, g_a.len, NW_GAMES), 0);
    T_EQ_INT(http("GET", "/v1/history", ghost, NULL, 0, NULL), 200);
    g_cells++;
    T_EQ_INT(nw_list_open(g_a.body, g_a.len, NW_HISTORY), 0);
    char path[48];
    snprintf(path, sizeof path, "/v1/games/%s", G);
    T_EQ_INT(http("GET", path, ghost, NULL, 0, NULL), 200);   // anyone may watch
    g_cells++;
    uint32_t v;
    NwBlob env;
    T_EQ_INT(nw_view_decode(g_a.body, g_a.len, &v, &env), NW_OK);
    T_CHECK(envelope_hides(env.p, (int)env.len, -1, NULL, NULL, NULL, "a nobody's GET game"));
    http("POST", "/v1/create", ghost, NULL, 0, OCTETS);
    CELL_REFUSED("POST /v1/create", "a token for nobody", 400, ASK_WHY_AUTH, 0);
    meta(NULL, ASK_JOIN, G, NULL, NULL);   // no bearer: refused before the account is asked
    {
        TableAsk a;
        memset(&a, 0, sizeof a);
        a.kind = ASK_JOIN;
        a.gid_len = 6;
        memcpy(a.gid, G, 6);
        uint8_t body[ASK_BYTES_MAX];
        int n = ask_encode(&a, body, sizeof body);
        http("POST", "/v1/meta", ghost, body, (size_t)n, OCTETS);
        CELL_REFUSED("POST /v1/meta join", "a token for nobody", 400, ASK_WHY_AUTH, 0);
    }
    // Nor may it subscribe.
    sock gs;
    ws_open(&gs, "nobody", -1, "", false);
    ws_ask(&gs, NW_WS_SUB_AS, ghost + 7, NW_SUB_GAMES, NULL);
    g_cells++;
    T_EQ_INT(gs.last_why, ASK_WHY_AUTH);
    close(gs.fd);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: smoke_matrix <path to cbase>\n"); return 2; }
    g_bin = argv[1];
    signal(SIGPIPE, SIG_IGN);
    start_server();
    if (g_port <= 0) return T_DONE();
    T_RUN(setup_users);
    T_RUN(bad_identities_everywhere);
    T_RUN(public_reads_ignore_the_caller);
    T_RUN(another_user_sees_only_their_own);
    T_RUN(console_takes_only_the_admin_token);
    T_RUN(stream_frames);
    T_RUN(hidden_card_game);
    T_RUN(account_deletion);
    T_RUN(a_token_for_nobody);
    fprintf(stderr, "matrix: %d matrix cells, %d envelopes and boards judged\n", g_cells, g_bodies_checked);
    T_EQ_INT(tp_stop(&g_child, SIGTERM, 15000), 0);
    return T_DONE();
}
