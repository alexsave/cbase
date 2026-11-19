/* The fake's signed webhooks, checked by a verifier that shares nothing with the fake: the HMAC here is
 * RFC 2104 written out over lib/sha256.c, not the OpenSSL call the fake signs with. A signature must pass
 * with the configured secret, fail with another, fail with one body byte changed, and fail when its t is
 * far from now (300 s, the tolerance Stripe's own libraries default to). */
#include "harness.h"
#include "../../../lib/sha256.h"

#define NOW 1700000000LL
#define TOLERANCE 300

enum { WH_OK, WH_MALFORMED, WH_MISMATCH, WH_STALE };

/* HMAC-SHA256 (RFC 2104): H((K ^ opad) || H((K ^ ipad) || m)), the key padded with zeros to the 64-byte block. */
static void hmac_sha256(const char *key, const void *msg, size_t n, uint8_t out[SHA256_DIGEST_LEN]) {
    uint8_t k[64] = {0}, ipad[64], opad[64], inner[SHA256_DIGEST_LEN];
    size_t kl = strlen(key);
    if (kl > sizeof k) sha256(key, kl, k);
    else memcpy(k, key, kl);
    for (size_t i = 0; i < sizeof k; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, ipad, sizeof ipad);
    sha256_update(&c, msg, n);
    sha256_final(&c, inner);
    sha256_init(&c);
    sha256_update(&c, opad, sizeof opad);
    sha256_update(&c, inner, sizeof inner);
    sha256_final(&c, out);
}

/* Stripe-Signature: t=T,v1=HEX  -  HEX is HMAC-SHA256(secret, "T." + body), lowercase. */
static int wh_verify(const char *header, const char *body, size_t bl, const char *secret, int64_t now) {
    if (strncmp(header, "t=", 2) != 0) return WH_MALFORMED;
    size_t tl = strspn(header + 2, "0123456789");
    int64_t t;
    if (!num_parse_i64(header + 2, tl, 0, INT64_MAX, &t)) return WH_MALFORMED;
    const char *v = header + 2 + tl;
    if (strncmp(v, ",v1=", 4) != 0 || strlen(v + 4) != 2 * SHA256_DIGEST_LEN) return WH_MALFORMED;
    static char msg[BODY_MAX + 32];
    int ml = snprintf(msg, sizeof msg, "%lld.", (long long)t);
    if (ml < 0 || (size_t)ml + bl >= sizeof msg) return WH_MALFORMED;
    memcpy(msg + ml, body, bl);
    uint8_t mac[SHA256_DIGEST_LEN];
    hmac_sha256(secret, msg, (size_t)ml + bl, mac);
    char hex[2 * SHA256_DIGEST_LEN + 1];
    for (size_t i = 0; i < sizeof mac; i++) snprintf(hex + 2 * i, 3, "%02x", mac[i]);
    if (strcmp(hex, v + 4) != 0) return WH_MISMATCH;
    int64_t age = now - t;
    if (age > TOLERANCE || age < -TOLERANCE) return WH_STALE;
    return WH_OK;
}

/* RFC 4231 test case 2 ("Jefe"), so the verifier's own HMAC is pinned before it judges the fake. */
static void hmac_is_rfc_4231(void) {
    uint8_t mac[SHA256_DIGEST_LEN];
    hmac_sha256("Jefe", "what do ya want for nothing?", 28, mac);
    static const uint8_t want[SHA256_DIGEST_LEN] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
        0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43};
    CHECK(memcmp(mac, want, sizeof want) == 0, "the test's HMAC matches RFC 4231 case 2");
}

static int hook_idx(int i) { return i % 64; }

