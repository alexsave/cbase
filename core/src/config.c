#include "config.h"

#include "cbase/app.h"

#include "b64/b64.h"
#include "env_file.h"
#include "util/err.h"
#include "util/log.h"
#include "num_parse.h"
#include "util/affinity.h"
#include "util/str.h"
#include "util/uuid.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *env(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}

static bool copy(char *dst, size_t cap, const char *name, const char *v, char *err, size_t ecap) {
    if (cb_strlcpy(dst, v, cap) >= cap) {
        snprintf(err, ecap, "%s is longer than %zu characters", name, cap - 1);
        return false;
    }
    return true;
}

// THE THREADS (OPTIMIZED_DESIGN.md step 10): serve is the loop, ONE request worker and one job
// worker by default. An inline route runs on the loop; the request worker is the tables' thread
// (what would block the loop: a read or write of the store, a password's hash, a document whose lock
// another thread holds), and the job worker the compute thread (the app's jobs). A second request
// worker only contended for the store's one writer and the allocator; a box that wants more says
// so (CBASE_REQ_WORKERS), and nothing is sized from the CPUs the process sees. An app whose
// handlers wait on something remote ships its own count (cbase/app.h req_workers), under the
// variable.
#define REQ_WORKERS_DEFAULT 1

// THE SLOW POOL (net/router.h CB_ROUTE_SLOW): a few threads for the handlers that hold theirs on a
// remote call for seconds or minutes, apart from the request pool so a status poll or a file is
// never queued behind one. 4 by default (an app that has slow routes ships its own count: as many
// as it lets wait on the remote at once), and NO queue by default: a slow request either starts
// at once or is refused at once, since one that waited would wait for a whole slow handler to end
// (minutes) with nothing on the wire. An app whose clients would rather wait ships a queue.
#define SLOW_WORKERS_DEFAULT 4
#define SLOW_QUEUE_DEFAULT   0
#define SLOW_QUEUE_MAX       4096

static bool num(const char *name, long dflt, long lo, long hi, long *out, char *err, size_t ecap) {
    const char *v = getenv(name);
    if (!v || !*v) { *out = dflt; return true; }
    int64_t x;
    if (!num_parse_i64_cstr(v, lo, hi, &x)) {
        snprintf(err, ecap, "%s=%s is not a number in %ld..%ld", name, v, lo, hi);
        return false;
    }
    *out = (long)x;
    return true;
}

// One of the saturation knob's two variables: unset is CB_CPU_AUTO, `off` CB_CPU_OFF, else a CPU
// number (a negative one is refused: -1 spells nothing, `off` does).
static bool cpu(const char *name, int *out, char *err, size_t ecap) {
    const char *v = getenv(name);
    if (!v || !*v) { *out = CB_CPU_AUTO; return true; }
    if (strcmp(v, "off") == 0) { *out = CB_CPU_OFF; return true; }
    int64_t x;
    if (!num_parse_i64_cstr(v, 0, 1023, &x)) {
        snprintf(err, ecap, "%s=%s is not off or a CPU number in 0..1023", name, v);
        return false;
    }
    *out = (int)x;
    return true;
}

static int check_lengths(const cb_config *c, char *err, size_t cap);

// The rate limits' bursts, one setting each, in cb_limit_kind order
// (rate_limits.h; rate_limits.c asserts the count). 0 in cfg: the rule's own.
const char *const cb_config_limit_names[CB_CONFIG_LIMITS] = {
    "CBASE_LIMIT_SIGNIN_IP", "CBASE_LIMIT_SIGNIN_NAME", "CBASE_LIMIT_SIGNUP_IP", "CBASE_LIMIT_SIGNUP_ALL",
    "CBASE_LIMIT_REFRESH_IP", "CBASE_LIMIT_PASSWORD_USER", "CBASE_LIMIT_CREATE_USER", "CBASE_LIMIT_CHAT_USER",
    "CBASE_LIMIT_BUMP_USER", "CBASE_LIMIT_ID_MISS_IP",
};

