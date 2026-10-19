// config.h - everything cbase reads from its environment, read once at start.
//
// Production sets these as the Lambda function's environment, from the
// CloudFormation parameters (providers/aws/ops/cbase.yaml); `cbase serve` reads the shell's.
// Every one has a development default except the secrets, whose absence is
// logged loudly (see each field). The AWS_* variables Lambda itself sets
// (credentials, region) are read by src/aws, not here.
#ifndef CB_CONFIG_H
#define CB_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    // CBASE_LISTEN, "host:port", default 127.0.0.1:8080. Port 0 asks the OS for
    // a free one (the smoke test reads the bound port from the log line).
    char listen_host[64];
    int  listen_port;
    // CBASE_STORE: "sqlite" or "dynamo". Default: sqlite under serve, dynamo
    // under Lambda (boot.c decides when this is empty).
    char store[16];
    // CBASE_DB, the SQLite file (store sqlite), default ./cbase.sqlite.
    char db_path[1024];
    // CBASE_TABLE, the DynamoDB table (store dynamo), default "cbase".
    char table[256];
    // CBASE_IOT_ENDPOINT, the account's AWS IoT Core data endpoint, a host
    // name ("<prefix>-ats.iot.<region>.amazonaws.com"): every push is also
    // published there over MQTT (realtime/rt_iot.h). Lambda only; empty means
    // no IoT transport.
    char iot_endpoint[256];
    // CBASE_CF_DISTRIBUTION, the CloudFront distribution in front of the API
    // (its id, E...): the one the edge cache's invalidations go to (app/edge.h).
    // Empty: nothing is invalidated (serve, tests, a first deploy).
    char cf_distribution[64];
    // CBASE_JWT_SECRET: HS256 key for access tokens. Unset: a random one per
    // process (logged as a warning: every session dies at restart).
    char jwt_secret[512];
    bool jwt_secret_generated;
    // CBASE_ADMIN_TOKEN: Bearer token for /admin/api/*, at least 32
    // characters. Unset: the console is refused.
    char admin_token[256];
    // CBASE_SECRETS_SSM: an SSM Parameter Store prefix ("/cbase/") the JWT
    // secret and the admin token above are read from when a Lambda container starts
    // (aws/ssm.h), so they are not in the function's environment. Empty: the
    // environment's.
    char secrets_ssm[256];
    // CBASE_ORIGIN_SECRET: the value CloudFront adds to every request it sends
    // on, as the header X-Cbase-Origin (providers/aws/ops/cbase.yaml). Set: an HTTP request
    // or an MQTT CONNECT (realtime/rt_iot.c) without it is refused before
    // anything else runs, so the Function URL and IoT Core's own hostname
    // cannot be used to walk around the edge (and around the viewer address
    // the edge vouches for). Unset (serve, tests): nothing is checked. At
    // least 32 characters when set.
    char origin_secret[128];
    // CBASE_ALLOWED_ORIGINS: "*" (default) or a comma list of origins echoed in
    // Access-Control-Allow-Origin.
    char allowed_origins[1024];
    // CBASE_PUBLIC_URL: the URL clients use (https://api.foolish.cards), for
    // logs and the admin console. Default http://<listen>.
    char public_url[512];
    // Tunables, each CBASE_<NAME> with these defaults:
    int  req_workers;     // CBASE_REQ_WORKERS 4: serve's request pool
    int  bot_workers;     // CBASE_BOT_WORKERS 1: serve's bot pool; see DESIGN.md "Kernel thread safety"
    long max_body;        // CBASE_MAX_BODY 1048576: request body bytes
    int  jwt_ttl_s;       // CBASE_JWT_TTL 3600: access token lifetime
    int  admin_fail_limit;// CBASE_ADMIN_FAIL_LIMIT 10: wrong console tokens per address an hour (rate_limits.h)
    int  rated_min_age_s; // CBASE_RATED_MIN_AGE 259200 (72 h): a game with a younger account ends unrated (M6)
    int  log_level;       // CBASE_LOG_LEVEL info: debug|info|warn|error
    // CBASE_BOT_SPEEDUP 1 (1..100): every timer of the bot loop (app/bots.h:
    // the pace a bot waits before it acts, the lease, the scan's staleness and
    // abandon windows, the drive's wall ceiling, serve's scan period) divided
    // by this. For the smoke tests, so a game against a bot and a crash's
    // lease expiry play out in seconds with every ratio between the timers
    // kept. `cbase serve` only: cb_boot refuses any other value under Lambda,
    // so a stray variable cannot make production's bots hurry.
    int  bot_speedup;
} cb_config;

// Reads the environment into cfg. CB_OK, or CB_E_INVALID with a sentence in err.
int cb_config_from_env(cb_config *cfg, char *err, size_t cap);

// Every setting as NAME and value, secrets replaced by "(set)" or "(unset)",
// for the startup log and the admin console's Config panel.
typedef void (*cb_config_fn)(const char *name, const char *value, bool secret, void *ud);
void cb_config_each(const cb_config *cfg, cb_config_fn fn, void *ud);

// The secrets as they must be to serve, once every source has been read
// (the environment, then SSM under Lambda): under Lambda the JWT secret must
// have been given (a random one per container would sign users out at
// random), and every secret that is set must be at least 32 characters.
// CB_OK, or CB_E_INVALID with a sentence in err.
int cb_config_check_secrets(const cb_config *cfg, bool lambda, char *err, size_t cap);

// A timer of the platform's own (a schedule's period under serve) as this
// process runs it: divided by CBASE_BOT_SPEEDUP, rounded up so never 0.
int64_t cb_config_scaled_ms(const cb_config *cfg, int64_t ms);

// The header CloudFront adds (CBASE_ORIGIN_SECRET above).
#define CB_ORIGIN_HEADER "X-Cbase-Origin"
// The header CloudFront puts the viewer's "ip:port" in (net/viewer_addr.h).
#define CB_VIEWER_HEADER "CloudFront-Viewer-Address"
// Did the request come through the edge? True when no secret is configured;
// else only for `value` equal to it (constant time). NULL is a missing header.
bool cb_config_origin_proven(const cb_config *cfg, const char *value);

// Is `origin` allowed by CBASE_ALLOWED_ORIGINS? Always true for "*".
bool cb_config_origin_allowed(const cb_config *cfg, const char *origin);

#endif
