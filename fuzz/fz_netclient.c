// fz_netclient.c - the client library every host runs (kernel netclient.h),
// fed what a hostile or broken server could send it: any status and body for
// any request it makes, any stream frame (binary or base64 text), any saved
// session blob, in any order, with time moving. The library is a state
// machine (the session, the tickets, the subscriptions, the version gates), so
// the input is a program of steps, not one body.
//
// Every span the library hands back must lie inside the body it was handed.
#include "fz.h"

#include "netclient.h"
#include "table.h"

static int64_t g_now;

// A body for a request: raw input bytes, or one of the server's own answers
// built here around input bytes, so a run reaches the states past sign-in.
static int body_of(fz_in *in, uint8_t *out, int cap) {
    uint8_t mode = fz_u8(in);
    size_t got;
    const uint8_t *b = fz_bytes(in, fz_u16(in) % 2048, &got);
    if (mode % 4 == 1) {   // a session
        NwSession s;
        memset(&s, 0, sizeof s);
        s.access_len = (uint16_t)(got < 64 ? got : 64);
        memcpy(s.access, b, s.access_len);
        s.refresh_len = 8; memcpy(s.refresh, "refresh!", 8);
        s.expires_at_ms = (uint64_t)(g_now + (int64_t)(fz_u16(in)) * 1000);
        s.user_id_len = 36; memcpy(s.user_id, "11111111-1111-4111-8111-111111111111", 36);
        s.username_len = 3; memcpy(s.username, "BOB", 3);
        int n = nw_session_encode(&s, out, cap);
        return n > 0 ? n : 0;
    }
    if (mode % 4 == 2) {   // a view around the bytes as its envelope
        NwBlob e = { b, (uint32_t)got };
        int n = nw_view_encode(fz_u32(in) % 64, e, out, cap);
        return n > 0 ? n : 0;
    }
    if (mode % 4 == 3) {   // an action response
        int n = table_action_response(fz_u8(in) % 3, fz_u8(in), fz_u32(in) % 64, out, cap);
        return n > 0 ? n : 0;
    }
    memcpy(out, b, got);
    return (int)got;
}

static void check_done(const NcDone *d, int len) {
    if (!d) return;
    FZ_CHECK(d->a_len == 0 || (uint64_t)d->a_off + d->a_len <= (uint64_t)len);
    FZ_CHECK(d->b_len == 0 || (uint64_t)d->b_off + d->b_len <= (uint64_t)len);
    FZ_CHECK(d->gid_len <= NW_GID_MAX);
}

