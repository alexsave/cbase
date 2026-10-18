// unit_edges.c - the foundation's edges the module suites leave: every error
// name, every HTTP reason, each configuration refusal, the JSON reader's
// malformed inputs (it reads AWS's answers and the console's bodies), SigV4's
// query ordering, and a validly signed token whose parts are not base64url.
#include "t.h"

#include "auth/jwt.h"
#include "aws/sigv4.h"
#include "config.h"
#include "net/http.h"
#include "../../shared/c/b64/b64.h"
#include "util/err.h"
#include "util/json.h"
#include "util/log.h"
#include "util/str.h"

#include <openssl/hmac.h>
#include <stdlib.h>
#include <string.h>

// ---- err.h ---------------------------------------------------------------------------------

static void every_error_has_its_name(void) {
    static const struct { int code; const char *name; } E[] = {
        { CB_OK, "CB_OK" }, { CB_E_NOTIMPL, "CB_E_NOTIMPL" }, { CB_E_NOT_FOUND, "CB_E_NOT_FOUND" },
        { CB_E_CONFLICT, "CB_E_CONFLICT" }, { CB_E_INVALID, "CB_E_INVALID" }, { CB_E_IO, "CB_E_IO" },
        { CB_E_NOMEM, "CB_E_NOMEM" }, { CB_E_AUTH, "CB_E_AUTH" }, { CB_E_FORBIDDEN, "CB_E_FORBIDDEN" },
        { CB_E_FULL, "CB_E_FULL" }, { CB_E_DB, "CB_E_DB" }, { CB_E_STATE, "CB_E_STATE" }, { CB_E_BUSY, "CB_E_BUSY" },
    };
    for (size_t i = 0; i < sizeof E / sizeof E[0]; i++) T_EQ_STR(cb_strerror(E[i].code), E[i].name);
    T_EQ_STR(cb_strerror(7), "CB_OK(count)");    // a count
    T_EQ_STR(cb_strerror(-99), "CB_E_UNKNOWN");
}

// ---- http.h --------------------------------------------------------------------------------

static void every_status_has_its_reason(void) {
    static const struct { int st; const char *r; } R[] = {
        { 101, "Switching Protocols" }, { 200, "OK" }, { 201, "Created" }, { 202, "Accepted" }, { 204, "No Content" },
        { 206, "Partial Content" }, { 400, "Bad Request" }, { 401, "Unauthorized" }, { 403, "Forbidden" },
        { 404, "Not Found" }, { 405, "Method Not Allowed" }, { 406, "Not Acceptable" }, { 408, "Request Timeout" },
        { 409, "Conflict" }, { 411, "Length Required" }, { 413, "Content Too Large" }, { 415, "Unsupported Media Type" },
        { 422, "Unprocessable Content" }, { 426, "Upgrade Required" }, { 429, "Too Many Requests" },
        { 431, "Request Header Fields Too Large" }, { 500, "Internal Server Error" }, { 501, "Not Implemented" },
        { 503, "Service Unavailable" }, { 505, "HTTP Version Not Supported" }, { 299, "Unknown" },
    };
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) T_EQ_STR(cb_http_reason(R[i].st), R[i].r);
}

// ---- config.h ------------------------------------------------------------------------------

static const char *const VARS[] = {
    "CBASE_LISTEN", "CBASE_STORE", "CBASE_DB", "CBASE_TABLE", "CBASE_IOT_ENDPOINT", "CBASE_CF_DISTRIBUTION",
    "CBASE_JWT_SECRET", "CBASE_ADMIN_TOKEN", "CBASE_ALLOWED_ORIGINS", "CBASE_PUBLIC_URL", "CBASE_REQ_WORKERS",
    "CBASE_BOT_WORKERS", "CBASE_MAX_BODY", "CBASE_JWT_TTL", "CBASE_LOG_LEVEL", "CBASE_ORIGIN_SECRET",
    "CBASE_BOT_SPEEDUP",
};

