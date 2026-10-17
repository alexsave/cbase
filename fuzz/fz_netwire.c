// fz_netwire.c - the binary surface's codec (kernel netwire.h), both ends:
// what the server decodes from a client (requests, stream frames, the text
// form of a frame) and what a client decodes from the server (the session,
// a view, a replay, every list and its rows). The same functions run in
// c_base, the web's wasm and the iOS app. Every decode that succeeds must
// re-encode to the same bytes.
#include "fz.h"

#include "netwire.h"

#define BIG (1 << 16)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1 || size > BIG) return 0;
    int which = data[0] % 4;
    const uint8_t *p = fz_dup(data + 1, size - 1);
    int len = (int)size - 1;
    uint8_t *out = malloc(BIG + 64);
    if (!out) abort();
    switch (which) {
    case 0: {   // server side: a request body, a stream frame
        static NwRequest q;
        if (nw_request_decode(p, len, &q) == NW_OK) {
            FZ_CHECK(q.name_len <= NW_NAME_MAX && q.password_len <= NW_PASSWORD_MAX);
            FZ_CHECK(q.refresh_len <= NW_REFRESH_MAX && q.message_len <= NW_CHAT_MAX);
            FZ_CHECK(nw_request_encode(&q, out, BIG) == len && memcmp(out, p, (size_t)len) == 0);
        }
        static NwFrame f;
        if (nw_frame_decode(p, len, &f) == NW_OK) {
            FZ_CHECK(f.gid_len <= NW_GID_MAX && f.token_len <= NW_ACCESS_MAX);
            FZ_CHECK(!f.blob.len || (f.blob.p >= p && f.blob.p + f.blob.len <= p + len));
            FZ_CHECK(nw_frame_encode(&f, out, BIG) == len && memcmp(out, p, (size_t)len) == 0);
        }
        break;
    }
    case 1: {   // the text form
        int k = nw_text_decode((const char *)p, len, out, BIG);
        if (k >= 0) {
            static NwFrame f;
            (void)nw_frame_decode(out, k, &f);
        }
        break;
    }
    case 2: {   // client side: single answers
        static NwSession s;
        if (nw_session_decode(p, len, &s) == NW_OK) {
            FZ_CHECK(s.access_len <= NW_ACCESS_MAX && s.refresh_len <= NW_REFRESH_MAX);
            FZ_CHECK(nw_session_encode(&s, out, BIG) == len && memcmp(out, p, (size_t)len) == 0);
        }
        uint32_t v; NwBlob e, m, x;
        if (nw_view_decode(p, len, &v, &e) == NW_OK) {
            FZ_CHECK(!e.len || (e.p >= p && e.p + e.len <= p + len));
            FZ_CHECK(nw_view_encode(v, e, out, BIG + 64) == len && memcmp(out, p, (size_t)len) == 0);
        }
        if (nw_replay_decode(p, len, &m, &x) == NW_OK)
            FZ_CHECK(nw_replay_encode(m, x, out, BIG + 64) == len && memcmp(out, p, (size_t)len) == 0);
        break;
    }
    case 3: {   // client side: the lists
        static const uint8_t kinds[] = { NW_GAMES, NW_CHATS, NW_BOTS, NW_BOARD, NW_HISTORY };
        for (size_t k = 0; k < sizeof kinds; k++) {
            int n = nw_list_open(p, len, kinds[k]);
            if (n < 0) continue;
            int has; uint32_t rating;
            if (kinds[k] == NW_HISTORY) FZ_CHECK(nw_history_rating(p, len, &has, &rating) == NW_OK);
            for (int i = 0; i < n && i < 512; i++) {
                int rc = 0;
                uint32_t v; NwBlob e;
                static NwChat c; static NwBot b; static NwBoardRow br; static NwHistoryRow h;
                switch (kinds[k]) {
                case NW_GAMES:
                    rc = nw_get_game(p, len, i, &v, &e);
                    if (!rc) FZ_CHECK(!e.len || (e.p >= p && e.p + e.len <= p + len));
                    break;
                case NW_CHATS: rc = nw_get_chat(p, len, i, &c); break;
                case NW_BOTS: rc = nw_get_bot(p, len, i, &b); break;
                case NW_BOARD: rc = nw_get_board(p, len, i, &br); break;
                case NW_HISTORY:
                    rc = nw_get_history(p, len, i, &h);
                    if (!rc) FZ_CHECK(h.n_players <= MAX_PLAYERS);
                    break;
                }
                // The list opened, so the whole body holds exactly n rows.
                FZ_CHECK(rc == NW_OK);
            }
        }
        break;
    }
    }
    free(out);
    free((void *)p);
    return 0;
}
