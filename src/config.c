#include "config.h"

#include "util/b64.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"
#include "util/uuid.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static bool num(const char *name, long dflt, long lo, long hi, long *out, char *err, size_t ecap) {
    const char *v = getenv(name);
    if (!v || !*v) { *out = dflt; return true; }
    char *end = NULL;
    long x = strtol(v, &end, 10);
    if (*end || x < lo || x > hi) {
        snprintf(err, ecap, "%s=%s is not a number in %ld..%ld", name, v, lo, hi);
        return false;
    }
    *out = x;
    return true;
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
    char *end = NULL;
    long port = strtol(colon + 1, &end, 10);
    if (*end || port < 0 || port > 65535) {
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
        cb_b64url_encode(raw, sizeof raw, c->jwt_secret, sizeof c->jwt_secret);
        c->jwt_secret_generated = true;
    }
    if (!copy(c->admin_token, sizeof c->admin_token, "CBASE_ADMIN_TOKEN", env("CBASE_ADMIN_TOKEN", ""), err, cap)) return CB_E_INVALID;
    if (!copy(c->secrets_ssm, sizeof c->secrets_ssm, "CBASE_SECRETS_SSM", env("CBASE_SECRETS_SSM", ""), err, cap)) return CB_E_INVALID;
    if (!copy(c->origin_secret, sizeof c->origin_secret, "CBASE_ORIGIN_SECRET", env("CBASE_ORIGIN_SECRET", ""), err, cap))
        return CB_E_INVALID;
    if (!copy(c->allowed_origins, sizeof c->allowed_origins, "CBASE_ALLOWED_ORIGINS", env("CBASE_ALLOWED_ORIGINS", "*"), err, cap))
        return CB_E_INVALID;
    char dflt_url[128];
    snprintf(dflt_url, sizeof dflt_url, "http://%s", listen);
    if (!copy(c->public_url, sizeof c->public_url, "CBASE_PUBLIC_URL", env("CBASE_PUBLIC_URL", dflt_url), err, cap)) return CB_E_INVALID;

    long v;
    if (!num("CBASE_REQ_WORKERS", 4, 1, 256, &v, err, cap)) return CB_E_INVALID;
    c->req_workers = (int)v;
    if (!num("CBASE_BOT_WORKERS", 1, 1, 64, &v, err, cap)) return CB_E_INVALID;
    c->bot_workers = (int)v;
    if (!num("CBASE_MAX_BODY", 1L << 20, 1024, 64L << 20, &v, err, cap)) return CB_E_INVALID;
    c->max_body = v;
    if (!num("CBASE_JWT_TTL", 3600, 60, 86400 * 7, &v, err, cap)) return CB_E_INVALID;
    c->jwt_ttl_s = (int)v;
    if (!num("CBASE_ADMIN_FAIL_LIMIT", 10, 1, 100000, &v, err, cap)) return CB_E_INVALID;
    c->admin_fail_limit = (int)v;
    if (!num("CBASE_RATED_MIN_AGE", 72 * 3600, 0, 30 * 86400, &v, err, cap)) return CB_E_INVALID;
    c->rated_min_age_s = (int)v;
    const char *lvl = env("CBASE_LOG_LEVEL", "info");
    c->log_level = cb_log_level_parse(lvl);
    if (c->log_level < 0) {
        snprintf(err, cap, "CBASE_LOG_LEVEL=%s is not debug, info, warn or error", lvl);
        return CB_E_INVALID;
    }
    // The lengths of what the environment gave (SSM's are checked again
    // once read, main_lambda.c): one rule, cb_config_check_secrets.
    return cb_config_check_secrets(c, false, err, cap);
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
    snprintf(b, sizeof b, "%d", c->bot_workers);
    fn("CBASE_BOT_WORKERS", b, false, ud);
    snprintf(b, sizeof b, "%ld", c->max_body);
    fn("CBASE_MAX_BODY", b, false, ud);
    snprintf(b, sizeof b, "%d", c->jwt_ttl_s);
    fn("CBASE_JWT_TTL", b, false, ud);
    snprintf(b, sizeof b, "%d", c->admin_fail_limit);
    fn("CBASE_ADMIN_FAIL_LIMIT", b, false, ud);
    snprintf(b, sizeof b, "%d", c->rated_min_age_s);
    fn("CBASE_RATED_MIN_AGE", b, false, ud);
    fn("CBASE_LOG_LEVEL", cb_log_level_name(c->log_level), false, ud);
}

int cb_config_check_secrets(const cb_config *c, bool lambda, char *err, size_t cap) {
    if (lambda && c->jwt_secret_generated) {
        snprintf(err, cap, "no CBASE_JWT_SECRET (environment or SSM): refusing to start with a random one");
        return CB_E_INVALID;
    }
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

bool cb_config_origin_proven(const cb_config *c, const char *value) {
    if (!c->origin_secret[0]) return true;
    if (!value) return false;
    size_t n = strlen(c->origin_secret);
    if (strlen(value) != n) return false;
    return CRYPTO_memcmp(value, c->origin_secret, n) == 0;
}

bool cb_config_origin_allowed(const cb_config *c, const char *origin) {
    const char *p = c->allowed_origins;
    size_t on = strlen(origin);
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        const char *t = e;
        while (t > p && t[-1] == ' ') t--;
        size_t n = (size_t)(t - p);
        if ((n == 1 && *p == '*') || (n == on && n > 0 && memcmp(p, origin, n) == 0)) return true;
        p = e;
    }
    return false;
}