static void env_clear(void) {
    for (size_t i = 0; i < sizeof VARS / sizeof VARS[0]; i++) unsetenv(VARS[i]);
}

// One variable set to `value` (the rest unset): the refusal's message names it.
static void refused(const char *name, const char *value, const char *says) {
    env_clear();
    setenv(name, value, 1);
    cb_config c;
    char err[512] = "";
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_E_INVALID);
    if (!strstr(err, says)) fprintf(stderr, "     %s=%s said \"%s\", want \"%s\"\n", name, value, err, says);
    T_CHECK(strstr(err, says) != NULL);
}

// What cb_config_each listed: every setting once, the secrets flagged (and
// only them), no secret's value anywhere.
typedef struct { int n, secrets; bool leaked, unknown, wrong_secret; } seen;
static bool is_secret_name(const char *n) {
    return !strcmp(n, "CBASE_JWT_SECRET") || !strcmp(n, "CBASE_ADMIN_TOKEN") || !strcmp(n, "CBASE_ORIGIN_SECRET");
}
static void see(const char *name, const char *value, bool secret, void *ud) {
    seen *s = ud;
    s->n++;
    if (secret) s->secrets++;
    if (strncmp(name, "CBASE_", 6) != 0) s->unknown = true;
    if (secret != is_secret_name(name)) s->wrong_secret = true;
    if (strstr(value, "0123456789abcdef0123456789abcdef") || strstr(value, "console-token") || strstr(value, "edge-origin")) s->leaked = true;
}

