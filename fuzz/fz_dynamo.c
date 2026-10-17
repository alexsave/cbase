// fz_dynamo.c - every reader of what DynamoDB answers (json_surface.h dw_):
// the error classification, the transaction's cancellation reasons, the
// response's Item / Items / Responses / LastEvaluatedKey / NextToken, and
// every item decoder the store turns a row back into a struct with. In
// production the body comes over TLS from AWS; a decoder that trusts it is one
// bad answer (or one item a buggy writer stored) from a crash.
#include "fz.h"

#include "json_surface.h"
#include "store/store.h"

// Every item decoder on one item.
static void items_of(cb_span item) {
    cb_user_row u; (void)dw_item_user(item, &u);
    cb_refresh_row rr; (void)dw_item_refresh(item, &rr);
    cb_game_row g; memset(&g, 0, sizeof g);
    if (dw_item_game(item, &g)) FZ_CHECK(!g.state_len || g.state);
    cb_game_row_free(&g);
    cb_member_row m; (void)dw_item_member(item, &m);
    cb_uuid id; bool bot; (void)dw_seat_key(item, id, &bot);
    cb_bot_row b; (void)dw_item_bot(item, &b);
    cb_rating_row rt; (void)dw_item_rating(item, &rt);
    cb_snapshot_row s; memset(&s, 0, sizeof s);
    (void)dw_item_snapshot(item, &s);
    cb_snapshot_row_free(&s);
    cb_chat_row c; memset(&c, 0, sizeof c);
    (void)dw_item_chat(item, &c);
    cb_chat_row_free(&c);
    cb_conn_row cn; (void)dw_item_conn(item, &cn);
    char gid[CB_GAME_ID_MAX + 1]; int64_t a, z; (void)dw_item_scan(item, gid, &a, &z);
    (void)dw_item_email_user(item, id);
    cb_buf k; cb_buf_init(&k); (void)dw_item_key(item, &k); cb_buf_free(&k);
    cb_buf st; cb_buf_init(&st);
    const char *names[64], *values[64];
    int n = dw_item_text(item, &st, names, values, 64);
    FZ_CHECK(n <= 64);
    for (int i = 0; i < n; i++) { (void)strlen(names[i]); if (values[i]) (void)strlen(values[i]); }
    cb_buf_free(&st);
    char sv[64]; int64_t nv; bool bv; uint8_t *bp = NULL; size_t bl = 0;
    (void)dw_get_s(item, "pk", sv, sizeof sv);
    (void)dw_get_n(item, "version", &nv);
    (void)dw_get_bool(item, "needs_bots", &bv);
    if (dw_get_b(item, "state", &bp, &bl)) free(bp);
    char exp[64]; (void)dw_export_kind(item, exp, sizeof exp);
}

static void walk(cb_span arr) {
    size_t pos = 0;
    cb_span e;
    for (int i = 0; i < 64 && dw_array_next(arr, &pos, &e); i++) {
        FZ_CHECK(e.p >= arr.p && e.p + e.n <= arr.p + arr.n);
        items_of(e);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    int status = data[0] < 128 ? 200 : 400 + (data[0] & 0x7f);
    size_t n = size - 1;
    const char *body = (const char *)fz_dup(data + 1, n);

    (void)dw_classify(status, body, n);
    (void)dw_throttled(body, n);
    char text[256]; dw_error_text(body, n, text, sizeof text); FZ_CHECK(strlen(text) < sizeof text);
    bool has;
    for (int i = 0; i < 4; i++) (void)dw_cancel_reason(body, n, i, &has);
    (void)dw_cond_failed_has_item(body, n);

    cb_span item;
    if (dw_item(body, n, &item)) items_of(item);
    if (dw_attributes(body, n, &item)) items_of(item);
    walk(dw_items(body, n));
    walk(dw_batch_items(body, n, "cbase"));
    (void)dw_last_key(body, n);
    (void)dw_unprocessed(body, n);
    char tok[128]; dw_next_token(body, n, tok, sizeof tok); FZ_CHECK(strlen(tok) < sizeof tok);
    int64_t rcu, wcu; (void)dw_provisioned(body, n, &rcu, &wcu);
    (void)dw_consumed(body, n);

    // An object walk over the whole body, as dw_item_text does over an item.
    size_t pos = 0; cb_span k, v;
    for (int i = 0; i < 256 && dw_object_next(cb_span_of(body, n), &pos, &k, &v); i++)
        FZ_CHECK(v.p >= body && v.p + v.n <= body + n);

    const char *ids[3] = { "11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222", "ab12cd" };
    const bool is_bot[3] = { false, true, false };
    dw_rating_read rr[6];
    dw_ratings_read(body, n, "cbase", ids, is_bot, 3, false, rr);
    dw_ratings_read(body, n, "cbase", ids, is_bot, 3, true, rr);
    const char *left[3];
    int u = dw_unprocessed_games(body, n, "cbase", ids, 3, left);
    FZ_CHECK(u >= 0 && u <= 3);
    free((void *)body);
    return 0;
}
