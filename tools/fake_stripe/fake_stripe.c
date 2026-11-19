/* fake_stripe: a local imitation of the few Stripe API calls an app makes (Connect Express
 * accounts, direct-charge Checkout sessions, refunds, signed webhooks).
 * Test and `make serve` only; never shipped. JSON appears here because it is Stripe's wire
 * format, the one thing a Stripe client can read (tools/ is outside the JSON gate, as
 * tools/cbase.d/test/fake_aws.c is for the same reason). See README.md for exactly what is and
 * is not imitated.
 *
 * Randomness: ids only, from lib/rng.h's splitmix64 over one state (G.rng) that --seed names
 * through lib/fnv.h, so the same seed and the same call order give the same ids. No other
 * random draw exists here. The clock is --now or POST /test/now; the real clock stamps
 * `created` and signatures only when neither is given, never an id. */
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE /* Darwin hides INADDR_LOOPBACK and the BSD socket extras behind this */
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../../lib/fnv.h"
#include "../../lib/num_parse.h"
#include "../../lib/rng.h"

/* ---------- growable buffer ---------- */
typedef struct { char *p; size_t n, cap; } sbuf;

static void sb_reserve(sbuf *b, size_t extra) {
    if (b->n + extra + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->n + extra + 1) c *= 2;
        b->p = realloc(b->p, c);
        if (!b->p) { perror("realloc"); exit(1); }
        b->cap = c;
    }
}
static void sb_add(sbuf *b, const char *s) {
    size_t l = strlen(s);
    sb_reserve(b, l);
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}
static void sb_addf(sbuf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int l = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (l < 0) return;
    if ((size_t)l < sizeof tmp) { sb_add(b, tmp); return; }
    char *big = malloc((size_t)l + 1);
    if (!big) { perror("malloc"); exit(1); }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)l + 1, fmt, ap);
    va_end(ap);
    sb_add(b, big);
    free(big);
}
static void sb_json_str(sbuf *b, const char *s) {
    sb_add(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') sb_add(b, "\\\"");
        else if (c == '\\') sb_add(b, "\\\\");
        else if (c == '\n') sb_add(b, "\\n");
        else if (c == '\r') sb_add(b, "\\r");
        else if (c == '\t') sb_add(b, "\\t");
        else if (c < 0x20) sb_addf(b, "\\u%04x", c);
        else { char t[2] = {(char)c, 0}; sb_add(b, t); }
    }
    sb_add(b, "\"");
}
static void sb_free(sbuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static void cpy(char *d, size_t n, const char *s) {
    size_t l = strlen(s);
    if (l >= n) l = n - 1;
    memmove(d, s, l);
    d[l] = 0;
}
#define CPY(dst, src) cpy((dst), sizeof(dst), (src))

/* ---------- state ---------- */
#define MAXACC 256
#define MAXSESS 1024
#define MAXCHG 1024
#define MAXREF 1024
#define MAXIDEM 1024
#define MAXITEM 8
#define MAXMETA 10
#define IDEM_RESP 8192
/* Stripe's own limits: an amount is at most 8 digits, a quantity at most 999999, so a session's total
 * (8 items) stays far inside int64. A time is bounded by the year 2100 so now + 86400 cannot overflow. */
#define AMOUNT_MAX INT64_C(99999999)
#define QTY_MAX INT64_C(999999)
#define TIME_MAX INT64_C(4102444800)

typedef struct {
    char id[40], country[3], currency[4], email[200];
    int charges_enabled, details_submitted;
    int64_t created;
} account_t;

typedef struct { char name[128], currency[4]; int64_t unit_amount, qty; } item_t;

enum { S_OPEN = 0, S_COMPLETE = 1, S_EXPIRED = 2 };

typedef struct {
    char id[64], pi[40], currency[4];
    char success_url[512], cancel_url[512], client_ref[200], email[200];
    char mk[MAXMETA][41], mv[MAXMETA][201];
    int acct, status, paid, nitems, nmeta, charge;
    item_t items[MAXITEM];
    int64_t amount_total, created, expires_at;
} session_t;

typedef struct {
    char id[40], pi[40], currency[4];
    int acct, sess;
    int64_t amount, refunded, created;
} charge_t;

typedef struct { char id[40]; int charge; int64_t amount, created; } refund_t;

typedef struct {
    uint8_t fp[32];
    char key[256];
    int status;
    int len;
    char resp[IDEM_RESP];
} idem_t;

static struct {
    uint64_t rng;
    int n_acc, n_sess, n_chg, n_ref, n_idem;
    account_t acc[MAXACC];
    session_t sess[MAXSESS];
    charge_t chg[MAXCHG];
    refund_t ref[MAXREF];
    idem_t idem[MAXIDEM];
} G;

static int64_t g_now;
static int g_now_set;
static char g_seed[128] = "fake_stripe";
static char g_api_key[128] = "sk_test_fake_cbase";
static char g_hook_url[512], g_hook_secret[256] = "whsec_fake_cbase";
static char g_base_url[256], g_data[512];
static int g_port;

static int64_t now_s(void) { return g_now_set ? g_now : (int64_t)time(NULL); }

/* ---------- seedable id generator: lib/rng.h's splitmix64, seeded from a name ----------
 * ONE SOURCE: G.rng is the only generator state, advanced only by rng_splitmix64 in new_id. The
 * --seed name reaches it through FNV-1a (lib/fnv.h) and rng_seed64, so a start from a name is a
 * draw and not a step (rng.h's note on sub-streams). Ids never read a clock. */
static uint64_t seed_state(const char *name) { return rng_seed64(fnv1a64_str(FNV64_OFFSET, name)); }
static void new_id(char *dst, size_t cap, const char *prefix) {
    static const char al[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    size_t n = strlen(prefix);
    if (n + 25 > cap) n = cap - 25;
    memcpy(dst, prefix, n);
    for (int i = 0; i < 24; i++) dst[n + (size_t)i] = al[rng_splitmix64(&G.rng) % 62];
    dst[n + 24] = 0;
}

/* ---------- persistence: counts then used prefixes of each array ---------- */
#define MAGIC_LEN 8
static const char MAGIC[MAGIC_LEN] = {'F', 'S', 'T', 'R', 'I', 'P', 'E', '1'};
static void save_state(void) {
    if (!g_data[0]) return;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_data);
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "fake_stripe: cannot write %s\n", tmp); return; }
    int c[5] = {G.n_acc, G.n_sess, G.n_chg, G.n_ref, G.n_idem};
    int ok = fwrite(MAGIC, 1, MAGIC_LEN, f) == MAGIC_LEN && fwrite(&G.rng, sizeof G.rng, 1, f) == 1 && fwrite(c, sizeof c, 1, f) == 1 &&
             fwrite(G.acc, sizeof G.acc[0], (size_t)G.n_acc, f) == (size_t)G.n_acc &&
             fwrite(G.sess, sizeof G.sess[0], (size_t)G.n_sess, f) == (size_t)G.n_sess &&
             fwrite(G.chg, sizeof G.chg[0], (size_t)G.n_chg, f) == (size_t)G.n_chg &&
             fwrite(G.ref, sizeof G.ref[0], (size_t)G.n_ref, f) == (size_t)G.n_ref &&
             fwrite(G.idem, sizeof G.idem[0], (size_t)G.n_idem, f) == (size_t)G.n_idem;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { fprintf(stderr, "fake_stripe: cannot write %s\n", tmp); remove(tmp); return; }
    if (rename(tmp, g_data) != 0) fprintf(stderr, "fake_stripe: cannot rename %s\n", tmp);
}
/* A data file is trusted for its format, not its bytes: every string is ended and every index and count
 * is in range before a handler can follow it (G.acc[s->acct], st[s->status], G.chg[rf->charge]). */