// n: the count a feed answered, or -1 after an answer (which does not say it).
static void events(int n) {
    for (int i = 0; i < (n < 0 ? NC_EVENTS_MAX : n); i++) {
        const NcEvent *e = nc_event(i);
        if (n < 0 && !e) break;
        FZ_CHECK(e != NULL);
        FZ_CHECK(e->gid_len <= NW_GID_MAX && e->payload_len <= NC_FRAME_MAX);
        const uint8_t *pl = nc_event_payload(i);
        if (e->payload_len) { FZ_CHECK(pl); volatile uint8_t x = pl[e->payload_len - 1]; (void)x; }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    nc_reset();
    g_now = 1790000000000LL;
    fz_in in = fz_in_of(data, size);
    static uint8_t body[NC_FRAME_MAX + 64];
    nc_set_boards(fz_u8(&in) & 1);
    for (int step = 0; step < 64 && fz_left(&in); step++) {
        switch (fz_u8(&in) % 10) {
        case 0: {   // the user asks for something
            NcIntent it;
            memset(&it, 0, sizeof it);
            it.kind = (uint8_t)(1 + fz_u8(&in) % 16);
            it.flag = fz_u8(&in) & 1;
            it.gid_len = 6; memcpy(it.gid, fz_u8(&in) & 1 ? "ab12cd" : "ff00ee", 6);
            it.name_len = 3; memcpy(it.name, "BOB", 3);
            it.password_len = 8; memcpy(it.password, "password", 8);
            size_t got;
            const uint8_t *b = fz_bytes(&in, fz_u8(&in) % 64, &got);
            it.bytes_len = (uint16_t)got; memcpy(it.bytes, b, got);
            it.intent = (int64_t)(fz_u8(&in)) - 1;
            (void)nc_do(&it, g_now);
            break;
        }
        case 1: {   // the next request is answered
            const NcRequest *r = nc_next_request(g_now);
            if (!r) break;
            FZ_CHECK(r->path_len <= NC_PATH_MAX && r->auth_len <= NC_AUTH_MAX && r->body_len <= NC_BODY_MAX);
            uint32_t id = r->id;
            int status = (int[]){ 200, 200, 200, 400, 401, 404, 409, 503, 0, 500 }[fz_u8(&in) % 10];
            int n = body_of(&in, body, sizeof body);
            uint8_t *exact = fz_dup(body, (size_t)n);
            check_done(nc_on_response(id, status, exact, n, g_now), n);
            events(-1);
            free(exact);
            break;
        }
        case 2: {   // a stream frame
            int is_text = fz_u8(&in) & 1;
            int n = body_of(&in, body, sizeof body);
            uint8_t *exact = fz_dup(body, (size_t)n);
            int k = nc_stream_in(exact, n, is_text, g_now);
            FZ_CHECK(k >= 0 && k <= NC_EVENTS_MAX);
            events(k);
            free(exact);
            break;
        }
        case 3: {
            const NcOut *o;
            for (int i = 0; i < 64 && (o = nc_stream_next(g_now)); i++) FZ_CHECK(o->len <= NC_TEXT_MAX);
            for (int i = 0; i < 64 && nc_next_done(); i++) {}
            break;
        }
        case 4: {
            int what = 1 + fz_u8(&in) % 3;
            const char *gid = fz_u8(&in) & 1 ? "ab12cd" : "ff00ee";
            if (fz_u8(&in) & 1) (void)nc_stream_sub(what, gid, 6);
            else (void)nc_stream_unsub(what, gid, 6);
            break;
        }
        case 5:
            if (fz_u8(&in) & 1) nc_stream_opened(g_now);
            else FZ_CHECK(nc_stream_closed(g_now) >= 0);
            break;
        case 6: {   // a saved session, as the host stored it
            size_t got;
            const uint8_t *b = fz_bytes(&in, fz_u16(&in) % NC_SESSION_BLOB_MAX, &got);
            uint8_t *exact = fz_dup(b, got);
            (void)nc_session_load(exact, (int)got);
            free(exact);
            uint8_t save[NC_SESSION_BLOB_MAX];
            int k = nc_session_save(save, sizeof save);
            FZ_CHECK(k <= (int)sizeof save);
            if (k > 0) FZ_CHECK(nc_session_load(save, k) == 0);
            break;
        }
        case 7: {   // the one-shot readers
            int status = fz_u8(&in) & 1 ? 200 : 400;
            int n = body_of(&in, body, sizeof body);
            uint8_t *x = fz_dup(body, (size_t)n);
            (void)nc_session_read(status, x, n);
            (void)nc_status_read(status, x, n);
            (void)nc_action_read(status, x, n);
            const NcGameRow *g = nc_view_read(status, x, n);
            if (g) FZ_CHECK((uint64_t)g->env_off + g->env_len <= (uint64_t)n);
            const NcReplay *rp = nc_replay_read(status, x, n);
            if (rp) FZ_CHECK((uint64_t)rp->moves_off + rp->moves_len <= (uint64_t)n && (uint64_t)rp->extras_off + rp->extras_len <= (uint64_t)n);
            static const int kinds[] = { NW_GAMES, NW_CHATS, NW_BOTS, NW_BOARD, NW_HISTORY };
            int kind = kinds[fz_u8(&in) % 5];
            int rows = nc_list_read(status, x, n, kind);
            for (int i = 0; i < rows && i < 256; i++) {
                const void *row = nc_row(kind, x, n, i);
                FZ_CHECK(row != NULL);
                if (kind == NW_GAMES) { const NcGameRow *gr = row; FZ_CHECK((uint64_t)gr->env_off + gr->env_len <= (uint64_t)n); }
                if (kind == NW_HISTORY) { const NcHistoryRow *h = row; FZ_CHECK((uint64_t)h->moves_off + h->moves_len <= (uint64_t)n); }
            }
            (void)nc_history_head(x, n);
            (void)nc_retry_ms(fz_u8(&in) & 1 ? NC_GET : NC_POST, fz_u8(&in) % 8, status, x, n);
            free(x);
            break;
        }
        case 8:
            g_now += (int64_t)fz_u16(&in) * 100;
            break;
        case 9:
            nc_gate_reset("ab12cd", 6);
            break;
        }
        FZ_CHECK(nc_due_ms(g_now) >= -1);
    }
    nc_reset();
    return 0;
}