static void configuration_refusals(void) {
    char longv[2100];
    memset(longv, 'x', sizeof longv - 1);
    longv[sizeof longv - 1] = 0;
    refused("CBASE_LISTEN", "nocolon", "is not host:port");
    refused("CBASE_LISTEN", ":80", "is not host:port");
    refused("CBASE_LISTEN", "h:", "is not host:port");
    refused("CBASE_LISTEN", "h:70000", "has no valid port");
    refused("CBASE_LISTEN", "h:8x", "has no valid port");
    refused("CBASE_LISTEN", "h: 80", "has no valid port");
    refused("CBASE_LISTEN", "h:+80", "has no valid port");
    refused("CBASE_LISTEN", "h:-0", "has no valid port");
    char longhost[400];
    snprintf(longhost, sizeof longhost, "%.300s:80", longv);
    refused("CBASE_LISTEN", longhost, "host is too long");
    refused("CBASE_STORE", "postgres", "is not sqlite or dynamo");
    refused("CBASE_DB", longv, "CBASE_DB is longer than");
    refused("CBASE_TABLE", longv, "CBASE_TABLE is longer than");
    refused("CBASE_IOT_ENDPOINT", longv, "CBASE_IOT_ENDPOINT is longer than");
    refused("CBASE_IOT_ENDPOINT", "a/b.iot.amazonaws.com", "is not a host name");
    refused("CBASE_IOT_ENDPOINT", "localhost:8443", "is not a host name");
    refused("CBASE_CF_DISTRIBUTION", longv, "CBASE_CF_DISTRIBUTION is longer than");
    refused("CBASE_CF_DISTRIBUTION", "E2abc", "is not a CloudFront distribution id");
    refused("CBASE_JWT_SECRET", "short", "at least 32 characters");
    refused("CBASE_JWT_SECRET", longv, "CBASE_JWT_SECRET is longer than");
    refused("CBASE_ADMIN_TOKEN", longv, "CBASE_ADMIN_TOKEN is longer than");
    refused("CBASE_ADMIN_TOKEN", "console-token", "CBASE_ADMIN_TOKEN must be at least 32 characters");   // L7
    refused("CBASE_ORIGIN_SECRET", "short", "CBASE_ORIGIN_SECRET must be at least 32 characters");
    refused("CBASE_ORIGIN_SECRET", longv, "CBASE_ORIGIN_SECRET is longer than");
    refused("CBASE_ALLOWED_ORIGINS", longv, "CBASE_ALLOWED_ORIGINS is longer than");
    refused("CBASE_PUBLIC_URL", longv, "CBASE_PUBLIC_URL is longer than");
    refused("CBASE_REQ_WORKERS", "0", "CBASE_REQ_WORKERS=0 is not a number in 1..256");
    refused("CBASE_REQ_WORKERS", "4x", "is not a number");
    refused("CBASE_BOT_WORKERS", "65", "is not a number in 1..64");
    refused("CBASE_MAX_BODY", "10", "is not a number in 1024..");
    refused("CBASE_JWT_TTL", "59", "is not a number in 60..");
    refused("CBASE_LOG_LEVEL", "loud", "is not debug, info, warn or error");
    // The test knob: a whole number of times faster, 1 (production's pace) to 100.
    refused("CBASE_BOT_SPEEDUP", "0", "CBASE_BOT_SPEEDUP=0 is not a number in 1..100");
    refused("CBASE_BOT_SPEEDUP", "-5", "is not a number in 1..100");
    refused("CBASE_BOT_SPEEDUP", "101", "is not a number in 1..100");
    refused("CBASE_BOT_SPEEDUP", "2.5", "is not a number in 1..100");
    refused("CBASE_BOT_SPEEDUP", "20x", "is not a number in 1..100");
    refused("CBASE_BOT_SPEEDUP", " 20", "is not a number in 1..100");
    refused("CBASE_BOT_SPEEDUP", "+20", "is not a number in 1..100");
    refused("CBASE_BOT_SPEEDUP", "20 ", "is not a number in 1..100");
    refused("CBASE_BOT_SPEEDUP", "99999999999999999999", "is not a number in 1..100");

    // Everything set and in range: read as given, and listed with the secrets redacted.
    env_clear();
    setenv("CBASE_LISTEN", "0.0.0.0:9000", 1);
    setenv("CBASE_STORE", "dynamo", 1);
    setenv("CBASE_CF_DISTRIBUTION", "E2ABC123", 1);
    setenv("CBASE_JWT_SECRET", "0123456789abcdef0123456789abcdef", 1);
    setenv("CBASE_ADMIN_TOKEN", "console-token-0123456789-abcdefgh", 1);   // at least 32 (L7)
    setenv("CBASE_ORIGIN_SECRET", "edge-origin-0123456789abcdef0123456789", 1);
    setenv("CBASE_REQ_WORKERS", "8", 1);
    setenv("CBASE_BOT_WORKERS", "2", 1);
    setenv("CBASE_MAX_BODY", "2048", 1);
    setenv("CBASE_JWT_TTL", "600", 1);
    setenv("CBASE_LOG_LEVEL", "warn", 1);
    setenv("CBASE_BOT_SPEEDUP", "100", 1);
    setenv("CBASE_ALLOWED_ORIGINS", " https://a.example , https://b.example ", 1);
    cb_config c;
    char err[256] = "";
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_STR(c.listen_host, "0.0.0.0");
    T_EQ_INT(c.listen_port, 9000);
    T_EQ_STR(c.store, "dynamo");
    T_EQ_INT(c.req_workers, 8);
    T_EQ_INT(c.bot_workers, 2);
    T_EQ_INT(c.max_body, 2048);
    T_EQ_INT(c.jwt_ttl_s, 600);
    T_EQ_INT(c.log_level, CB_LOG_WARN);
    T_EQ_INT(c.bot_speedup, 100);
    T_CHECK(!c.jwt_secret_generated);
    T_EQ_STR(c.public_url, "http://0.0.0.0:9000");
    seen s = { 0 };
    cb_config_each(&c, see, &s);
    T_CHECK(s.n >= (int)(sizeof VARS / sizeof VARS[0]));   // every setting this test knows, and any added since
    T_EQ_INT(s.secrets, 3);
    T_CHECK(!s.leaked);
    T_CHECK(!s.unknown);
    T_CHECK(!s.wrong_secret);
    // The origin list: trimmed, exact, no prefix matches.
    T_CHECK(cb_config_origin_allowed(&c, "https://a.example"));
    T_CHECK(cb_config_origin_allowed(&c, "https://b.example"));
    T_CHECK(!cb_config_origin_allowed(&c, "https://a.exampl"));
    T_CHECK(!cb_config_origin_allowed(&c, "https://a.example.evil"));
    T_CHECK(!cb_config_origin_allowed(&c, ""));
    // No secret: one is made for the process, and said to be.
    unsetenv("CBASE_JWT_SECRET");
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_CHECK(c.jwt_secret_generated);
    T_CHECK(strlen(c.jwt_secret) >= 32);
    env_clear();
}

