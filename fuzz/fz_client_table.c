// fz_client_table.c - the client's reading of what the server sends about a
// game (kernel client_table.h): a response envelope (GET /v1/games/<id>, the
// create and meta answers, a dashboard row, a stream VIEW), a cast (a stream
// CAST), an as2/as3 push, and a bare masked board. client_table.h's rule is
// REFUSE, NEVER CLAMP: a payload that does not read whole is refused, never
// read past. Runs in the web's wasm and the iOS app on bytes off the network.
//
// Input: [mode][identity length u16][identity bytes][payload]. The identity
// is what a host hands back from an earlier envelope; mode 0 adopts the
// payload as an envelope first and keeps the identity it gives.
#include "fz.h"

#include "client_table.h"

static ClientTable g_c;
static ClientSlot g_slot;

static void read_push(void) {
    for (int i = 0; i < 256; i++) {
        int r = client_push_next(&g_c);
        if (r != 1) break;
        FZ_CHECK(g_c.view.num_players >= 0 && g_c.view.num_players <= MAX_PLAYERS);
    }
    (void)client_push_final(&g_c);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fz_in in = fz_in_of(data, size);
    uint8_t mode = fz_u8(&in);
    size_t il, pl;
    const uint8_t *ib = fz_bytes(&in, fz_u16(&in) % 2048, &il);
    const uint8_t *pb = fz_bytes(&in, fz_left(&in), &pl);
    uint8_t *ident = fz_dup(ib, il), *p = fz_dup(pb, pl);
    client_init(&g_c, &g_slot);
    static uint8_t kept[4096];
    int kept_len = 0;
    switch (mode % 5) {
    case 0:
        if (client_adopt_envelope(&g_c, p, (int)pl) == CLIENT_OK) {
            const TableView *v = &g_c.view;
            FZ_CHECK(v->num_players >= 0 && v->num_players <= MAX_PLAYERS);
            FZ_CHECK(v->my_seat >= -1 && v->my_seat < v->num_players);
            FZ_CHECK(v->my_hand_count >= 0 && v->my_hand_count <= MAX_HAND_SIZE);
            ViewRules vr;
            (void)client_view_rules(v, 0, 0, 0, &vr);
            kept_len = client_identity(&g_c, kept, sizeof kept);
            FZ_CHECK(kept_len >= 0);
            int at = client_identity_at(&g_c);
            FZ_CHECK(at >= -1 && at <= (int)pl);
            // A cast against the identity the envelope gave.
            if (kept_len > 0 && il > 0 && client_cast_open(&g_c, ident, (int)il, kept, kept_len) == CLIENT_OK) read_push();
        }
        break;
    case 1:
        if (client_cast_open(&g_c, p, (int)pl, ident, (int)il) == CLIENT_OK) read_push();
        break;
    case 2: case 3:
        if (client_push_open(&g_c, p, (int)pl, mode % 5 == 3, ident, (int)il, fz_u8(&in)) == CLIENT_OK) read_push();
        break;
    case 4: {
        int viewer = (int)(mode >> 4) % (MAX_PLAYERS + 1) - 1;
        if (client_adopt_state(&g_c, p, (int)pl, viewer) == CLIENT_OK) {
            ViewRules vr;
            (void)client_view_rules(&g_c.view, 0, 0, 0, &vr);
        }
        break;
    }
    }
    // Reading with no push open is refused, never undefined.
    FZ_CHECK(client_push_next(&g_c) < 0 || g_c.open);
    free(ident);
    free(p);
    return 0;
}
