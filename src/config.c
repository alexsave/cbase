#include "config.h"

#include "util/b64.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"
#include "util/uuid.h"

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

    if (!copy(c->db_path, sizeof c->db_path, "CBASE_DB", env("CBASE_DB", "./cbase.sqlite"), err, cap)) return CB_E_INVALID;
    const char *secret = env("CBASE_JWT_SECRET", NULL);
    if (secret) {
        if (strlen(secret) < 32) {
            snprintf(err, cap, "CBASE_JWT_SECRET must be at least 32 characters");
            return CB_E_INVALID;
        }
        if (!copy(c->jwt_secret, sizeof c->jwt_secret, "CBASE_JWT_SECRET", secret, err, cap)) return CB_E_INVALID;
    } else {
        uint8_t raw[32];
        cb_random_bytes(raw, sizeof raw);
        cb_b64url_encode(raw, sizeof raw, c->jwt_secret, sizeof c->jwt_secret);
        c->jwt_secret_generated = true;
    }
    if (!copy(c->anon_key, sizeof c->anon_key, "CBASE_ANON_KEY", env("CBASE_ANON_KEY", ""), err, cap)) return CB_E_INVALID;
    if (!copy(c->admin_token, sizeof c->admin_token, "CBASE_ADMIN_TOKEN", env("CBASE_ADMIN_TOKEN", ""), err, cap)) return CB_E_INVALID;
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
    if (!num("CBASE_MAX_RESIDENT", 1024, 1, 1000000, &v, err, cap)) return CB_E_INVALID;
    c->max_resident = (int)v;
    if (!num("CBASE_MAX_BODY", 1L << 20, 1024, 64L << 20, &v, err, cap)) return CB_E_INVALID;
    c->max_body = v;
    if (!num("CBASE_JWT_TTL", 3600, 60, 86400 * 7, &v, err, cap)) return CB_E_INVALID;
    c->jwt_ttl_s = (int)v;
    const char *lvl = env("CBASE_LOG_LEVEL", "info");
    c->log_level = cb_log_level_parse(lvl);
    if (c->log_level < 0) {
        snprintf(err, cap, "CBASE_LOG_LEVEL=%s is not debug, info, warn or error", lvl);
        return CB_E_INVALID;
    }
    return CB_OK;
}

void cb_config_each(const cb_config *c, cb_config_fn fn, void *ud) {
    char b[32];
    snprintf(b, sizeof b, "%s:%d", c->listen_host, c->listen_port);
    fn("CBASE_LISTEN", b, false, ud);
    fn("CBASE_DB", c->db_path, false, ud);
    fn("CBASE_JWT_SECRET", c->jwt_secret_generated ? "(unset: random per process)" : "(set)", true, ud);
    fn("CBASE_ANON_KEY", c->anon_key[0] ? "(set)" : "(unset: apikey check off)", true, ud);
    fn("CBASE_ADMIN_TOKEN", c->admin_token[0] ? "(set)" : "(unset: console refused)", true, ud);
    fn("CBASE_ALLOWED_ORIGINS", c->allowed_origins, false, ud);
    fn("CBASE_PUBLIC_URL", c->public_url, false, ud);
    snprintf(b, sizeof b, "%d", c->req_workers);
    fn("CBASE_REQ_WORKERS", b, false, ud);
    snprintf(b, sizeof b, "%d", c->bot_workers);
    fn("CBASE_BOT_WORKERS", b, false, ud);
    snprintf(b, sizeof b, "%d", c->max_resident);
    fn("CBASE_MAX_RESIDENT", b, false, ud);
    snprintf(b, sizeof b, "%ld", c->max_body);
    fn("CBASE_MAX_BODY", b, false, ud);
    snprintf(b, sizeof b, "%d", c->jwt_ttl_s);
    fn("CBASE_JWT_TTL", b, false, ud);
    fn("CBASE_LOG_LEVEL", cb_log_level_name(c->log_level), false, ud);
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
