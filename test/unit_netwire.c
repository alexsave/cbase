// unit_netwire.c - the binary surface's bytes (foolish/c/src/netwire.h), held
// to hand-written fixtures: every encoder writes exactly these bytes, every
// decoder reads them back, and every prefix and every body one byte too long
// is refused. Prefixes are copied into exactly-sized heap buffers, so a reader
// that looks past the end is an ASan failure under `make check-asan`.
#include "t.h"

#include "netwire.h"

#include <stdlib.h>

// Every proper prefix of `p` and `p` plus one byte must be refused by `dec`.
typedef int (*dec_fn)(const uint8_t *p, int len);

static int refused_everywhere(const uint8_t *p, int n, dec_fn dec) {
    int bad = 0;
    for (int k = 0; k < n; k++) {
        uint8_t *c = malloc(k ? (size_t)k : 1);
        memcpy(c, p, (size_t)k);
        if (dec(c, k) >= 0) { fprintf(stderr, "     a %d-byte prefix of %d was read\n", k, n); bad++; }
        free(c);
    }
    uint8_t *x = malloc((size_t)n + 1);
    memcpy(x, p, (size_t)n);
    x[n] = 0;
    if (dec(x, n + 1) >= 0) { fprintf(stderr, "     a body one byte long was read\n"); bad++; }
    free(x);
    return bad;
}

static int dec_request(const uint8_t *p, int len) { NwRequest q; return nw_request_decode(p, len, &q); }
static int dec_session(const uint8_t *p, int len) { NwSession s; return nw_session_decode(p, len, &s); }
static int dec_view(const uint8_t *p, int len) { uint32_t v; NwBlob b; return nw_view_decode(p, len, &v, &b); }
static int dec_frame(const uint8_t *p, int len) { NwFrame f; return nw_frame_decode(p, len, &f); }
static int dec_games(const uint8_t *p, int len) { return nw_list_open(p, len, NW_GAMES); }
static int dec_board(const uint8_t *p, int len) { return nw_list_open(p, len, NW_BOARD); }
static int dec_history(const uint8_t *p, int len) { return nw_list_open(p, len, NW_HISTORY); }
static int dec_chats(const uint8_t *p, int len) { return nw_list_open(p, len, NW_CHATS); }
static int dec_bots(const uint8_t *p, int len) { return nw_list_open(p, len, NW_BOTS); }
static int dec_replay(const uint8_t *p, int len) { NwBlob a, b; return nw_replay_decode(p, len, &a, &b); }

static void requests(void) {
    uint8_t out[64];
    NwRequest q, r;
    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNIN;
    q.name_len = 1; q.name[0] = 'A';
    q.password_len = 2; memcpy(q.password, "pw", 2);
    const uint8_t signin[] = { 1, 2, 1, 'A', 2, 'p', 'w' };
    T_EQ_MEM(out, nw_request_encode(&q, out, sizeof out), signin, sizeof signin);
    T_EQ_INT(nw_request_decode(signin, sizeof signin, &r), NW_OK);
    T_EQ_INT(r.kind, NW_SIGNIN);
    T_EQ_MEM(r.name, r.name_len, "A", 1);
    T_EQ_MEM(r.password, r.password_len, "pw", 2);
    T_EQ_INT(refused_everywhere(signin, sizeof signin, dec_request), 0);

    memset(&q, 0, sizeof q);
    q.kind = NW_CHAT_SEND;
    q.message_len = 3; memcpy(q.message, "hi!", 3);
    const uint8_t chat[] = { 1, 6, 3, 0, 'h', 'i', '!' };
    T_EQ_MEM(out, nw_request_encode(&q, out, sizeof out), chat, sizeof chat);
    T_EQ_INT(refused_everywhere(chat, sizeof chat, dec_request), 0);

    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNOUT;
    q.scope = NW_SCOPE_GLOBAL;
    const uint8_t out_g[] = { 1, 4, 1 };
    T_EQ_MEM(out, nw_request_encode(&q, out, sizeof out), out_g, sizeof out_g);
    const uint8_t bad_scope[] = { 1, 4, 2 };
    T_EQ_INT(nw_request_decode(bad_scope, sizeof bad_scope, &r), NW_E_WIRE);
    q.scope = 7;
    T_EQ_INT(nw_request_encode(&q, out, sizeof out), NW_E_WIRE);

    memset(&q, 0, sizeof q);
    q.kind = NW_REFRESH;
    q.refresh_len = 2; memcpy(q.refresh, "rt", 2);
    const uint8_t refresh[] = { 1, 3, 2, 'r', 't' };
    T_EQ_MEM(out, nw_request_encode(&q, out, sizeof out), refresh, sizeof refresh);
    T_EQ_INT(nw_request_encode(&q, out, 4), NW_E_CAP);

    // Bounds: a name one past NW_NAME_MAX is neither written nor read.
    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNUP;
    q.name_len = NW_NAME_MAX + 1;
    T_EQ_INT(nw_request_encode(&q, out, sizeof out), NW_E_WIRE);
    uint8_t longname[4 + NW_NAME_MAX + 1 + 1];
    memset(longname, 'x', sizeof longname);
    longname[0] = 1; longname[1] = NW_SIGNUP; longname[2] = NW_NAME_MAX + 1;
    longname[3 + NW_NAME_MAX + 1] = 0;   // an empty password after it
    T_EQ_INT(nw_request_decode(longname, sizeof longname, &r), NW_E_WIRE);
    // Another version, or a kind that is not a request.
    const uint8_t v2[] = { 2, 2, 1, 'A', 2, 'p', 'w' };
    T_EQ_INT(nw_request_decode(v2, sizeof v2, &r), NW_E_WIRE);
    const uint8_t not_req[] = { 1, NW_SESSION };
    T_EQ_INT(nw_request_decode(not_req, sizeof not_req, &r), NW_E_WIRE);
}

