/* What the fake does with a request that is too large, malformed or out of range, and with a state file or
 * option it cannot trust. The fake reads a request into ONE buffer that holds the headers and the body
 * together (REQ_MAX of body behind up to 8 KiB more), so a Content-Length that fits REQ_MAX alone can still
 * overflow it when the headers are long: that was a write past the end of reqbuf, and it is held here by
 * three sizes of body behind about 9 KB of headers (too big by a lot, exactly the room left, one byte over).
 * Run it under AddressSanitizer too (the fake built with -fsanitize=address,undefined): an out-of-bounds
 * write is then a report, not only a wrong answer. */
#include "harness.h"

#define REQ_MAX (1 << 20)               /* fake_stripe.c: the largest Content-Length it accepts */
#define REQBUF (REQ_MAX + 8192)         /* fake_stripe.c: sizeof reqbuf, headers and body together */
#define NOW "1700000000"

static const char *BIN;
static char wire[REQBUF + 4096];        /* the request as sent */

/* POST /test/now: a pad header of padlen bytes, `extra` header lines, then Content-Length: clen_text, and a
 * body of body_len bytes ("t=NOW" then "&x=aaaa..."). Returns the length of the whole request; *hlen is
 * that of the headers (up to and with the blank line). */
static size_t build(size_t padlen, const char *extra, const char *clen_text, size_t body_len, size_t *hlen) {
    size_t n = (size_t)snprintf(wire, sizeof wire, "POST /test/now HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: ");
    memset(wire + n, 'p', padlen);
    n += padlen;
    n += (size_t)snprintf(wire + n, sizeof wire - n, "\r\n%sContent-Length: %s\r\nConnection: close\r\n\r\n", extra, clen_text);
    *hlen = n;
    static const char HEAD[] = "t=" NOW "&x=";
    for (size_t i = 0; i < body_len; i++) wire[n + i] = i < sizeof HEAD - 1 ? HEAD[i] : 'a';
    return n + body_len;
}

static int serving(int port) {
    reply r;
    api(port, "GET", "/v1/nothing", NULL, NULL, NULL, &r);
    return r.status == 404 && has(r.body, "Unrecognized request URL");
}

static void test_overflow(int port) {
    reply r;
    char cl[32];
    size_t hlen;

    /* about 9 KB of headers and a body at the whole of REQ_MAX: it passes REQ_MAX alone and not reqbuf */
    snprintf(cl, sizeof cl, "%d", REQ_MAX);
    size_t len = build(9000, "", cl, REQ_MAX, &hlen);
    CHECK(hlen > 8192 && hlen + REQ_MAX > REQBUF - 1, "the case is built as meant (headers %zu)", hlen);
    exchange(port, wire, len, &r);
    CHECK(r.status == 400 && has(r.body, "do not fit"), "9 KB of headers and Content-Length 1048576 is refused with 400: %d %s", r.status, r.body);
    CHECK(has(r.body, "\"type\":\"invalid_request_error\""), "the refusal is in Stripe's error shape: %s", r.body);
    CHECK(serving(port), "the fake serves the request after the oversized one");

    /* the Content-Length that fills exactly the room left (hlen + clen == sizeof reqbuf - 1), with the real body */
    build(9000, "", "1000000", 0, &hlen);   /* the digits of the real one are the same count */
    size_t room = (size_t)REQBUF - 1 - hlen;
    snprintf(cl, sizeof cl, "%zu", room);
    CHECK(strlen(cl) == 7, "the exact-fit Content-Length keeps its digit count (%s)", cl);
    len = build(9000, "", cl, room, &hlen);
    CHECK(hlen + room == (size_t)REQBUF - 1, "the exact-fit case is exact: %zu + %zu", hlen, room);
    exchange(port, wire, len, &r);
    CHECK(r.status == 200 && has(r.body, "\"now\":" NOW), "a body that fills the room left exactly is accepted: %d %.200s", r.status, r.body);
    CHECK(serving(port), "the fake serves the request after the exact-fit one");

    /* one byte over: refused, body or not */
    snprintf(cl, sizeof cl, "%zu", room + 1);
    len = build(9000, "", cl, room + 1, &hlen);
    exchange(port, wire, len, &r);
    CHECK(r.status == 400 && has(r.body, "do not fit"), "one byte more than fits is refused with 400: %d %.200s", r.status, r.body);
    CHECK(serving(port), "the fake serves the request after the one-byte-over one");

    /* the same refusal when the client sends no body at all (it only declares one) */
    len = build(9000, "", cl, 0, &hlen);
    exchange(port, wire, len, &r);
    CHECK(r.status == 400, "a declared body that does not fit is refused before it is read: %d", r.status);
    CHECK(serving(port), "the fake serves the request after a declared-only one");

    /* one past the size limit itself: 413, as before */
    snprintf(cl, sizeof cl, "%d", REQ_MAX + 1);
    len = build(0, "", cl, 0, &hlen);
    exchange(port, wire, len, &r);
    CHECK(r.status == 413, "Content-Length one past the limit is 413: %d", r.status);
    CHECK(serving(port), "the fake serves the request after the 413 one");
}

