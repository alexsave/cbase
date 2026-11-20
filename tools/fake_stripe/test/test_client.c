/* Drives every call the fake Stripe imitates, as an app's server will. */
#include "harness.h"

#define NOW 1700000000LL
static const char *BIN;

static fs_server start_std(const char *seed) {
    char nowarg[32];
    snprintf(nowarg, sizeof nowarg, "%lld", NOW);
    return fs_start(BIN, 1, "--seed", seed, "--now", nowarg, (char *)NULL);
}

static void session_body(char *dst, size_t cap, long long price, long long fee, const char *extra) {
    snprintf(dst, cap,
             "mode=payment&success_url=http%%3A%%2F%%2Flocalhost%%2Fok&cancel_url=http%%3A%%2F%%2Flocalhost%%2Fno"
             "&line_items[0][price_data][currency]=usd&line_items[0][price_data][product_data][name]=Ticket"
             "&line_items[0][price_data][unit_amount]=%lld&line_items[0][quantity]=1"
             "&line_items[1][price_data][currency]=usd&line_items[1][price_data][product_data][name]=Card+processing"
             "&line_items[1][price_data][unit_amount]=%lld&line_items[1][quantity]=1"
             "&metadata[event]=ev42&metadata[user]=u7%s",
             price, fee, extra ? extra : "");
}