// ---- json.h: the reader ----------------------------------------------------------------------

static bool valid(const char *s) { return cb_json_valid(s, strlen(s)); }

static void json_numbers_and_literals(void) {
    T_CHECK(valid("0"));
    T_CHECK(valid("-0.5E-3"));
    T_CHECK(valid("1e+5"));
    T_CHECK(valid("123"));
    T_CHECK(!valid("-"));
    T_CHECK(!valid("01"));      // a leading zero, then a number
    T_CHECK(!valid("1."));
    T_CHECK(!valid("1e"));
    T_CHECK(!valid("1e+"));
    T_CHECK(!valid(".5"));
    T_CHECK(!valid("+1"));
    T_CHECK(!valid("tru"));
    T_CHECK(!valid("nul"));
    T_CHECK(!valid("falsey"));
    T_CHECK(!valid(""));
    T_CHECK(!valid("[1,]"));
    T_CHECK(!valid("{\"a\" 1}"));
    T_CHECK(!valid("{\"a\":1,}"));
    T_CHECK(!valid("{1:1}"));
    T_CHECK(!valid("[1 2]"));
    T_CHECK(valid(" { \"a\" : [ true , false , null ] } "));
    // Nesting: 64 deep reads, 65 does not.
    char deep[200];
    memset(deep, '[', 65);
    memset(deep + 65, ']', 65);
    deep[130] = 0;
    T_CHECK(!valid(deep));
    T_CHECK(valid(deep + 1) == false);   // unbalanced
    memset(deep, '[', 64);
    memset(deep + 64, ']', 64);
    deep[128] = 0;
    T_CHECK(valid(deep));
}

static void json_strings(void) {
    T_CHECK(valid("\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\""));
    T_CHECK(valid("\"\\u00e9\\uD83D\\uDE00\""));
    T_CHECK(!valid("\"\\x\""));            // not an escape
    T_CHECK(!valid("\"\\u12g4\""));        // not hex
    T_CHECK(!valid("\"\\u12\""));          // short
    T_CHECK(!valid("\"abc"));              // unterminated
    T_CHECK(!valid("\"a\x01\""));          // a raw control byte
    T_CHECK(!valid("\"\\"));               // a backslash at the end

    char out[64];
    // Every escape, unescaped.
    const char *t = "\"q\\\"s\\\\l\\/b\\bf\\fn\\nr\\rt\\t\"";
    T_EQ_INT(cb_json_unescape(cb_span_of(t, strlen(t)), out, sizeof out), 16);
    T_EQ_MEM(out, 16, "q\"s\\l/b\bf\fn\nr\rt\t", 16);
    // UTF-8 of one, two, three and four bytes (a surrogate pair).
    t = "\"\\u0041\\u00e9\\u20ac\\ud83d\\ude00\"";
    T_EQ_INT(cb_json_unescape(cb_span_of(t, strlen(t)), out, sizeof out), 1 + 2 + 3 + 4);
    T_EQ_MEM(out, 10, "A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", 10);
    // Refusals.
    static const char *const BAD[] = {
        "\"\\ud83d\"",            // a high surrogate alone
        "\"\\ud83dx\\ude00\"",    // not followed by \u
        "\"\\ud83d\\u0041\"",     // followed by a non-low
        "\"\\ude00\"",            // a low surrogate alone
        "\"\\u00zz\"",            // not hex
        "\"\\u00\"",              // short
        "\"\\q\"",                // unknown escape
        "\"\\\"",                 // escape at the end
        "\"a\"b\"",               // a quote inside
        "\"\x1f\"",               // a control byte
        "abc",                    // no quotes
        "\"",                     // one quote
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        int r = cb_json_unescape(cb_span_of(BAD[i], strlen(BAD[i])), out, sizeof out);
        if (r != -1) fprintf(stderr, "     unescape #%zu answered %d\n", i, r);
        T_EQ_INT(r, -1);
    }
    // The capacity: exactly enough (with the NUL), then one short; and zero.
    t = "\"abcd\"";
    T_EQ_INT(cb_json_unescape(cb_span_of(t, 6), out, 5), 4);
    T_EQ_INT(cb_json_unescape(cb_span_of(t, 6), out, 4), -1);
    T_EQ_INT(cb_json_unescape(cb_span_of(t, 6), out, 0), -1);
    t = "\"\\u20ac\"";
    T_EQ_INT(cb_json_unescape(cb_span_of(t, strlen(t)), out, 3), -1);   // three bytes and the NUL do not fit
}

