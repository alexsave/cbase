// seeds.c - writes the seed inputs of the platform's fuzz targets into
// <dir>/<target>/ (`make fuzz-seeds`, then `make fuzz-merge` minimises them with
// what the fuzzers found; a product's own targets get theirs from its own seed
// writer, APP_FUZZ_SEEDS). Each seed is made by the code that makes the real
// thing: the DynamoDB items the store's own writers produce, the tests' recorded
// Lambda event, a real bcrypt hash and a real minted token. A
// seed that is bytes nobody sends teaches the fuzzer nothing a random byte
// would not.
//
// Seed files are named seed-<what>, so a merge keeps them recognisable.
#include "fz.h"

#include "auth/bcrypt.h"
#include "auth/jwt.h"
#include "aws_json.h"
#include "store/store_priv.h"
#include "net/ws.h"
#include "b64/b64.h"

#include "mqtt/mqtt.h"
#include "hex/hex.h"
#include "util/str.h"

#include "fixture_cloudfront_event.h"


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

// cb_viewer_addr_parse: [cap][text] (core/fuzz/fz_viewer_addr.c); cb_viewer_key: [mode][cap][header NUL xff NUL peer]
// (core/fuzz/fz_viewer_key.c).
#define PUT1L(t, n, sel, lit) put1(t, n, sel, lit, sizeof(lit) - 1)
static void seeds_viewer(void) {
    PUT1L("viewer_addr", "v4", 64, "203.0.113.7:50238");
    PUT1L("viewer_addr", "v4-port-1", 64, "203.0.113.7:1");
    PUT1L("viewer_addr", "v4-port-65535", 64, "203.0.113.7:65535");
    PUT1L("viewer_addr", "v4-port-0", 64, "203.0.113.7:0");
    PUT1L("viewer_addr", "v4-port-65536", 64, "203.0.113.7:65536");
    PUT1L("viewer_addr", "v4-no-port", 64, "203.0.113.7");
    PUT1L("viewer_addr", "v6-bare", 64, "2001:db8:1:2::9:50238");
    PUT1L("viewer_addr", "v6-bracket", 64, "[2001:db8:1:2::9]:50238");
    PUT1L("viewer_addr", "v6-bracket-v4", 64, "[203.0.113.7]:80");
    PUT1L("viewer_addr", "v6-unclosed", 64, "[2001:db8::1:80");
    PUT1L("viewer_addr", "v6-loopback", 64, "[::1]:80");
    PUT1L("viewer_addr", "blank", 64, "203.0.113.7:80 ");
    PUT1L("viewer_addr", "x63", 64, "1111111111111111111111111111111111111111111111111111111111111:1");
    PUT1L("viewer_addr", "x64", 64, "11111111111111111111111111111111111111111111111111111111111111:1");
    PUT1L("viewer_addr", "cap-0", 0, "203.0.113.7:80");
    PUT1L("viewer_addr", "cap-8", 8, "[2001:db8:1:2::9]:443");
    PUT1L("viewer_addr", "cap-exact", 8, "1.2.3.4:1");
    PUT1L("viewer_key", "serve-proven", 3, "\x40" "203.0.113.7:50238\0" "203.0.113.99:80\0" "130.176.0.1");
    PUT1L("viewer_key", "serve-unproven", 1, "\x40" "203.0.113.7:50238\0\0" "130.176.0.1");
    PUT1L("viewer_key", "serve-no-secret", 0, "\x40" "203.0.113.7:50238\0\0" "130.176.0.1");
    PUT1L("viewer_key", "edge", 4, "\x40" "[2001:db8:1:2::9]:443\0\0" "130.176.0.1");
    PUT1L("viewer_key", "xff-only", 3, "\x40\0" "203.0.113.99:80\0" "130.176.0.1");
    PUT1L("viewer_key", "small-cap", 3, "\x08" "203.0.113.7:50238\0\0" "130.176.0.1");
}

