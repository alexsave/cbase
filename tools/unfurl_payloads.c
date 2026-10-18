// unfurl_payloads.c - prints the /m/<payload> cases test/fixture_unfurl.h holds.
//
// Every envelope is made by the kernel: a fixture the kernel's own tests sealed
// (foolish/e2e/msg_wire.test.ts, foolish/c/tests/msg_wire_test.c) is decoded with
// msg_decode, its roster edited where a case needs other names, encoded again
// with msg_encode and checked with msg_replay, then written as base32 with
// replay_b32_encode. No byte of an envelope is laid out here.
//
// Output: one "label<TAB>payload" line per case, the payload exactly as it
// goes after /m/ in the URL (already percent-encoded where a case needs it).
// The goldens are what the TypeScript route (foolish/src/app/m/[payload]/
// layout.tsx) answered for these payloads under `next start`; see the header
// comment of test/fixture_unfurl.h for the whole regeneration recipe.
//
//   make -C c_base build/unfurl-payloads && c_base/build/unfurl-payloads
#include "game.h"
#include "msg_wire.h"
#include "replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int unhex(const char *h, unsigned char *out, int cap) {
    int n = 0;
    for (; h[0] && h[1]; h += 2) {
        if (n == cap) return -1;
        unsigned v;
        if (sscanf(h, "%2x", &v) != 1) return -1;
        out[n++] = (unsigned char)v;
    }
    return n;
}

// The kernel-sealed fixtures this file starts from.
// msg_wire.test.ts FIXTURES[1]: 3 seats, Ann0..Ann2, live at turn 10 (format 2).
static const char *LIVE3 =
    "f7020002efcdab89674523010a0000030001000000000000000079d87206410d37d302c19dfb6cacbc8bebf879d242622082315709cc0f"
    "183788030004416e6e300104416e6e310204416e6e320a00012951da5bef3096f9f7bf2cfb58d013f6d7fa";
// msg_wire.test.ts FIXTURES5[0]: 2 seats, Ann0 and Ann1, live at turn 12 (format 5).
static const char *LIVE2 =
    "f7050002efcdab89674523010c000002000200000000000000001e9986e393a186985bc91663988c3c607d4fe2267201f3652ebe7c89db"
    "67cc55341200020004416e6e300104416e6e310c001d179167621fb4cab1ae1a25e54fca";
// msg_wire_test.c REMATCH_FINISHED_HEX: 4 seats, Alex Mira Jonas Priya, finished (format 5).
static const char *FINISHED4 =
    "f7050003606000000000000064000004011100000000000000006d380f1cae7638124850ee2cbae14e7fc19e018780f568cd99f0b18252"
    "d0fe21e43400040004416c657801044d69726102054a6f6e6173030550726979616400"
    "0370d96bdfca53aaf159b08e7238ee50564efe1f1864a345b71d69ef4147da284b47f32f0cdca05a1cc4661a827ad6cfc3e8d7bafa";
// msg_wire_test.c REMATCH_LOBBY_HEX: the rematch lobby of FINISHED4 (format 6).
static const char *REMATCH4 =
    "f70600006060000000000000000002040100f887535ca3ce410d5792a42bb5bb707687ad8735658d6b9311cd750e096921e6a88a4dea"
    "8f3814aa34120000ff44c9ed0100040004416c657801044d69726102054a6f6e6173030550726979610000";

static unsigned char g_bytes[8192], g_out[8192];
static char g_b32[16384];
static Game g_game;

// Decodes `hex`, lets `edit` change the envelope, encodes it again, proves it
// replays (when `must_replay`), and returns its base32 in g_b32.
static const char *make(const char *hex, void (*edit)(MsgEnvelope *), int must_replay) {
    int n = unhex(hex, g_bytes, sizeof g_bytes);
    MsgEnvelope e;
    if (n < 0 || msg_decode(g_bytes, n, &e) != MSG_EOK) { fprintf(stderr, "fixture does not decode\n"); exit(1); }
    if (edit) edit(&e);
    int m = msg_encode(&e, g_out, sizeof g_out);
    if (m < 0) { fprintf(stderr, "msg_encode: %d\n", m); exit(1); }
    MsgEnvelope back;
    int rc = msg_decode(g_out, m, &back);
    if (rc == MSG_EOK) rc = msg_replay(&back, &g_game);
    if (must_replay && rc != MSG_EOK) { fprintf(stderr, "case does not replay: %d\n", rc); exit(1); }
    if (!must_replay && (msg_decode(g_out, m, &back) != MSG_EOK || rc == MSG_EOK)) {
        fprintf(stderr, "case meant to decode and then fail its replay did not\n");
        exit(1);
    }
    if (replay_b32_encode(g_out, m, g_b32, sizeof g_b32) < 0) { fprintf(stderr, "b32\n"); exit(1); }
    return g_b32;
}