int main(int argc, char **argv) {
    const char *bin = argc > 1 ? argv[1] : "build/fake_stripe";
    hmac_is_rfc_4231();
    hook_start();
    reply r;
    fs_server s = fs_start(bin, 1, "--seed", "wh", "--now", "1700000000", (char *)NULL);
    char acct[64], sid[80], sid2[80], pi[64], path[200], form[200];
    api(s.port, "POST", "/v1/accounts", NULL, NULL, "type=express&country=US", &r);
    jstr(r.body, "id", acct, sizeof acct);
    snprintf(path, sizeof path, "/test/accounts/%s/complete_onboarding", acct);
    http(s.port, "POST", path, NULL, "", &r);
    const char *sb = "mode=payment&success_url=http%3A%2F%2Fa%2Fok&line_items[0][price_data][currency]=usd"
                     "&line_items[0][price_data][product_data][name]=T&line_items[0][price_data][unit_amount]=1000&line_items[0][quantity]=1";
    api(s.port, "POST", "/v1/checkout/sessions", acct, NULL, sb, &r);
    jstr(r.body, "id", sid, sizeof sid);
    api(s.port, "POST", "/v1/checkout/sessions", acct, NULL, sb, &r);
    jstr(r.body, "id", sid2, sizeof sid2);
    snprintf(path, sizeof path, "/test/sessions/%s/pay", sid);
    http(s.port, "POST", path, NULL, "", &r);
    jstr(r.body, "payment_intent", pi, sizeof pi);
    snprintf(path, sizeof path, "/v1/checkout/sessions/%s/expire", sid2);
    api(s.port, "POST", path, acct, NULL, "", &r);
    snprintf(form, sizeof form, "payment_intent=%s&amount=400", pi);
    api(s.port, "POST", "/v1/refunds", acct, NULL, form, &r);
    CHECK(hook_wait(4), "four webhooks arrive (got %d)", hook_count());

    const char *want[4] = {"account.updated", "checkout.session.completed", "checkout.session.expired", "charge.refunded"};
    for (int i = 0; i < 4 && i < hook_count(); i++) {
        hook_t *h = &HK.h[hook_idx(i)];
        char tag[64];
        snprintf(tag, sizeof tag, "\"type\":\"%s\"", want[i]);
        CHECK(has(h->body, tag), "event %d is %s: %s", i, want[i], h->body);
        size_t bl = strlen(h->body);
        CHECK(wh_verify(h->sig, h->body, bl, FS_SECRET, NOW) == WH_OK, "%s passes with the configured secret (sig %s)", want[i], h->sig);
        CHECK(wh_verify(h->sig, h->body, bl, "whsec_wrong", NOW) == WH_MISMATCH, "%s fails with the wrong secret", want[i]);
        CHECK(wh_verify(h->sig, h->body, bl, FS_SECRET, NOW + TOLERANCE) == WH_OK, "%s still passes at the tolerance's edge", want[i]);
        CHECK(wh_verify(h->sig, h->body, bl, FS_SECRET, NOW + TOLERANCE + 1) == WH_STALE, "%s is stale just past the tolerance", want[i]);
        CHECK(wh_verify(h->sig, h->body, bl, FS_SECRET, NOW - TOLERANCE - 1) == WH_STALE, "%s is stale for a t in the future too", want[i]);
        CHECK(wh_verify(h->sig, h->body, bl, FS_SECRET, NOW + 1000) == WH_STALE, "%s is stale a long time later", want[i]);
        char tampered[BODY_MAX];
        snprintf(tampered, sizeof tampered, "%s", h->body);
        tampered[bl / 2] ^= 1;
        CHECK(wh_verify(h->sig, tampered, bl, FS_SECRET, NOW) == WH_MISMATCH, "%s fails with one byte changed", want[i]);
        CHECK(strncmp(h->sig, "t=1700000000,v1=", 16) == 0 && strlen(h->sig) == 16 + 64, "%s header shape: %s", want[i], h->sig);
    }
    fs_stop(&s);

    /* a server with a different secret does not verify under ours */
    int n = hook_count();
    fs_server s2 = fs_start(bin, 1, "--seed", "wh2", "--now", "1700000000", "--webhook-secret", "whsec_other", (char *)NULL);
    api(s2.port, "POST", "/v1/accounts", NULL, NULL, "type=express&country=US", &r);
    jstr(r.body, "id", acct, sizeof acct);
    snprintf(path, sizeof path, "/test/accounts/%s/complete_onboarding", acct);
    http(s2.port, "POST", path, NULL, "", &r);
    CHECK(hook_wait(n + 1), "webhook from the second server");
    hook_t *h = &HK.h[hook_idx(n)];
    CHECK(wh_verify(h->sig, h->body, strlen(h->body), "whsec_other", NOW) == WH_OK, "own secret verifies");
    CHECK(wh_verify(h->sig, h->body, strlen(h->body), FS_SECRET, NOW) == WH_MISMATCH, "the secret of the first server does not");
    fs_stop(&s2);
    hook_stop();
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("test_webhook: all ok\n");
    return 0;
}