static void json_paths(void) {
    static const char J[] = "{\"a\":{\"b\":[10,{\"c\":\"x\"},-9223372036854775808]},\"n\":9223372036854775807,"
                            "\"big\":9223372036854775808,\"f\":1.5,\"e\":1e3,\"t\":true,\"z\":null,\"s\":\"str\",\"empty\":{},\"ea\":[]}";
    size_t n = strlen(J);
    int64_t v;
    T_CHECK(cb_json_get_i64(J, n, "a.b.0", &v) && v == 10);
    T_CHECK(cb_json_get_i64(J, n, "a.b.2", &v) && v == INT64_MIN);
    T_CHECK(cb_json_get_i64(J, n, "n", &v) && v == INT64_MAX);
    T_CHECK(!cb_json_get_i64(J, n, "big", &v));   // past int64
    T_CHECK(!cb_json_get_i64(J, n, "f", &v));     // a fraction
    T_CHECK(!cb_json_get_i64(J, n, "e", &v));     // an exponent
    T_CHECK(!cb_json_get_i64(J, n, "s", &v));     // not a number
    char s[16];
    T_CHECK(cb_json_get_str(J, n, "a.b.1.c", s, sizeof s) && strcmp(s, "x") == 0);
    T_CHECK(!cb_json_get_str(J, n, "n", s, sizeof s));
    T_CHECK(!cb_json_get_str(J, n, "s", s, 3));   // does not fit
    bool b = false;
    T_CHECK(cb_json_get_bool(J, n, "t", &b) && b);
    T_CHECK(!cb_json_get_bool(J, n, "z", &b));
    T_EQ_INT(cb_json_find(J, n, "z", NULL), CB_JSON_NULL);
    T_EQ_INT(cb_json_find(J, n, "missing", NULL), CB_JSON_MISSING);
    T_EQ_INT(cb_json_find(J, n, "s.child", NULL), CB_JSON_MISSING);   // a scalar has no children
    T_EQ_INT(cb_json_find(J, n, "empty.k", NULL), CB_JSON_MISSING);
    T_EQ_INT(cb_json_find(J, n, "ea.0", NULL), CB_JSON_MISSING);
    T_EQ_INT(cb_json_find(J, n, "a.b.3", NULL), CB_JSON_MISSING);     // past the end
    T_EQ_INT(cb_json_find(J, n, "a.b.x", NULL), CB_JSON_MISSING);     // not an index
    T_EQ_INT(cb_json_find(J, n, "a.b.", NULL), CB_JSON_ARR);          // a trailing dot names the value itself
    T_EQ_INT(cb_json_find(J, n, "a..b", NULL), CB_JSON_MISSING);      // an empty key inside the path
    T_EQ_INT(cb_json_find(J, n, "a.b.1234567890", NULL), CB_JSON_MISSING);   // ten digits
    T_EQ_INT(cb_json_array_len(J, n, "a.b"), 3);
    T_EQ_INT(cb_json_array_len(J, n, "ea"), 0);
    T_EQ_INT(cb_json_array_len(J, n, "a"), -1);   // not an array
    // Malformed documents along the path.
    static const char *const BAD[] = { "{\"a\" 1}", "{\"a\":1 \"b\":2}", "{\"a\":1,", "{1:2}", "[1 2]", "[1,", "{\"a\":[1,2" };
    T_EQ_INT(cb_json_find(BAD[0], strlen(BAD[0]), "a", NULL), CB_JSON_ERR);
    T_EQ_INT(cb_json_find(BAD[1], strlen(BAD[1]), "b", NULL), CB_JSON_ERR);
    T_EQ_INT(cb_json_find(BAD[2], strlen(BAD[2]), "b", NULL), CB_JSON_ERR);
    T_EQ_INT(cb_json_find(BAD[3], strlen(BAD[3]), "a", NULL), CB_JSON_ERR);
    T_EQ_INT(cb_json_find(BAD[4], strlen(BAD[4]), "1", NULL), CB_JSON_ERR);
    T_EQ_INT(cb_json_find(BAD[5], strlen(BAD[5]), "1", NULL), CB_JSON_ERR);
    T_EQ_INT(cb_json_array_len(BAD[6], strlen(BAD[6]), "a"), -1);
    T_EQ_INT(cb_json_find("", 0, "a", NULL), CB_JSON_ERR);
    T_EQ_INT(cb_json_find("{\"a\":1}", 7, "b", NULL), CB_JSON_MISSING);
    // A key longer than the matcher's buffer never matches (and does not overflow it).
    char key[400], doc[512];
    memset(key, 'k', 300);
    key[300] = 0;
    snprintf(doc, sizeof doc, "{\"%s\":1}", key);
    T_EQ_INT(cb_json_find(doc, strlen(doc), key, NULL), CB_JSON_MISSING);
}

