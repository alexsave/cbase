// seeds.c - writes the seed inputs of every fuzz target into <dir>/<target>/
// (`make fuzz-seeds`, then `make fuzz-merge` minimises them with what the
// fuzzers found). Each seed is made by the code that makes the real thing:
// the kernel's own encoders and a real game played through the kernel, the
// DynamoDB items the store's own writers produce, the tests' recorded Lambda
// event and unfurl payloads, a real bcrypt hash and a real minted token. A
// seed that is bytes nobody sends teaches the fuzzer nothing a random byte
// would not.
//
// Seed files are named seed-<what>, so a merge keeps them recognisable.
#include "fz.h"

#include "auth/bcrypt.h"
#include "auth/jwt.h"
#include "json_surface.h"
#include "net/ws.h"
#include "store/store.h"
#include "util/b64.h"

#include "../../shared/c/mqtt/mqtt.h"
#include "util/hex.h"
#include "util/str.h"

#include "fixture_cloudfront_event.h"
#include "fixture_unfurl.h"

#include "ask.h"
#include "awire.h"
#include "legal.h"
#include "netwire.h"
#include "table.h"

#include <errno.h>
#include <sys/stat.h>

static const char *g_root;
static int g_written;

static void put(const char *target, const char *name, const void *p, size_t n) {
    char dir[512], path[700];
    snprintf(dir, sizeof dir, "%s/%s", g_root, target);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/seed-%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (n) fwrite(p, 1, n, f);
    fclose(f);
    g_written++;
}
static void puts_(const char *target, const char *name, const char *s) { put(target, name, s, strlen(s)); }
// A seed with one leading selector byte.
static void put1(const char *target, const char *name, uint8_t sel, const void *p, size_t n) {
    uint8_t *b = malloc(n + 1);
    b[0] = sel;
    memcpy(b + 1, p, n);
    put(target, name, b, n + 1);
    free(b);
}

#define U1 "a0000000-0000-4000-8000-000000000001"
#define U2 "a0000000-0000-4000-8000-000000000002"
#define B1 "b0000000-0000-4000-8000-000000000001"

// ---- a real game, played through the kernel ------------------------------------------------

static Game *g_game;
static TableSnaps *g_snaps;
static Table g_t;
static uint8_t g_env[8][1 << 16];
static int g_env_len[8], g_n_env;
static uint8_t g_cast[16][1 << 16];
static int g_cast_len[16], g_n_cast;
static uint8_t g_state[1 << 16], g_roster[ROSTER_BYTES];
static int g_state_len;
static uint8_t g_wire[32][64];
static int g_wire_len[32], g_n_wire;

static void grab(int viewer_count, uint32_t version) {
    for (int v = -1; v < viewer_count && g_n_env < 8; v++) {
        int n = table_envelope(&g_t, "ab12cd", 6, v, version, g_env[g_n_env], sizeof g_env[0]);
        if (n > 0) g_env_len[g_n_env++] = n;
        if (g_n_cast < 16) {
            int c = table_cast(&g_t, "ab12cd", 6, v, version, g_cast[g_n_cast], sizeof g_cast[0]);
            if (c > 0) g_cast_len[g_n_cast++] = c;
        }
    }
}

static void play_game(void) {
    g_game = calloc(1, sizeof *g_game);
    g_snaps = calloc(1, sizeof *g_snaps);
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 1);
    table_init(&g_t, g_game, g_snaps);
    if (table_create(&g_t, U1, 36, "ANN", 3) != TABLE_OK) { fprintf(stderr, "seeds: create failed\n"); exit(1); }
    grab(1, 1);
    if (table_join(&g_t, U2, 36, "BOB", 3) < 0) { fprintf(stderr, "seeds: join failed\n"); exit(1); }
    grab(2, 2);
    if (table_ready(&g_t, U1, 36, seed) < 0 || table_ready(&g_t, U2, 36, seed) < 0) {
        fprintf(stderr, "seeds: ready failed\n");
        exit(1);
    }
    grab(2, 3);
    // A few moves, each the first legal one of a seat that has one.
    static LegalMove moves[512];
    for (int step = 0; step < 12 && g_game->status == GAME_STATUS_PLAYING; step++) {
        for (int s = 0; s < g_game->num_players; s++) {
            int n = legal_moves_into(g_game, s, moves, 512), k = -1;
            for (int i = 0; i < n && k < 0; i++) if (moves[i].type != MOVE_WAIT) k = i;
            if (k < 0) continue;
            AwireAction a;
            memset(&a, 0, sizeof a);
            a.kind = moves[k].type;
            a.n = moves[k].n_cards;
            for (int i = 0; i < a.n; i++) { a.cards[i] = moves[k].cards[i]; a.attacks[i] = moves[k].attack_cards[i]; }
            uint8_t w[64];
            int wn = awire_encode(&a, w, sizeof w);
            if (wn <= 0) continue;
            if (g_n_wire < 32) { memcpy(g_wire[g_n_wire], w, (size_t)wn); g_wire_len[g_n_wire++] = wn; }
            (void)table_act(&g_t, s == 0 ? U1 : U2, 36, w, wn, -1, 0);
            if (step % 4 == 0) grab(2, (uint32_t)(4 + step));
            break;
        }
    }
    g_state_len = table_seal(&g_t, g_game, &g_t.r, g_state, sizeof g_state);
    if (g_state_len < 0) g_state_len = 0;
    (void)roster_encode(&g_t.r, g_roster, sizeof g_roster);
}

// ---- the targets -----------------------------------------------------------------------------