static void session(void) {
    NwSession s, t;
    memset(&s, 0, sizeof s);
    s.access_len = 2; memcpy(s.access, "ab", 2);
    s.refresh_len = 1; s.refresh[0] = 'r';
    s.expires_at_ms = 0x0102030405ull;
    s.user_id_len = 1; s.user_id[0] = 'u';
    s.username_len = 1; s.username[0] = 'N';
    const uint8_t want[] = { 1, 20, 2, 0, 'a', 'b', 1, 'r', 5, 4, 3, 2, 1, 0, 1, 'u', 1, 'N' };
    uint8_t out[64];
    T_EQ_MEM(out, nw_session_encode(&s, out, sizeof out), want, sizeof want);
    T_EQ_INT(nw_session_decode(want, sizeof want, &t), NW_OK);
    T_EQ_MEM(&s, sizeof s, &t, sizeof t);
    T_EQ_INT(refused_everywhere(want, sizeof want, dec_session), 0);
}

static void views_and_replays(void) {
    const uint8_t env[] = { 0xaa, 0xbb };
    uint8_t out[64];
    const uint8_t want[] = { 1, 21, 7, 0, 0, 0, 2, 0, 0, 0, 0xaa, 0xbb };
    NwBlob b = { env, 2 };
    T_EQ_MEM(out, nw_view_encode(7, b, out, sizeof out), want, sizeof want);
    uint32_t v;
    NwBlob got;
    T_EQ_INT(nw_view_decode(want, sizeof want, &v, &got), NW_OK);
    T_EQ_INT(v, 7);
    T_CHECK(got.p == want + 10 && got.len == 2);   // points into the body, copies nothing
    T_EQ_INT(refused_everywhere(want, sizeof want, dec_view), 0);
    // A blob length past the body.
    const uint8_t lies[] = { 1, 21, 7, 0, 0, 0, 0xff, 0xff, 0xff, 0x7f, 0xaa };
    T_EQ_INT(nw_view_decode(lies, sizeof lies, &v, &got), NW_E_WIRE);

    const uint8_t rep[] = { 1, 22, 1, 0, 0, 0, 0xff, 0, 0, 0, 0 };
    NwBlob m = { (const uint8_t *)"\xff", 1 }, e = { NULL, 0 };
    T_EQ_MEM(out, nw_replay_encode(m, e, out, sizeof out), rep, sizeof rep);
    T_EQ_INT(refused_everywhere(rep, sizeof rep, dec_replay), 0);
}