static void json_writer_escapes(void) {
    cb_buf b;
    cb_buf_init(&b);
    const char in[] = "\"\\\n\r\t\b\f\x01\x1f\x7f\xc3\xa9";
    cb_json_escape(&b, in, sizeof in - 1);
    T_EQ_STR(cb_buf_cstr(&b), "\\\"\\\\\\n\\r\\t\\b\\f\\u0001\\u001f\x7f\xc3\xa9");
    // What it writes reads back as what went in.
    cb_buf q;
    cb_buf_init(&q);
    cb_buf_printf(&q, "\"%s\"", cb_buf_cstr(&b));
    char out[64];
    T_EQ_INT(cb_json_unescape(cb_span_of(cb_buf_cstr(&q), q.len), out, sizeof out), (int)(sizeof in - 1));
    T_EQ_MEM(out, sizeof in - 1, in, sizeof in - 1);
    cb_buf_free(&q);
    cb_buf_free(&b);
    // The writer's own refusals: a value where a key is due, a key outside an object, too deep.
    cb_jw w;
    cb_buf_init(&b);
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_int(&w, 1);
    T_CHECK(!cb_jw_ok(&w));
    cb_buf_free(&b);
    cb_buf_init(&b);
    cb_jw_init(&w, &b);
    cb_jw_arr(&w);
    cb_jw_key(&w, "k");
    T_CHECK(w.err);
    cb_buf_free(&b);
    cb_buf_init(&b);
    cb_jw_init(&w, &b);
    for (int i = 0; i < CB_JW_DEPTH + 1; i++) cb_jw_arr(&w);
    T_CHECK(w.err);
    cb_buf_free(&b);
}

// ---- sigv4: the canonical query --------------------------------------------------------------