// Ten upper-case letters and digits: an Apple team or key id.
static bool apple_id_ok(const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) return false;
    return n == 10;
}

// CBASE_APPLE_* (config.h SIGN IN WITH APPLE).
static int apple_from_env(cb_config *c, char *err, size_t cap) {
    if (!copy(c->apple_client_ids, sizeof c->apple_client_ids, "CBASE_APPLE_CLIENT_IDS", env("CBASE_APPLE_CLIENT_IDS", ""), err, cap) ||
        !copy(c->apple_team_id, sizeof c->apple_team_id, "CBASE_APPLE_TEAM_ID", env("CBASE_APPLE_TEAM_ID", ""), err, cap) ||
        !copy(c->apple_key_id, sizeof c->apple_key_id, "CBASE_APPLE_KEY_ID", env("CBASE_APPLE_KEY_ID", ""), err, cap) ||
        !copy(c->apple_private_key, sizeof c->apple_private_key, "CBASE_APPLE_PRIVATE_KEY", env("CBASE_APPLE_PRIVATE_KEY", ""), err, cap) ||
        !copy(c->apple_redirect_uri, sizeof c->apple_redirect_uri, "CBASE_APPLE_REDIRECT_URI", env("CBASE_APPLE_REDIRECT_URI", ""), err, cap) ||
        !copy(c->apple_issuer, sizeof c->apple_issuer, "CBASE_APPLE_ISSUER", env("CBASE_APPLE_ISSUER", "https://appleid.apple.com"), err, cap))
        return CB_E_INVALID;
    if (strncmp(c->apple_issuer, "https://", 8) != 0 || !c->apple_issuer[8] || strchr(c->apple_issuer + 8, '/')) {
        snprintf(err, cap, "CBASE_APPLE_ISSUER=%s is not https://<host>[:port]", c->apple_issuer);
        return CB_E_INVALID;
    }
    for (const char *p = c->apple_client_ids; *p; p++)
        if ((unsigned char)*p <= ' ' || *p == '"' || *p == '\\') {
            snprintf(err, cap, "CBASE_APPLE_CLIENT_IDS holds a blank or a quote: a comma list of ids, nothing else");
            return CB_E_INVALID;
        }
    bool any = c->apple_team_id[0] || c->apple_key_id[0] || c->apple_private_key[0];
    if (any && !(apple_id_ok(c->apple_team_id) && apple_id_ok(c->apple_key_id) && c->apple_private_key[0])) {
        snprintf(err, cap, "CBASE_APPLE_TEAM_ID, CBASE_APPLE_KEY_ID (each 10 letters and digits) and CBASE_APPLE_PRIVATE_KEY go together");
        return CB_E_INVALID;
    }
    if (any && !c->apple_client_ids[0]) {
        snprintf(err, cap, "CBASE_APPLE_PRIVATE_KEY is set but CBASE_APPLE_CLIENT_IDS names no client");
        return CB_E_INVALID;
    }
    if (c->apple_redirect_uri[0] && strncmp(c->apple_redirect_uri, "https://", 8) != 0) {
        snprintf(err, cap, "CBASE_APPLE_REDIRECT_URI must be an https URL");
        return CB_E_INVALID;
    }
    return CB_OK;
}

