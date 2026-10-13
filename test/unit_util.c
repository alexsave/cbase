// unit_util.c - the util layer: b64, hex, uuid, clock, str, json, log, pool.
#include "t.h"

#include "util/b64.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/hex.h"
#include "util/json.h"
#include "util/log.h"
#include "util/pool.h"
#include "util/str.h"
#include "util/uuid.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

// RFC 4648 section 10 test vectors, both directions.
static void b64_rfc4648_vectors(void) {
    static const char *in[]  = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char *std[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
    static const char *url[] = { "", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy" };
    for (int i = 0; i < 7; i++) {
        char out[32];
        uint8_t back[32];
        size_t n = strlen(in[i]);
        T_EQ_INT(cb_b64_encode((const uint8_t *)in[i], n, out, sizeof out), (long long)strlen(std[i]));
        T_EQ_STR(out, std[i]);
        T_EQ_INT(cb_b64url_encode((const uint8_t *)in[i], n, out, sizeof out), (long long)strlen(url[i]));
        T_EQ_STR(out, url[i]);
        T_EQ_INT(cb_b64_decode(std[i], strlen(std[i]), back, sizeof back), (long long)n);
        T_EQ_MEM(back, n, in[i], n);
        T_EQ_INT(cb_b64_decode(url[i], strlen(url[i]), back, sizeof back), (long long)n);
        T_EQ_MEM(back, n, in[i], n);
    }
}

static void b64_round_trip_all_bytes(void) {
    uint8_t raw[256], back[256];
    char enc[400];
    for (int i = 0; i < 256; i++) raw[i] = (uint8_t)(255 - i);
    for (size_t n = 0; n <= 256; n += 17) {
        int k = cb_b64_encode(raw, n, enc, sizeof enc);
        T_EQ_INT(k, (long long)cb_b64_len(n));
        T_EQ_INT(cb_b64_decode(enc, (size_t)k, back, sizeof back), (long long)n);
        T_EQ_MEM(back, n, raw, n);
        k = cb_b64url_encode(raw, n, enc, sizeof enc);
        T_EQ_INT(k, (long long)cb_b64url_len(n));
        T_CHECK(strchr(enc, '+') == NULL && strchr(enc, '/') == NULL && strchr(enc, '=') == NULL);
        T_EQ_INT(cb_b64_decode(enc, (size_t)k, back, sizeof back), (long long)n);
        T_EQ_MEM(back, n, raw, n);
    }
}

static void b64_refusals(void) {
    uint8_t out[16];
    char small[4];
    T_EQ_INT(cb_b64_decode("Zm9v!", 5, out, sizeof out), -1);      // not in either alphabet
    T_EQ_INT(cb_b64_decode("Z", 1, out, sizeof out), -1);          // no encoding is 1 mod 4
    T_EQ_INT(cb_b64_decode("Zg=a", 4, out, sizeof out), -1);       // padding in the middle
    T_EQ_INT(cb_b64_decode("Zh==", 4, out, sizeof out), -1);       // non-zero leftover bits
    T_EQ_INT(cb_b64_decode("Zm9vYmFy", 8, out, 5), -1);            // does not fit
    T_EQ_INT(cb_b64_encode((const uint8_t *)"foo", 3, small, sizeof small), -1);   // no room for the NUL
}

static void hex_and_bytea(void) {
    const uint8_t raw[] = { 0x00, 0x01, 0xab, 0xff, 0x10 };
    char out[32];
    uint8_t back[8];
    T_EQ_INT(cb_hex_encode(raw, 5, out, sizeof out), 10);
    T_EQ_STR(out, "0001abff10");
    T_EQ_INT(cb_bytea_encode(raw, 5, out, sizeof out), 12);
    T_EQ_STR(out, "\\x0001abff10");
    T_EQ_INT(cb_hex_decode(out, 12, back, sizeof back), 5);
    T_EQ_MEM(back, 5, raw, 5);
    T_EQ_INT(cb_hex_decode("0001ABFF10", 10, back, sizeof back), 5);
    T_EQ_MEM(back, 5, raw, 5);
    T_EQ_INT(cb_hex_decode("abc", 3, back, sizeof back), -1);
    T_EQ_INT(cb_hex_decode("zz", 2, back, sizeof back), -1);
    T_EQ_INT(cb_hex_encode(raw, 5, out, 10), -1);
}

static void uuid_shape(void) {
    char a[CB_UUID_LEN + 1], b[CB_UUID_LEN + 1];
    cb_uuid_v4(a);
    cb_uuid_v4(b);
    T_EQ_INT(strlen(a), CB_UUID_LEN);
    T_CHECK(cb_uuid_valid(a, strlen(a)));
    T_CHECK(strcmp(a, b) != 0);
    T_CHECK(a[14] == '4');
    T_CHECK(a[19] == '8' || a[19] == '9' || a[19] == 'a' || a[19] == 'b');
    T_CHECK(!cb_uuid_valid("not-a-uuid", 10));
    T_CHECK(cb_uuid_valid("6F9619FF-8B86-D011-B42D-00C04FC964FF", 36));
}

static void iso8601(void) {
    char out[40];
    T_EQ_INT(cb_iso8601(0, out, sizeof out), 32);
    T_EQ_STR(out, "1970-01-01T00:00:00.000000+00:00");
    T_EQ_INT(cb_iso8601(1760358896789LL, out, sizeof out), 32);
    T_EQ_STR(out, "2025-10-13T12:34:56.789000+00:00");
    int64_t ms = 0;
    T_EQ_INT(cb_iso8601_parse(out, strlen(out), &ms), CB_OK);
    T_EQ_INT(ms, 1760358896789LL);
    const char *psql = "2025-10-13 14:34:56.789123+02";
    T_EQ_INT(cb_iso8601_parse(psql, strlen(psql), &ms), CB_OK);
    T_EQ_INT(ms, 1760358896789LL);
    T_EQ_INT(cb_iso8601_parse("2024-02-29T00:00:00Z", 20, &ms), CB_OK);
    T_EQ_INT(ms, 1709164800000LL);
    T_EQ_INT(cb_iso8601_parse("2024-02-29", 10, &ms), CB_E_INVALID);
    T_EQ_INT(cb_iso8601_parse("2024-13-01T00:00:00Z", 20, &ms), CB_E_INVALID);
    T_CHECK(cb_mono_ms() > 0 && cb_now_ms() > 1700000000000LL);
}

static void strings(void) {
    cb_buf b;
    cb_buf_init(&b);
    T_EQ_STR(cb_buf_cstr(&b), "");
    cb_buf_printf(&b, "%d-%s", 42, "x");
    cb_buf_puts(&b, "yz");
    T_EQ_STR(cb_buf_cstr(&b), "42-xyz");
    cb_buf_consume(&b, 3);
    T_EQ_STR(cb_buf_cstr(&b), "xyz");
    size_t n = 0;
    uint8_t *p = cb_buf_take(&b, &n);
    T_EQ_INT(n, 3);
    T_EQ_STR((char *)p, "xyz");
    free(p);
    T_CHECK(b.p == NULL && b.len == 0);
    char out[16];
    T_EQ_INT(cb_url_decode("a%20b+c%2Fd", 11, out, sizeof out, true), 7);
    T_EQ_STR(out, "a b c/d");
    T_EQ_INT(cb_url_decode("a+b", 3, out, sizeof out, false), 3);
    T_EQ_STR(out, "a+b");
    T_EQ_INT(cb_url_decode("%2", 2, out, sizeof out, true), -1);
    T_EQ_INT(cb_url_decode("%zz", 3, out, sizeof out, true), -1);
    T_EQ_INT(cb_strlcpy(out, "0123456789abcdefXYZ", 8), 19);
    T_EQ_STR(out, "0123456");
    T_CHECK(cb_span_eq_nocase(CB_SPAN_LIT("Content-Type"), "content-type"));
}

static void json_writer(void) {
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "s", "a\"b\\c\n\x01\xc3\xa9");
    cb_jw_kint(&w, "i", -7);
    cb_jw_kbool(&w, "t", true);
    cb_jw_knull(&w, "z");
    cb_jw_key(&w, "arr");
    cb_jw_arr(&w);
    cb_jw_int(&w, 1);
    cb_jw_str(&w, NULL);
    const uint8_t bytes[] = { 0xde, 0xad };
    cb_jw_bytea(&w, bytes, 2);
    cb_jw_end(&w);
    cb_jw_end(&w);
    T_CHECK(cb_jw_ok(&w));
    T_EQ_STR(cb_buf_cstr(&b), "{\"s\":\"a\\\"b\\\\c\\n\\u0001\xc3\xa9\",\"i\":-7,\"t\":true,\"z\":null,"
                              "\"arr\":[1,null,\"\\\\xdead\"]}");
    T_CHECK(cb_json_valid((const char *)b.p, b.len));
    // A value where a key is due is an error, not a malformed document on the wire.
    cb_buf_reset(&b);
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_int(&w, 1);
    T_CHECK(!cb_jw_ok(&w));
    cb_buf_free(&b);
}

static void json_reader(void) {
    const char *doc = " {\"email\":\"a@b.c\",\"password\":\"p\\u00e9\\ud83d\\ude00\",\"data\":{\"username\":\"ALEX\"},"
                      "\"n\":-12,\"f\":1.5,\"ok\":true,\"none\":null,"
                      "\"config\":{\"postgres_changes\":[{\"event\":\"*\"},{\"filter\":\"player_id=eq.x\"}]}} ";
    size_t n = strlen(doc);
    char s[64];
    int64_t i = 0;
    bool b = false;
    T_CHECK(cb_json_valid(doc, n));
    T_CHECK(cb_json_get_str(doc, n, "email", s, sizeof s));
    T_EQ_STR(s, "a@b.c");
    T_CHECK(cb_json_get_str(doc, n, "password", s, sizeof s));
    T_EQ_STR(s, "p\xc3\xa9\xf0\x9f\x98\x80");
    T_CHECK(cb_json_get_str(doc, n, "data.username", s, sizeof s));
    T_EQ_STR(s, "ALEX");
    T_CHECK(cb_json_get_i64(doc, n, "n", &i));
    T_EQ_INT(i, -12);
    T_CHECK(!cb_json_get_i64(doc, n, "f", &i));
    T_CHECK(cb_json_get_bool(doc, n, "ok", &b) && b);
    T_EQ_INT(cb_json_find(doc, n, "none", NULL), CB_JSON_NULL);
    T_EQ_INT(cb_json_find(doc, n, "missing", NULL), CB_JSON_MISSING);
    T_EQ_INT(cb_json_find(doc, n, "data.username.deeper", NULL), CB_JSON_MISSING);
    T_EQ_INT(cb_json_array_len(doc, n, "config.postgres_changes"), 2);
    T_CHECK(cb_json_get_str(doc, n, "config.postgres_changes.1.filter", s, sizeof s));
    T_EQ_STR(s, "player_id=eq.x");
    T_EQ_INT(cb_json_find(doc, n, "config.postgres_changes.2", NULL), CB_JSON_MISSING);
    T_CHECK(!cb_json_get_str(doc, n, "email", s, 3));   // too long for the buffer
    // Malformed documents.
    T_CHECK(!cb_json_valid("{\"a\":}", 6));
    T_CHECK(!cb_json_valid("[1,]", 4));
    T_CHECK(!cb_json_valid("{\"a\":1} x", 9));
    T_CHECK(!cb_json_valid("\"\\ud800\"", 8) || cb_json_unescape(CB_SPAN_LIT("\"\\ud800\""), s, sizeof s) == -1);
    // Tolerant: it validates what it walks, so a document cut after the value
    // asked for still answers, and one cut before it is an error.
    T_EQ_INT(cb_json_find("{\"a\":[1,2", 9, "a.1", NULL), CB_JSON_NUM);
    T_EQ_INT(cb_json_find("{\"a\":[1,", 8, "a.1", NULL), CB_JSON_ERR);
}

static void log_ring(void) {
    cb_log_init(CB_LOG_INFO, 4);
    uint64_t start = cb_log_last_seq();
    cb_log(CB_LOG_DEBUG, "t", "dropped");
    for (int k = 0; k < 6; k++) cb_log(k % 2 ? CB_LOG_WARN : CB_LOG_INFO, "t", "line %d", k);
    T_EQ_INT(cb_log_last_seq(), start + 6);
    cb_log_entry e[8];
    size_t n = cb_log_read(0, CB_LOG_DEBUG, e, 8);
    T_EQ_INT(n, 4);   // the ring keeps the last four
    T_EQ_STR(e[0].msg, "line 2");
    T_EQ_STR(e[3].msg, "line 5");
    n = cb_log_read(0, CB_LOG_WARN, e, 8);
    T_EQ_INT(n, 2);
    T_EQ_STR(e[0].msg, "line 3");
    T_EQ_INT(cb_log_read(cb_log_last_seq(), CB_LOG_DEBUG, e, 8), 0);
}

static atomic_int g_ran;
static void bump(void *ud) { (void)ud; atomic_fetch_add(&g_ran, 1); }

static void pool_runs_everything(void) {
    cb_pool *p = cb_pool_new(3, "t");
    T_CHECK(p != NULL);
    for (int k = 0; k < 500; k++) T_EQ_INT(cb_pool_submit(p, bump, NULL), CB_OK);
    cb_pool_free(p);   // drains the queue first
    T_EQ_INT(atomic_load(&g_ran), 500);
}

int main(void) {
    T_RUN(b64_rfc4648_vectors);
    T_RUN(b64_round_trip_all_bytes);
    T_RUN(b64_refusals);
    T_RUN(hex_and_bytea);
    T_RUN(uuid_shape);
    T_RUN(iso8601);
    T_RUN(strings);
    T_RUN(json_writer);
    T_RUN(json_reader);
    T_RUN(log_ring);
    T_RUN(pool_runs_everything);
    return T_DONE();
}