static void seeds_http(void) {
    puts_("http", "get", "\x10GET /v1/games?since=3 HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer abc.def.ghi\r\n\r\n");
    puts_("http", "post", "\x10POST /v1/action HTTP/1.1\r\nHost: x\r\nContent-Type: application/octet-stream\r\nContent-Length: 5\r\n\r\nhello");
    puts_("http", "upgrade", "\xffGET /mqtt HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                             "Sec-WebSocket-Protocol: mqtt\r\n\r\n");
    puts_("http", "chunked", "\x10POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    puts_("http", "close10", "\x01HEAD / HTTP/1.0\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
}

static void seeds_ws(void) {
    uint8_t f[64];
    uint8_t mask[4] = { 1, 2, 3, 4 };
    size_t h = cb_ws_frame_header(f + 1, true, CB_WS_OP_TEXT, 5, mask);
    f[0] = 0xff;
    memcpy(f + 1 + h, "hello", 5);
    cb_ws_unmask(f + 1 + h, 5, mask);
    put("ws", "text-masked", f, 1 + h + 5);
    h = cb_ws_frame_header(f + 1, true, CB_WS_OP_CLOSE, 2, NULL);
    f[1 + h] = 0x03; f[2 + h] = 0xe8;
    put("ws", "close", f, 1 + h + 2);
    uint8_t big[600];
    big[0] = 0xff;
    h = cb_ws_frame_header(big + 1, false, CB_WS_OP_BIN, 300, mask);
    memset(big + 1 + h, 'x', 300);
    size_t at = 1 + h + 300;
    at += cb_ws_frame_header(big + at, true, CB_WS_OP_CONT, 4, mask);
    memcpy(big + at, "tail", 4);
    put("ws", "fragmented", big, at + 4);
    f[0] = 0x7f;
    h = cb_ws_frame_header(f + 1, true, CB_WS_OP_PING, 0, NULL);
    put("ws", "ping", f, 1 + h);
}

static void seeds_codec(void) {
    put1("codec", "b64", 0, "aGVsbG8gd29ybGQ=", 16);
    put1("codec", "b64url", 1, "aGVsbG8gd29ybGQ_-A", 18);
    put1("codec", "hex", 2, "\\x0102abCD", 10);
    put1("codec", "uuid", 3, U1, 36);
    put1("codec", "iso", 4, "2026-01-05 10:11:12.345678+00", 29);
    put1("codec", "isoz", 4, "2026-10-17T14:43:00Z", 20);
    put1("codec", "url", 5, "a%20b%2Fc", 9);
    put1("codec", "urlplus", 6, "a+b%3D", 6);
}

static void seeds_query(void) {
    puts_("query", "token", "token=a.b.c");
    puts_("query", "many", "select=view&game_id=eq.abc&apikey=k%20y&a=b+c&empty=&flag");
    puts_("query", "bad", "a=%zz&b=%");
}

static void seeds_event(void) {
    puts_("event", "cloudfront", CLOUDFRONT_SIGNUP_EVENT);
    puts_("event", "fn-url-b64",
          "{\"version\":\"2.0\",\"rawPath\":\"/v1/action\",\"rawQueryString\":\"a=1\",\"cookies\":[\"s=1\",\"t=2\"],"
          "\"headers\":{\"authorization\":\"Bearer x.y.z\",\"content-type\":\"application/octet-stream\"},"
          "\"requestContext\":{\"http\":{\"method\":\"POST\",\"path\":\"/v1/action\"}},\"body\":\"AQIDBA==\",\"isBase64Encoded\":true}");
    puts_("event", "scan", "\"scan\"");
    puts_("event", "bot-drive", "\"bot_drive:ab12cd\"");
    puts_("event", "eventbridge", "{\"source\":\"aws.events\",\"detail-type\":\"Scheduled Event\",\"detail\":{}}");
}

// Every object under an "Item" key, anywhere in a request body: the item
// layout exactly as the store writes it, and so as DynamoDB hands it back.
static int g_n_items;
static char *g_items[64];
static void harvest(cb_span v, int depth) {
    if (depth > 12 || v.n == 0) return;
    size_t pos = 0;
    cb_span k, x;
    if (v.p[0] == '{') {
        while (dw_object_next(v, &pos, &k, &x)) {
            if (k.n == 6 && memcmp(k.p, "\"Item\"", 6) == 0 && g_n_items < 64) g_items[g_n_items++] = fz_cstr((const uint8_t *)x.p, x.n);
            harvest(x, depth + 1);
        }
    } else if (v.p[0] == '[') {
        while (dw_array_next(v, &pos, &x)) harvest(x, depth + 1);
    }
}
static void take(dw_req *r) {
    harvest(cb_span_of((const char *)r->body.p, r->body.len), 0);
    dw_req_free(r);
    dw_req_init(r);
}

static void dynamo_items(void) {
    dw_req r;
    dw_req_init(&r);
    cb_user_row u;
    memset(&u, 0, sizeof u);
    snprintf(u.id, sizeof u.id, U1);
    snprintf(u.email, sizeof u.email, "ann@x.io");
    snprintf(u.password_hash, sizeof u.password_hash, "$2b$04$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
    snprintf(u.username, sizeof u.username, "ANN");
    u.created_at = 1000;
    dw_user_create(&r, "cbase", &u, 1000); take(&r);
    dw_user_import(&r, "cbase", &u); take(&r);
    cb_refresh_row rr;
    memset(&rr, 0, sizeof rr);
    memset(rr.token_hash, 'a', 64);
    snprintf(rr.user_id, sizeof rr.user_id, U1);
    snprintf(rr.session_id, sizeof rr.session_id, U2);
    dw_refresh_put(&r, "cbase", &rr); take(&r);
    cb_game_row g;
    memset(&g, 0, sizeof g);
    snprintf(g.id, sizeof g.id, "ab12cd");
    g.state = g_state; g.state_len = (size_t)g_state_len;
    g.roster = g_roster; g.roster_len = sizeof g_roster;
    static uint8_t logs[4] = { 1, 2, 3, 4 };
    g.logs = logs; g.logs_len = 4;
    g.has_seed = true; g.version = 3; g.needs_bots = true; g.created_at = 1; g.updated_at = 2;
    dw_game_create(&r, "cbase", &g, U1); take(&r);
    cb_snapshot_row s;
    memset(&s, 0, sizeof s);
    snprintf(s.id, sizeof s.id, U2);
    snprintf(s.game_id, sizeof s.game_id, "ab12cd");
    s.n_players = 2;
    snprintf(s.player_ids[0], sizeof s.player_ids[0], U1);
    snprintf(s.player_ids[1], sizeof s.player_ids[1], U2);
    s.moves = logs; s.moves_len = 4;
    dw_snapshot_put(&r, "cbase", &s); take(&r);
    cb_chat_row c;
    memset(&c, 0, sizeof c);
    snprintf(c.id, sizeof c.id, U2);
    snprintf(c.game_id, sizeof c.game_id, "ab12cd");
    snprintf(c.user_id, sizeof c.user_id, U1);
    c.message = "hi \"there\"";
    dw_chat_put(&r, "cbase", &c); take(&r);
    cb_rating_row rt;
    memset(&rt, 0, sizeof rt);
    snprintf(rt.user_id, sizeof rt.user_id, U1);
    snprintf(rt.username, sizeof rt.username, "ANN");
    rt.elo_rating = 1033; rt.previous_elo = 1020; rt.games_played = 7;
    dw_rating_import(&r, "cbase", &rt); take(&r);
    cb_bot_row b;
    memset(&b, 0, sizeof b);
    snprintf(b.id, sizeof b.id, B1);
    snprintf(b.nickname, sizeof b.nickname, "%%Miami 1");
    snprintf(b.strategy_key, sizeof b.strategy_key, "random");
    b.elo_rating = 1010;
    (void)dw_bots_replace(&r, "cbase", &b, 1, NULL, 0); take(&r);
    dw_req_free(&r);
}

static void seeds_dynamo(void) {
    cb_buf b;
    cb_buf_init(&b);
    for (int i = 0; i < g_n_items; i++) {
        char name[32];
        cb_buf_reset(&b);
        cb_buf_putc(&b, 0);
        cb_buf_printf(&b, "{\"Item\":%s}", g_items[i]);
        snprintf(name, sizeof name, "item-%d", i);
        put("dynamo", name, b.p, b.len);
        put("json", name, b.p, b.len);
        cb_buf_reset(&b);
        cb_buf_putc(&b, 0);
        cb_buf_printf(&b, "{\"Items\":[%s,%s],\"Count\":2,\"LastEvaluatedKey\":{\"pk\":{\"S\":\"x\"}},\"ConsumedCapacity\":{\"CapacityUnits\":1.5}}",
                      g_items[i], g_items[(i + 1) % g_n_items]);
        snprintf(name, sizeof name, "items-%d", i);
        put("dynamo", name, b.p, b.len);
        cb_buf_reset(&b);
        cb_buf_putc(&b, 0);
        cb_buf_printf(&b, "{\"Responses\":{\"cbase\":[%s]},\"UnprocessedKeys\":{}}", g_items[i]);
        snprintf(name, sizeof name, "batch-%d", i);
        put("dynamo", name, b.p, b.len);
    }
    put1("dynamo", "cond", 0x80 | 0, "{\"__type\":\"com.amazonaws.dynamodb.v20120810#ConditionalCheckFailedException\",\"message\":\"The conditional request failed\",\"Item\":{\"pk\":{\"S\":\"GAME#ab12cd\"}}}", 132);
    puts_("dynamo", "cancel",
          "\x80{\"__type\":\"com.amazonaws.dynamodb.v20120810#TransactionCanceledException\",\"message\":\"Transaction cancelled\","
          "\"CancellationReasons\":[{\"Code\":\"None\"},{\"Code\":\"ConditionalCheckFailed\",\"Item\":{\"pk\":{\"S\":\"x\"}}},{\"Code\":\"ThrottlingError\"}]}");
    puts_("dynamo", "throttle", "\x80{\"__type\":\"com.amazonaws.dynamodb.v20120810#ProvisionedThroughputExceededException\",\"message\":\"slow down\"}");
    puts_("dynamo", "describe",
          "\x01{\"Table\":{\"ProvisionedThroughput\":{\"ReadCapacityUnits\":25,\"WriteCapacityUnits\":25},"
          "\"GlobalSecondaryIndexes\":[{\"IndexName\":\"gsi1\",\"ProvisionedThroughput\":{\"ReadCapacityUnits\":5,\"WriteCapacityUnits\":5}}]}}");
    puts_("dynamo", "execute", "\x01{\"Items\":[],\"NextToken\":\"abc\"}");
    // The one-pass decode's corners (json_surface.c "one pass over an item"): an
    // escaped key, a type twice, a second type, an escaped slash in a blob,
    // backslash runs before a quote, an object keyed "0" in ConsumedCapacity.
    puts_("dynamo", "decode-escaped-key", "\x01{\"\\u0049tem\":{\"id\":{\"S\":\"a\\\\\\\"b\"},\"email\":{\"S\":\"e\"},\"password_hash\":{\"S\":\"h\"},\"created_at\":{\"N\":\"1\",\"N\":\"2\"}},\"ConsumedCapacity\":{\"0\":{\"CapacityUnits\":3}}}");
    puts_("dynamo", "decode-types", "\x01{\"Item\":{\"id\":{\"X\":1,\"S\":\"g\"},\"status\":{\"N\":\"1\"},\"version\":{\"N\":\"1\"},\"state\":{\"B\":\"AQ\\/9\"},\"id\":{\"S\":\"h\"},\"version\":\"7\"}}");
    puts_("dynamo", "decode-runs", "\x01{\"Items\":[{\"pk\":{\"S\":\"\\\\\\\\\"},\"sk\":{\"S\":\"\\\\\\\"\\\\\"}}],\"ConsumedCapacity\":[{\"CapacityUnits\":\"2\"}]}");
    cb_buf_free(&b);

    // The backend against a fake AWS: a program, then answers.
    // [split][program...][status pick][u16 len][body]...
    static const struct { uint8_t op; int item; } P[] = { { 0, 0 }, { 2, 3 }, { 3, -1 }, { 8, -1 }, { 11, -1 }, { 12, -1 }, { 13, -1 } };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
        cb_buf a;
        cb_buf_init(&a);
        cb_buf_putc(&a, 0);
        cb_buf_putc(&a, (char)P[i].op);
        a.p[0] = (uint8_t)((a.len) * 256 / 64);   // patched below
        size_t split = a.len;
        cb_buf body;
        cb_buf_init(&body);
        int it = P[i].item >= 0 && P[i].item < g_n_items ? P[i].item : (int)(i % (size_t)g_n_items);
        if (P[i].item >= 0) cb_buf_printf(&body, "{\"Item\":%s}", g_items[it]);
        else cb_buf_printf(&body, "{\"Items\":[%s],\"Count\":1}", g_items[it]);
        cb_buf_putc(&a, 0);
        uint16_t n = (uint16_t)body.len;
        cb_buf_putc(&a, (char)(n & 0xff));
        cb_buf_putc(&a, (char)(n >> 8));
        cb_buf_append(&a, body.p, body.len);
        // data[0] * (size - 1) / 256 + 1 == split
        size_t size = a.len;
        for (int d = 0; d < 256; d++)
            if (1 + (size_t)d * (size - 1) / 256 == split) { a.p[0] = (uint8_t)d; break; }
        char name[32];
        snprintf(name, sizeof name, "prog-%zu", i);
        put("dynamo_store", name, a.p, a.len);
        cb_buf_free(&a);
        cb_buf_free(&body);
    }
}

static void seeds_jwt(void) {
    cb_jwt_claims c;
    memset(&c, 0, sizeof c);
    snprintf(c.sub, sizeof c.sub, U1);
    snprintf(c.session_id, sizeof c.session_id, U2);
    snprintf(c.email, sizeof c.email, "ann@x.io");
    snprintf(c.username, sizeof c.username, "ANN");
    c.iat = 1790000000;
    c.exp = 1790003600;
    char tok[CB_JWT_MAX + 1];
    tok[0] = 0;
    int n = cb_jwt_mint("fuzz-secret-fuzz-secret-fuzz-secret!", &c, "https://fuzz", tok + 1, sizeof tok - 1);
    put("jwt", "minted", tok, (size_t)n + 1);
    cb_buf claims;
    cb_buf_init(&claims);
    cb_js_jwt_claims(&claims, &c, "https://fuzz");
    size_t hl;
    const char *h = cb_js_jwt_header(&hl);
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_putc(&b, 1);
    cb_buf_append(&b, h, hl);
    cb_buf_putc(&b, (char)0xff);
    cb_buf_append(&b, claims.p, claims.len);
    put("jwt", "parts", b.p, b.len);
    put1("json", "claims", 0, claims.p, claims.len);
    put1("json", "header", 0, h, hl);
    cb_buf_free(&b);
    cb_buf_free(&claims);
    puts_("json", "admin-query", "\x00{\"statement\":\"SELECT 1\",\"confirm\":\"yes \\u00e9\"}");
    puts_("json", "admin-id", "\x00{\"id\":\"" U1 "\"}");
}

static void seeds_bcrypt(void) {
    char h[CB_BCRYPT_HASH_LEN + 1];
    if (cb_bcrypt_hash("password", 4, h) != 0) { fprintf(stderr, "seeds: bcrypt failed\n"); exit(1); }
    char b[128];
    int n = snprintf(b, sizeof b, "%s%cpassword", h, 0);
    put("bcrypt", "cost4", b, (size_t)n);
    puts_("bcrypt", "gotrue", "$2a$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
}

static void seeds_https(void) {
    puts_("https_response", "len", "\x00HTTP/1.1 200 OK\r\nContent-Type: application/x-amz-json-1.0\r\nContent-Length: 2\r\n\r\n{}");
    puts_("https_response", "chunked", "\x00HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5;ext=1\r\npedia\r\n0\r\nTrailer: x\r\n\r\n");
    puts_("https_response", "eof", "\x02HTTP/1.1 202 Accepted\r\nConnection: close\r\n\r\nbody to the close");
    puts_("https_response", "head", "\x01HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\n");
    puts_("https_response", "nobody", "\x00HTTP/1.1 204 No Content\r\n\r\n");
}

static void seeds_table_wire(void) {
    for (int i = 0; i < g_n_wire && i < 6; i++) {
        uint8_t buf[128];
        memcpy(buf, "ab12cd", 6);
        memcpy(buf + 6, g_wire[i], (size_t)g_wire_len[i]);
        int n = table_request_encode(buf, sizeof buf, 6, g_wire_len[i], i & 1, 7);
        char name[32];
        snprintf(name, sizeof name, "request-%d", i);
        if (n > 0) put("table_wire", name, buf, (size_t)n);
        snprintf(name, sizeof name, "awire-%d", i);
        put("table_wire", name, g_wire[i], (size_t)g_wire_len[i]);
    }
    for (int k = ASK_JOIN; k <= ASK_BUMP; k++) {
        TableAsk a;
        memset(&a, 0, sizeof a);
        a.kind = (uint8_t)k;
        a.gid_len = 6; memcpy(a.gid, "ab12cd", 6);
        if (k == ASK_ADD_BOT || k == ASK_EXIT) { a.bot_len = 36; memcpy(a.bot, B1, 36); }
        if (k == ASK_EXIT) { a.player_len = 36; memcpy(a.player, U2, 36); }
        if (k == ASK_REARRANGE_HAND) { a.n_idx = 3; a.idx[0] = 2; a.idx[1] = 0; a.idx[2] = 1; }
        if (k == ASK_REARRANGE_PLAYERS) { a.n_order = 2; a.order[0].len = 36; memcpy(a.order[0].id, U2, 36); a.order[1].len = 36; memcpy(a.order[1].id, U1, 36); }
        if (k == ASK_UPDATE_NAME) { a.title_len = 9; memcpy(a.title, "Ann's \xe2\x9c\x93", 9); }
        if (k == ASK_SET_RULES) a.passing = 1;
        uint8_t out[ASK_BYTES_MAX];
        int n = ask_encode(&a, out, sizeof out);
        char name[32];
        snprintf(name, sizeof name, "ask-%d", k);
        if (n > 0) put("table_wire", name, out, (size_t)n);
    }
    uint8_t out[16];
    AskRefusal r = { ASK_WHY_REFUSED, -3, 7 };
    put("table_wire", "refusal", out, (size_t)ask_refusal_encode(&r, out, sizeof out));
    put("table_wire", "response", out, (size_t)table_action_response(TABLE_STATUS_REJECTED, 4, 9, out, sizeof out));
}

static void seeds_netwire(void) {
    static uint8_t out[1 << 17];
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNIN; q.name_len = 3; memcpy(q.name, "ANN", 3); q.password_len = 8; memcpy(q.password, "password", 8);
    put1("netwire", "signin", 0, out, (size_t)nw_request_encode(&q, out, sizeof out));
    memset(&q, 0, sizeof q);
    q.kind = NW_CHAT_SEND; q.message_len = 5; memcpy(q.message, "hello", 5);
    put1("netwire", "chat", 0, out, (size_t)nw_request_encode(&q, out, sizeof out));
    static NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_VIEW; f.gid_len = 6; memcpy(f.gid, "ab12cd", 6); f.version = 3;
    f.blob.p = g_env[0]; f.blob.len = (uint32_t)g_env_len[0];
    put1("netwire", "ws-view", 1, out, (size_t)nw_frame_encode(&f, out, sizeof out));
    static NwSession s;
    memset(&s, 0, sizeof s);
    s.access_len = 5; memcpy(s.access, "a.b.c", 5); s.refresh_len = 4; memcpy(s.refresh, "rfsh", 4);
    s.expires_at_ms = 1790000000000ull; s.user_id_len = 36; memcpy(s.user_id, U1, 36); s.username_len = 3; memcpy(s.username, "ANN", 3);
    put1("netwire", "session", 2, out, (size_t)nw_session_encode(&s, out, sizeof out));
    NwBlob e = { g_env[1], (uint32_t)g_env_len[1] };
    put1("netwire", "view", 2, out, (size_t)nw_view_encode(5, e, out, sizeof out));
    NwBlob m = { g_wire[0], (uint32_t)g_wire_len[0] };
    put1("netwire", "replay", 2, out, (size_t)nw_replay_encode(m, e, out, sizeof out));
    // Lists.
    int at = nw_list_head(NW_GAMES, 2, out, sizeof out);
    at += nw_put_game(3, e, out + at, (int)sizeof out - at);
    NwBlob e2 = { g_env[2], (uint32_t)g_env_len[2] };
    at += nw_put_game(4, e2, out + at, (int)sizeof out - at);
    put1("netwire", "games", 3, out, (size_t)at);
    static NwChat c;
    memset(&c, 0, sizeof c);
    c.id_len = 36; memcpy(c.id, U2, 36); c.user_id_len = 36; memcpy(c.user_id, U1, 36); c.message_len = 2; memcpy(c.message, "hi", 2);
    at = nw_list_head(NW_CHATS, 1, out, sizeof out);
    at += nw_put_chat(&c, out + at, (int)sizeof out - at);
    put1("netwire", "chats", 3, out, (size_t)at);
    static NwHistoryRow h;
    memset(&h, 0, sizeof h);
    h.id_len = 36; memcpy(h.id, U2, 36); h.n_players = 2;
    h.player_len[0] = 36; memcpy(h.player[0], U1, 36); h.player_len[1] = 36; memcpy(h.player[1], U2, 36);
    h.moves = m; h.extras = m;
    at = nw_history_head(1, 1033, 1, out, sizeof out);
    at += nw_put_history(&h, out + at, (int)sizeof out - at);
    put1("netwire", "history", 3, out, (size_t)at);
    static NwBoardRow br;
    memset(&br, 0, sizeof br);
    br.id_len = 36; memcpy(br.id, B1, 36); br.name_len = 3; memcpy(br.name, "BOT", 3); br.is_bot = 1; br.rating = 1200;
    at = nw_list_head(NW_BOARD, 1, out, sizeof out);
    at += nw_put_board(&br, out + at, (int)sizeof out - at);
    put1("netwire", "board", 3, out, (size_t)at);
}

static void seeds_client_table(void) {
    for (int i = 0; i < g_n_env; i++) {
        // [mode 0][identity len 0][envelope]
        uint8_t *b = malloc((size_t)g_env_len[i] + 3);
        b[0] = 0; b[1] = 0; b[2] = 0;
        memcpy(b + 3, g_env[i], (size_t)g_env_len[i]);
        char name[32];
        snprintf(name, sizeof name, "envelope-%d", i);
        put("client_table", name, b, (size_t)g_env_len[i] + 3);
        free(b);
    }
    for (int i = 0; i < g_n_cast; i++) {
        uint8_t *b = malloc((size_t)g_cast_len[i] + 3);
        b[0] = 1; b[1] = 0; b[2] = 0;
        memcpy(b + 3, g_cast[i], (size_t)g_cast_len[i]);
        char name[32];
        snprintf(name, sizeof name, "cast-%d", i);
        put("client_table", name, b, (size_t)g_cast_len[i] + 3);
        free(b);
    }
}

static void seeds_unfurl(void) {
    for (size_t i = 0; i < sizeof UNFURL_GOLDENS / sizeof UNFURL_GOLDENS[0]; i++) {
        char name[32];
        snprintf(name, sizeof name, "golden-%zu", i);
        puts_("unfurl", name, UNFURL_GOLDENS[i].payload);
    }
}

static void seeds_import(void) {
    char sh[2 * sizeof g_state + 1], rh[2 * ROSTER_BYTES + 1];
    cb_hex_encode(g_state, (size_t)g_state_len, sh, sizeof sh);
    cb_hex_encode(g_roster, sizeof g_roster, rh, sizeof rh);
    char hash[CB_BCRYPT_HASH_LEN + 1];
    cb_bcrypt_hash("password", 4, hash);
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_printf(&b,
                  "id,email,encrypted_password,username,created_at,last_sign_in_at\n"
                  "\"" U1 "\",\"ann@x.io\",\"%s\",\"ANN\",\"2026-01-05 10:11:12.345678+00\",\"5000\"\n"
                  "\"" U2 "\",\"bob@x.io\",\"%s\",,\"2000\",\n", hash, hash);
    cb_buf_putc(&b, 0);
    cb_buf_puts(&b, "id,nickname,strategy_key,elo_rating,previous_elo,games_played,created_at,updated_at\n"
                    "\"" B1 "\",\"%Miami 1\",\"random\",\"1010\",\"1000\",\"4\",\"1\",\"2\"\n");
    cb_buf_putc(&b, 0);
    cb_buf_puts(&b, "user_id,username,elo_rating,previous_elo,games_played,updated_at\n\"" U1 "\",\"ANN\",\"1033\",\"1020\",\"7\",\"3000\"\n");
    cb_buf_putc(&b, 0);
    cb_buf_printf(&b,
                  "id,status,state,roster,needs_bots,game_seed,logs_packed,version,round_epoch,created_at,updated_at,last_commit_at\n"
                  "\"ab12cd\",\"playing\",\"\\x%s\",\"\\x%s\",\"false\",,,\"3\",\"0\",\"1000\",\"4000\",\"3900\"\n", sh, rh);
    cb_buf_putc(&b, 0);
    cb_buf_puts(&b, "game_id,player_id,joined_at\n\"ab12cd\",\"" U1 "\",\"1000\"\n\"ab12cd\",\"" U2 "\",\"1000\"\n");
    cb_buf_putc(&b, 0);
    cb_buf_puts(&b, "game_id,bot_id\n");
    cb_buf_putc(&b, 0);
    cb_buf_puts(&b, "id,game_id,player_ids,moves,extras,created_at\n\"" U2 "\",\"ab12cd\",\"" U1 ";" U2 "\",\"0102ff\",,\"4500\"\n");
    cb_buf_putc(&b, 0);
    cb_buf_puts(&b, "id,game_id,user_id,message,is_system,created_at\n\"" B1 "\",\"ab12cd\",\"" U1 "\",\"hi, \"\"you\"\"\n2nd line\",\"false\",\"4600\"\n");
    put("import", "export", b.p, b.len);
    cb_buf_free(&b);
}

// The netclient target's programs (fz_netclient.c): sign in, then ask for
// things and answer them with the server's real bodies, and feed the stream.
static void nc_raw(cb_buf *b, const uint8_t *p, size_t n) {   // body_of mode 0
    cb_buf_putc(b, 0);
    cb_buf_putc(b, (char)(n & 0xff));
    cb_buf_putc(b, (char)(n >> 8));
    cb_buf_append(b, p, n);
}
static void seeds_netclient(void) {
    static uint8_t out[1 << 16];
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_putc(&b, 0);                                   // boards off
    // sign in: intent kind 1 -> NC_DO_SIGN_IN
    const uint8_t signin[] = { 0, 1, 0, 0, 0, 0 };
    cb_buf_append(&b, signin, sizeof signin);
    // the answer: step 1, status pick 0 (200), body mode 1 (a session), 8 token bytes, expiry
    const uint8_t sess[] = { 1, 0, 1, 8, 0, 'a', '.', 'b', '.', 'c', 'd', 'e', 'f', 0x10, 0x0e };
    cb_buf_append(&b, sess, sizeof sess);
    // the dashboard: intent kind 8 -> NC_DO_GAMES, answered with a real list
    const uint8_t games[] = { 0, 8, 0, 0, 0, 0 };
    cb_buf_append(&b, games, sizeof games);
    cb_buf_putc(&b, 1);
    cb_buf_putc(&b, 0);
    NwBlob e = { g_env[1], (uint32_t)g_env_len[1] };
    int at = nw_list_head(NW_GAMES, 1, out, sizeof out);
    at += nw_put_game(3, e, out + at, (int)sizeof out - at);
    nc_raw(&b, out, (size_t)at);
    // open the game: kind 9 -> NC_DO_OPEN_GAME, answered with a view (mode 2)
    const uint8_t open[] = { 0, 9, 0, 1, 0, 0 };
    cb_buf_append(&b, open, sizeof open);
    cb_buf_putc(&b, 1);
    cb_buf_putc(&b, 0);
    cb_buf_putc(&b, 0);
    int vn = nw_view_encode(4, e, out, sizeof out);
    cb_buf_putc(&b, (char)(vn & 0xff));
    cb_buf_putc(&b, (char)(vn >> 8));
    cb_buf_append(&b, out, (size_t)vn);
    // the stream: subscribe, open, a VIEW and a CAST frame
    const uint8_t sub[] = { 4, 2, 1, 1, 5, 1 };
    cb_buf_append(&b, sub, sizeof sub);
    static NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_VIEW; f.gid_len = 6; memcpy(f.gid, "ab12cd", 6); f.version = 5;
    f.blob.p = g_env[2]; f.blob.len = (uint32_t)g_env_len[2];
    int fn = nw_frame_encode(&f, out, sizeof out);
    cb_buf_putc(&b, 2); cb_buf_putc(&b, 0);
    nc_raw(&b, out, (size_t)fn);
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_CAST; f.gid_len = 6; memcpy(f.gid, "ab12cd", 6); f.version = 6;
    f.blob.p = g_cast[3]; f.blob.len = (uint32_t)g_cast_len[3];
    fn = nw_frame_encode(&f, out, sizeof out);
    cb_buf_putc(&b, 2); cb_buf_putc(&b, 0);
    nc_raw(&b, out, (size_t)fn);
    const uint8_t drain[] = { 3, 8, 0x30, 0x75, 3 };
    cb_buf_append(&b, drain, sizeof drain);
    put("netclient", "session-games-stream", b.p, b.len);
    cb_buf_free(&b);
}

// The table target's programs (fz_table.c): a seed, the table's shape, then
// legal moves with a probe between them.
static void seeds_table(void) {
    static const uint8_t SHAPES[][2] = { { 1, 1 }, { 2, 0 }, { 1, 3 }, { 3, 3 }, { 0, 5 } };
    for (size_t k = 0; k < sizeof SHAPES / sizeof SHAPES[0]; k++) {
        uint8_t p[1024];
        size_t n = 0;
        for (int i = 0; i < 32; i++) p[n++] = (uint8_t)(i * 11 + k);
        p[n++] = SHAPES[k][0];       // humans - 1
        p[n++] = SHAPES[k][1];       // bots
        for (int b = 0; b < 5; b++) p[n++] = 0;   // every bot plays at random
        for (int i = 0; i < 150 && n + 8 < sizeof p; i++) {
            p[n++] = 0; p[n++] = (uint8_t)i; p[n++] = (uint8_t)(i * 7); p[n++] = 0;   // a legal move
            if (i % 5 == 2) { p[n++] = 3; p[n++] = (uint8_t)i; p[n++] = 0; p[n++] = 0; p[n++] = 0; p[n++] = (uint8_t)(i * 13); p[n++] = 1; }
            if (i % 9 == 4) { p[n++] = 7; }
        }
        char name[32];
        snprintf(name, sizeof name, "shape-%zu", k);
        put("table", name, p, n);
    }
}

// The sequence target's programs (fz_sequence.c): op = 12 * user + step.
#define OP(user, step) (uint8_t)(12 * (user) + (step))
static void seeds_sequence(void) {
    uint8_t p[1024];
    size_t n;
    // One player and a bot: create, add the bot, start, then moves and bot
    // drives, with reads and a stream subscription along the way.
    n = 0;
    p[n++] = 7; p[n++] = 0; p[n++] = 0; p[n++] = 0;          // seed
    p[n++] = OP(0, 0);                                        // create
    // conn0 CONNECT as user0: one packet, CONNECT, user 0, a good token, its own client id, keep-alive 60 s
    p[n++] = OP(0, 7); p[n++] = 0; p[n++] = 1; p[n++] = 0; p[n++] = 0; p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 1;
    // conn0 SUBSCRIBE: user 0, one filter, its own seat's casts of game 0, packet id 1
    p[n++] = OP(0, 7); p[n++] = 0; p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 0; p[n++] = 0; p[n++] = 1; p[n++] = 0; p[n++] = 1; p[n++] = 0;
    p[n++] = OP(0, 1); p[n++] = 2; p[n++] = 0; p[n++] = 0;  // ADD_BOT
    p[n++] = OP(0, 1); p[n++] = 1; p[n++] = 0; p[n++] = 0;  // START
    for (int i = 0; i < 40; i++) {
        p[n++] = OP(0, 3); p[n++] = 0; p[n++] = 0; p[n++] = (uint8_t)i; p[n++] = 0;   // a legal move
        p[n++] = OP(0, 8); p[n++] = 1; p[n++] = 0;                                  // the bots
        if (i % 6 == 3) { p[n++] = OP(0, 9); p[n++] = 0xff; p[n++] = 0x0f; p[n++] = OP(0, 8); p[n++] = 0; }   // time, then the heartbeat
        if (i % 11 == 7) { p[n++] = OP(0, 10); p[n++] = 4; p[n++] = 0; p[n++] = OP(0, 8); p[n++] = 0; p[n++] = OP(0, 8); p[n++] = 1; p[n++] = 0; }   // a planted marker, the scan, a drive
        if (i % 8 == 0) { p[n++] = OP(0, 5); p[n++] = 1; p[n++] = 0; }               // GET the game
    }
    put("sequence", "solo-vs-bot", p, n);
    // Two players: create, join, both start, play; the other one reads, a
    // spectator stream, an unauthorised probe, a CAS race.
    n = 0;
    p[n++] = 9; p[n++] = 0; p[n++] = 0; p[n++] = 0;
    p[n++] = OP(0, 0);
    p[n++] = OP(1, 1); p[n++] = 0; p[n++] = 0; p[n++] = 0;  // user1 JOIN
    p[n++] = OP(0, 1); p[n++] = 1; p[n++] = 0; p[n++] = 0;  // user0 START
    p[n++] = OP(1, 1); p[n++] = 1; p[n++] = 0; p[n++] = 0;  // user1 START
    // conn2: user2 CONNECTs and subscribes to the game's spectator casts (s/<game>)
    p[n++] = OP(2, 7); p[n++] = 2; p[n++] = 1; p[n++] = 0; p[n++] = 2; p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 1;
    p[n++] = OP(2, 7); p[n++] = 2; p[n++] = 1; p[n++] = 1; p[n++] = 2; p[n++] = 0; p[n++] = 0; p[n++] = 3; p[n++] = 0; p[n++] = 1; p[n++] = 0;
    // conn1: user1 CONNECTs and subscribes to its views (u/<user1>/v)
    p[n++] = OP(1, 7); p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 1; p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 1;
    p[n++] = OP(1, 7); p[n++] = 1; p[n++] = 1; p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 0; p[n++] = 0; p[n++] = 1; p[n++] = 0;
    // conn3, three packets in one message: user1's CONNECT, its seat's casts and chat, a PINGREQ
    p[n++] = OP(1, 7); p[n++] = 3; p[n++] = 0; p[n++] = 1;
    p[n++] = 0; p[n++] = 1; p[n++] = 1; p[n++] = 1; p[n++] = 7; p[n++] = 1;
    p[n++] = 1; p[n++] = 1; p[n++] = 1; p[n++] = 0; p[n++] = 1; p[n++] = 0; p[n++] = 0; p[n++] = 2; p[n++] = 0; p[n++] = 2; p[n++] = 0;
    p[n++] = 3; p[n++] = 1;
    p[n++] = OP(0, 10); p[n++] = 1;                         // a racing writer
    for (int i = 0; i < 40; i++) {
        p[n++] = OP(i & 1, 3); p[n++] = 0; p[n++] = 0; p[n++] = (uint8_t)(i * 3); p[n++] = 0;
        if (i % 10 == 0) { p[n++] = OP(1, 5); p[n++] = 0; }  // user1's dashboard
        if (i % 10 == 5) { p[n++] = OP(3, 6); p[n++] = (uint8_t)i; p[n++] = (uint8_t)(i % 5); p[n++] = 0; p[n++] = 4; p[n++] = 'x'; p[n++] = 'y'; p[n++] = 'z'; p[n++] = 'w'; }
    }
    p[n++] = OP(0, 1); p[n++] = 3; p[n++] = 0; p[n++] = 0;  // EXIT
    p[n++] = OP(1, 1); p[n++] = 3; p[n++] = 0; p[n++] = 1;  // EXIT
    put("sequence", "two-players", p, n);
}

// The MQTT codec, the hub's side and the IoT authorizer's request: packets the
// shared codec writes, a whole session as WebSocket messages, and the event IoT
// documents with a real token's shape.
static void seeds_mqtt(void) {
    uint8_t b[2048], all[4096];
    const char *uid = U1;
    char id[64], tok[64], topic[96];
    snprintf(id, sizeof id, "%s-seed", uid);
    snprintf(tok, sizeof tok, "tok-%s", uid);
    snprintf(topic, sizeof topic, "u/%s/#", uid);
    MqttTopic t[2] = { { topic, (int)strlen(topic), 0 }, { "s/ab12cd", 8, 0 } };
    // Client packets, back to back (mode 1), and server packets (mode 0).
    int at = 0;
    all[at++] = 0x11;
    at += mqtt_write_connect(all + at, (int)sizeof all - at, id, (int)strlen(id), 60, "token", 5, (const uint8_t *)tok, (int)strlen(tok));
    at += mqtt_write_subscribe(all + at, (int)sizeof all - at, 1, t, 2);
    at += mqtt_write_unsubscribe(all + at, (int)sizeof all - at, 2, t, 1);
    at += mqtt_write_bare(all + at, (int)sizeof all - at, MQTT_PINGREQ);
    at += mqtt_write_bare(all + at, (int)sizeof all - at, MQTT_DISCONNECT);
    put("mqtt", "client-stream", all, (size_t)at);
    at = 0;
    all[at++] = 0x10;
    static const uint8_t codes[] = { 0, MQTT_SUBACK_FAILURE };
    at += mqtt_write_connack(all + at, (int)sizeof all - at, 0, 0);
    at += mqtt_write_suback(all + at, (int)sizeof all - at, 1, codes, 2);
    snprintf(topic, sizeof topic, "u/%s/v", uid);
    static const uint8_t push[] = { 1, NW_WS_VIEW, 6, 'a', 'b', '1', '2', 'c', 'd', 7, 0, 0, 0, 1, 0, 0, 0, 0xee };
    at += mqtt_write_publish(all + at, (int)sizeof all - at, topic, (int)strlen(topic), push, sizeof push);
    at += mqtt_write_unsuback(all + at, (int)sizeof all - at, 2);
    at += mqtt_write_bare(all + at, (int)sizeof all - at, MQTT_PINGRESP);
    put("mqtt", "server-stream", all, (size_t)at);
    static const char match[] = "\x02u/+/c/#\0u/a/c/g1";
    put("mqtt", "match", match, sizeof match - 1);
    // The hub: [query token?] then messages of [length/8][time step][bytes].
    at = 0;
    all[at++] = 0;
    int parts[4], np = 0;
    parts[np++] = mqtt_write_connect(b, sizeof b, id, (int)strlen(id), 60, "token", 5, (const uint8_t *)tok, (int)strlen(tok));
    for (int i = 0; i < 1; i++) {
        all[at++] = (uint8_t)((parts[0] - 1) / 8);
        all[at++] = 0;
        memcpy(all + at, b, (size_t)parts[0]);
        at += parts[0];
    }
    int n = mqtt_write_subscribe(b, sizeof b, 1, t, 2);
    all[at++] = (uint8_t)((n - 1) / 8);
    all[at++] = 0;
    memcpy(all + at, b, (size_t)n);
    at += n;
    n = mqtt_write_bare(b, sizeof b, MQTT_PINGREQ);
    all[at++] = 0;
    all[at++] = 0;
    memcpy(all + at, b, (size_t)n);
    at += n;
    put("mqtt_hub", "session", all, (size_t)at);
    // The authorizer: the request IoT documents, the token as the MQTT password.
    char pw64[128], ev[1024];
    cb_b64_encode((const uint8_t *)tok, strlen(tok), pw64, sizeof pw64);
    snprintf(ev, sizeof ev,
             "{\"token\":\"\",\"signatureVerified\":false,\"protocols\":[\"tls\",\"http\",\"mqtt\"],"
             "\"protocolData\":{\"tls\":{\"serverName\":\"api.foolish.cards\"},\"http\":{\"headers\":"
             "{\"X-Cbase-Origin\":\"0123456789abcdef0123456789abcdef\"},\"queryString\":"
             "\"?x-amz-customauthorizer-name=cbase-stream\"},\"mqtt\":{\"username\":\"token\",\"password\":\"%s\","
             "\"clientId\":\"%s\"}},\"connectionMetadata\":{\"id\":\"c1\"}}",
             pw64, id);
    puts_("iot_auth", "connect", ev);
    puts_("event", "iot-connect", ev);   // the Lambda's own decoder takes the same event
    snprintf(ev, sizeof ev, "{\"protocolData\":{\"http\":{\"headers\":{},\"queryString\":\"?token=%s\"}}}", tok);
    puts_("iot_auth", "query-token", ev);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: seeds <corpus dir>\n"); return 2; }
    g_root = argv[1];
    if (mkdir(g_root, 0755) != 0 && errno != EEXIST) { perror(g_root); return 1; }
    play_game();
    dynamo_items();
    seeds_http();
    seeds_ws();
    seeds_codec();
    seeds_query();
    seeds_event();
    seeds_dynamo();
    seeds_jwt();
    seeds_bcrypt();
    seeds_https();
    seeds_table_wire();
    seeds_netwire();
    seeds_client_table();
    seeds_unfurl();
    seeds_import();
    seeds_netclient();
    seeds_table();
    seeds_sequence();
    seeds_mqtt();
    printf("seeds: %d files (%d DynamoDB items, %d envelopes, %d casts, %d moves)\n", g_written, g_n_items, g_n_env,
           g_n_cast, g_n_wire);
    return 0;
}