// num_parse_i64: [mode][range for mode 3][text] (core/fuzz/fz_num.c).
static void seeds_num(void) {
    put1("num", "speedup", 0, "20", 2);
    put1("num", "space", 0, " 20", 3);
    put1("num", "plus", 0, "+20", 3);
    put1("num", "trailing", 0, "20x", 3);
    put1("num", "neg", 0, "-5", 2);
    put1("num", "max", 0, "9223372036854775807", 19);
    put1("num", "min", 0, "-9223372036854775808", 20);
    put1("num", "max-plus-one", 0, "9223372036854775808", 19);
    put1("num", "min-minus-one", 0, "-9223372036854775809", 20);
    put1("num", "u64-max", 0, "18446744073709551615", 20);
    put1("num", "zeros", 0, "0007", 4);
    put1("num", "neg-zero", 0, "-0", 2);
    put1("num", "empty", 0, "", 0);
    put1("num", "dash", 0, "-", 1);
    put1("num", "nul", 0, "12\0" "3", 4);
    put1("num", "after-wrap", 1, "-1", 2);
    put1("num", "cursor", 1, "42", 2);
    put1("num", "port", 2, "8443", 4);
    put1("num", "port-zero", 2, "0", 1);
    put1("num", "port-over", 2, "65536", 5);
    // Mode 3: the range from the input, little-endian lo then hi.
    uint8_t r[18];
    int64_t lo = -5, hi = 5;
    memcpy(r, &lo, 8);
    memcpy(r + 8, &hi, 8);
    memcpy(r + 16, "-5", 2);
    put1("num", "range", 3, r, 18);
    memcpy(r, &hi, 8);
    memcpy(r + 8, &lo, 8);
    r[16] = '0';
    put1("num", "range-reversed", 3, r, 17);
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
    // The literals at the edges of jobs.h: a 63-byte key taken and a 64-byte
    // one refused, a second colon, a schedule name of 31 and 32 bytes.
    char lit[128];
    snprintf(lit, sizeof lit, "\"drive:%.63s\"", "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_");
    puts_("event", "job-key-63", lit);
    snprintf(lit, sizeof lit, "\"drive:%.64s\"", "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_x");
    puts_("event", "job-key-64", lit);
    puts_("event", "job-two-colons", "\"a:b:c\"");
    puts_("event", "schedule-31", "\"abcdefghijklmnopqrstuvwxyz_0123\"");
    puts_("event", "schedule-32", "\"abcdefghijklmnopqrstuvwxyz_01234\"");
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
// The DynamoDB store itself, answering 200 "{}" to everything: every request
// body it sends is harvested, so the items are exactly the ones its writes
// put (the row codecs in store/rows_foolish.c and rows_auth.c).
static int harvest_send(void *ud, const char *target, const uint8_t *body, size_t len, int *status, cb_buf *resp) {
    (void)ud; (void)target;
    harvest(cb_span_of((const char *)body, len), 0);
    *status = 200;
    cb_buf_puts(resp, "{}");
    return CB_OK;
}

static void dynamo_items(void) {
    cb_store *st = NULL;
    if (cb_store_open_dynamo_with("cbase", harvest_send, NULL, &st) != CB_OK) abort();
    cb_user_row u;
    memset(&u, 0, sizeof u);
    snprintf(u.id, sizeof u.id, U1);
    snprintf(u.email, sizeof u.email, "ann@x.io");
    snprintf(u.password_hash, sizeof u.password_hash, "$2b$04$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
    snprintf(u.username, sizeof u.username, "ANN");
    u.created_at = 1000;
    (void)CB_STORE_CALL(st, user_create, &u);
    (void)CB_STORE_CALL(st, user_import, &u);
    cb_refresh_row rf;
    memset(&rf, 0, sizeof rf);
    memset(rf.token_hash, 'a', 64);
    snprintf(rf.user_id, sizeof rf.user_id, U1);
    snprintf(rf.session_id, sizeof rf.session_id, U2);
    rf.created_at = 5;
    rf.expires_at = 9000;
    (void)CB_STORE_CALL(st, refresh_put, &rf);
    cb_store_close(st);
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
    // The one-pass decode's corners (core_json.c or aws_json.c "one pass over an item"): an
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
    int n = cb_jwt_mint("fuzz-secret-fuzz-secret-fuzz-secret!", &c, tok + 1, sizeof tok - 1);
    put("jwt", "minted", tok, (size_t)n + 1);
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

// The sequence target's programs (fz_sequence.c): op = 12 * user + step.
#define OP(user, step) (uint8_t)(12 * (user) + (step))

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
    static const uint8_t push[] = { 1, 0x21, 6, 'a', 'b', '1', '2', 'c', 'd', 7, 0, 0, 0, 1, 0, 0, 0, 0xee };
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
    b64_encode((const uint8_t *)tok, strlen(tok), pw64, sizeof pw64);
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
    dynamo_items();
    seeds_http();
    seeds_ws();
    seeds_codec();
    seeds_query();
    seeds_num();
    seeds_viewer();
    seeds_event();
    seeds_dynamo();
    seeds_jwt();
    seeds_bcrypt();
    seeds_https();
    seeds_mqtt();
    printf("seeds: %d files (%d DynamoDB items)\n", g_written, g_n_items);
    return 0;
}