#define END(a) ((a)[sizeof(a) - 1] = 0)
static int sane_state(const int c[5]) {
    for (int i = 0; i < c[0]; i++) { account_t *a = &G.acc[i]; END(a->id); END(a->country); END(a->currency); END(a->email); }
    for (int i = 0; i < c[1]; i++) {
        session_t *s = &G.sess[i];
        END(s->id); END(s->pi); END(s->currency); END(s->success_url); END(s->cancel_url); END(s->client_ref); END(s->email);
        if (s->acct < 0 || s->acct >= c[0] || s->status < S_OPEN || s->status > S_EXPIRED) return 0;
        if (s->nitems < 0 || s->nitems > MAXITEM || s->nmeta < 0 || s->nmeta > MAXMETA || s->charge < -1 || s->charge >= c[2]) return 0;
        for (int k = 0; k < MAXITEM; k++) { END(s->items[k].name); END(s->items[k].currency); }
        for (int k = 0; k < MAXMETA; k++) { END(s->mk[k]); END(s->mv[k]); }
    }
    for (int i = 0; i < c[2]; i++) {
        charge_t *ch = &G.chg[i];
        END(ch->id); END(ch->pi); END(ch->currency);
        if (ch->acct < 0 || ch->acct >= c[0] || ch->sess < 0 || ch->sess >= c[1]) return 0;
    }
    for (int i = 0; i < c[3]; i++) {
        refund_t *rf = &G.ref[i];
        END(rf->id);
        if (rf->charge < 0 || rf->charge >= c[2]) return 0;
    }
    for (int i = 0; i < c[4]; i++) {
        idem_t *e = &G.idem[i];
        END(e->key); END(e->resp);
        if (e->len < 0 || e->len >= IDEM_RESP) return 0;
    }
    return 1;
}

static int load_state(void) {
    FILE *f = fopen(g_data, "rb");
    if (!f) return 0;
    char m[MAGIC_LEN];
    int c[5];
    uint64_t rng;
    int ok = fread(m, 1, MAGIC_LEN, f) == MAGIC_LEN && memcmp(m, MAGIC, MAGIC_LEN) == 0 && fread(&rng, sizeof rng, 1, f) == 1 &&
             fread(c, sizeof c, 1, f) == 1 && c[0] >= 0 && c[0] <= MAXACC && c[1] >= 0 && c[1] <= MAXSESS &&
             c[2] >= 0 && c[2] <= MAXCHG && c[3] >= 0 && c[3] <= MAXREF && c[4] >= 0 && c[4] <= MAXIDEM;
    if (ok) {
        ok = fread(G.acc, sizeof G.acc[0], (size_t)c[0], f) == (size_t)c[0] &&
             fread(G.sess, sizeof G.sess[0], (size_t)c[1], f) == (size_t)c[1] &&
             fread(G.chg, sizeof G.chg[0], (size_t)c[2], f) == (size_t)c[2] &&
             fread(G.ref, sizeof G.ref[0], (size_t)c[3], f) == (size_t)c[3] &&
             fread(G.idem, sizeof G.idem[0], (size_t)c[4], f) == (size_t)c[4];
    }
    fclose(f);
    if (ok) ok = sane_state(c);
    if (!ok) { fprintf(stderr, "fake_stripe: bad data file %s\n", g_data); return -1; }
    G.rng = rng;
    G.n_acc = c[0]; G.n_sess = c[1]; G.n_chg = c[2]; G.n_ref = c[3]; G.n_idem = c[4];
    return 1;
}

/* ---------- request, response, events ---------- */
typedef struct { char *k, *v; } kv;
#define MAXKV 128
typedef struct {
    char method[8], path[300], auth[300], stripe_account[80], idem_key[256];
    char *body; size_t body_len;   /* the raw body, untouched: the idempotency fingerprint reads it */
    char *formbuf;                 /* a copy the form is split and decoded in place in (parse_form) */
    kv form[MAXKV]; int nform;
} req_t;

typedef struct {
    int status;
    char ctype[48], location[600], extra[200];
    sbuf body;
} resp_t;

typedef struct { char type[48]; sbuf json; } event_t;
/* The events one request queued, delivered after its response. Grows as needed: one POST /test/now can
 * expire every open session at once, and an event lost to a fixed bound would be a silent test pass. */
static event_t *evq;
static size_t n_ev, cap_ev;

static const char *form_get(const req_t *r, const char *key) {
    for (int i = 0; i < r->nform; i++) if (strcmp(r->form[i].k, key) == 0) return r->form[i].v;
    return NULL;
}
static int form_has_prefix(const req_t *r, const char *pre) {
    size_t n = strlen(pre);
    for (int i = 0; i < r->nform; i++) if (strncmp(r->form[i].k, pre, n) == 0) return 1;
    return 0;
}
static int hexv(int c) { return isdigit(c) ? c - '0' : (c | 32) - 'a' + 10; }
static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            *o++ = (char)(hexv(s[1]) * 16 + hexv(s[2]));
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}
static void parse_form(req_t *r) {
    r->nform = 0;
    r->formbuf = malloc(r->body_len + 1);
    if (!r->formbuf) { perror("malloc"); exit(1); }
    memcpy(r->formbuf, r->body, r->body_len);
    r->formbuf[r->body_len] = 0;
    char *p = r->formbuf;
    while (p && *p && r->nform < MAXKV) {
        char *amp = strchr(p, '&');
        if (amp) *amp = 0;
        char *eq = strchr(p, '=');
        if (eq) *eq++ = 0;
        url_decode(p);
        if (eq) url_decode(eq);
        r->form[r->nform].k = p;
        r->form[r->nform].v = eq ? eq : p + strlen(p);
        r->nform++;
        p = amp ? amp + 1 : NULL;
    }
}