static void test_content_length(int port) {
    static const char *const BAD[] = {"12abc", "-1", "+12", "0x10", "1 2", "1e3", "", "99999999999999999999", "12.0"};
    reply r;
    size_t hlen;
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        size_t len = build(0, "", BAD[i], 12, &hlen);
        exchange(port, wire, len, &r);
        CHECK(r.status == 400 && has(r.body, "plain decimal number"), "Content-Length '%s' is refused with 400: %d %.120s", BAD[i], r.status, r.body);
        CHECK(serving(port), "the fake serves the request after Content-Length '%s'", BAD[i]);
    }
    /* what is accepted: a plain number, with blanks around it */
    size_t len = build(0, "", "12", 12, &hlen);
    exchange(port, wire, len, &r);
    CHECK(r.status == 200 && has(r.body, "\"now\":" NOW), "a plain Content-Length is accepted: %d %s", r.status, r.body);
    len = build(0, "", "  12 \t", 12, &hlen);
    exchange(port, wire, len, &r);
    CHECK(r.status == 200, "blanks around the number are accepted: %d %s", r.status, r.body);
    /* two Content-Length headers: equal is fine, different is a refusal */
    len = build(0, "Content-Length: 12\r\n", "12", 12, &hlen);
    exchange(port, wire, len, &r);
    CHECK(r.status == 200, "two equal Content-Length headers are accepted: %d %s", r.status, r.body);
    len = build(0, "Content-Length: 5\r\n", "12", 12, &hlen);
    exchange(port, wire, len, &r);
    CHECK(r.status == 400 && has(r.body, "two different"), "two different Content-Length headers are refused: %d %.120s", r.status, r.body);
    CHECK(serving(port), "the fake serves the request after the two-header one");
}

static void onboard_account(int port, char *acct, size_t cap) {
    reply r;
    char path[128];
    api(port, "POST", "/v1/accounts", NULL, NULL, "type=express&country=US", &r);
    jstr(r.body, "id", acct, cap);
    snprintf(path, sizeof path, "/test/accounts/%s/complete_onboarding", acct);
    http(port, "POST", path, NULL, "", &r);
}

static void session_form(char *dst, size_t cap, const char *unit, const char *qty, const char *succ, const char *canc) {
    snprintf(dst, cap,
             "mode=payment&success_url=%s&cancel_url=%s"
             "&line_items[0][price_data][currency]=usd&line_items[0][price_data][product_data][name]=T"
             "&line_items[0][price_data][unit_amount]=%s&line_items[0][quantity]=%s&metadata[event]=ev1&metadata[user]=u1",
             succ, canc, unit, qty);
}

