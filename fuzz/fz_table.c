// fz_table.c - the kernel's rules as a player can reach them: random tables
// and random move and ask bytes through table_act and table_ask, with full
// ground truth (this harness sees every hand, the stock and the table). It
// stands where the native server's sem_fuzz.c stood (retired with that server,
// tree 56ad0609): the one place a move becomes legal or not is the kernel, so
// if this holds, no transport lets a player cheat.
//
// After every step:
//   NO PHANTOM CARD   an applied attack, cover or pass names only cards the
//                     acting seat held before it;
//   NO MUTATION       a refused move leaves the board byte for byte as it was
//                     (game.c's PASS_OVERFLOW abort excepted, which ends the
//                     game on purpose, as sem_fuzz allowed);
//   LEGAL AGREEMENT   every move legal_moves_into enumerates for a seat is
//                     applied when that seat sends it;
//   CONSERVATION      while a game is played, every card of the deck is in
//                     exactly one place (a hand, the stock, the trump, the
//                     table, or counted in the discard), none twice, none new;
//   SEATS             the board's seat count is the roster's, every hand count
//                     and the stock are within their capacities;
//   an actor with no seat is refused, and nothing crashes.
//
// The input is a program (fz.h fz_in): the table's shape first (players,
// bots, the deal seed), then steps - a legal move, a legal move with one card
// swapped, raw wire bytes, an ask from raw bytes or from parts, a stranger.
#include "fz.h"

#include "ask.h"
#include "awire.h"
#include "card.h"
#include "game.h"
#include "legal.h"
#include "table.h"

static Game *g_game, *g_pre;
static TableSnaps *g_snaps;
static Table g_t;

#define N_IDS 8
static const char *const ID[N_IDS] = {
    "p0000000-0000-4000-8000-000000000000", "p1000000-0000-4000-8000-000000000001",
    "p2000000-0000-4000-8000-000000000002", "p3000000-0000-4000-8000-000000000003",
    "b0000000-0000-4000-8000-000000000000", "b1000000-0000-4000-8000-000000000001",
    "b2000000-0000-4000-8000-000000000002", "b3000000-0000-4000-8000-000000000003",
};
static const char *const BRAIN[] = { "random", "handwritten", "robusta" };

static int deck_size(int n) {
    int k = 0;
    for (int id = 0; id < 52; id++) k += card_of_id(id).value >= min_value_for(n);
    return k;
}

static bool held(const Player *p, Card c) {
    for (int i = 0; i < p->hand_count; i++) if (card_eq(p->hand[i], c)) return true;
    return false;
}

static void check_board(void) {
    const Game *g = g_t.g;
    FZ_CHECK(g->num_players >= 0 && g->num_players <= MAX_PLAYERS);
    FZ_CHECK(g->num_players == g_t.r.n);
    for (int s = 0; s < g->num_players; s++)
        FZ_CHECK(g->players[s].hand_count >= 0 && g->players[s].hand_count <= MAX_HAND_SIZE);
    FZ_CHECK(g->deck_count >= 0 && g->deck_count <= MAX_DECK);
    FZ_CHECK(g->num_battles >= 0 && g->num_battles <= MAX_BATTLES);
    if (g->status != GAME_STATUS_PLAYING) return;
    int seen[52] = { 0 }, concrete = 0;
#define TALLY(card)                                             \
    do {                                                        \
        Card c_ = (card);                                       \
        if (card_is_none(c_)) break;                            \
        int id_ = card_to_id(c_);                               \
        FZ_CHECK(id_ >= 0 && id_ < 52);                         \
        FZ_CHECK(card_of_id(id_).value >= min_value_for(g->num_players)); \
        seen[id_]++;                                            \
        concrete++;                                             \
    } while (0)
    for (int s = 0; s < g->num_players; s++)
        for (int i = 0; i < g->players[s].hand_count; i++) TALLY(g->players[s].hand[i]);
    for (int i = 0; i < g->deck_count; i++) TALLY(g->deck[i]);
    if (g->has_flipped) TALLY(g->flipped);
    for (int i = 0; i < g->num_battles; i++) { TALLY(g->table_battles[i].attack); TALLY(g->table_battles[i].defense); }
#undef TALLY
    for (int id = 0; id < 52; id++) FZ_CHECK(seen[id] <= 1);
    FZ_CHECK(concrete + g->discard_pile_length == deck_size(g->num_players));
}

