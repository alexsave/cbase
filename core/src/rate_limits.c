// rate_limits.c - in-memory rate limits (rate_limits.h).
#include "rate_limits.h"

#include "cbase/provider.h"

#include "util/str.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../../../shared/c/fnv.h"

#define MIN_MS (60LL * 1000)

// The numbers. A person using the app is far below each one; a script is not.
cb_limit_rule cb_limit_rules[CB_LIMIT_KINDS] = {
    [CB_LIMIT_SIGNIN_IP]     = { "signin per address", 20, 5 * MIN_MS },
    [CB_LIMIT_SIGNIN_NAME]   = { "signin per account", 10, 5 * MIN_MS },
    [CB_LIMIT_SIGNUP_IP]     = { "signup per address", 5, 60 * MIN_MS },
    [CB_LIMIT_SIGNUP_ALL]    = { "signup per container", 60, 60 * MIN_MS },
    [CB_LIMIT_REFRESH_IP]    = { "refresh per address", 60, 5 * MIN_MS },
    [CB_LIMIT_PASSWORD_USER] = { "password change per account", 5, 60 * MIN_MS },
    [CB_LIMIT_CREATE_USER]   = { "create per account", 20, 10 * MIN_MS },
    [CB_LIMIT_CHAT_USER]     = { "chat per account", 30, MIN_MS },
    [CB_LIMIT_SUB_USER]      = { "subscribe per account", 120, MIN_MS },
    [CB_LIMIT_BUMP_USER]     = { "bump per account", 30, MIN_MS },
    [CB_LIMIT_ADMIN_FAIL_IP] = { "wrong admin token per address", 10, 60 * MIN_MS },
};

// An open-addressed table of buckets. A key's slot is found within PROBE
// slots of its hash; when all of them hold other keys, the one heard from
// longest ago is replaced (a bucket evicted is a bucket forgiven: best
// effort, and memory stays fixed at SLOTS buckets whatever arrives).
#define SLOTS 4096
#define PROBE 8

typedef struct {
    uint64_t h;          // 0: empty
    double   tokens;
    int64_t  last_ms;
} bucket;

static bucket g_b[SLOTS];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

// FNV-1a over the kind and the key; never 0 (0 marks an empty slot).
static uint64_t hash_of(cb_limit_kind kind, const char *key) {
    uint64_t h = fnv1a64_str(fnv1a64_step(FNV64_OFFSET_SHORT, (uint64_t)(kind + 1)), key);
    return h ? h : 1;
}

static bool take_or_peek(cb_limit_kind kind, const char *key, int64_t now_ms, int *retry_s, bool take);

void cb_limit_set_admin_fail_burst(int burst) {
    if (burst > 0) cb_limit_rules[CB_LIMIT_ADMIN_FAIL_IP].burst = burst;
}

bool cb_limit_take(cb_limit_kind kind, const char *key, int64_t now_ms, int *retry_s) {
    return take_or_peek(kind, key, now_ms, retry_s, true);
}

bool cb_limit_peek(cb_limit_kind kind, const char *key, int64_t now_ms) {
    return take_or_peek(kind, key, now_ms, NULL, false);
}

static bool take_or_peek(cb_limit_kind kind, const char *key, int64_t now_ms, int *retry_s, bool take) {
    if (retry_s) *retry_s = 0;
    if ((int)kind < 0 || kind >= CB_LIMIT_KINDS || !key || !key[0]) return true;
    const cb_limit_rule *r = &cb_limit_rules[kind];
    const double rate = (double)r->burst / (double)r->window_ms;   // tokens per ms
    uint64_t h = hash_of(kind, key);
    pthread_mutex_lock(&g_mu);
    bucket *b = NULL, *empty = NULL, *oldest = NULL;
    for (int i = 0; i < PROBE && !b; i++) {
        bucket *c = &g_b[(h + (uint64_t)i) % SLOTS];
        if (c->h == h) b = c;
        else if (c->h == 0) { if (!empty) empty = c; }
        else if (!oldest || c->last_ms < oldest->last_ms) oldest = c;
    }
    if (!b) {
        b = empty ? empty : oldest;
        b->h = h;
        b->tokens = r->burst;
        b->last_ms = now_ms;
    }
    if (now_ms > b->last_ms) {
        b->tokens += (double)(now_ms - b->last_ms) * rate;
        if (b->tokens > r->burst) b->tokens = r->burst;
        b->last_ms = now_ms;
    }
    bool ok = b->tokens >= 1.0;
    if (ok && take) b->tokens -= 1.0;
    else if (retry_s) *retry_s = (int)((1.0 - b->tokens) / rate / 1000.0) + 1;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

void cb_limits_reset(void) {
    pthread_mutex_lock(&g_mu);
    memset(g_b, 0, sizeof g_b);
    pthread_mutex_unlock(&g_mu);
}

// "1.2.3.4:5678" -> "1.2.3.4"; "2001:db8::1:5678" or "[2001:db8::1]:5678" ->
// the /64 "2001:db8::/64". Anything that does not parse is kept as it came
// (still a key: whoever sent it keeps sending the same text).
static bool canonical(const char *host, char *out, size_t cap) {
    struct in_addr v4;
    struct in6_addr v6;
    if (inet_pton(AF_INET, host, &v4) == 1) return inet_ntop(AF_INET, &v4, out, (socklen_t)cap) != NULL;
    if (inet_pton(AF_INET6, host, &v6) != 1) return false;
    memset(v6.s6_addr + 8, 0, 8);
    char t[INET6_ADDRSTRLEN];
    if (!inet_ntop(AF_INET6, &v6, t, sizeof t)) return false;
    snprintf(out, cap, "%s/64", t);
    return true;
}

static void address_key(const char *in, char *out, size_t cap) {
    char a[128];
    if (cb_strlcpy(a, in, sizeof a) >= sizeof a) { cb_strlcpy(out, in, cap); return; }
    if (canonical(a, out, cap)) return;            // no port
    char *host = a;
    if (*host == '[') {                            // [v6]:port
        host++;
        char *close = strchr(host, ']');
        if (close) *close = 0;
    } else {
        char *colon = strrchr(host, ':');          // v4:port, or v6:port
        if (colon) *colon = 0;
    }
    if (!canonical(host, out, cap)) cb_strlcpy(out, in, cap);
}

void cb_viewer_key(const cb_ctx *ctx, const cb_http_req *req, char *out, size_t cap) {
    if (cap) out[0] = 0;
    const char *v = NULL;
    if (ctx->prov && ctx->prov->viewer_header) v = cb_http_header_get(req, ctx->prov->viewer_header);
    if (!v || !*v) v = req->peer;
    if (v && *v) address_key(v, out, cap);
}