static void lists(void) {
    uint8_t out[256];
    // Two games: the writer reserves the head and writes it last.
    const uint8_t games[] = { 1, 23, 2, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0x0a, 2, 0, 0, 0, 0, 0, 0, 0 };
    int n = NW_LIST_HEAD_BYTES;
    NwBlob e1 = { (const uint8_t *)"\x0a", 1 }, e2 = { NULL, 0 };
    n += nw_put_game(1, e1, out + n, (int)sizeof out - n);
    n += nw_put_game(2, e2, out + n, (int)sizeof out - n);
    T_EQ_INT(nw_list_head(NW_GAMES, 2, out, NW_LIST_HEAD_BYTES), NW_LIST_HEAD_BYTES);
    T_EQ_MEM(out, n, games, sizeof games);
    T_EQ_INT(nw_list_open(games, sizeof games, NW_GAMES), 2);
    uint32_t v;
    NwBlob b;
    T_EQ_INT(nw_get_game(games, sizeof games, 1, &v, &b), NW_OK);
    T_EQ_INT(v, 2);
    T_EQ_INT(b.len, 0);
    T_EQ_INT(nw_get_game(games, sizeof games, 2, &v, &b), NW_E_WIRE);
    T_EQ_INT(nw_list_open(games, sizeof games, NW_BOARD), NW_E_WIRE);   // another kind
    T_EQ_INT(refused_everywhere(games, sizeof games, dec_games), 0);
    // A count that promises a row the body does not hold.
    uint8_t three[sizeof games];
    memcpy(three, games, sizeof games);
    three[2] = 3;
    T_EQ_INT(nw_list_open(three, sizeof three, NW_GAMES), NW_E_WIRE);

    NwBoardRow r, got;
    memset(&r, 0, sizeof r);
    r.is_bot = 1;
    r.id_len = 1; r.id[0] = 'b';
    r.name_len = 2; memcpy(r.name, "%X", 2);
    r.rating = 1500;
    r.games = 3;
    const uint8_t board[] = { 1, 26, 1, 0, 1, 1, 'b', 2, '%', 'X', 0xdc, 0x05, 0, 0, 3, 0, 0, 0 };
    n = NW_LIST_HEAD_BYTES + nw_put_board(&r, out + NW_LIST_HEAD_BYTES, (int)sizeof out - NW_LIST_HEAD_BYTES);
    nw_list_head(NW_BOARD, 1, out, NW_LIST_HEAD_BYTES);
    T_EQ_MEM(out, n, board, sizeof board);
    T_EQ_INT(nw_get_board(board, sizeof board, 0, &got), NW_OK);
    T_EQ_MEM(&got, sizeof got, &r, sizeof r);
    T_EQ_INT(refused_everywhere(board, sizeof board, dec_board), 0);
    uint8_t not_bool[sizeof board];
    memcpy(not_bool, board, sizeof board);
    not_bool[4] = 2;
    T_EQ_INT(nw_list_open(not_bool, sizeof not_bool, NW_BOARD), NW_E_WIRE);

    NwBot bot, bgot;
    memset(&bot, 0, sizeof bot);
    bot.id_len = 1; bot.id[0] = 'i';
    bot.nickname_len = 1; bot.nickname[0] = 'n';
    bot.key_len = 1; bot.key[0] = 'k';
    const uint8_t bots[] = { 1, 25, 1, 0, 1, 'i', 1, 'n', 1, 'k' };
    n = NW_LIST_HEAD_BYTES + nw_put_bot(&bot, out + NW_LIST_HEAD_BYTES, (int)sizeof out - NW_LIST_HEAD_BYTES);
    nw_list_head(NW_BOTS, 1, out, NW_LIST_HEAD_BYTES);
    T_EQ_MEM(out, n, bots, sizeof bots);
    T_EQ_INT(nw_get_bot(bots, sizeof bots, 0, &bgot), NW_OK);
    T_EQ_MEM(&bgot, sizeof bgot, &bot, sizeof bot);
    T_EQ_INT(refused_everywhere(bots, sizeof bots, dec_bots), 0);

    NwChat c, cgot;
    memset(&c, 0, sizeof c);
    c.id_len = 1; c.id[0] = 'm';
    c.user_id_len = 1; c.user_id[0] = 'u';
    c.created_ms = 0x010203;
    c.is_system = 1;
    c.message_len = 2; memcpy(c.message, "hi", 2);
    const uint8_t chats[] = { 1, 24, 1, 0, 1, 'm', 1, 'u', 3, 2, 1, 0, 0, 0, 1, 2, 0, 'h', 'i' };
    n = NW_LIST_HEAD_BYTES + nw_put_chat(&c, out + NW_LIST_HEAD_BYTES, (int)sizeof out - NW_LIST_HEAD_BYTES);
    nw_list_head(NW_CHATS, 1, out, NW_LIST_HEAD_BYTES);
    T_EQ_MEM(out, n, chats, sizeof chats);
    T_EQ_INT(nw_get_chat(chats, sizeof chats, 0, &cgot), NW_OK);
    T_EQ_MEM(&cgot, sizeof cgot, &c, sizeof c);
    T_EQ_INT(refused_everywhere(chats, sizeof chats, dec_chats), 0);
    c.message_len = NW_CHAT_MAX + 1;
    T_EQ_INT(nw_put_chat(&c, out, sizeof out), NW_E_WIRE);

    NwHistoryRow h, hgot;
    memset(&h, 0, sizeof h);
    h.id_len = 1; h.id[0] = 's';
    h.created_ms = 1;
    h.n_players = 2;
    h.player_len[0] = 1; h.player[0][0] = 'p';
    h.player_len[1] = 1; h.player[1][0] = 'q';
    h.moves.p = (const uint8_t *)"\xff";
    h.moves.len = 1;
    const uint8_t hist[] = { 1, 27, 1, 0xb0, 0x04, 0, 0, 1, 0,
                             1, 's', 1, 0, 0, 0, 0, 0, 2, 1, 'p', 1, 'q', 1, 0, 0, 0, 0xff, 0, 0, 0, 0 };
    n = NW_HISTORY_HEAD_BYTES + nw_put_history(&h, out + NW_HISTORY_HEAD_BYTES, (int)sizeof out - NW_HISTORY_HEAD_BYTES);
    T_EQ_INT(nw_history_head(1, 1200, 1, out, NW_HISTORY_HEAD_BYTES), NW_HISTORY_HEAD_BYTES);
    T_EQ_MEM(out, n, hist, sizeof hist);
    int has;
    uint32_t rating;
    T_EQ_INT(nw_history_rating(hist, sizeof hist, &has, &rating), NW_OK);
    T_CHECK(has == 1 && rating == 1200);
    T_EQ_INT(nw_get_history(hist, sizeof hist, 0, &hgot), NW_OK);
    T_EQ_MEM(hgot.id, hgot.id_len, "s", 1);
    T_EQ_INT(hgot.n_players, 2);
    T_EQ_MEM(hgot.player[1], hgot.player_len[1], "q", 1);
    T_CHECK(hgot.moves.len == 1 && hgot.moves.p[0] == 0xff && hgot.extras.len == 0);
    T_EQ_INT(refused_everywhere(hist, sizeof hist, dec_history), 0);
    // No rating is said as zero, and a rating beside "none" is refused.
    uint8_t none[sizeof hist];
    memcpy(none, hist, sizeof hist);
    none[2] = 0;
    T_EQ_INT(nw_list_open(none, sizeof none, NW_HISTORY), NW_E_WIRE);
    h.n_players = MAX_PLAYERS + 1;
    T_EQ_INT(nw_put_history(&h, out, sizeof out), NW_E_WIRE);
}