static void new_account(int port, char *id, size_t cap) {
    reply r;
    api(port, "POST", "/v1/accounts", NULL, NULL, "type=express&country=US&email=owner%40example.com", &r);
    jstr(r.body, "id", id, cap);
}
static void onboard(int port, const char *acct) {
    reply r;
    char path[128];
    snprintf(path, sizeof path, "/test/accounts/%s/complete_onboarding", acct);
    int n = hook_count();
    http(port, "POST", path, NULL, "", &r);
    hook_wait(n + 1);
}
static void make_session(int port, const char *acct, char *sid, size_t cap, long long price, const char *extra) {
    reply r;
    char body[2048];
    session_body(body, sizeof body, price, 100, extra);
    api(port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    jstr(r.body, "id", sid, cap);
}

static void test_errors(fs_server *s) {
    reply r;
    http(s->port, "POST", "/v1/accounts", "Content-Type: application/x-www-form-urlencoded\r\n", "", &r);
    CHECK(r.status == 401 && has(r.body, "did not provide an API key"), "no key: %d %s", r.status, r.body);
    http(s->port, "POST", "/v1/accounts", "Authorization: Bearer sk_test_wrong_key_here\r\n", "", &r);
    CHECK(r.status == 401 && has(r.body, "Invalid API Key provided") && has(r.body, "\"type\":\"invalid_request_error\""), "bad key: %d %s", r.status, r.body);
    char acct[64];
    new_account(s->port, acct, sizeof acct);
    onboard(s->port, acct);
    api(s->port, "POST", "/v1/checkout/sessions", acct, NULL,
        "mode=payment&line_items[0][price_data][currency]=usd&line_items[0][price_data][product_data][name]=T"
        "&line_items[0][price_data][unit_amount]=100&line_items[0][quantity]=1", &r);
    CHECK(r.status == 400 && has(r.body, "\"code\":\"parameter_missing\"") && has(r.body, "\"param\":\"success_url\"") &&
          has(r.body, "Missing required param: success_url."), "missing param: %d %s", r.status, r.body);
    api(s->port, "GET", "/v1/accounts/acct_doesnotexist", NULL, NULL, NULL, &r);
    CHECK(r.status == 404 && has(r.body, "\"code\":\"resource_missing\"") && has(r.body, "No such account: 'acct_doesnotexist'"), "unknown account: %d %s", r.status, r.body);
    api(s->port, "GET", "/v1/checkout/sessions/cs_test_nope", acct, NULL, NULL, &r);
    CHECK(r.status == 404 && has(r.body, "No such checkout.session: 'cs_test_nope'"), "unknown session: %d %s", r.status, r.body);
    api(s->port, "POST", "/v1/checkout/sessions/cs_test_nope/expire", acct, NULL, "", &r);
    CHECK(r.status == 404, "expire unknown session: %d", r.status);
    api(s->port, "POST", "/v1/refunds", acct, NULL, "charge=ch_nope", &r);
    CHECK(r.status == 404 && has(r.body, "No such charge: 'ch_nope'"), "unknown charge: %d %s", r.status, r.body);
    api(s->port, "POST", "/v1/refunds", acct, NULL, "", &r);
    CHECK(r.status == 400 && has(r.body, "parameter_missing"), "refund without charge: %d %s", r.status, r.body);
    api(s->port, "POST", "/v1/checkout/sessions", NULL, NULL, "mode=payment", &r);
    CHECK(r.status == 400 && has(r.body, "Stripe-Account"), "no Stripe-Account: %d %s", r.status, r.body);
    api(s->port, "POST", "/v1/checkout/sessions", "acct_unknown", NULL, "mode=payment", &r);
    CHECK(r.status == 403 && has(r.body, "account_invalid"), "unknown Stripe-Account: %d %s", r.status, r.body);
    api(s->port, "GET", "/v1/nothing", NULL, NULL, NULL, &r);
    CHECK(r.status == 404 && has(r.body, "Unrecognized request URL"), "unknown route: %d", r.status);
}

static void test_accounts(fs_server *s) {
    reply r;
    int before = hook_count();
    api(s->port, "POST", "/v1/accounts", NULL, NULL, "type=express&country=NO&email=eier%40example.no", &r);
    char acct[64], cur[8];
    CHECK(r.status == 200 && has(r.body, "\"object\":\"account\"") && has(r.body, "\"charges_enabled\":false") &&
          has(r.body, "\"country\":\"NO\"") && has(r.body, "\"email\":\"eier@example.no\""), "create: %d %s", r.status, r.body);
    jstr(r.body, "id", acct, sizeof acct);
    jstr(r.body, "default_currency", cur, sizeof cur);
    CHECK(strncmp(acct, "acct_", 5) == 0 && strcmp(cur, "nok") == 0, "id %s currency %s", acct, cur);
    api(s->port, "POST", "/v1/accounts", NULL, NULL, "type=standard", &r);
    CHECK(r.status == 400, "non-express refused: %d", r.status);
    api(s->port, "POST", "/v1/accounts", NULL, NULL, "type=express&country=norway", &r);
    CHECK(r.status == 400 && has(r.body, "\"param\":\"country\""), "bad country: %d %s", r.status, r.body);
    char path[128];
    snprintf(path, sizeof path, "/v1/accounts/%s", acct);
    api(s->port, "GET", path, NULL, NULL, NULL, &r);
    CHECK(r.status == 200 && has(r.body, acct) && has(r.body, "\"charges_enabled\":false"), "retrieve: %d %s", r.status, r.body);

    char form[256];
    snprintf(form, sizeof form, "account=%s&type=account_onboarding&refresh_url=http%%3A%%2F%%2Fx%%2Fr&return_url=http%%3A%%2F%%2Fx%%2Fd", acct);
    api(s->port, "POST", "/v1/account_links", NULL, NULL, form, &r);
    CHECK(r.status == 200 && has(r.body, "\"object\":\"account_link\"") && has(r.body, "/onboard/") && has(r.body, "\"expires_at\":1700000300"),
          "account link: %d %s", r.status, r.body);
    snprintf(form, sizeof form, "account=%s&type=account_onboarding&refresh_url=http%%3A%%2F%%2Fx%%2Fr", acct);
    api(s->port, "POST", "/v1/account_links", NULL, NULL, form, &r);
    CHECK(r.status == 400 && has(r.body, "\"param\":\"return_url\""), "link missing return_url: %d %s", r.status, r.body);
    api(s->port, "POST", "/v1/account_links", NULL, NULL, "account=acct_nope&type=account_update", &r);
    CHECK(r.status == 404, "link unknown account: %d", r.status);
    char onb[200], url[200];
    api(s->port, "POST", "/v1/account_links", NULL, NULL, "account=acct_nope&type=account_update", &r);
    snprintf(form, sizeof form, "account=%s&type=account_update", acct);
    api(s->port, "POST", "/v1/account_links", NULL, NULL, form, &r);
    jstr(r.body, "url", url, sizeof url);
    snprintf(onb, sizeof onb, "%s", strstr(url, "/onboard/"));
    http(s->port, "GET", onb, NULL, NULL, &r);
    CHECK(r.status == 200 && has(r.body, "Fake Stripe onboarding"), "onboarding page: %d", r.status);

    /* a session cannot be made until onboarding completes */
    char body[2048];
    session_body(body, sizeof body, 2500, 106, NULL);
    api(s->port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 400 && has(r.body, "capabilities"), "session before onboarding: %d %s", r.status, r.body);
    CHECK(hook_quiet(before), "no webhook before onboarding completes");

    snprintf(path, sizeof path, "/test/accounts/%s/complete_onboarding", acct);
    http(s->port, "POST", path, NULL, "", &r);
    CHECK(r.status == 200 && has(r.body, "\"charges_enabled\":true"), "complete onboarding: %d %s", r.status, r.body);
    CHECK(hook_wait(before + 1), "account.updated webhook arrives");
    CHECK(has(HK.h[before % 64].body, "\"type\":\"account.updated\"") && has(HK.h[before % 64].body, "\"charges_enabled\":true") &&
          has(HK.h[before % 64].body, acct), "account.updated body: %s", HK.h[before % 64].body);
    snprintf(path, sizeof path, "/v1/accounts/%s", acct);
    api(s->port, "GET", path, NULL, NULL, NULL, &r);
    CHECK(has(r.body, "\"charges_enabled\":true") && has(r.body, "\"details_submitted\":true"), "retrieve after onboarding: %s", r.body);
    snprintf(path, sizeof path, "/test/accounts/%s/complete_onboarding", acct);
    http(s->port, "POST", path, NULL, "", &r);
    CHECK(hook_quiet(before + 1), "second complete_onboarding sends no second webhook");
    http(s->port, "POST", "/test/accounts/acct_nope/complete_onboarding", NULL, "", &r);
    CHECK(r.status == 404, "complete_onboarding unknown: %d", r.status);
}

static void test_checkout_and_refund(fs_server *s) {
    reply r;
    char acct[64], other[64], sid[80], pi[64], ch[64], path[200];
    new_account(s->port, acct, sizeof acct);
    new_account(s->port, other, sizeof other);
    onboard(s->port, acct);
    onboard(s->port, other);
    int base = hook_count();

    /* the fake computes no fee: the second line item is whatever the app put there */
    const int64_t fee = 106;
    char body[2048];
    session_body(body, sizeof body, 2500, fee, NULL);
    api(s->port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 200 && has(r.body, "\"object\":\"checkout.session\"") && has(r.body, "\"amount_total\":2606") &&
          has(r.body, "\"status\":\"open\"") && has(r.body, "\"payment_status\":\"unpaid\"") && has(r.body, "\"currency\":\"usd\"") &&
          has(r.body, "\"success_url\":\"http://localhost/ok\"") && has(r.body, "\"cancel_url\":\"http://localhost/no\"") &&
          has(r.body, "\"application_fee_amount\":null") && has(r.body, "\"payment_intent\":null") &&
          has(r.body, "\"event\":\"ev42\"") && has(r.body, "\"expires_at\":1700086400") && has(r.body, "/c/pay/cs_test_"),
          "create session: %d %s", r.status, r.body);
    jstr(r.body, "id", sid, sizeof sid);
    CHECK(strncmp(sid, "cs_test_", 8) == 0, "session id %s", sid);

    char b2[2300];
    session_body(b2, sizeof b2, 2500, fee, "&payment_intent_data[application_fee_amount]=50");
    api(s->port, "POST", "/v1/checkout/sessions", acct, NULL, b2, &r);
    CHECK(r.status == 400 && has(r.body, "application_fee_amount"), "nonzero application fee refused: %d %s", r.status, r.body);
    session_body(b2, sizeof b2, 2500, fee, "&payment_intent_data[application_fee_amount]=0");
    api(s->port, "POST", "/v1/checkout/sessions", acct, NULL, b2, &r);
    CHECK(r.status == 200, "zero application fee accepted: %d", r.status);
    api(s->port, "POST", "/v1/checkout/sessions", acct, NULL,
        "mode=payment&success_url=http%3A%2F%2Fa&line_items[0][price]=price_123&line_items[0][quantity]=1", &r);
    CHECK(r.status == 400, "price ids refused: %d", r.status);
    api(s->port, "POST", "/v1/checkout/sessions", acct, NULL, "mode=payment&success_url=http%3A%2F%2Fa", &r);
    CHECK(r.status == 400 && has(r.body, "\"param\":\"line_items\""), "missing line_items: %d %s", r.status, r.body);

    snprintf(path, sizeof path, "/v1/checkout/sessions/%s", sid);
    api(s->port, "GET", path, acct, NULL, NULL, &r);
    CHECK(r.status == 200 && has(r.body, sid) && has(r.body, "\"status\":\"open\""), "retrieve: %d %s", r.status, r.body);
    api(s->port, "GET", path, other, NULL, NULL, &r);
    CHECK(r.status == 404, "another account cannot see the session: %d", r.status);

    /* idempotency */
    char s1[80], s2[80];
    session_body(body, sizeof body, 1000, 61, NULL);
    api(s->port, "POST", "/v1/checkout/sessions", acct, "key-1", body, &r);
    jstr(r.body, "id", s1, sizeof s1);
    CHECK(r.status == 200 && !has(r.hdr, "Idempotent-Replayed"), "first call: %d", r.status);
    api(s->port, "POST", "/v1/checkout/sessions", acct, "key-1", body, &r);
    jstr(r.body, "id", s2, sizeof s2);
    CHECK(r.status == 200 && strcmp(s1, s2) == 0 && has(r.hdr, "Idempotent-Replayed: true"), "same key and body replays: %s vs %s", s1, s2);
    session_body(body, sizeof body, 1001, 61, NULL);
    api(s->port, "POST", "/v1/checkout/sessions", acct, "key-1", body, &r);
    CHECK(r.status == 400 && has(r.body, "\"type\":\"idempotency_error\"") && has(r.body, "Keys for idempotent requests can only be used with the same parameters"),
          "same key, different body: %d %s", r.status, r.body);
    api(s->port, "POST", "/v1/checkout/sessions", acct, "key-2", body, &r);
    jstr(r.body, "id", s2, sizeof s2);
    CHECK(r.status == 200 && strcmp(s1, s2) != 0, "a new key makes a new session");

    /* pay: webhook, status, double pay, expire after pay */
    snprintf(path, sizeof path, "/test/sessions/%s/pay", sid);
    http(s->port, "POST", path, NULL, "", &r);
    CHECK(r.status == 200 && has(r.body, "\"status\":\"complete\"") && has(r.body, "\"payment_status\":\"paid\"") && has(r.body, "\"payment_intent\":\"pi_"),
          "pay: %d %s", r.status, r.body);
    jstr(r.body, "payment_intent", pi, sizeof pi);
    CHECK(hook_wait(base + 1), "checkout.session.completed webhook arrives");
    CHECK(has(HK.h[base % 64].body, "\"type\":\"checkout.session.completed\"") && has(HK.h[base % 64].body, sid) &&
          has(HK.h[base % 64].body, "\"user\":\"u7\"") && has(HK.h[base % 64].body, acct), "completed body: %s", HK.h[base % 64].body);
    http(s->port, "POST", path, NULL, "", &r);
    CHECK(r.status == 400, "pay twice: %d", r.status);
    snprintf(path, sizeof path, "/v1/checkout/sessions/%s/expire", sid);
    api(s->port, "POST", path, acct, NULL, "", &r);
    CHECK(r.status == 400 && has(r.body, "can be expired"), "expire a paid session: %d %s", r.status, r.body);

    /* refund: the price only, the fee is kept */
    api(s->port, "POST", "/v1/refunds", acct, NULL, "payment_intent=pi_nope", &r);
    CHECK(r.status == 404, "refund unknown pi: %d", r.status);
    char form[200];
    snprintf(form, sizeof form, "payment_intent=%s&amount=2500", pi);
    api(s->port, "POST", "/v1/refunds", other, NULL, form, &r);
    CHECK(r.status == 404, "another account cannot refund it: %d", r.status);
    api(s->port, "POST", "/v1/refunds", acct, NULL, form, &r);
    CHECK(r.status == 200 && has(r.body, "\"object\":\"refund\"") && has(r.body, "\"amount\":2500") && has(r.body, "\"status\":\"succeeded\"") && has(r.body, pi),
          "refund: %d %s", r.status, r.body);
    jstr(r.body, "charge", ch, sizeof ch);
    CHECK(hook_wait(base + 2), "charge.refunded webhook arrives");
    CHECK(has(HK.h[(base + 1) % 64].body, "\"type\":\"charge.refunded\"") && has(HK.h[(base + 1) % 64].body, "\"amount_refunded\":2500") &&
          has(HK.h[(base + 1) % 64].body, "\"refunded\":false") && has(HK.h[(base + 1) % 64].body, ch), "refunded body: %s", HK.h[(base + 1) % 64].body);
    snprintf(form, sizeof form, "charge=%s&amount=107", ch);
    api(s->port, "POST", "/v1/refunds", acct, NULL, form, &r);
    CHECK(r.status == 400 && has(r.body, "greater than unrefunded amount"), "over-refund: %d %s", r.status, r.body);
    snprintf(form, sizeof form, "charge=%s", ch);
    api(s->port, "POST", "/v1/refunds", acct, NULL, form, &r);
    CHECK(r.status == 200 && has(r.body, "\"amount\":106"), "refund the remainder by default: %d %s", r.status, r.body);
    CHECK(hook_wait(base + 3) && has(HK.h[(base + 2) % 64].body, "\"refunded\":true"), "fully refunded flag");
    api(s->port, "POST", "/v1/refunds", acct, NULL, form, &r);
    CHECK(r.status == 400, "refund of a fully refunded charge: %d", r.status);

    /* expire */
    int n = hook_count();
    make_session(s->port, acct, sid, sizeof sid, 900, NULL);
    snprintf(path, sizeof path, "/v1/checkout/sessions/%s/expire", sid);
    api(s->port, "POST", path, acct, NULL, "", &r);
    CHECK(r.status == 200 && has(r.body, "\"status\":\"expired\"") && has(r.body, "\"url\":null"), "expire: %d %s", r.status, r.body);
    CHECK(hook_wait(n + 1) && has(HK.h[n % 64].body, "\"type\":\"checkout.session.expired\"") && has(HK.h[n % 64].body, sid), "expired webhook");
    api(s->port, "POST", path, acct, NULL, "", &r);
    CHECK(r.status == 400, "expire twice: %d", r.status);
    snprintf(path, sizeof path, "/test/sessions/%s/pay", sid);
    http(s->port, "POST", path, NULL, "", &r);
    CHECK(r.status == 400, "pay an expired session: %d", r.status);
    snprintf(path, sizeof path, "/v1/checkout/sessions/%s", sid);
    api(s->port, "GET", path, acct, NULL, NULL, &r);
    CHECK(has(r.body, "\"status\":\"expired\""), "retrieve expired: %s", r.body);
}

static void test_clock(fs_server *s) {
    reply r;
    char acct[64], sid[80], sid2[80], form[64];
    new_account(s->port, acct, sizeof acct);
    onboard(s->port, acct);
    char body[2300];
    session_body(body, sizeof body, 500, 50, "&expires_at=1700000100");
    api(s->port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 400 && has(r.body, "at least 30 minutes"), "expires_at too soon: %d %s", r.status, r.body);
    session_body(body, sizeof body, 500, 50, "&expires_at=1700090000");
    api(s->port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    CHECK(r.status == 400 && has(r.body, "less than 24 hours"), "expires_at too late: %d %s", r.status, r.body);
    session_body(body, sizeof body, 500, 50, "&expires_at=1700003600");
    api(s->port, "POST", "/v1/checkout/sessions", acct, NULL, body, &r);
    jstr(r.body, "id", sid, sizeof sid);
    CHECK(r.status == 200 && has(r.body, "\"expires_at\":1700003600") && has(r.body, "\"created\":1700000000"), "short session: %d %s", r.status, r.body);
    make_session(s->port, acct, sid2, sizeof sid2, 500, NULL);
    int n = hook_count();
    http(s->port, "POST", "/test/now", NULL, "t=1700003599", &r);
    CHECK(r.status == 200 && has(r.body, "\"expired\":0"), "before the deadline: %s", r.body);
    CHECK(hook_quiet(n), "nothing expires early");
    http(s->port, "POST", "/test/now", NULL, "t=1700003600", &r);
    CHECK(r.status == 200 && has(r.body, "\"expired\":1"), "at the deadline: %s", r.body);
    CHECK(hook_wait(n + 1) && has(HK.h[n % 64].body, "\"type\":\"checkout.session.expired\"") && has(HK.h[n % 64].body, sid), "timed expiry webhook");
    CHECK(has(HK.h[n % 64].sig, "t=1700003600,v1="), "signature carries the injected now: %s", HK.h[n % 64].sig);
    char path[200];
    snprintf(path, sizeof path, "/v1/checkout/sessions/%s", sid2);
    api(s->port, "GET", path, acct, NULL, NULL, &r);
    CHECK(has(r.body, "\"status\":\"open\""), "the 24 hour session is still open");
    snprintf(form, sizeof form, "x=1");
    http(s->port, "POST", "/test/now", NULL, form, &r);
    CHECK(r.status == 400, "now without t: %d", r.status);
}

static void test_pages(fs_server *s) {
    reply r;
    char acct[64], sid[80], path[200];
    new_account(s->port, acct, sizeof acct);
    onboard(s->port, acct);
    int n = hook_count();
    make_session(s->port, acct, sid, sizeof sid, 700, NULL);
    snprintf(path, sizeof path, "/c/pay/%s", sid);
    http(s->port, "GET", path, NULL, NULL, &r);
    CHECK(r.status == 200 && has(r.body, "<button>Pay</button>") && has(r.body, "<button>Cancel</button>") && has(r.body, "800 usd"), "checkout page: %d %s", r.status, r.body);
    snprintf(path, sizeof path, "/c/pay/%s/cancel", sid);
    http(s->port, "POST", path, NULL, "", &r);
    CHECK(r.status == 303 && has(r.hdr, "Location: http://localhost/no"), "cancel redirects: %d %s", r.status, r.hdr);
    CHECK(hook_quiet(n), "cancel sends no webhook");
    snprintf(path, sizeof path, "/c/pay/%s/pay", sid);
    http(s->port, "POST", path, NULL, "", &r);
    CHECK(r.status == 303 && has(r.hdr, "Location: http://localhost/ok"), "pay redirects: %d %s", r.status, r.hdr);
    CHECK(hook_wait(n + 1) && has(HK.h[n % 64].body, "checkout.session.completed"), "page pay completes the session");
    snprintf(path, sizeof path, "/c/pay/%s", sid);
    http(s->port, "GET", path, NULL, NULL, &r);
    CHECK(has(r.body, "no longer open"), "page of a paid session");
}

/* Clients at once. The fake answers on several request workers (cbase's request pool), and one lock keeps
 * its state whole: CONC_T clients each make CONC_N sessions as fast as they can, and every session must come
 * back with an id of its own and, read back, the amount its client asked for. Without the lock two sessions
 * made together take the same slot (one is lost, or one reads back the other's amount), or draw the same id. */
#define CONC_T 4
#define CONC_N 100
typedef struct { int port, t; const char *acct; char ids[CONC_N][80]; int status[CONC_N]; } conc_arg;
static long long conc_price(int t, int i) { return 10000 + t * 1000 + i; }
static void *conc_client(void *ud) {
    conc_arg *a = ud;
    for (int i = 0; i < CONC_N; i++) {
        char body[2048];
        reply r;
        session_body(body, sizeof body, conc_price(a->t, i), 1,
                     "&metadata[a]=1&metadata[b]=2&metadata[c]=3&metadata[d]=4&metadata[e]=5&metadata[f]=6&metadata[g]=7&metadata[h]=8");
        api(a->port, "POST", "/v1/checkout/sessions", a->acct, NULL, body, &r);
        a->status[i] = r.status;
        if (!jstr(r.body, "id", a->ids[i], sizeof a->ids[i])) a->ids[i][0] = 0;
    }
    return NULL;
}
static void test_concurrent(void) {
    static conc_arg a[CONC_T];
    char acct[64];
    fs_server s = start_std("conc");
    new_account(s.port, acct, sizeof acct);
    onboard(s.port, acct);
    pthread_t th[CONC_T];
    for (int t = 0; t < CONC_T; t++) {
        memset(&a[t], 0, sizeof a[t]);
        a[t].port = s.port;
        a[t].t = t;
        a[t].acct = acct;
        pthread_create(&th[t], NULL, conc_client, &a[t]);
    }
    for (int t = 0; t < CONC_T; t++) pthread_join(th[t], NULL);
    int made = 0, dup = 0, lost = 0, wrong = 0;
    for (int t = 0; t < CONC_T; t++)
        for (int i = 0; i < CONC_N; i++) {
            if (a[t].status[i] != 200 || !a[t].ids[i][0]) continue;
            made++;
            for (int u = 0; u <= t; u++)
                for (int j = 0; j < (u == t ? i : CONC_N); j++)
                    if (strcmp(a[t].ids[i], a[u].ids[j]) == 0) dup++;
            reply r;
            char path[160], want[64];
            snprintf(path, sizeof path, "/v1/checkout/sessions/%s", a[t].ids[i]);
            api(s.port, "GET", path, acct, NULL, NULL, &r);
            snprintf(want, sizeof want, "\"amount_total\":%lld,", conc_price(t, i) + 1);
            if (r.status != 200) lost++;
            else if (!has(r.body, want)) wrong++;
        }
    CHECK(made == CONC_T * CONC_N, "%d clients at once make every session: %d of %d", CONC_T, made, CONC_T * CONC_N);
    CHECK(dup == 0, "every session made at once has an id of its own (%d shared)", dup);
    CHECK(lost == 0 && wrong == 0, "every session made at once reads back as it was made (%d lost, %d with another's amount)", lost, wrong);
    fs_stop(&s);
}

static void first_account_id(int seed_variant, const char *seed, const char *now, char *id, size_t cap) {
    (void)seed_variant;
    fs_server s = fs_start(BIN, 0, "--seed", seed, "--now", now, (char *)NULL);
    new_account(s.port, id, cap);
    fs_stop(&s);
}
static void test_ids(void) {
    char a[64], b[64], c[64], d[64];
    first_account_id(0, "alpha", "1700000000", a, sizeof a);
    first_account_id(1, "alpha", "1800000000", b, sizeof b);
    first_account_id(2, "beta", "1700000000", c, sizeof c);
    CHECK(strcmp(a, b) == 0, "same seed gives the same id whatever the clock: %s vs %s", a, b);
    CHECK(strcmp(a, c) != 0, "a different seed gives a different id: %s vs %s", a, c);
    /* the default seed is also fixed */
    fs_server s = fs_start(BIN, 0, (char *)NULL);
    new_account(s.port, d, sizeof d);
    fs_stop(&s);
    fs_server s2 = fs_start(BIN, 0, (char *)NULL);
    char e[64];
    new_account(s2.port, e, sizeof e);
    fs_stop(&s2);
    CHECK(strcmp(d, e) == 0 && strcmp(d, a) != 0, "default seed is a fixed name: %s %s", d, e);
    /* not clock-based: the same seed a second apart still gives the same id */
    fs_server s3 = fs_start(BIN, 0, "--seed", "alpha", (char *)NULL);
    char f1[64], f2[64];
    new_account(s3.port, f1, sizeof f1);
    fs_stop(&s3);
    msleep(1100);
    fs_server s4 = fs_start(BIN, 0, "--seed", "alpha", (char *)NULL);
    new_account(s4.port, f2, sizeof f2);
    fs_stop(&s4);
    CHECK(strcmp(f1, f2) == 0 && strcmp(f1, a) == 0, "ids do not depend on the wall clock: %s %s %s", f1, f2, a);
}

static void test_data_file(void) {
    char data[64];
    snprintf(data, sizeof data, "/tmp/fake_stripe_test_%d.dat", (int)getpid());
    unlink(data);
    reply r;
    char acct[64], sid[80], acct2[64], path[200], pi[64];
    fs_server s = fs_start(BIN, 0, "--seed", "persist", "--data", data, (char *)NULL);
    new_account(s.port, acct, sizeof acct);
    onboard(s.port, acct);
    make_session(s.port, acct, sid, sizeof sid, 1234, NULL);
    snprintf(path, sizeof path, "/test/sessions/%s/pay", sid);
    http(s.port, "POST", path, NULL, "", &r);
    jstr(r.body, "payment_intent", pi, sizeof pi);
    char body[2048];
    session_body(body, sizeof body, 1000, 61, NULL);
    api(s.port, "POST", "/v1/checkout/sessions", acct, "persist-key", body, &r);
    fs_stop(&s);

    fs_server s2 = fs_start(BIN, 0, "--seed", "persist", "--data", data, (char *)NULL);
    snprintf(path, sizeof path, "/v1/accounts/%s", acct);
    api(s2.port, "GET", path, NULL, NULL, NULL, &r);
    CHECK(r.status == 200 && has(r.body, "\"charges_enabled\":true"), "account survives a restart: %d %s", r.status, r.body);
    snprintf(path, sizeof path, "/v1/checkout/sessions/%s", sid);
    api(s2.port, "GET", path, acct, NULL, NULL, &r);
    CHECK(r.status == 200 && has(r.body, "\"payment_status\":\"paid\""), "paid session survives: %d %s", r.status, r.body);
    char form[200];
    snprintf(form, sizeof form, "payment_intent=%s&amount=1000", pi);
    api(s2.port, "POST", "/v1/refunds", acct, NULL, form, &r);
    CHECK(r.status == 200, "the charge survives (refund works): %d %s", r.status, r.body);
    api(s2.port, "POST", "/v1/checkout/sessions", acct, "persist-key", body, &r);
    CHECK(r.status == 200 && has(r.hdr, "Idempotent-Replayed: true"), "idempotency keys survive: %d", r.status);
    new_account(s2.port, acct2, sizeof acct2);
    CHECK(strcmp(acct, acct2) != 0, "the id generator continues, never repeats: %s", acct2);
    fs_stop(&s2);
    unlink(data);

    /* the id stream resumes where it stopped: after a restart, the second id equals an uninterrupted run's second id */
    char ref1[64], ref2[64], p1[64], p2[64];
    fs_server rf = fs_start(BIN, 0, "--seed", "resume", (char *)NULL);
    new_account(rf.port, ref1, sizeof ref1);
    new_account(rf.port, ref2, sizeof ref2);
    fs_stop(&rf);
    fs_server a1 = fs_start(BIN, 0, "--seed", "resume", "--data", data, (char *)NULL);
    new_account(a1.port, p1, sizeof p1);
    fs_stop(&a1);
    fs_server a2 = fs_start(BIN, 0, "--seed", "resume", "--data", data, (char *)NULL);
    new_account(a2.port, p2, sizeof p2);
    fs_stop(&a2);
    CHECK(strcmp(p1, ref1) == 0 && strcmp(p2, ref2) == 0, "id stream resumes after restart: %s %s vs %s %s", p1, p2, ref1, ref2);
    unlink(data);
}

int main(int argc, char **argv) {
    BIN = argc > 1 ? argv[1] : "build/fake_stripe";
    hook_start();
    fs_server s = start_std("main");
    test_errors(&s);
    test_accounts(&s);
    test_checkout_and_refund(&s);
    test_clock(&s);
    test_pages(&s);
    fs_stop(&s);
    test_concurrent();
    test_ids();
    test_data_file();
    hook_stop();
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("test_client: all ok\n");
    return 0;
}