static void err(resp_t *r, int status, const char *type, const char *code, const char *param, const char *fmt, ...) {
    char msg[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    r->status = status;
    sb_add(&r->body, "{\"error\":{");
    if (code) { sb_addf(&r->body, "\"code\":\"%s\",", code); }
    sb_add(&r->body, "\"message\":");
    sb_json_str(&r->body, msg);
    if (param) { sb_addf(&r->body, ",\"param\":\"%s\"", param); }
    sb_addf(&r->body, ",\"type\":\"%s\"}}", type);
}
static void err_missing(resp_t *r, const char *param) {
    err(r, 400, "invalid_request_error", "parameter_missing", param, "Missing required param: %s.", param);
}
static void err_nosuch(resp_t *r, const char *what, const char *id, const char *param) {
    err(r, 400 + 4, "invalid_request_error", "resource_missing", param, "No such %s: '%s'", what, id);
}

/* ---------- json builders ---------- */
static void j_str_or_null(sbuf *b, const char *s) { if (s && s[0]) sb_json_str(b, s); else sb_add(b, "null"); }

static void j_account(sbuf *b, const account_t *a) {
    sb_addf(b, "{\"id\":\"%s\",\"object\":\"account\",\"type\":\"express\",\"country\":\"%s\",\"email\":", a->id, a->country);
    j_str_or_null(b, a->email);
    sb_addf(b, ",\"default_currency\":\"%s\",\"charges_enabled\":%s,\"payouts_enabled\":%s,\"details_submitted\":%s,"
               "\"capabilities\":{\"card_payments\":\"%s\",\"transfers\":\"%s\"},\"created\":%lld,\"livemode\":false}",
            a->currency, a->charges_enabled ? "true" : "false", a->charges_enabled ? "true" : "false",
            a->details_submitted ? "true" : "false", a->charges_enabled ? "active" : "inactive",
            a->charges_enabled ? "active" : "inactive", (long long)a->created);
}
static void j_session(sbuf *b, const session_t *s) {
    static const char *st[] = {"open", "complete", "expired"};
    sb_addf(b, "{\"id\":\"%s\",\"object\":\"checkout.session\",\"amount_subtotal\":%lld,\"amount_total\":%lld,"
               "\"currency\":\"%s\",\"mode\":\"payment\",\"status\":\"%s\",\"payment_status\":\"%s\",\"url\":",
            s->id, (long long)s->amount_total, (long long)s->amount_total, s->currency, st[s->status],
            s->paid ? "paid" : "unpaid");
    if (s->status == S_OPEN) sb_addf(b, "\"%s/c/pay/%s\"", g_base_url, s->id); else sb_add(b, "null");
    sb_add(b, ",\"success_url\":");
    sb_json_str(b, s->success_url);
    sb_add(b, ",\"cancel_url\":");
    sb_json_str(b, s->cancel_url);
    sb_add(b, ",\"client_reference_id\":");
    j_str_or_null(b, s->client_ref);
    sb_add(b, ",\"customer_email\":");
    j_str_or_null(b, s->email);
    sb_add(b, ",\"metadata\":{");
    for (int i = 0; i < s->nmeta; i++) {
        if (i) sb_add(b, ",");
        sb_json_str(b, s->mk[i]);
        sb_add(b, ":");
        sb_json_str(b, s->mv[i]);
    }
    sb_add(b, "},\"payment_intent\":");
    j_str_or_null(b, s->pi);
    sb_addf(b, ",\"application_fee_amount\":null,\"account\":\"%s\",\"created\":%lld,\"expires_at\":%lld,\"livemode\":false}",
            G.acc[s->acct].id, (long long)s->created, (long long)s->expires_at);
}
static void j_charge(sbuf *b, const charge_t *c) {
    sb_addf(b, "{\"id\":\"%s\",\"object\":\"charge\",\"amount\":%lld,\"amount_captured\":%lld,\"amount_refunded\":%lld,"
               "\"currency\":\"%s\",\"paid\":true,\"refunded\":%s,\"status\":\"succeeded\",\"payment_intent\":\"%s\","
               "\"application_fee_amount\":null,\"created\":%lld,\"livemode\":false}",
            c->id, (long long)c->amount, (long long)c->amount, (long long)c->refunded, c->currency,
            c->refunded == c->amount ? "true" : "false", c->pi, (long long)c->created);
}
static void j_refund(sbuf *b, const refund_t *rf) {
    const charge_t *c = &G.chg[rf->charge];
    sb_addf(b, "{\"id\":\"%s\",\"object\":\"refund\",\"amount\":%lld,\"currency\":\"%s\",\"charge\":\"%s\","
               "\"payment_intent\":\"%s\",\"status\":\"succeeded\",\"reason\":null,\"created\":%lld}",
            rf->id, (long long)rf->amount, c->currency, c->id, c->pi, (long long)rf->created);
}

/* ---------- events ---------- */
static void queue_event(const char *type, int acct, const sbuf *object) {
    if (n_ev == cap_ev) {
        cap_ev = cap_ev ? cap_ev * 2 : 16;
        evq = realloc(evq, cap_ev * sizeof *evq);
        if (!evq) { perror("realloc"); exit(1); }
    }
    event_t *e = &evq[n_ev++];
    memset(e, 0, sizeof *e);
    CPY(e->type, type);
    char id[40];
    new_id(id, sizeof id, "evt_");
    sb_addf(&e->json, "{\"id\":\"%s\",\"object\":\"event\",\"api_version\":\"2025-01-27.acacia\",\"created\":%lld,"
                      "\"data\":{\"object\":", id, (long long)now_s());
    sb_add(&e->json, object->p);
    sb_addf(&e->json, "},\"livemode\":false,\"pending_webhooks\":1,\"request\":{\"id\":null,\"idempotency_key\":null},"
                      "\"type\":\"%s\",\"account\":\"%s\"}", type, G.acc[acct].id);
}

static void hex_hmac(const char *secret, const char *data, size_t n, char out[65]) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int ml = 0;
    HMAC(EVP_sha256(), secret, (int)strlen(secret), (const unsigned char *)data, n, md, &ml);
    for (unsigned i = 0; i < ml; i++) sprintf(out + 2 * i, "%02x", md[i]);
    out[2 * ml] = 0;
}

/* http://HOST[:PORT][/PATH] -> host, port (default 80), path (default "/"). */
static int parse_hook_url(const char *u, char *host, size_t hcap, int *port, char *path, size_t pcap) {
    if (strncmp(u, "http://", 7) != 0) return 0;
    const char *h = u + 7;
    size_t hl = strcspn(h, ":/");
    if (hl == 0 || hl >= hcap) return 0;
    memcpy(host, h, hl);
    host[hl] = 0;
    const char *q = h + hl;
    *port = 80;
    if (*q == ':') {
        q++;
        size_t pl = strcspn(q, "/");
        int64_t v;
        if (!num_parse_i64(q, pl, 1, 65535, &v)) return 0;
        *port = (int)v;
        q += pl;
    }
    if (!*q) q = "/";
    if (strlen(q) >= pcap) return 0;
    memcpy(path, q, strlen(q) + 1);
    return 1;
}