static void frames(void) {
    uint8_t out[64];
    NwFrame f, g;

    memset(&f, 0, sizeof f);
    f.kind = NW_WS_AUTH;
    f.token_len = 3; memcpy(f.token, "jwt", 3);
    const uint8_t auth[] = { 1, 40, 3, 0, 'j', 'w', 't' };
    T_EQ_MEM(out, nw_frame_encode(&f, out, sizeof out), auth, sizeof auth);
    T_EQ_INT(refused_everywhere(auth, sizeof auth, dec_frame), 0);

    memset(&f, 0, sizeof f);
    f.kind = NW_WS_SUB;
    f.ref = 7;
    f.what = NW_SUB_GAME;
    f.gid_len = 2; memcpy(f.gid, "g1", 2);
    f.version = 5;
    const uint8_t sub[] = { 1, 41, 7, 0, 2, 2, 'g', '1', 5, 0, 0, 0 };
    T_EQ_MEM(out, nw_frame_encode(&f, out, sizeof out), sub, sizeof sub);
    T_EQ_INT(nw_frame_decode(sub, sizeof sub, &g), NW_OK);
    T_CHECK(g.ref == 7 && g.what == NW_SUB_GAME && g.version == 5 && g.gid_len == 2);
    T_EQ_INT(refused_everywhere(sub, sizeof sub, dec_frame), 0);
    f.what = 9;
    T_EQ_INT(nw_frame_encode(&f, out, sizeof out), NW_E_WIRE);

    memset(&f, 0, sizeof f);
    f.kind = NW_WS_ACK;
    f.ref = 7;
    f.why = 9;
    const uint8_t ack[] = { 1, 50, 7, 0, 9, 0 };
    T_EQ_MEM(out, nw_frame_encode(&f, out, sizeof out), ack, sizeof ack);
    T_EQ_INT(refused_everywhere(ack, sizeof ack, dec_frame), 0);

    memset(&f, 0, sizeof f);
    f.kind = NW_WS_CAST;
    f.gid_len = 2; memcpy(f.gid, "g1", 2);
    f.version = 9;
    f.blob.p = (const uint8_t *)"\x0c";
    f.blob.len = 1;
    const uint8_t cast[] = { 1, 52, 2, 'g', '1', 9, 0, 0, 0, 1, 0, 0, 0, 0x0c };
    T_EQ_MEM(out, nw_frame_encode(&f, out, sizeof out), cast, sizeof cast);
    T_EQ_INT(nw_frame_decode(cast, sizeof cast, &g), NW_OK);
    T_CHECK(g.blob.p == cast + 13 && g.blob.len == 1 && g.version == 9);
    T_EQ_INT(refused_everywhere(cast, sizeof cast, dec_frame), 0);

    memset(&f, 0, sizeof f);
    f.kind = NW_WS_CHAT;
    f.gid_len = 2; memcpy(f.gid, "g1", 2);
    f.chat.id_len = 1; f.chat.id[0] = 'm';
    f.chat.user_id_len = 1; f.chat.user_id[0] = 'u';
    f.chat.created_ms = 6;
    f.chat.message_len = 2; memcpy(f.chat.message, "hi", 2);
    const uint8_t chat[] = { 1, 53, 2, 'g', '1', 1, 'm', 1, 'u', 6, 0, 0, 0, 0, 0, 0, 2, 0, 'h', 'i' };
    T_EQ_MEM(out, nw_frame_encode(&f, out, sizeof out), chat, sizeof chat);
    T_EQ_INT(refused_everywhere(chat, sizeof chat, dec_frame), 0);

    const uint8_t ping[] = { 1, 43 }, pong[] = { 1, 54 };
    T_EQ_INT(nw_frame_decode(ping, 2, &g), NW_OK);
    T_EQ_INT(g.kind, NW_WS_PING);
    T_EQ_INT(nw_frame_decode(pong, 2, &g), NW_OK);
    const uint8_t not_frame[] = { 1, NW_SIGNIN };
    T_EQ_INT(nw_frame_decode(not_frame, 2, &g), NW_E_WIRE);
}

