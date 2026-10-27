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

#define CB_CONFIG_LIMITS 10   // the settable rate limits (rate_limits.h, all but the admin one)
extern const char *const cb_config_limit_names[CB_CONFIG_LIMITS];

// The saturation knob's two values that are not a CPU (cb_config.loop_cpu and bg_cpu).
#define CB_CPU_OFF  (-1)   // not pinned
#define CB_CPU_AUTO (-2)   // serve picks the CPU at start (cb_affinity_pick)

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
    // CBASE_PUBLIC_URL was given (not defaulted from CBASE_LISTEN): `cbase
    // serve` outside dev mode refuses to start without it, because it is the
    // JWT issuer (auth.c) and http://0.0.0.0:8080 is no one's address.
    bool public_url_given;
    // CBASE_DEV=1: the explicit development and test mode of `cbase serve`.
    // Only here may the JWT secret be a random one per process and the public
    // URL be defaulted from the listen address. Production gives both.
    bool dev;
    // Tunables, each CBASE_<NAME> with these defaults:
    int  req_workers;     // CBASE_REQ_WORKERS 1: serve's request pool (config.c THE THREADS)
    int  bot_workers;     // CBASE_BOT_WORKERS 1: serve's bot pool; see DESIGN.md "Kernel thread safety"
    // THE SATURATION KNOB (util/affinity.h): CBASE_LOOP_CPU, the CPU serve's loop is pinned to, and
    // CBASE_BG_CPU, the CPU every other serve thread is pinned to. Each is a CPU number, `off`
    // (CB_CPU_OFF) or unset. Both unset is CB_CPU_AUTO in both: serve picks the two lowest CPUs it
    // may use when it starts, and pins nothing where it has fewer than two or cannot pin. Either one
    // given takes the knob by hand, and the other, if unset, is then CB_CPU_OFF.
    int  loop_cpu;
    int  bg_cpu;
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
    // Serve's connection limits (net/server.h): CBASE_MAX_CONNS 4096 open
    // connections (a new one past it is refused, and logged), CBASE_LISTEN_BACKLOG 512.
    int  max_conns;
    int  listen_backlog;
    // Inline dispatch (net/server.h): a CB_ROUTE_INLINE request runs on the loop
    // thread when the loop pass has inline time left; a run longer than this many microseconds of the loop thread's CPU
    // (or its route's own budget) is over budget, and three of them within a second switch inline off for 5 s for
    // every route whose home is not the loop (router.h CB_ROUTE_HOME).
    // CBASE_INLINE_BUDGET_US, default 2000; 0 turns inline dispatch off.
    int  inline_budget_us;
    // CBASE_IO_URING=1: the network loop waits on io_uring and hands a pass's sends to the kernel in
    // the same system call (net/loop.h), where Linux allows it; elsewhere, or where the kernel or a
    // seccomp filter refuses it, epoll as before (said in the log). Default 0: epoll.
    bool io_uring;
    // CBASE_CAST_DELTA, default 1: the app casts a commit that only moved as a
    // delta (the moves, the viewer's own draws and a board check, about a fifth of
    // the bytes) for a client kernel that rebuilds it into the board cast. 0 casts
    // every commit as the board cast, the switch back if a deployed client ever
    // cannot read deltas; a page with an older kernel refetches at each move until
    // it reloads.
    bool cast_delta;
    // The bursts of the rate limits (rate_limits.h), per window, indexed by
    // cb_limit_kind (0 = the rule's own default). Set by CBASE_LIMIT_<KIND>:
    // SIGNIN_IP 20/5min, SIGNIN_NAME 10/5min, SIGNUP_IP 5/h, SIGNUP_ALL 60/h,
    // REFRESH_IP 60/5min, PASSWORD_USER 5/h, CREATE_USER 20/10min,
    // CHAT_USER 30/min, BUMP_USER 30/min, ID_MISS_IP 30/5min. The admin one is
    // CBASE_ADMIN_FAIL_LIMIT above.
    int  limit_burst[CB_CONFIG_LIMITS];
    // SIGN IN WITH APPLE (auth/oidc.h; docs/AUTH_APPLE.md is the checklist). Off unless
    // CBASE_APPLE_CLIENT_IDS names at least one client.
    //   CBASE_APPLE_CLIENT_IDS    comma list of the audiences an identity token may carry: the iOS
    //                             app's bundle id, the web's Services ID
    //   CBASE_APPLE_TEAM_ID       the developer team (10 characters): the client secret's issuer
    //   CBASE_APPLE_KEY_ID        the Sign in with Apple key's id (10 characters)
    //   CBASE_APPLE_PRIVATE_KEY   that key, the .p8 file's PEM text ("\n" may stand for each line
    //                             break) (secret). With the two above it turns on the code exchange
    //                             and revocation; without them, sign-in verifies tokens only
    //   CBASE_APPLE_REDIRECT_URI  the web flow's return URL, which the code exchange repeats
    //   CBASE_APPLE_ISSUER        https://appleid.apple.com: the tokens' "iss" and the host of
    //                             /auth/keys, /auth/token and /auth/revoke (a test's stand-in names its own)
    char apple_client_ids[512];
    char apple_team_id[16];
    char apple_key_id[16];
    char apple_private_key[2048];
    char apple_redirect_uri[512];
    char apple_issuer[256];
} cb_config;

// Reads the environment into cfg. CB_OK, or CB_E_INVALID with a sentence in err.
int cb_config_from_env(cb_config *cfg, char *err, size_t cap);

// Every setting as NAME and value, secrets replaced by "(set)" or "(unset)",
// for the startup log and the admin console's Config panel.
typedef void (*cb_config_fn)(const char *name, const char *value, bool secret, void *ud);
void cb_config_each(const cb_config *cfg, cb_config_fn fn, void *ud);

// The secrets as they must be to serve, once every source has been read
// (the environment, then SSM under Lambda): the JWT secret must have been
// given (a random one per process would sign users out at every restart),
// on the one-shot platform always and under serve unless CBASE_DEV=1; and every secret that is set must be at least 32 characters.
// CB_OK, or CB_E_INVALID with a sentence in err.
int cb_config_check_secrets(const cb_config *cfg, bool lambda, char *err, size_t cap);

// What `cbase serve` must have before it listens: cb_config_check_secrets
// (the JWT secret given unless CBASE_DEV=1) and, outside dev mode, an explicit
// CBASE_PUBLIC_URL that does not name 0.0.0.0. CB_OK, or CB_E_INVALID with a
// sentence in err.
int cb_config_check_serve(const cb_config *cfg, char *err, size_t cap);

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