static void deliver(const event_t *e) {
    if (!g_hook_url[0]) return;
    char host[128], path[400];
    int port;
    if (!parse_hook_url(g_hook_url, host, sizeof host, &port, path, sizeof path)) { fprintf(stderr, "fake_stripe: bad webhook url\n"); return; }
    if (strcmp(host, "localhost") == 0) CPY(host, "127.0.0.1");
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { fprintf(stderr, "fake_stripe: webhook host must be an IPv4 address\n"); return; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fprintf(stderr, "fake_stripe: webhook %s not delivered: %s\n", e->type, strerror(errno)); return; }
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "fake_stripe: webhook %s not delivered: %s\n", e->type, strerror(errno));
        close(fd);
        return;
    }
    /* the signed payload is "<t>.<raw body>", exactly as Stripe signs it */
    int64_t t = now_s();
    sbuf signed_payload = {0};
    sb_addf(&signed_payload, "%lld.", (long long)t);
    sb_add(&signed_payload, e->json.p);
    char sig[65];
    hex_hmac(g_hook_secret, signed_payload.p, signed_payload.n, sig);
    sb_free(&signed_payload);
    sbuf out = {0};
    sb_addf(&out, "POST %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: Stripe/1.0 (+https://stripe.com/docs/webhooks)\r\n"
                  "Content-Type: application/json; charset=utf-8\r\nStripe-Signature: t=%lld,v1=%s\r\n"
                  "Content-Length: %zu\r\nConnection: close\r\n\r\n", path, host, port, (long long)t, sig, e->json.n);
    sb_add(&out, e->json.p);
    size_t off = 0;
    while (off < out.n) {
        ssize_t w = send(fd, out.p + off, out.n - off, MSG_NOSIGNAL);
        if (w <= 0) break;
        off += (size_t)w;
    }
    char buf[256];
    ssize_t rl = recv(fd, buf, sizeof buf - 1, 0);
    if (rl > 0) { buf[rl] = 0; char *nl = strchr(buf, '\r'); if (nl) *nl = 0; fprintf(stderr, "fake_stripe: webhook %s -> %s\n", e->type, buf); }
    sb_free(&out);
    close(fd);
}
static void flush_events(void) {
    for (size_t i = 0; i < n_ev; i++) { deliver(&evq[i]); sb_free(&evq[i].json); }
    n_ev = 0;
}

/* ---------- lookups ---------- */
static int find_acct(const char *id) {
    for (int i = 0; i < G.n_acc; i++) if (strcmp(G.acc[i].id, id) == 0) return i;
    return -1;
}
static int find_sess(const char *id) {
    for (int i = 0; i < G.n_sess; i++) if (strcmp(G.sess[i].id, id) == 0) return i;
    return -1;
}
static const char *currency_for(const char *country) {
    static const char *tab[][2] = {{"US", "usd"}, {"NO", "nok"}, {"SG", "sgd"}, {"GB", "gbp"}, {"CA", "cad"},
                                   {"CH", "chf"}, {"AU", "aud"}, {"SE", "sek"}, {"DK", "dkk"}, {"JP", "jpy"}};
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) if (strcmp(tab[i][0], country) == 0) return tab[i][1];
    return "eur";
}

/* resolves the Stripe-Account header to an account index, or writes the error */
static int need_connected(const req_t *rq, resp_t *r) {
    if (!rq->stripe_account[0]) {
        err(r, 400, "invalid_request_error", NULL, NULL,
            "This fake only imitates direct charges: send a Stripe-Account header naming a connected account.");
        return -1;
    }
    int a = find_acct(rq->stripe_account);
    if (a < 0) {
        err(r, 403, "invalid_request_error", "account_invalid", NULL,
            "The provided key '%.12s****' does not have access to account '%s' (or that account does not exist). "
            "Application access may have been revoked.", g_api_key, rq->stripe_account);
        return -1;
    }
    return a;
}

/* ---------- handlers ---------- */
static void h_account_create(const req_t *rq, resp_t *r) {
    const char *type = form_get(rq, "type"), *country = form_get(rq, "country"), *email = form_get(rq, "email");
    if (type && strcmp(type, "express") != 0) {
        err(r, 400, "invalid_request_error", "parameter_invalid_enum", "type", "Invalid type: this fake supports only 'express'.");
        return;
    }
    if (!country || !*country) country = "US";
    if (strlen(country) != 2 || !isupper((unsigned char)country[0]) || !isupper((unsigned char)country[1])) {
        err(r, 400, "invalid_request_error", "parameter_invalid_enum", "country", "Invalid country: %s", country);
        return;
    }
    if (G.n_acc >= MAXACC) { err(r, 400, "invalid_request_error", NULL, NULL, "The fake is full (accounts)."); return; }
    account_t *a = &G.acc[G.n_acc++];
    memset(a, 0, sizeof *a);
    new_id(a->id, sizeof a->id, "acct_");
    CPY(a->country, country);
    CPY(a->currency, currency_for(country));
    if (email) CPY(a->email, email);
    a->created = now_s();
    r->status = 200;
    j_account(&r->body, a);
}
static void h_account_get(const char *id, resp_t *r) {
    int a = find_acct(id);
    if (a < 0) { err_nosuch(r, "account", id, NULL); r->status = 404; return; }
    r->status = 200;
    j_account(&r->body, &G.acc[a]);
}
static void h_account_link(const req_t *rq, resp_t *r) {
    const char *acct = form_get(rq, "account"), *type = form_get(rq, "type");
    if (!acct || !*acct) { err_missing(r, "account"); return; }
    if (!type || !*type) { err_missing(r, "type"); return; }
    if (strcmp(type, "account_onboarding") != 0 && strcmp(type, "account_update") != 0) {
        err(r, 400, "invalid_request_error", "parameter_invalid_enum", "type", "Invalid type: %s", type);
        return;
    }
    if (strcmp(type, "account_onboarding") == 0) {
        if (!form_get(rq, "refresh_url")) { err_missing(r, "refresh_url"); return; }
        if (!form_get(rq, "return_url")) { err_missing(r, "return_url"); return; }
    }
    if (find_acct(acct) < 0) { err_nosuch(r, "account", acct, "account"); r->status = 404; return; }
    r->status = 200;
    sb_addf(&r->body, "{\"object\":\"account_link\",\"created\":%lld,\"expires_at\":%lld,\"url\":\"%s/onboard/%s\"}",
            (long long)now_s(), (long long)now_s() + 300, g_base_url, acct);
}

/* A decimal integer from a form value, in lo..hi (lib/num_parse.h: no blank, no '+', no tail, no wrap). */
static int parse_int(const char *s, int64_t lo, int64_t hi, int64_t *out) {
    return num_parse_i64_cstr(s, lo, hi, out);
}

/* A URL that is echoed into a Location header must carry no control character, space or DEL: %0D%0A
 * in a success_url would otherwise split the 303 response. Stripe refuses such a URL as invalid too. */
static int url_ok(const char *u) {
    for (; *u; u++) if ((unsigned char)*u <= 0x20 || (unsigned char)*u == 0x7f) return 0;
    return 1;
}

