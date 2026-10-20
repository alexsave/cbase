// unit_util.c - the util layer: bytea, uuid, clock, str, json, log, pool.
#include "t.h"

#include "../../../shared/c/b64/b64.h"
#include "util/clock.h"
#include "util/err.h"
#include "../../../shared/c/hex/hex.h"
#include "util/bytea.h"
#include "util/json.h"
#include "util/log.h"
#include "util/pool.h"
#include "util/str.h"
#include "util/uuid.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

// base64 and hex are shared/c's, tested there (b64_test.c, hex_test.c).

// The hex itself is shared/c/hex's, tested there (hex_test.c); this is the
// Postgres prefix around it.
static void bytea(void) {
    const uint8_t raw[] = { 0x00, 0x01, 0xab, 0xff, 0x10 };
    char out[32];
    uint8_t back[8];
    T_EQ_INT(cb_bytea_encode(raw, 5, out, sizeof out), 12);
    T_EQ_STR(out, "\\x0001abff10");
    T_EQ_INT(cb_bytea_decode(out, 12, back, sizeof back), 5);
    T_EQ_MEM(back, 5, raw, 5);
    T_EQ_INT(cb_bytea_decode("0001ABFF10", 10, back, sizeof back), 5);   // the prefix is optional
    T_EQ_MEM(back, 5, raw, 5);
    T_EQ_INT(cb_bytea_decode("\\xabc", 5, back, sizeof back), -1);
    T_EQ_INT(cb_bytea_encode(raw, 5, out, 12), -1);
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
    // An empty span may carry no pointer: an empty string, and no memcpy from
    // NULL (undefined behaviour that UBSan reports under glibc).
    out[0] = 'x';
    T_CHECK(cb_strncpy0(out, sizeof out, NULL, 0));
    T_EQ_STR(out, "");
    T_CHECK(cb_strncpy0(out, sizeof out, "abc", 3));
    T_EQ_STR(out, "abc");
    T_CHECK(!cb_strncpy0(out, 4, "abcd", 4));
    T_EQ_STR(out, "");
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
    cb_jw_end(&w);
    cb_jw_end(&w);
    T_CHECK(cb_jw_ok(&w));
    T_EQ_STR(cb_buf_cstr(&b), "{\"s\":\"a\\\"b\\\\c\\n\\u0001\xc3\xa9\",\"i\":-7,\"t\":true,\"z\":null,"
                              "\"arr\":[1,null]}");
    T_CHECK(cb_json_valid((const char *)b.p, b.len));
    // A value where a key is due is an error, not a malformed document on the wire.
    cb_buf_reset(&b);
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_int(&w, 1);
    T_CHECK(!cb_jw_ok(&w));
    cb_buf_free(&b);
}

// cb_jw_b64 writes what escaping the encoding as a string wrote, without the
// copy and the escape pass; for every length the encoding's padding takes.
static void json_writer_b64(void) {
    uint8_t bytes[300];
    for (size_t i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)(i * 37 + 11);   // 0x22 and 0x5c among them
    for (size_t n = 0; n <= sizeof bytes; n += n < 8 ? 1 : 29) {
        cb_buf a, b;
        cb_buf_init(&a);
        cb_buf_init(&b);
        cb_jw wa, wb;
        cb_jw_init(&wa, &a);
        cb_jw_init(&wb, &b);
        cb_jw_obj(&wa);
        cb_jw_kb64(&wa, "B", bytes, n);
        cb_jw_kint(&wa, "after", 1);
        cb_jw_end(&wa);
        char enc[512];
        T_CHECK(b64_encode(bytes, n, enc, sizeof enc) >= 0);
        cb_jw_obj(&wb);
        cb_jw_kstr(&wb, "B", enc);
        cb_jw_kint(&wb, "after", 1);
        cb_jw_end(&wb);
        T_CHECK(cb_jw_ok(&wa));
        T_EQ_STR(cb_buf_cstr(&a), cb_buf_cstr(&b));
        T_EQ_INT(a.len, b.len);
        cb_buf_free(&a);
        cb_buf_free(&b);
    }
    // In an array, comma-separated like any value; refused where a key is due.
    cb_buf o;
    cb_buf_init(&o);
    cb_jw w;
    cb_jw_init(&w, &o);
    cb_jw_arr(&w);
    cb_jw_b64(&w, (const uint8_t *)"hi", 2);
    cb_jw_b64(&w, NULL, 0);
    cb_jw_end(&w);
    T_CHECK(cb_jw_ok(&w));
    T_EQ_STR(cb_buf_cstr(&o), "[\"aGk=\",\"\"]");
    cb_buf_reset(&o);
    cb_jw_init(&w, &o);
    cb_jw_obj(&w);
    cb_jw_b64(&w, (const uint8_t *)"hi", 2);   // a value where a key is due
    T_CHECK(!cb_jw_ok(&w));
    T_EQ_STR(cb_buf_cstr(&o), "{");
    cb_buf_free(&o);
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
    T_EQ_STR(s, "");                                     // and none of it is left behind
    {
        const char lone[] = "{\"t\":\"r\\udae7\"}";   // the 'r' unescapes, the lone surrogate does not
        strcpy(s, "stale");
        T_CHECK(!cb_json_get_str(lone, sizeof lone - 1, "t", s, sizeof s));
        T_EQ_STR(s, "");
        strcpy(s, "stale");
        T_CHECK(!cb_json_get_str(lone, sizeof lone - 1, "missing", s, sizeof s));
        T_EQ_STR(s, "");
    }
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

static int g_formatted;
static int formatted(void) { return ++g_formatted; }

// A line below the level is dropped before its arguments are evaluated or its format run
// (log.h cb_log_enabled): a hot path's debug line costs one load.
static void log_level_first(void) {
    cb_log_init(CB_LOG_INFO, 4);
    const uint64_t start = cb_log_last_seq();
    g_formatted = 0;
    CB_DEBUG("t", "never %d", formatted());
    T_EQ_INT(g_formatted, 0);
    T_EQ_INT(cb_log_last_seq(), start);
    CB_INFO("t", "kept %d", formatted());
    T_EQ_INT(g_formatted, 1);
    T_EQ_INT(cb_log_last_seq(), start + 1);
    T_CHECK(!cb_log_enabled(CB_LOG_DEBUG));
    T_CHECK(cb_log_enabled(CB_LOG_ERROR + 1));   // above ERROR counts as ERROR
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
    T_RUN(bytea);
    T_RUN(uuid_shape);
    T_RUN(iso8601);
    T_RUN(strings);
    T_RUN(json_writer);
    T_RUN(json_writer_b64);
    T_RUN(json_reader);
    T_RUN(log_ring);
    T_RUN(log_level_first);
    T_RUN(pool_runs_everything);
    return T_DONE();
}