int cb_config_from_env(cb_config *c, char *err, size_t cap) {
    memset(c, 0, sizeof *c);
    const char *listen = env("CBASE_LISTEN", "127.0.0.1:8080");
    const char *colon = strrchr(listen, ':');
    if (!colon || colon == listen || !colon[1]) {
        snprintf(err, cap, "CBASE_LISTEN=%s is not host:port", listen);
        return CB_E_INVALID;
    }
    if (!cb_strncpy0(c->listen_host, sizeof c->listen_host, listen, (size_t)(colon - listen))) {
        snprintf(err, cap, "CBASE_LISTEN host is too long");
        return CB_E_INVALID;
    }
    int64_t port;
    if (!num_parse_i64_cstr(colon + 1, 0, 65535, &port)) {
        snprintf(err, cap, "CBASE_LISTEN=%s has no valid port", listen);
        return CB_E_INVALID;
    }
    c->listen_port = (int)port;

    const char *store = env("CBASE_STORE", "");
    if (*store && strcmp(store, "sqlite") != 0 && strcmp(store, "dynamo") != 0) {
        snprintf(err, cap, "CBASE_STORE=%s is not sqlite or dynamo", store);
        return CB_E_INVALID;
    }
    cb_strlcpy(c->store, store, sizeof c->store);
    if (!copy(c->db_path, sizeof c->db_path, "CBASE_DB", env("CBASE_DB", "./cbase.sqlite"), err, cap)) return CB_E_INVALID;
    if (!copy(c->table, sizeof c->table, "CBASE_TABLE", env("CBASE_TABLE", "cbase"), err, cap)) return CB_E_INVALID;
    if (!copy(c->iot_endpoint, sizeof c->iot_endpoint, "CBASE_IOT_ENDPOINT", env("CBASE_IOT_ENDPOINT", ""), err, cap))
        return CB_E_INVALID;
    for (const char *h = c->iot_endpoint; *h; h++)
        if (!((*h >= 'a' && *h <= 'z') || (*h >= '0' && *h <= '9') || *h == '-' || *h == '.')) {
            snprintf(err, cap, "CBASE_IOT_ENDPOINT=%s is not a host name", c->iot_endpoint);
            return CB_E_INVALID;
        }
    if (!copy(c->cf_distribution, sizeof c->cf_distribution, "CBASE_CF_DISTRIBUTION", env("CBASE_CF_DISTRIBUTION", ""), err, cap))
        return CB_E_INVALID;
    for (const char *d = c->cf_distribution; *d; d++)
        if (!((*d >= 'A' && *d <= 'Z') || (*d >= '0' && *d <= '9'))) {
            snprintf(err, cap, "CBASE_CF_DISTRIBUTION=%s is not a CloudFront distribution id", c->cf_distribution);
            return CB_E_INVALID;
        }
    const char *secret = env("CBASE_JWT_SECRET", NULL);
    if (secret) {
        if (!copy(c->jwt_secret, sizeof c->jwt_secret, "CBASE_JWT_SECRET", secret, err, cap)) return CB_E_INVALID;
    } else {
        uint8_t raw[32];
        cb_random_bytes(raw, sizeof raw);
        b64url_encode(raw, sizeof raw, c->jwt_secret, sizeof c->jwt_secret);
        c->jwt_secret_generated = true;
    }
    if (!copy(c->admin_token, sizeof c->admin_token, "CBASE_ADMIN_TOKEN", env("CBASE_ADMIN_TOKEN", ""), err, cap)) return CB_E_INVALID;
    if (!copy(c->secrets_ssm, sizeof c->secrets_ssm, "CBASE_SECRETS_SSM", env("CBASE_SECRETS_SSM", ""), err, cap)) return CB_E_INVALID;
    if (!copy(c->origin_secret, sizeof c->origin_secret, "CBASE_ORIGIN_SECRET", env("CBASE_ORIGIN_SECRET", ""), err, cap))
        return CB_E_INVALID;
    if (!copy(c->allowed_origins, sizeof c->allowed_origins, "CBASE_ALLOWED_ORIGINS", env("CBASE_ALLOWED_ORIGINS", ""), err, cap))
        return CB_E_INVALID;
    if (apple_from_env(c, err, cap) != CB_OK) return CB_E_INVALID;
    {
        const char *dev = getenv("CBASE_DEV");
        if (dev && *dev && strcmp(dev, "1") != 0 && strcmp(dev, "0") != 0) {
            snprintf(err, cap, "CBASE_DEV=%s is not 1 or 0", dev);
            return CB_E_INVALID;
        }
        c->dev = dev && strcmp(dev, "1") == 0;
    }
    c->public_url_given = getenv("CBASE_PUBLIC_URL") && *getenv("CBASE_PUBLIC_URL");
    char dflt_url[128];
    snprintf(dflt_url, sizeof dflt_url, "http://%s", listen);
    if (!copy(c->public_url, sizeof c->public_url, "CBASE_PUBLIC_URL", env("CBASE_PUBLIC_URL", dflt_url), err, cap)) return CB_E_INVALID;

    long v;
    if (!num("CBASE_REQ_WORKERS", REQ_WORKERS_DEFAULT, 1, 256, &v, err, cap)) return CB_E_INVALID;
    c->req_workers = (int)v;
    if (!num("CBASE_JOB_WORKERS", 1, 1, 64, &v, err, cap)) return CB_E_INVALID;
    c->job_workers = (int)v;
    if (!num("CBASE_SLOW_WORKERS", SLOW_WORKERS_DEFAULT, 1, 256, &v, err, cap)) return CB_E_INVALID;
    c->slow_workers = (int)v;
    if (!num("CBASE_SLOW_QUEUE", SLOW_QUEUE_DEFAULT, 0, SLOW_QUEUE_MAX, &v, err, cap)) return CB_E_INVALID;
    c->slow_queue = (int)v;
    // THE SATURATION KNOB (util/affinity.h): automatic by default (both unset), by hand once either
    // is given, and then the other, unset, is not pinned (config.h loop_cpu).
    if (!cpu("CBASE_LOOP_CPU", &c->loop_cpu, err, cap)) return CB_E_INVALID;
    if (!cpu("CBASE_BG_CPU", &c->bg_cpu, err, cap)) return CB_E_INVALID;
    if ((c->loop_cpu == CB_CPU_AUTO) != (c->bg_cpu == CB_CPU_AUTO)) {
        if (c->loop_cpu == CB_CPU_AUTO) c->loop_cpu = CB_CPU_OFF;
        else c->bg_cpu = CB_CPU_OFF;
    }
    if (!num("CBASE_MAX_BODY", 1L << 20, 1024, 64L << 20, &v, err, cap)) return CB_E_INVALID;
    c->max_body = v;
    if (!num("CBASE_JWT_TTL", 3600, 60, 86400 * 7, &v, err, cap)) return CB_E_INVALID;
    c->jwt_ttl_s = (int)v;
    if (!num("CBASE_ADMIN_FAIL_LIMIT", 10, 1, 100000, &v, err, cap)) return CB_E_INVALID;
    c->admin_fail_limit = (int)v;
    if (!num("CBASE_CLOCK_SPEEDUP", 1, 1, 100, &v, err, cap)) return CB_E_INVALID;
    c->clock_speedup = (int)v;
    if (!num("CBASE_MAX_CONNS", 16384, 1, 1000000, &v, err, cap)) return CB_E_INVALID;
    c->max_conns = (int)v;
    if (!num("CBASE_LISTEN_BACKLOG", 512, 1, 65535, &v, err, cap)) return CB_E_INVALID;
    c->listen_backlog = (int)v;
    if (!num("CBASE_INLINE_BUDGET_US", 2000, 0, 1000000, &v, err, cap)) return CB_E_INVALID;
    c->inline_budget_us = (int)v;
    if (!num("CBASE_IO_URING", 0, 0, 1, &v, err, cap)) return CB_E_INVALID;
    c->io_uring = v == 1;
    if (!num("CBASE_CAST_DELTA", 1, 0, 1, &v, err, cap)) return CB_E_INVALID;
    c->cast_delta = v == 1;
    for (int i = 0; i < CB_CONFIG_LIMITS; i++) {
        if (!num(cb_config_limit_names[i], 0, 1, 1000000, &v, err, cap)) return CB_E_INVALID;
        c->limit_burst[i] = (int)v;
    }
    const char *lvl = env("CBASE_LOG_LEVEL", "info");
    c->log_level = cb_log_level_parse(lvl);
    if (c->log_level < 0) {
        snprintf(err, cap, "CBASE_LOG_LEVEL=%s is not debug, info, warn or error", lvl);
        return CB_E_INVALID;
    }
    // The lengths of what the environment gave (the platform parameter store's are
    // checked again once read); whether the JWT secret had to be given is
    // the caller's (cb_config_check_secrets, cb_config_check_serve).
    return check_lengths(c, err, cap);
}