/* amounts have Stripe's own limits, so a total cannot overflow; a URL cannot split the redirect */
static void test_session_fields(int port) {
    reply r;
    char acct[64], body[1024];
    onboard_account(port, acct, sizeof acct);
    static const char OK_URL[] = "http%3A%2F%2Fa%2Fok";
    session_form(body, sizeof body, "99999999", "999999", OK_URL, OK_URL);
    api(port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 200 && has(r.body, "\"amount_total\":99999899000001"), "the largest amount and quantity are taken: %d %.200s", r.status, r.body);
    session_form(body, sizeof body, "100000000", "1", OK_URL, OK_URL);
    api(port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 400 && has(r.body, "unit_amount"), "a unit_amount past 99999999 is refused: %d %.200s", r.status, r.body);
    session_form(body, sizeof body, "9223372036854775807", "9223372036854775807", OK_URL, OK_URL);
    api(port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 400, "int64 amounts that would overflow the total are refused: %d %.200s", r.status, r.body);
    session_form(body, sizeof body, "1", "1000000", OK_URL, OK_URL);
    api(port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 400 && has(r.body, "quantity"), "a quantity past 999999 is refused: %d %.200s", r.status, r.body);
    session_form(body, sizeof body, "12abc", "1", OK_URL, OK_URL);
    api(port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 400, "an amount with a tail is refused: %d", r.status);
    session_form(body, sizeof body, "+5", "1", OK_URL, OK_URL);
    api(port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 400, "an amount with a plus sign is refused: %d", r.status);
    session_form(body, sizeof body, "-5", "1", OK_URL, OK_URL);
    api(port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 400, "a negative amount is refused: %d", r.status);

    session_form(body, sizeof body, "100", "1", "http%3A%2F%2Fa%2F%0D%0ASet-Cookie%3A+x%3D1", OK_URL);
    api(port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 400 && has(r.body, "\"param\":\"success_url\""), "a success_url with CR LF is refused: %d %.200s", r.status, r.body);
    session_form(body, sizeof body, "100", "1", OK_URL, "http%3A%2F%2Fa%2F%0D%0ASet-Cookie%3A+x%3D1");
    api(port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 400 && has(r.body, "\"param\":\"cancel_url\""), "a cancel_url with CR LF is refused: %d %.200s", r.status, r.body);
    session_form(body, sizeof body, "100", "1", "http%3A%2F%2Fa+b", OK_URL);
    api(port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 400, "a success_url with a space is refused: %d", r.status);

    http(port, "POST", "/test/now", NULL, "t=-1", &r);
    CHECK(r.status == 400, "POST /test/now with a negative t is refused: %d", r.status);
    http(port, "POST", "/test/now", NULL, "t=12abc", &r);
    CHECK(r.status == 400, "POST /test/now with a tail is refused: %d", r.status);
    http(port, "POST", "/test/now", NULL, "t=9999999999999", &r);
    CHECK(r.status == 400, "POST /test/now past the year 2100 is refused: %d", r.status);
}

/* one POST /test/now can expire every open session: every event must be sent, not the first 64 */
static void test_many_events(void) {
    enum { N = 70 };
    reply r;
    char acct[64], body[1024];
    fs_server s = fs_start(BIN, 1, "--seed", "many", "--now", NOW, (char *)NULL);
    int b0 = hook_count();
    onboard_account(s.port, acct, sizeof acct);
    CHECK(hook_wait(b0 + 1), "account.updated arrives");
    int base = hook_count();
    session_form(body, sizeof body, "100", "1", "http%3A%2F%2Fa%2Fok", "http%3A%2F%2Fa%2Fno");
    for (int i = 0; i < N; i++) {
        api(s.port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
        CHECK(r.status == 200, "session %d is made: %d", i, r.status);
    }
    http(s.port, "POST", "/test/now", NULL, "t=1700086400", &r);
    CHECK(r.status == 200 && has(r.body, "\"expired\":70"), "all %d open sessions expire at once: %d %s", N, r.status, r.body);
    CHECK(hook_wait(base + N), "%d checkout.session.expired webhooks arrive (got %d)", N, hook_count() - base);
    CHECK(hook_quiet(base + N), "and no more than that (got %d)", hook_count() - base);
    fs_stop(&s);
}

/* ---- a data file the fake cannot trust ---- */
#define SESS_ACCT_OFF 3952   /* session_t: id, pi, currency, two urls, client_ref, email, mk[10][41], mv[10][201], then the ints */
static size_t slurp(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}
static void spit(const char *path, const char *buf, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot write %s\n", path); exit(1); }
    if (fwrite(buf, 1, n, f) != n) { printf("FAIL: cannot write %s\n", path); exit(1); }
    fclose(f);
}
static void put_int(char *p, int v) { memcpy(p, &v, sizeof v); }
static int get_int(const char *p) { int v; memcpy(&v, p, sizeof v); return v; }

static char *find(char *hay, size_t n, const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++) if (memcmp(hay + i, needle, k) == 0) return hay + i;
    return NULL;
}