static void h_session_create(const req_t *rq, resp_t *r) {
    int a = need_connected(rq, r);
    if (a < 0) return;
    const char *mode = form_get(rq, "mode"), *succ = form_get(rq, "success_url"), *canc = form_get(rq, "cancel_url");
    if (!mode || !*mode) { err_missing(r, "mode"); return; }
    if (strcmp(mode, "payment") != 0) {
        err(r, 400, "invalid_request_error", "parameter_invalid_enum", "mode", "Invalid mode: this fake supports only 'payment'.");
        return;
    }
    if (!succ || !*succ) { err_missing(r, "success_url"); return; }
    if (!url_ok(succ)) { err(r, 400, "invalid_request_error", "url_invalid", "success_url", "Invalid URL: success_url."); return; }
    if (canc && !url_ok(canc)) { err(r, 400, "invalid_request_error", "url_invalid", "cancel_url", "Invalid URL: cancel_url."); return; }
    if (!form_has_prefix(rq, "line_items[0]")) { err_missing(r, "line_items"); return; }
    const char *fee = form_get(rq, "payment_intent_data[application_fee_amount]");
    if (fee && strcmp(fee, "0") != 0) {
        err(r, 400, "invalid_request_error", "parameter_invalid_integer", "payment_intent_data[application_fee_amount]",
            "This fake takes no application fee; it refuses a nonzero application_fee_amount.");
        return;
    }
    if (!G.acc[a].charges_enabled) {
        err(r, 400, "invalid_request_error", NULL, NULL,
            "Your destination account needs to have at least one of the following capabilities enabled: card_payments (onboarding is not complete).");
        return;
    }
    if (G.n_sess >= MAXSESS) { err(r, 400, "invalid_request_error", NULL, NULL, "The fake is full (sessions)."); return; }
    session_t *s = &G.sess[G.n_sess];
    memset(s, 0, sizeof *s);
    s->acct = a;
    s->charge = -1;
    /* line items */
    for (int i = 0; i < MAXITEM; i++) {
        char pre[48], k[160];
        snprintf(pre, sizeof pre, "line_items[%d]", i);
        if (!form_has_prefix(rq, pre)) break;
        item_t *it = &s->items[s->nitems];
        snprintf(k, sizeof k, "line_items[%d][price]", i);
        if (form_get(rq, k)) {
            err(r, 400, "invalid_request_error", "parameter_invalid_empty", k, "This fake takes inline price_data only, not price ids.");
            return;
        }
        snprintf(k, sizeof k, "line_items[%d][price_data][currency]", i);
        const char *cur = form_get(rq, k);
        if (!cur || !*cur) { err_missing(r, k); return; }
        snprintf(k, sizeof k, "line_items[%d][price_data][product_data][name]", i);
        const char *name = form_get(rq, k);
        if (!name || !*name) { err_missing(r, k); return; }
        snprintf(k, sizeof k, "line_items[%d][price_data][unit_amount]", i);
        const char *ua = form_get(rq, k);
        if (!ua || !*ua) { err_missing(r, k); return; }
        if (!parse_int(ua, 0, AMOUNT_MAX, &it->unit_amount)) {
            err(r, 400, "invalid_request_error", "parameter_invalid_integer", k, "Invalid integer: %s", ua);
            return;
        }
        snprintf(k, sizeof k, "line_items[%d][quantity]", i);
        const char *q = form_get(rq, k);
        if (!q || !*q) { err_missing(r, k); return; }
        if (!parse_int(q, 1, QTY_MAX, &it->qty)) {
            err(r, 400, "invalid_request_error", "parameter_invalid_integer", k, "Invalid integer: %s", q);
            return;
        }
        if (strlen(cur) != 3) { err(r, 400, "invalid_request_error", "parameter_invalid_enum", "currency", "Invalid currency: %s", cur); return; }
        for (int c = 0; c < 3; c++) it->currency[c] = (char)tolower((unsigned char)cur[c]);
        if (s->nitems > 0 && strcmp(it->currency, s->items[0].currency) != 0) {
            err(r, 400, "invalid_request_error", NULL, NULL, "You cannot combine currencies on a single Checkout Session.");
            return;
        }
        CPY(it->name, name);
        s->amount_total += it->unit_amount * it->qty;
        s->nitems++;
    }
    CPY(s->currency, s->items[0].currency);
    CPY(s->success_url, succ);
    if (canc) CPY(s->cancel_url, canc);
    const char *cr = form_get(rq, "client_reference_id"), *em = form_get(rq, "customer_email");
    if (cr) CPY(s->client_ref, cr);
    if (em) CPY(s->email, em);
    for (int i = 0; i < rq->nform; i++) {
        const char *k = rq->form[i].k;
        if (strncmp(k, "metadata[", 9) == 0 && k[strlen(k) - 1] == ']' && s->nmeta < MAXMETA) {
            char key[41];
            snprintf(key, sizeof key, "%.*s", (int)strlen(k) - 10 > 40 ? 40 : (int)strlen(k) - 10, k + 9);
            CPY(s->mk[s->nmeta], key);
            CPY(s->mv[s->nmeta], rq->form[i].v);
            s->nmeta++;
        }
    }
    int64_t now = now_s();
    s->created = now;
    s->expires_at = now + 86400;
    const char *ea = form_get(rq, "expires_at");
    if (ea) {
        int64_t e;
        if (!parse_int(ea, 0, TIME_MAX, &e)) { err(r, 400, "invalid_request_error", "parameter_invalid_integer", "expires_at", "Invalid integer: %s", ea); return; }
        if (e < now + 1800) { err(r, 400, "invalid_request_error", NULL, "expires_at", "The `expires_at` timestamp must be at least 30 minutes after the Checkout Session is created."); return; }
        if (e > now + 86400) { err(r, 400, "invalid_request_error", NULL, "expires_at", "The `expires_at` timestamp must be less than 24 hours after the Checkout Session is created."); return; }
        s->expires_at = e;
    }
    new_id(s->id, sizeof s->id, "cs_test_");
    G.n_sess++;
    r->status = 200;
    j_session(&r->body, s);
}

static int sess_for_request(const req_t *rq, resp_t *r, const char *id) {
    int a = need_connected(rq, r);
    if (a < 0) return -1;
    int s = find_sess(id);
    if (s < 0 || G.sess[s].acct != a) {
        err_nosuch(r, "checkout.session", id, "session");
        r->status = 404;
        return -1;
    }
    return s;
}
static void h_session_get(const req_t *rq, const char *id, resp_t *r) {
    int s = sess_for_request(rq, r, id);
    if (s < 0) return;
    r->status = 200;
    j_session(&r->body, &G.sess[s]);
}
static void expire_session(int s) {
    G.sess[s].status = S_EXPIRED;
    sbuf o = {0};
    j_session(&o, &G.sess[s]);
    queue_event("checkout.session.expired", G.sess[s].acct, &o);
    sb_free(&o);
}
static void h_session_expire(const req_t *rq, const char *id, resp_t *r) {
    int s = sess_for_request(rq, r, id);
    if (s < 0) return;
    if (G.sess[s].status != S_OPEN) {
        err(r, 400, "invalid_request_error", NULL, NULL, "Only Checkout Sessions with a status in `open` can be expired.");
        return;
    }
    expire_session(s);
    r->status = 200;
    j_session(&r->body, &G.sess[s]);
}