// How cb_config_each shows one of the knob's CPUs: `number` is its value printed.
static const char *cpu_shown(int v, const char *number) {
    if (v == CB_CPU_AUTO) return "(unset: auto, picked at start)";
    if (v == CB_CPU_OFF) return "off (not pinned)";
    return number;
}

void cb_config_each(const cb_config *c, cb_config_fn fn, void *ud) {
    char b[96];
    snprintf(b, sizeof b, "%s:%d", c->listen_host, c->listen_port);
    fn("CBASE_LISTEN", b, false, ud);
    fn("CBASE_STORE", c->store[0] ? c->store : "(by mode)", false, ud);
    fn("CBASE_DB", c->db_path, false, ud);
    fn("CBASE_TABLE", c->table, false, ud);
    fn("CBASE_IOT_ENDPOINT", c->iot_endpoint, false, ud);
    fn("CBASE_CF_DISTRIBUTION", c->cf_distribution[0] ? c->cf_distribution : "(unset: no edge invalidation)", false, ud);
    fn("CBASE_JWT_SECRET", c->jwt_secret_generated ? "(unset: random per process)" : "(set)", true, ud);
    fn("CBASE_ADMIN_TOKEN", c->admin_token[0] ? "(set)" : "(unset: console refused)", true, ud);
    fn("CBASE_ORIGIN_SECRET", c->origin_secret[0] ? "(set)" : "(unset: the origin is open)", true, ud);
    fn("CBASE_SECRETS_SSM", c->secrets_ssm[0] ? c->secrets_ssm : "(unset: secrets from the environment)", false, ud);
    fn("CBASE_ALLOWED_ORIGINS", c->allowed_origins, false, ud);
    fn("CBASE_PUBLIC_URL", c->public_url, false, ud);
    snprintf(b, sizeof b, "%d", c->req_workers);
    fn("CBASE_REQ_WORKERS", b, false, ud);
    snprintf(b, sizeof b, "%d", c->job_workers);
    fn("CBASE_JOB_WORKERS", b, false, ud);
    snprintf(b, sizeof b, "%d", c->slow_workers);
    fn("CBASE_SLOW_WORKERS", b, false, ud);
    snprintf(b, sizeof b, "%d", c->slow_queue);
    fn("CBASE_SLOW_QUEUE", b, false, ud);
    snprintf(b, sizeof b, "%d", c->loop_cpu);
    fn("CBASE_LOOP_CPU", cpu_shown(c->loop_cpu, b), false, ud);
    snprintf(b, sizeof b, "%d", c->bg_cpu);
    fn("CBASE_BG_CPU", cpu_shown(c->bg_cpu, b), false, ud);
    snprintf(b, sizeof b, "%ld", c->max_body);
    fn("CBASE_MAX_BODY", b, false, ud);
    snprintf(b, sizeof b, "%d", c->jwt_ttl_s);
    fn("CBASE_JWT_TTL", b, false, ud);
    snprintf(b, sizeof b, "%d", c->admin_fail_limit);
    fn("CBASE_ADMIN_FAIL_LIMIT", b, false, ud);
    fn("CBASE_LOG_LEVEL", cb_log_level_name(c->log_level), false, ud);
    snprintf(b, sizeof b, "%d", c->clock_speedup);
    fn("CBASE_CLOCK_SPEEDUP", b, false, ud);
    fn("CBASE_DEV", c->dev ? "1" : "0", false, ud);
    snprintf(b, sizeof b, "%d", c->max_conns);
    fn("CBASE_MAX_CONNS", b, false, ud);
    snprintf(b, sizeof b, "%d", c->listen_backlog);
    fn("CBASE_LISTEN_BACKLOG", b, false, ud);
    snprintf(b, sizeof b, "%d", c->inline_budget_us);
    fn("CBASE_INLINE_BUDGET_US", b, false, ud);
    fn("CBASE_IO_URING", c->io_uring ? "1" : "0", false, ud);
    fn("CBASE_CAST_DELTA", c->cast_delta ? "1" : "0", false, ud);
    fn("CBASE_APPLE_CLIENT_IDS", c->apple_client_ids[0] ? c->apple_client_ids : "(unset: no Sign in with Apple)", false, ud);
    fn("CBASE_APPLE_TEAM_ID", c->apple_team_id, false, ud);
    fn("CBASE_APPLE_KEY_ID", c->apple_key_id, false, ud);
    fn("CBASE_APPLE_PRIVATE_KEY", c->apple_private_key[0] ? "(set)" : "(unset: no code exchange, no revocation)", true, ud);
    fn("CBASE_APPLE_REDIRECT_URI", c->apple_redirect_uri, false, ud);
    fn("CBASE_APPLE_ISSUER", c->apple_issuer, false, ud);
    for (int i = 0; i < CB_CONFIG_LIMITS; i++) {
        if (c->limit_burst[i]) snprintf(b, sizeof b, "%d", c->limit_burst[i]);
        else snprintf(b, sizeof b, "(default)");
        fn(cb_config_limit_names[i], b, false, ud);
    }
}