static void set_name(MsgEnvelope *e, int i, const char *name) {
    size_t n = strlen(name);
    memcpy(e->joins[i].name, name, n);
    e->joins[i].name_len = (uint8_t)n;
}

// Names an HTML page must escape, one of each of the five characters.
static void edit_html(MsgEnvelope *e) {
    set_name(e, 0, "<b>Ann</b>");
    set_name(e, 1, "Tom & \"Jerry\" 'J'");
}
// Names that are not ASCII.
static void edit_utf8(MsgEnvelope *e) {
    set_name(e, 0, "\xd0\x81\xd0\xbb\xd0\xba\xd0\xb0");   // Cyrillic
    set_name(e, 1, "Zo\xc3\xab \xf0\x9f\x83\x8f");        // e-diaeresis, a joker card emoji
}
// Bytes that are not UTF-8: a lone continuation byte and a cut sequence.
// msg_decode lets them through (only control bytes are refused), so the TS
// route showed whatever TextDecoder made of them.
static void edit_bad_utf8(MsgEnvelope *e) {
    set_name(e, 0, "A\x80" "B");
    set_name(e, 1, "C\xe2\x82");
}
// Two seats, one of them unnamed: the empty name is dropped before the count,
// so the title says "2 players".
static void edit_one_empty(MsgEnvelope *e) {
    set_name(e, 0, "");
    set_name(e, 1, "Bob");
}
// A lobby with one named seat: the title falls back to "N players".
static void edit_one_join(MsgEnvelope *e) {
    e->n_joins = 1;
    e->carry_key = 0;
    e->carry_fool = MSG_NO_FOOL;
}
// A plain (not rematch) lobby, two seats joined of four.
static void edit_plain_lobby(MsgEnvelope *e) {
    e->n_joins = 2;
    e->carry_key = 0;
    e->carry_fool = MSG_NO_FOOL;
}

// The header claims one more finished bout than the chain closed.
static void edit_lying_round(MsgEnvelope *e) { e->round = (uint8_t)(e->round + 1); }

int main(void) {
    char *live3 = strdup(make(LIVE3, NULL, 1));
    printf("live3_turn10\t1%s\n", live3);
    printf("live2_turn12\t1%s\n", make(LIVE2, NULL, 1));
    printf("finished4\t1%s\n", make(FINISHED4, NULL, 1));
    printf("rematch4\t1%s\n", make(REMATCH4, NULL, 1));
    printf("lobby_one_name\t1%s\n", make(REMATCH4, edit_one_join, 1));
    printf("lobby_plain\t1%s\n", make(REMATCH4, edit_plain_lobby, 1));
    printf("names_html\t1%s\n", make(LIVE2, edit_html, 1));
    printf("names_utf8\t1%s\n", make(LIVE2, edit_utf8, 1));
    printf("names_bad_utf8\t1%s\n", make(LIVE2, edit_bad_utf8, 1));
    printf("names_one_empty\t1%s\n", make(LIVE2, edit_one_empty, 1));
    char *share = strdup(make(LIVE2, NULL, 1));
    // A share link's extras suffix: replay_b32_decode stops at the dash.
    printf("share_suffix\t1%s-ABCDEFGH\n", share);
    // Base32 is case-blind in the kernel's decoder.
    for (char *p = share; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    printf("lowercase\t1%s\n", share);
    // Percent-encoded blanks around it: the route decodes, then trims.
    printf("percent_blanks\t%%20%%0A1%s%%20\n", live3);
    // Cut in half: msg_decode or msg_replay refuses it.
    live3[strlen(live3) / 2] = 0;
    printf("truncated\t1%s\n", live3);
    printf("other_version\t2%s\n", make(LIVE3, NULL, 1));
    printf("garbage\thello-world\n");
    printf("one_garbage\t1zzzz9999\n");
    printf("bad_escape\t1%%E0%%A4%%A\n");
    // A good game with an escape that is not UTF-8 after it: decodeURIComponent
    // throws on it, so the whole link is generic, though the base32 decoder
    // would have skipped the byte.
    printf("bad_utf8_escape\t1%s%%FF\n", make(LIVE2, NULL, 1));
    // A header that claims a bout the chain never closed: msg_decode accepts it
    // and only msg_replay (MSG_EROUND) refuses it, so a route that skipped the
    // replay would claim "turn 10" here.
    printf("replay_refuses\t1%s\n", make(LIVE3, edit_lying_round, 0));
    return 0;
}