static int pay_session(int s) {
    session_t *ss = &G.sess[s];
    if (G.n_chg >= MAXCHG) return 0;
    ss->status = S_COMPLETE;
    ss->paid = 1;
    new_id(ss->pi, sizeof ss->pi, "pi_");
    charge_t *c = &G.chg[G.n_chg];
    memset(c, 0, sizeof *c);
    new_id(c->id, sizeof c->id, "ch_");
    CPY(c->pi, ss->pi);
    CPY(c->currency, ss->currency);
    c->acct = ss->acct;
    c->sess = s;
    c->amount = ss->amount_total;
    c->created = now_s();
    ss->charge = G.n_chg++;
    sbuf o = {0};
    j_session(&o, ss);
    queue_event("checkout.session.completed", ss->acct, &o);
    sb_free(&o);
    return 1;
}

static void h_refund(const req_t *rq, resp_t *r) {
    int a = need_connected(rq, r);
    if (a < 0) return;
    const char *chg = form_get(rq, "charge"), *pi = form_get(rq, "payment_intent"), *am = form_get(rq, "amount");
    if ((!chg || !*chg) && (!pi || !*pi)) { err_missing(r, "charge"); return; }
    int ci = -1;
    for (int i = 0; i < G.n_chg; i++) {
        if (G.chg[i].acct != a) continue;
        if ((chg && *chg && strcmp(G.chg[i].id, chg) == 0) || (pi && *pi && strcmp(G.chg[i].pi, pi) == 0)) { ci = i; break; }
    }
    if (ci < 0) {
        if (chg && *chg) err_nosuch(r, "charge", chg, "charge"); else err_nosuch(r, "payment_intent", pi, "payment_intent");
        r->status = 404;
        return;
    }
    charge_t *c = &G.chg[ci];
    int64_t amount = c->amount - c->refunded;
    if (amount == 0) {
        err(r, 400, "invalid_request_error", "charge_already_refunded", "charge", "Charge %s has already been refunded.", c->id);
        return;
    }
    if (am) {
        if (!parse_int(am, 1, INT64_MAX, &amount)) {
            err(r, 400, "invalid_request_error", "parameter_invalid_integer", "amount", "Invalid positive integer");
            return;
        }
    }
    if (amount > c->amount - c->refunded) {
        err(r, 400, "invalid_request_error", NULL, "amount", "Refund amount (%lld) is greater than unrefunded amount on charge (%lld)",
            (long long)amount, (long long)(c->amount - c->refunded));
        return;
    }
    if (G.n_ref >= MAXREF) { err(r, 400, "invalid_request_error", NULL, NULL, "The fake is full (refunds)."); return; }
    refund_t *rf = &G.ref[G.n_ref++];
    memset(rf, 0, sizeof *rf);
    new_id(rf->id, sizeof rf->id, "re_");
    rf->charge = ci;
    rf->amount = amount;
    rf->created = now_s();
    c->refunded += amount;
    sbuf o = {0};
    j_charge(&o, c);
    queue_event("charge.refunded", a, &o);
    sb_free(&o);
    r->status = 200;
    j_refund(&r->body, rf);
}

/* ---------- test-only control routes ---------- */
static void h_test_now(const req_t *rq, resp_t *r) {
    int64_t t;
    if (!parse_int(form_get(rq, "t"), 0, TIME_MAX, &t)) { err_missing(r, "t"); return; }
    g_now = t;
    g_now_set = 1;
    int n = 0;
    for (int i = 0; i < G.n_sess; i++)
        if (G.sess[i].status == S_OPEN && G.sess[i].expires_at <= g_now) { expire_session(i); n++; }
    r->status = 200;
    sb_addf(&r->body, "{\"now\":%lld,\"expired\":%d}", (long long)g_now, n);
}
static void h_test_onboard(const char *id, resp_t *r) {
    int a = find_acct(id);
    if (a < 0) { err_nosuch(r, "account", id, NULL); r->status = 404; return; }
    account_t *ac = &G.acc[a];
    int changed = !ac->charges_enabled || !ac->details_submitted;
    ac->charges_enabled = 1;
    ac->details_submitted = 1;
    if (changed) {
        sbuf o = {0};
        j_account(&o, ac);
        queue_event("account.updated", a, &o);
        sb_free(&o);
    }
    r->status = 200;
    j_account(&r->body, ac);
}
static int pay_checked(int s, resp_t *r) {
    if (G.sess[s].status != S_OPEN) {
        err(r, 400, "invalid_request_error", NULL, NULL, "This Checkout Session is not open (status %s).",
            G.sess[s].status == S_COMPLETE ? "complete" : "expired");
        return 0;
    }
    if (!pay_session(s)) { err(r, 400, "invalid_request_error", NULL, NULL, "The fake is full (charges)."); return 0; }
    return 1;
}
static void h_test_pay(const char *id, resp_t *r) {
    int s = find_sess(id);
    if (s < 0) { err_nosuch(r, "checkout.session", id, "session"); r->status = 404; return; }
    if (!pay_checked(s, r)) return;
    r->status = 200;
    j_session(&r->body, &G.sess[s]);
}

/* ---------- minimal local pages ---------- */
static void page(resp_t *r, const char *title, const char *inner) {
    r->status = 200;
    CPY(r->ctype, "text/html; charset=utf-8");
    sb_addf(&r->body, "<!doctype html><html><head><meta charset=utf-8><title>%s</title></head><body><h1>%s</h1>%s</body></html>",
            title, title, inner);
}
static void h_page_pay(const char *id, resp_t *r) {
    int s = find_sess(id);
    if (s < 0) { err_nosuch(r, "checkout.session", id, "session"); r->status = 404; return; }
    session_t *ss = &G.sess[s];
    if (ss->status != S_OPEN) { page(r, "Session closed", "<p>This Checkout Session is no longer open.</p>"); return; }
    char inner[1500];
    snprintf(inner, sizeof inner,
             "<p>Total: %lld %s</p><form method=post action=\"/c/pay/%s/pay\"><button>Pay</button></form>"
             "<form method=post action=\"/c/pay/%s/cancel\"><button>Cancel</button></form>",
             (long long)ss->amount_total, ss->currency, id, id);
    page(r, "Fake Stripe Checkout", inner);
}
static void redirect(resp_t *r, const char *url) {
    r->status = 303;
    CPY(r->location, url);
}
static void h_page_action(const char *id, int pay, resp_t *r) {
    int s = find_sess(id);
    if (s < 0) { err_nosuch(r, "checkout.session", id, "session"); r->status = 404; return; }
    if (pay) {
        if (!pay_checked(s, r)) return;
        redirect(r, G.sess[s].success_url);
    } else {
        redirect(r, G.sess[s].cancel_url[0] ? G.sess[s].cancel_url : G.sess[s].success_url);
    }
}