static int check_lengths(const cb_config *c, char *err, size_t cap) {
    struct { const char *name; const char *v; } s[] = {
        { "CBASE_JWT_SECRET", c->jwt_secret },
        { "CBASE_ADMIN_TOKEN", c->admin_token },
        { "CBASE_ORIGIN_SECRET", c->origin_secret },
    };
    for (size_t i = 0; i < sizeof s / sizeof s[0]; i++)
        if (s[i].v[0] && strlen(s[i].v) < 32) {
            snprintf(err, cap, "%s must be at least 32 characters", s[i].name);
            return CB_E_INVALID;
        }
    return CB_OK;
}

int cb_config_check_secrets(const cb_config *c, bool lambda, char *err, size_t cap) {
    if (c->jwt_secret_generated && (lambda || !c->dev)) {
        snprintf(err, cap, "no CBASE_JWT_SECRET (environment or SSM): refusing to start with a random one%s",
                 lambda ? "" : " (CBASE_DEV=1 allows it for development and tests)");
        return CB_E_INVALID;
    }
    return check_lengths(c, err, cap);
}

int cb_config_check_serve(const cb_config *c, bool storeless, char *err, size_t cap) {
    int rc = storeless ? check_lengths(c, err, cap) : cb_config_check_secrets(c, false, err, cap);
    if (rc != CB_OK) return rc;
    // Only a CPU given by number is refused where threads cannot be pinned; auto pins nothing there.
    if ((c->loop_cpu >= 0 || c->bg_cpu >= 0) && !cb_affinity_supported()) {
        snprintf(err, cap, "CBASE_LOOP_CPU and CBASE_BG_CPU pin threads, which needs Linux");
        return CB_E_INVALID;
    }
    if (c->dev || (storeless && !c->public_url_given)) return rc;
    if (!c->public_url_given) {
        snprintf(err, cap, "no CBASE_PUBLIC_URL: it is the token issuer, and the default (http://%s:%d) is no one's address "
                 "(CBASE_DEV=1 allows it)", c->listen_host, c->listen_port);
        return CB_E_INVALID;
    }
    const char *h = strstr(c->public_url, "://");
    h = h ? h + 3 : c->public_url;
    if (strncmp(h, "0.0.0.0", 7) == 0 || strncmp(h, "[::]", 4) == 0) {
        snprintf(err, cap, "CBASE_PUBLIC_URL=%s names the wildcard address, not the address clients use", c->public_url);
        return CB_E_INVALID;
    }
    return CB_OK;
}