static int seat_id(int seat, const char **id) {
    if (seat < 0 || seat >= g_t.r.n) return -1;
    *id = g_t.r.seats[seat].id;
    return g_t.r.seats[seat].id_len;
}

// table_act by `seat` with these wire bytes, every invariant held to it.
static int act(int seat, const uint8_t *wire, int n) {
    const char *id;
    int il = seat_id(seat, &id);
    if (il < 0) return -1;
    memcpy(g_pre, g_t.g, offsetof(Game, logs));
    const Player before = g_t.g->players[seat];
    int rc = table_act(&g_t, id, il, wire, n, -1, 0);
    AwireAction a;
    if (rc == TABLE_APPLIED && awire_decode(wire, n, &a) &&
        (a.kind == AWIRE_ATTACK || a.kind == AWIRE_COVER || a.kind == AWIRE_PASS))
        for (int i = 0; i < a.n; i++) FZ_CHECK(held(&before, a.cards[i]));
    if (rc != TABLE_APPLIED && !(rc == TABLE_REJECTED && engine_last_reject == ENGINE_REJECT_PASS_OVERFLOW))
        FZ_CHECK(memcmp(g_pre, g_t.g, offsetof(Game, logs)) == 0);
    return rc;
}

// The legal moves of `seat` that are wire (a wait is not), into moves.
static int legal_of(int seat, LegalMove *moves, int cap) {
    if (g_t.g->status != GAME_STATUS_PLAYING || seat < 0 || seat >= g_t.g->num_players) return 0;
    int all = legal_moves_into(g_t.g, seat, moves, cap), n = 0;
    for (int i = 0; i < all; i++) if (moves[i].type != MOVE_WAIT) moves[n++] = moves[i];
    return n;
}