/* ---------- dispatcher ---------- */
/* Match PREFIX<id>SUFFIX exactly (id non-empty, no '/'); suffix may be "". sscanf cannot do this: it
 * reports success even when a trailing literal fails to match. */
static int route_id(const char *p, const char *pre, const char *suf, char *id, size_t cap) {
    size_t pl = strlen(pre), sl = strlen(suf), n = strlen(p);
    if (n < pl + sl + 1 || strncmp(p, pre, pl) != 0 || strcmp(p + n - sl, suf) != 0) return 0;
    size_t il = n - pl - sl;
    if (il >= cap || memchr(p + pl, '/', il)) return 0;
    memcpy(id, p + pl, il);
    id[il] = 0;
    return 1;
}

static int mutating;

static void route(req_t *rq, resp_t *r) {
    const char *p = rq->path;
    char id[96];
    int is_post = strcmp(rq->method, "POST") == 0, is_get = strcmp(rq->method, "GET") == 0;

    /* test-only and page routes: no API key */
    if (strcmp(p, "/test/now") == 0 && is_post) { mutating = 1; h_test_now(rq, r); return; }
    if (route_id(p, "/test/accounts/", "/complete_onboarding", id, sizeof id) && is_post) { mutating = 1; h_test_onboard(id, r); return; }
    if (route_id(p, "/test/sessions/", "/pay", id, sizeof id) && is_post) { mutating = 1; h_test_pay(id, r); return; }
    if (route_id(p, "/onboard/", "", id, sizeof id) && is_get) {
        page(r, "Fake Stripe onboarding", "<p>No real onboarding happens here; a test calls complete_onboarding.</p>");
        return;
    }
    if (route_id(p, "/c/pay/", "/pay", id, sizeof id) && is_post) { mutating = 1; h_page_action(id, 1, r); return; }
    if (route_id(p, "/c/pay/", "/cancel", id, sizeof id) && is_post) { h_page_action(id, 0, r); return; }
    if (route_id(p, "/c/pay/", "", id, sizeof id) && is_get) { h_page_pay(id, r); return; }

    if (strncmp(p, "/v1/", 4) != 0) { err(r, 404, "invalid_request_error", NULL, NULL, "Unrecognized request URL (%s %s).", rq->method, p); return; }

    /* API key: Authorization: Bearer <key> */
    if (!rq->auth[0]) {
        err(r, 401, "invalid_request_error", NULL, NULL,
            "You did not provide an API key. You need to provide your API key in the Authorization header, using Bearer auth (e.g. 'Authorization: Bearer YOUR_SECRET_KEY').");
        return;
    }
    if (strncmp(rq->auth, "Bearer ", 7) != 0 || strcmp(rq->auth + 7, g_api_key) != 0) {
        const char *k = strncmp(rq->auth, "Bearer ", 7) == 0 ? rq->auth + 7 : rq->auth;
        size_t kl = strlen(k);
        err(r, 401, "invalid_request_error", NULL, NULL, "Invalid API Key provided: %.8s%s%s", k, kl > 12 ? "********" : "", kl > 12 ? k + kl - 4 : "");
        return;
    }

    /* Idempotency-Key on POST: same key + same request replays; same key + different request is a 400 */
    int use_idem = is_post && rq->idem_key[0];
    uint8_t fp[32];
    if (use_idem) {
        sbuf mat = {0};
        sb_addf(&mat, "%s\n%s\n", rq->path, rq->stripe_account);
        sb_reserve(&mat, rq->body_len);
        memcpy(mat.p + mat.n, rq->body, rq->body_len);
        mat.n += rq->body_len;
        unsigned fl = 0;
        EVP_Digest(mat.p, mat.n, fp, &fl, EVP_sha256(), NULL);
        sb_free(&mat);
        for (int i = 0; i < G.n_idem; i++) {
            if (strcmp(G.idem[i].key, rq->idem_key) != 0) continue;
            if (memcmp(G.idem[i].fp, fp, 32) != 0) {
                err(r, 400, "idempotency_error", NULL, NULL,
                    "Keys for idempotent requests can only be used with the same parameters they were first used with. "
                    "Try using a key other than '%s' if you meant to execute a different request.", rq->idem_key);
                return;
            }
            r->status = G.idem[i].status;
            sb_add(&r->body, G.idem[i].resp);
            CPY(r->extra, "Idempotent-Replayed: true\r\n");
            return;
        }
    }

    if (is_post && strcmp(p, "/v1/accounts") == 0) { mutating = 1; h_account_create(rq, r); }
    else if (is_get && route_id(p, "/v1/accounts/", "", id, sizeof id)) h_account_get(id, r);
    else if (is_post && strcmp(p, "/v1/account_links") == 0) { mutating = 1; h_account_link(rq, r); }
    else if (is_post && strcmp(p, "/v1/checkout/sessions") == 0) { mutating = 1; h_session_create(rq, r); }
    else if (is_post && route_id(p, "/v1/checkout/sessions/", "/expire", id, sizeof id)) { mutating = 1; h_session_expire(rq, id, r); }
    else if (is_get && route_id(p, "/v1/checkout/sessions/", "", id, sizeof id)) h_session_get(rq, id, r);
    else if (is_post && strcmp(p, "/v1/refunds") == 0) { mutating = 1; h_refund(rq, r); }
    else { err(r, 404, "invalid_request_error", NULL, NULL, "Unrecognized request URL (%s %s). This fake imitates only the calls its README lists.", rq->method, p); return; }

    if (use_idem && r->body.n < IDEM_RESP && G.n_idem < MAXIDEM) {
        idem_t *e = &G.idem[G.n_idem++];
        memcpy(e->fp, fp, 32);
        CPY(e->key, rq->idem_key);
        e->status = r->status;
        e->len = (int)r->body.n;
        memcpy(e->resp, r->body.p, r->body.n + 1);
        mutating = 1;
    }
}

/* ---------- http ---------- */
#define REQ_MAX (1 << 20)
static char reqbuf[REQ_MAX + 8192];

static void send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}
static const char *reason(int s) {
    switch (s) { case 200: return "OK"; case 303: return "See Other"; case 400: return "Bad Request"; case 401: return "Unauthorized";
                 case 403: return "Forbidden"; case 500: return "Internal Server Error"; case 404: return "Not Found"; case 413: return "Payload Too Large"; default: return "Error"; }
}
static void write_resp(int fd, resp_t *r) {
    if (r->status == 0) r->status = 500;   /* a handler that answered nothing is a bug, never a status 0 */
    sbuf o = {0};
    sb_addf(&o, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n", r->status, reason(r->status),
            r->ctype[0] ? r->ctype : "application/json", r->body.n);
    if (r->location[0]) sb_addf(&o, "Location: %s\r\n", r->location);
    if (r->extra[0]) sb_add(&o, r->extra);
    sb_add(&o, "\r\n");
    if (r->body.n) sb_add(&o, r->body.p);
    send_all(fd, o.p, o.n);
    sb_free(&o);
}