bool cb_config_origin_proven(const cb_config *c, const char *value) {
    if (!c->origin_secret[0]) return true;
    if (!value) return false;
    size_t n = strlen(c->origin_secret);
    if (strlen(value) != n) return false;
    return CRYPTO_memcmp(value, c->origin_secret, n) == 0;
}

// Is `want` (on bytes) one of the comma list's entries, blanks around each trimmed?
static bool origin_listed(const char *p, const char *want, size_t on) {
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        const char *t = e;
        while (t > p && t[-1] == ' ') t--;
        size_t n = (size_t)(t - p);
        if (n == on && n > 0 && memcmp(p, want, n) == 0) return true;
        p = e;
    }
    return false;
}

bool cb_config_origin_allowed(const cb_config *c, const char *origin) {
    return strcmp(origin, "*") != 0 && origin_listed(c->allowed_origins, origin, strlen(origin));
}

bool cb_config_origin_any(const cb_config *c) { return origin_listed(c->allowed_origins, "*", 1); }

// Was the variable given? The rule env() reads by: set and not empty.
static bool given(const char *name) {
    const char *v = getenv(name);
    return v && *v;
}

// THE APP'S DEFAULTS (cbase/app.h "serve's defaults"): each over cbase's own, under the
// environment's. Checked whether or not the environment overrides it, so an app's bad value is
// found on the first boot, not on the first one without the variable.
static int app_defaults(cb_config *c, const cb_app *app, char *err, size_t cap) {
    if (app->req_workers < 0 || app->req_workers > 256) {
        snprintf(err, cap, "the app's req_workers %d is not in 1..256 (0: cbase's default)", app->req_workers);
        return CB_E_INVALID;
    }
    if (app->req_workers && !given("CBASE_REQ_WORKERS")) c->req_workers = app->req_workers;
    if (app->slow_workers < 0 || app->slow_workers > 256) {
        snprintf(err, cap, "the app's slow_workers %d is not in 1..256 (0: cbase's default)", app->slow_workers);
        return CB_E_INVALID;
    }
    if (app->slow_workers && !given("CBASE_SLOW_WORKERS")) c->slow_workers = app->slow_workers;
    if (app->slow_queue < 0 || app->slow_queue > SLOW_QUEUE_MAX) {
        snprintf(err, cap, "the app's slow_queue %d is not in 0..%d (0: cbase's default, none)", app->slow_queue, SLOW_QUEUE_MAX);
        return CB_E_INVALID;
    }
    if (app->slow_queue && !given("CBASE_SLOW_QUEUE")) c->slow_queue = app->slow_queue;
    if (app->log_level) {
        int lvl = cb_log_level_parse(app->log_level);
        if (lvl < 0) {
            snprintf(err, cap, "the app's log_level %s is not debug, info, warn or error", app->log_level);
            return CB_E_INVALID;
        }
        if (!given("CBASE_LOG_LEVEL")) c->log_level = lvl;
    }
    return CB_OK;
}

int cb_config_load(cb_config *cfg, const cb_app *app, char *err, size_t cap) {
    int rc = given("CBASE_ENV_FILE") ? cb_env_file_load(getenv("CBASE_ENV_FILE"), err, cap) : CB_OK;
    if (rc == CB_OK) rc = cb_config_from_env(cfg, err, cap);
    if (rc == CB_OK && app) rc = app_defaults(cfg, app, err, cap);
    if (rc == CB_OK && app && app->config) rc = app->config(cfg, err, cap);
    return rc;
}

int64_t cb_config_scaled_ms(const cb_config *cfg, int64_t ms) {
    const int64_t k = cfg && cfg->clock_speedup > 1 ? cfg->clock_speedup : 1;
    return (ms + k - 1) / k;
}