static int wire_of(const LegalMove *m, uint8_t *out, int cap) {
    AwireAction a;
    memset(&a, 0, sizeof a);
    a.kind = m->type;
    a.n = m->n_cards;
    for (int i = 0; i < m->n_cards && i < AWIRE_MAX_CARDS; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
    return awire_encode(&a, out, cap);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    g_game = calloc(1, sizeof *g_game);
    g_pre = calloc(1, sizeof *g_pre);
    g_snaps = calloc(1, sizeof *g_snaps);
    if (!g_game || !g_pre || !g_snaps) abort();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!g_game) LLVMFuzzerInitialize(NULL, NULL);
    fz_in in = fz_in_of(data, size);
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = fz_u8(&in);
    int humans = 1 + fz_u8(&in) % 4, bots = fz_u8(&in) % 5;
    if (humans + bots < 2) bots = 1;
    if (humans + bots > MAX_PLAYERS) bots = MAX_PLAYERS - humans;
    memset(g_game, 0, sizeof *g_game);
    table_init(&g_t, g_game, g_snaps);
    FZ_CHECK(table_create(&g_t, ID[0], 36, "P0", 2) == TABLE_OK);
    for (int h = 1; h < humans; h++) FZ_CHECK(table_join(&g_t, ID[h], 36, "PX", 2) >= 0);
    for (int b = 0; b < bots; b++) {
        const char *brain = BRAIN[fz_u8(&in) % 3];
        (void)table_add_bot(&g_t, ID[0], 36, ID[4 + b], 36, "%Bot", 4, brain, (int)strlen(brain), seed);
    }
    for (int h = 0; h < humans; h++) (void)table_ready(&g_t, ID[h], 36, seed);
    check_board();

    static LegalMove moves[1024];
    uint8_t wire[2 + 2 * AWIRE_MAX_CARDS];
    for (int step = 0; step < 200 && fz_left(&in); step++) {
        uint8_t op = fz_u8(&in);
        switch (op % 8) {
        case 0: case 1: case 2: {   // a legal move, from the seat named or the next that has one
            int start = fz_u8(&in), seat = -1, n = 0;
            for (int k = 0; k < MAX_PLAYERS && n == 0; k++) {
                seat = (start + k) % (g_t.r.n ? g_t.r.n : 1);
                n = legal_of(seat, moves, 1024);
            }
            if (n == 0) break;
            int w = wire_of(&moves[fz_u16(&in) % n], wire, sizeof wire);
            FZ_CHECK(w > 0);
            int rc = act(seat, wire, w);
            FZ_CHECK(rc == TABLE_APPLIED);   // the enumerator and the applier agree
            break;
        }
        case 3: {   // a legal move with one card swapped for any card
            int seat = fz_u8(&in) % (g_t.r.n ? g_t.r.n : 1);
            int n = legal_of(seat, moves, 1024);
            if (n == 0) break;
            LegalMove m = moves[fz_u16(&in) % n];
            if (m.n_cards > 0) {
                int i = fz_u8(&in) % m.n_cards;
                Card c = card_of_id(fz_u8(&in) % 52);
                if (fz_u8(&in) & 1) m.cards[i] = c; else m.attack_cards[i] = c;
            } else {
                m.type = (int8_t)(fz_u8(&in) % 5);
            }
            int w = wire_of(&m, wire, sizeof wire);
            if (w > 0) (void)act(seat, wire, w);
            break;
        }
        case 4: {   // raw wire bytes from any seat
            int seat = fz_u8(&in) % (g_t.r.n ? g_t.r.n : 1);
            size_t got;
            const uint8_t *b = fz_bytes(&in, fz_u8(&in) % sizeof wire, &got);
            memcpy(wire, b, got);
            (void)act(seat, wire, (int)got);
            break;
        }
        case 5: {   // an ask from raw bytes, by any seat or a stranger
            size_t got;
            const uint8_t *b = fz_bytes(&in, fz_u8(&in) % 96, &got);
            TableAsk a;
            if (ask_decode(b, (int)got, &a) != ASK_OK) break;
            int who = fz_u8(&in) % N_IDS;
            const char *brain = BRAIN[fz_u8(&in) % 3];
            TableAskBot bot = { ID[4 + fz_u8(&in) % 4], 36, "%Bot", 4, brain, (int)strlen(brain) };
            (void)table_ask(&g_t, ID[who], 36, "ASK", 3, &a, a.kind == ASK_ADD_BOT ? &bot : NULL, seed);
            break;
        }
        case 6: {   // an ask from parts
            TableAsk a;
            memset(&a, 0, sizeof a);
            a.kind = (uint8_t)(1 + fz_u8(&in) % 10);
            a.gid_len = 6;
            memcpy(a.gid, "ab12cd", 6);
            uint8_t arg = fz_u8(&in);
            if (a.kind == ASK_EXIT || a.kind == ASK_ADD_BOT) {
                if (arg & 1) { a.bot_len = 36; memcpy(a.bot, ID[4 + (arg >> 1) % 4], 36); }
                else if (arg & 2) { a.player_len = 36; memcpy(a.player, ID[(arg >> 2) % 4], 36); }
            }
            if (a.kind == ASK_REARRANGE_HAND) {
                a.n_idx = (uint8_t)(arg % MAX_HAND_SIZE);
                for (int i = 0; i < a.n_idx; i++) a.idx[i] = fz_u8(&in);
            }
            if (a.kind == ASK_REARRANGE_PLAYERS) {
                a.n_order = (uint8_t)(arg % (MAX_PLAYERS + 1));
                for (int i = 0; i < a.n_order; i++) { a.order[i].len = 36; memcpy(a.order[i].id, ID[fz_u8(&in) % N_IDS], 36); }
            }
            if (a.kind == ASK_SET_RULES) a.passing = arg & 1;
            int who = fz_u8(&in) % N_IDS;
            const char *brain = BRAIN[fz_u8(&in) % 3];
            TableAskBot bot = { ID[4 + fz_u8(&in) % 4], 36, "%Bot", 4, brain, (int)strlen(brain) };
            (void)table_ask(&g_t, ID[who], 36, "ASK", 3, &a, a.kind == ASK_ADD_BOT ? &bot : NULL, seed);
            break;
        }
        case 7: {   // someone with no seat moves
            static const char STRANGER[] = "s0000000-0000-4000-8000-00000000000s";
            if (table_seat_of(&g_t, STRANGER, 36) >= 0) break;
            int n = legal_of(0, moves, 1024);
            int w = n ? wire_of(&moves[0], wire, sizeof wire) : 0;
            if (w <= 0) { wire[0] = AWIRE_PICKUP; wire[1] = 0; w = 2; }
            memcpy(g_pre, g_t.g, offsetof(Game, logs));
            int rc = table_act(&g_t, STRANGER, 36, wire, w, -1, 0);
            FZ_CHECK(rc == TABLE_E_NOT_SEATED || rc == TABLE_MOOT);
            FZ_CHECK(memcmp(g_pre, g_t.g, offsetof(Game, logs)) == 0);
            break;
        }
        }
        check_board();
    }
    return 0;
}