static void test_data_file(void) {
    static char file[1 << 20], bad[1 << 20];
    char data[96], badpath[128], acct[64], sid[80], body[1024];
    reply r;
    snprintf(data, sizeof data, "/tmp/fake_stripe_limits_%d.dat", (int)getpid());
    snprintf(badpath, sizeof badpath, "%s.bad", data);
    unlink(data);
    fs_server s = fs_start(BIN, 0, "--seed", "file", "--data", data, (char *)NULL);
    onboard_account(s.port, acct, sizeof acct);
    session_form(body, sizeof body, "100", "1", "http%3A%2F%2Fa%2Fok", "http%3A%2F%2Fa%2Fno");
    api(s.port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    jstr(r.body, "id", sid, sizeof sid);
    fs_stop(&s);
    size_t n = slurp(data, file, sizeof file);
    CHECK(n > 0, "the data file is written");
    char *at = n ? find(file, n, sid) : NULL;
    if (!at || (size_t)(at - file) + SESS_ACCT_OFF + 24 > n) { CHECK(0, "the session is in the data file"); unlink(data); return; }
    char *ints = at + SESS_ACCT_OFF;   /* acct, status, paid, nitems, nmeta, charge */
    CHECK(get_int(ints) == 0 && get_int(ints + 4) == 0 && get_int(ints + 8) == 0 && get_int(ints + 12) == 1 &&
          get_int(ints + 16) == 2 && get_int(ints + 20) == -1, "the session's integers are where this test reads them (the layout moved?)");

    memcpy(bad, file, n);
    fs_server ok = fs_start(BIN, 0, "--data", data, (char *)NULL);
    api(ok.port, "GET", "/v1/accounts/acct_nope", NULL, NULL, NULL, &r);
    CHECK(r.status == 404, "the unmodified file loads and serves: %d", r.status);
    fs_stop(&ok);

    char *bints = bad + (ints - file);
    put_int(bints, 7);    /* an account index past the one account */
    spit(badpath, bad, n);
    CHECK(fs_start_fails(BIN, "--data", badpath, (char *)NULL) == 1, "a session whose account index is out of range is refused at start");
    put_int(bints, 0);
    put_int(bints + 4, 9);    /* a status that is not open, complete or expired */
    spit(badpath, bad, n);
    CHECK(fs_start_fails(BIN, "--data", badpath, (char *)NULL) == 1, "a session whose status is out of range is refused at start");
    put_int(bints + 4, 0);
    put_int(bints + 12, 99);    /* more line items than the table has */
    spit(badpath, bad, n);
    CHECK(fs_start_fails(BIN, "--data", badpath, (char *)NULL) == 1, "a session with too many line items is refused at start");
    put_int(bints + 12, 1);
    put_int(bints + 20, 5);    /* a charge index past the (empty) charges */
    spit(badpath, bad, n);
    CHECK(fs_start_fails(BIN, "--data", badpath, (char *)NULL) == 1, "a session whose charge index is out of range is refused at start");
    spit(badpath, file, n - 1);
    CHECK(fs_start_fails(BIN, "--data", badpath, (char *)NULL) == 1, "a truncated file is refused at start");
    memcpy(bad, file, n);
    bad[0] ^= 1;
    spit(badpath, bad, n);
    CHECK(fs_start_fails(BIN, "--data", badpath, (char *)NULL) == 1, "a file with the wrong magic is refused at start");
    unlink(data);
    unlink(badpath);
}

static void test_options(void) {
    CHECK(fs_start_fails(BIN, "--port", "12abc", (char *)NULL) == 2, "--port 12abc is refused");
    CHECK(fs_start_fails(BIN, "--port", "70000", (char *)NULL) == 2, "--port 70000 is refused");
    CHECK(fs_start_fails(BIN, "--port", "-1", (char *)NULL) == 2, "--port -1 is refused");
    CHECK(fs_start_fails(BIN, "--now", "-5", (char *)NULL) == 2, "--now -5 is refused");
    CHECK(fs_start_fails(BIN, "--now", "17e8", (char *)NULL) == 2, "--now 17e8 is refused");
    CHECK(fs_start_fails(BIN, "--now", "99999999999999999999", (char *)NULL) == 2, "--now past 64 bits is refused");
    CHECK(fs_start_fails(BIN, "--port", (char *)NULL) == 2, "an option without its value is refused");
    CHECK(fs_start_fails(BIN, "--nonsense", "1", (char *)NULL) == 2, "an unknown option is refused");
}

int main(int argc, char **argv) {
    BIN = argc > 1 ? argv[1] : "build/fake_stripe";
    hook_start();
    fs_server s = fs_start(BIN, 0, "--seed", "limits", "--now", NOW, (char *)NULL);
    test_overflow(s.port);
    test_content_length(s.port);
    test_session_fields(s.port);
    fs_stop(&s);
    test_many_events();
    test_data_file();
    test_options();
    hook_stop();
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("test_limits: all ok\n");
    return 0;
}