static void text_form(void) {
    // RFC 4648 section 10.
    static const char *const in[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char *const b64[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
    char t[16];
    uint8_t b[16];
    for (int i = 0; i < 7; i++) {
        int n = nw_text_encode((const uint8_t *)in[i], (int)strlen(in[i]), t, sizeof t);
        T_EQ_MEM(t, n, b64[i], strlen(b64[i]));
        n = nw_text_decode(b64[i], (int)strlen(b64[i]), b, sizeof b);
        T_EQ_MEM(b, n, in[i], strlen(in[i]));
    }
    // Every byte value survives the trip.
    uint8_t all[256], back[256];
    char text[352];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    int n = nw_text_encode(all, 256, text, sizeof text);
    T_EQ_INT(n, 344);
    T_EQ_INT(nw_text_decode(text, n, back, sizeof back), 256);
    T_EQ_MEM(back, 256, all, 256);
    T_EQ_INT(nw_text_decode("Zg=", 3, b, sizeof b), NW_E_WIRE);
    T_EQ_INT(nw_text_decode("Zg==Zg==", 8, b, sizeof b), NW_E_WIRE);
    T_EQ_INT(nw_text_decode("Z!==", 4, b, sizeof b), NW_E_WIRE);
    T_EQ_INT(nw_text_decode("Z=g=", 4, b, sizeof b), NW_E_WIRE);
    T_EQ_INT(nw_text_decode("Zm9v", 4, b, 2), NW_E_CAP);
    T_EQ_INT(nw_text_encode((const uint8_t *)"foo", 3, t, 3), NW_E_CAP);
}

int main(void) {
    T_RUN(requests);
    T_RUN(session);
    T_RUN(views_and_replays);
    T_RUN(lists);
    T_RUN(frames);
    T_RUN(text_form);
    return T_DONE();
}