static void sigv4_query_order(void) {
    cb_aws_creds c;
    memset(&c, 0, sizeof c);
    snprintf(c.access_key, sizeof c.access_key, "AKIDEXAMPLE");
    snprintf(c.secret_key, sizeof c.secret_key, "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
    snprintf(c.region, sizeof c.region, "us-east-1");
    cb_sigv4_req r = { .method = "GET", .host = "example.amazonaws.com", .path = "/", .service = "service",
                       .now_ms = 1440938160000LL };
    cb_buf can, sts;
    cb_buf_init(&can);
    cb_buf_init(&sts);
    char sig[65];
    // Sorted by key then value; a key with no '=' signs as "key=" and sorts before its valued twins;
    // a key that is a prefix of another sorts first.
    r.query = "b=2&a=1&a&ab=1&a=0&&";
    T_EQ_INT(cb_sigv4_debug(&c, &r, &can, &sts, sig), CB_OK);
    const char *q = strchr(cb_buf_cstr(&can), '\n');
    q = q ? strchr(q + 1, '\n') : NULL;
    T_CHECK(q && strncmp(q + 1, "a=&a=0&a=1&ab=1&b=2\n", 20) == 0);
    // Sixty-four pairs sign; sixty-five do not.
    cb_buf many;
    cb_buf_init(&many);
    for (int i = 0; i < 64; i++) cb_buf_printf(&many, "%sk%02d=v", i ? "&" : "", i);
    r.query = cb_buf_cstr(&many);
    T_EQ_INT(cb_sigv4_debug(&c, &r, &can, &sts, sig), CB_OK);
    cb_buf_puts(&many, "&k64=v");
    r.query = cb_buf_cstr(&many);
    T_EQ_INT(cb_sigv4_debug(&c, &r, &can, &sts, sig), CB_E_INVALID);
    cb_buf_free(&many);
    cb_buf_free(&can);
    cb_buf_free(&sts);
}

// ---- jwt: a correctly signed token whose parts are not base64url --------------------------------

#define SECRET "edges-secret-edges-secret-edges-secret!!"

static void signed_parts_must_be_base64url(void) {
    cb_jwt_claims c;
    memset(&c, 0, sizeof c);
    snprintf(c.sub, sizeof c.sub, "aaaaaaaa-0000-4000-8000-000000000001");
    snprintf(c.session_id, sizeof c.session_id, "aaaaaaaa-0000-4000-8000-000000000002");
    c.iat = 1000;
    c.exp = 5000;
    char tok[CB_JWT_MAX];
    T_CHECK(cb_jwt_mint(SECRET, &c, "x", tok, sizeof tok) > 0);
    cb_jwt_claims got;
    T_EQ_INT(cb_jwt_verify(SECRET, tok, 2000, &got), CB_OK);
    // Respell the header with the standard alphabet's padding, or empty it,
    // and sign what results with the real secret: the signature holds, the
    // part is still refused.
    const char *d1 = strchr(tok, '.'), *d2 = strchr(d1 + 1, '.');
    const char *heads[] = { "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9==", "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9+", "" };
    for (size_t i = 0; i < sizeof heads / sizeof heads[0]; i++) {
        char msg[CB_JWT_MAX], out[CB_JWT_MAX + 64];
        int mn = snprintf(msg, sizeof msg, "%s.%.*s", heads[i], (int)(d2 - d1 - 1), d1 + 1);
        unsigned char mac[32];
        unsigned int ml = 0;
        HMAC(EVP_sha256(), SECRET, (int)strlen(SECRET), (const unsigned char *)msg, (size_t)mn, mac, &ml);
        char s64[64];
        b64url_encode(mac, 32, s64, sizeof s64);
        snprintf(out, sizeof out, "%s.%s", msg, s64);
        T_EQ_INT(cb_jwt_verify(SECRET, out, 2000, &got), CB_E_AUTH);
    }
    // A header that is valid JSON, encoded in the STANDARD alphabet: only the
    // base64url check can refuse it (the decoder takes both alphabets).
    {
        static const char hj[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\",\"k\":\"?>?>?>\"}";
        char h64[128];
        b64_encode((const uint8_t *)hj, strlen(hj), h64, sizeof h64);
        char *eq = strchr(h64, '=');
        if (eq) *eq = 0;
        T_CHECK(strpbrk(h64, "+/") != NULL);   // the case is the one meant
        char m2[CB_JWT_MAX], o2[CB_JWT_MAX + 64];
        int n2 = snprintf(m2, sizeof m2, "%s.%.*s", h64, (int)(d2 - d1 - 1), d1 + 1);
        unsigned char mac2[32];
        unsigned int ml2 = 0;
        HMAC(EVP_sha256(), SECRET, (int)strlen(SECRET), (const unsigned char *)m2, (size_t)n2, mac2, &ml2);
        char sig2[64];
        b64url_encode(mac2, 32, sig2, sizeof sig2);
        snprintf(o2, sizeof o2, "%s.%s", m2, sig2);
        T_EQ_INT(cb_jwt_verify(SECRET, o2, 2000, &got), CB_E_AUTH);
    }
    // The same for the payload: padded, then the standard alphabet.
    char msg[CB_JWT_MAX], out[CB_JWT_MAX + 64];
    int mn = snprintf(msg, sizeof msg, "%.*s.%.*s=", (int)(d1 - tok), tok, (int)(d2 - d1 - 1), d1 + 1);
    unsigned char mac[32];
    unsigned int ml = 0;
    HMAC(EVP_sha256(), SECRET, (int)strlen(SECRET), (const unsigned char *)msg, (size_t)mn, mac, &ml);
    char s64[64];
    b64url_encode(mac, 32, s64, sizeof s64);
    snprintf(out, sizeof out, "%s.%s", msg, s64);
    T_EQ_INT(cb_jwt_verify(SECRET, out, 2000, &got), CB_E_AUTH);
    // An empty secret signs nothing.
    T_EQ_INT(cb_jwt_mint("", &c, "x", tok, sizeof tok), CB_E_INVALID);
    T_EQ_INT(cb_jwt_verify("", tok, 2000, &got), CB_E_AUTH);
}

// ---- str and log ------------------------------------------------------------------------------

static void url_decode_and_levels(void) {
    char out[16];
    T_EQ_INT(cb_url_decode("a%20b+c", 7, out, sizeof out, true), 5);
    T_EQ_STR(out, "a b c");
    T_EQ_INT(cb_url_decode("a+c", 3, out, sizeof out, false), 3);
    T_EQ_STR(out, "a+c");
    T_EQ_INT(cb_url_decode("%4", 2, out, sizeof out, false), -1);     // truncated
    T_EQ_INT(cb_url_decode("%zz", 3, out, sizeof out, false), -1);    // not hex
    T_EQ_INT(cb_url_decode("%4a%4A", 6, out, sizeof out, false), 2);  // both cases
    T_EQ_STR(out, "JJ");
    T_EQ_INT(cb_url_decode("abcd", 4, out, 4, false), -1);            // no room for the NUL
    T_EQ_INT(cb_url_decode("", 0, out, 0, false), -1);
    T_EQ_INT(cb_url_decode("", 0, out, 1, false), 0);
    char d[4];
    T_CHECK(!cb_strncpy0(d, 0, "abc", 1));
    T_CHECK(!cb_strncpy0(d, sizeof d, "abcd", 4));
    T_EQ_STR(d, "");
    T_CHECK(cb_strncpy0(d, sizeof d, NULL, 0));
    T_EQ_STR(d, "");
    // Levels by name both ways.
    T_EQ_INT(cb_log_level_parse("debug"), CB_LOG_DEBUG);
    T_EQ_INT(cb_log_level_parse("ERROR"), CB_LOG_ERROR);
    T_EQ_INT(cb_log_level_parse("verbose"), -1);
    T_EQ_STR(cb_log_level_name(CB_LOG_WARN), "WARN");
}

int main(void) {
    T_RUN(every_error_has_its_name);
    T_RUN(every_status_has_its_reason);
    T_RUN(configuration_refusals);
    T_RUN(json_numbers_and_literals);
    T_RUN(json_strings);
    T_RUN(json_paths);
    T_RUN(json_writer_escapes);
    T_RUN(sigv4_query_order);
    T_RUN(signed_parts_must_be_base64url);
    T_RUN(url_decode_and_levels);
    return T_DONE();
}
