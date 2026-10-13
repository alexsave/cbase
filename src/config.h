// config.h - everything cbase reads from its environment, read once at start.
//
// Production sets these in /etc/cbase/env (ops/). Every one has a development
// default except the secrets, whose absence is logged loudly (see each field).
#ifndef CB_CONFIG_H
#define CB_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    // CBASE_LISTEN, "host:port", default 127.0.0.1:8080. Port 0 asks the OS for
    // a free one (the smoke test reads the bound port from the log line).
    char listen_host[64];
    int  listen_port;
    // CBASE_DB, default ./cbase.sqlite.
    char db_path[1024];
    // CBASE_JWT_SECRET: HS256 key for access tokens. Unset: a random one per
    // process (logged as a warning: every session dies at restart).
    char jwt_secret[512];
    bool jwt_secret_generated;
    // CBASE_ANON_KEY: the opaque string clients send as `apikey`. Unset: the
    // apikey check is OFF (development only; logged as a warning).
    char anon_key[1024];
    // CBASE_ADMIN_TOKEN: Bearer token for /admin/api/*. Unset: the console is refused.
    char admin_token[256];
    // CBASE_ALLOWED_ORIGINS: "*" (default) or a comma list of origins echoed in
    // Access-Control-Allow-Origin.
    char allowed_origins[1024];
    // CBASE_PUBLIC_URL: the URL clients use (https://api.foolish.cards), for
    // logs and the admin console. Default http://<listen>.
    char public_url[512];
    // Tunables, each CBASE_<NAME> with these defaults:
    int  req_workers;     // CBASE_REQ_WORKERS 4: the request pool
    int  bot_workers;     // CBASE_BOT_WORKERS 1: see DESIGN.md "Kernel thread safety"
    int  max_resident;    // CBASE_MAX_RESIDENT 1024: games kept in RAM (LRU)
    long max_body;        // CBASE_MAX_BODY 1048576: request body bytes
    int  jwt_ttl_s;       // CBASE_JWT_TTL 3600: access token lifetime
    int  log_level;       // CBASE_LOG_LEVEL info: debug|info|warn|error
} cb_config;

// Reads the environment into cfg. CB_OK, or CB_E_INVALID with a sentence in err.
int cb_config_from_env(cb_config *cfg, char *err, size_t cap);

// Every setting as NAME and value, secrets replaced by "(set)" or "(unset)",
// for the startup log and the admin console's Config panel.
typedef void (*cb_config_fn)(const char *name, const char *value, bool secret, void *ud);
void cb_config_each(const cb_config *cfg, cb_config_fn fn, void *ud);

// Is `origin` allowed by CBASE_ALLOWED_ORIGINS? Always true for "*".
bool cb_config_origin_allowed(const cb_config *cfg, const char *origin);

#endif