/* Why read_request answered -400, for the error message. */
static const char *req_problem = "bad request";

/* 0: rq is filled. -1: nothing to answer (a dead or half connection). -413: too large. -400: a header
 * that cannot be trusted (req_problem says which): a Content-Length that is not a plain decimal
 * number, two of them that differ, or one that does not fit in reqbuf behind the headers. */
static int read_request(int fd, req_t *rq) {
    size_t have = 0;
    char *hend = NULL;
    while (!hend) {
        if (have >= sizeof reqbuf - 1) return -413;
        ssize_t n = recv(fd, reqbuf + have, sizeof reqbuf - 1 - have, 0);
        if (n <= 0) return -1;
        have += (size_t)n;
        reqbuf[have] = 0;
        hend = strstr(reqbuf, "\r\n\r\n");
    }
    *hend = 0;
    size_t hlen = (size_t)(hend - reqbuf) + 4;
    memset(rq, 0, sizeof *rq);
    char *line = reqbuf;
    char *eol = strstr(line, "\r\n");
    if (eol) *eol = 0;
    char target[400] = "";
    if (sscanf(line, "%7s %399s", rq->method, target) != 2) return -1;
    char *q = strchr(target, '?');
    if (q) *q = 0;
    CPY(rq->path, target);
    int64_t clen = 0;
    int have_cl = 0;
    while (eol) {
        line = eol + 2;
        if (!*line) break;
        eol = strstr(line, "\r\n");
        if (eol) *eol = 0;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ') v++;
        if (!strcasecmp(line, "Content-Length")) {
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = strlen(v);
            while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
            int64_t c;
            if (!num_parse_i64(v, vl, 0, INT64_MAX, &c)) { req_problem = "Content-Length is not a plain decimal number"; return -400; }
            if (have_cl && c != clen) { req_problem = "two different Content-Length headers"; return -400; }
            clen = c;
            have_cl = 1;
        }
        else if (!strcasecmp(line, "Authorization")) CPY(rq->auth, v);
        else if (!strcasecmp(line, "Stripe-Account")) CPY(rq->stripe_account, v);
        else if (!strcasecmp(line, "Idempotency-Key")) CPY(rq->idem_key, v);
    }
    if (clen > REQ_MAX) return -413;
    size_t blen = (size_t)clen;
    /* THE HEADERS AND THE BODY SHARE reqbuf: the header block may be as long as the buffer allows, so
     * REQ_MAX on the body alone is not a bound. The body and its NUL must fit behind the headers. */
    if (hlen + blen > sizeof reqbuf - 1) { req_problem = "the headers and the body do not fit together"; return -400; }
    while (have < hlen + blen) {
        ssize_t n = recv(fd, reqbuf + have, hlen + blen - have, 0);
        if (n <= 0) return -1;
        have += (size_t)n;
    }
    reqbuf[hlen + blen] = 0;
    rq->body = reqbuf + hlen;
    rq->body_len = blen;
    return 0;
}

/* A refused request may still be sending its body. End our side, then read and drop what arrives (up to
 * 2 MiB, until the client closes or the 5 s receive timeout) so the close is a FIN: closing with unread
 * data is an RST, which can take the answer away from a client that is still writing. */
static void drain(int fd) {
    char buf[4096];
    size_t total = 0;
    shutdown(fd, SHUT_WR);
    while (total < 2 * (size_t)REQ_MAX) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        total += (size_t)n;
    }
}

static void usage(void) {
    fprintf(stderr,
            "usage: fake_stripe [--port N] [--seed NAME] [--now UNIX] [--api-key KEY] [--webhook-url URL]\n"
            "                   [--webhook-secret SECRET] [--base-url URL] [--data FILE]\n");
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    /* every option takes one value: the index moves two at a time, in the header and nowhere else */
    for (int i = 1; i < argc; i += 2) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        int64_t n;
        if (!v) { usage(); return 2; }
        if (!strcmp(a, "--port")) {
            if (!num_parse_i64_cstr(v, 0, 65535, &n)) { fprintf(stderr, "fake_stripe: bad --port %s\n", v); return 2; }
            g_port = (int)n;
        } else if (!strcmp(a, "--seed")) CPY(g_seed, v);
        else if (!strcmp(a, "--now")) {
            if (!num_parse_i64_cstr(v, 0, TIME_MAX, &n)) { fprintf(stderr, "fake_stripe: bad --now %s\n", v); return 2; }
            g_now = n;
            g_now_set = 1;
        } else if (!strcmp(a, "--api-key")) CPY(g_api_key, v);
        else if (!strcmp(a, "--webhook-url")) CPY(g_hook_url, v);
        else if (!strcmp(a, "--webhook-secret")) CPY(g_hook_secret, v);
        else if (!strcmp(a, "--base-url")) CPY(g_base_url, v);
        else if (!strcmp(a, "--data")) CPY(g_data, v);
        else { usage(); return 2; }
    }
    G.rng = seed_state(g_seed);
    if (g_data[0] && load_state() < 0) return 1;

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("fake_stripe: socket"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)g_port);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 16) < 0) { perror("fake_stripe: bind"); return 1; }
    socklen_t sl = sizeof sa;
    getsockname(ls, (struct sockaddr *)&sa, &sl);
    g_port = ntohs(sa.sin_port);
    if (!g_base_url[0]) snprintf(g_base_url, sizeof g_base_url, "http://127.0.0.1:%d", g_port);
    printf("listening 127.0.0.1:%d\n", g_port);
    fflush(stdout);

    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { if (errno == EINTR || errno == ECONNABORTED) continue; perror("fake_stripe: accept"); break; }
        struct timeval tv = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        req_t rq;
        resp_t r;
        memset(&r, 0, sizeof r);
        mutating = 0;
        int rc = read_request(fd, &rq);
        if (rc == 0) {
            if (rq.body_len) parse_form(&rq);
            route(&rq, &r);
            if (mutating) save_state();
            write_resp(fd, &r);
            free(rq.formbuf);
        } else if (rc == -413) {
            err(&r, 413, "invalid_request_error", NULL, NULL, "Request too large.");
            write_resp(fd, &r);
            drain(fd);
        } else if (rc == -400) {
            err(&r, 400, "invalid_request_error", NULL, NULL, "Invalid request: %s.", req_problem);
            write_resp(fd, &r);
            drain(fd);
        }
        close(fd);
        sb_free(&r.body);
        flush_events();
    }
    return 1;   /* the accept loop only ends on an error, which is not a clean exit */
}
